#include "camera_manager.hpp"
#include "utils.hpp"
#include <algorithm>

CameraManager::CameraManager(const ParsedConfig& cfg, AlarmQueue& q) : cfg_(cfg), queue_(q) {}
CameraManager::~CameraManager() { stop(); }

bool CameraManager::initialize() {
    nodes_.reserve(cfg_.cameras.size());
    for (const auto& c : cfg_.cameras) {
        int pre = c->pre_buffer_iframes>0 ? c->pre_buffer_iframes : cfg_.global->pre_buffer_iframes;
        int post = c->post_buffer_iframes>0 ? c->post_buffer_iframes : cfg_.global->post_buffer_iframes;
        auto n = std::make_unique<CameraNode>(c, pre, post);
        if (!n->initialize()) return false;
        nodes_.push_back(std::move(n));
    }
    for (size_t i=0; i<cfg_.cameras.size(); ++i) {
        auto& c = cfg_.cameras[i];
        by_serial_[c->serialid]=i;
        std::string h = extract_ip_hex_from_rtsp(c->rtsp_url);
        by_hex_[h]=i;
        by_dotted_[hexToIp(h)]=i;
    }
    return true;
}

void CameraManager::start() {
    if (alarm_thr_.joinable()) return;
    for (auto& n : nodes_) n->start();
    alarm_thr_ = std::jthread([this](std::stop_token st){ alarm_loop(st); });
    monitor_thr_ = std::jthread([this](std::stop_token st){ monitor_loop(st); });
}

void CameraManager::stop() {
    if (!alarm_thr_.joinable()) return;
    LOG_INFO("MGR", "Stopping manager threads...");
    alarm_thr_.request_stop();
    monitor_thr_.request_stop();
    LOG_INFO("MGR", "Waiting for threads to exit...");
    alarm_thr_.join();
    monitor_thr_.join();
    LOG_INFO("MGR", "Threads stopped. Stopping nodes...");
    for (auto& n : nodes_) n->stop();
    { std::lock_guard lock(state_mtx_); active_.clear(); }
    LOG_INFO("MGR", "Manager stopped cleanly.");
}

void CameraManager::alarm_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        AlarmEvent evt;
        if (queue_.try_pop(evt, std::chrono::milliseconds(100))) {
            LOG_DEBUG("MGR", "Event popped: Serial=", evt.serial_id, " Status=", evt.status);
            int idx = find_idx(evt.serial_id, evt.address, evt.address_ip);
            if (idx < 0 || idx >= (int)nodes_.size()) {
                LOG_WARN("MGR", "Unknown alarm: ", evt.serial_id);
                continue;
            }
            nodes_[idx]->on_alarm(evt);
            std::lock_guard lock(state_mtx_);
            if (evt.isStart()) {
                double timeout = cfg_.global->max_event_total_duration_s;
                if (idx >= 0 && idx < (int)cfg_.cameras.size()) {
                    if (cfg_.cameras[idx]->max_event_total_duration_s > 0) {
                        timeout = cfg_.cameras[idx]->max_event_total_duration_s;
                    }
                }
                active_[evt.serial_id] = ActiveEvent{
                    .start_time = std::chrono::steady_clock::now(),
                    .node_idx = idx,
                    .timeout_override = timeout
                };
            } else if (evt.isStop()) {
                active_.erase(evt.serial_id);
            }
        }
    }
}

void CameraManager::monitor_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        check_timeouts();
    }
}

void CameraManager::check_timeouts() {
    if (cfg_.global->max_event_total_duration_s <= 0) return;
    std::vector<std::pair<std::string, int>> timed_out;
    {
        std::lock_guard lock(state_mtx_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = active_.begin(); it != active_.end(); ) {
            const auto& active_evt = it->second;
            double elapsed = std::chrono::duration<double>(now - active_evt.start_time).count();
            if (elapsed >= active_evt.timeout_override) {
                LOG_WARN("MGR", "TIMEOUT: ", it->first, " (", (int)elapsed,
                         "s >= ", (int)active_evt.timeout_override, "s). Forcing STOP.");
                timed_out.push_back({it->first, active_evt.node_idx});
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& [serial_id, node_idx] : timed_out) {
        AlarmEvent e;
        e.serial_id = serial_id;
        e.status = "Stop";
        e.type = "Alarm";
        e.event_type = "Timeout";
        nodes_[node_idx]->on_alarm(e);
    }
}

int CameraManager::find_idx(const std::string& s, const std::string& h, const std::string& d) const {
    auto it = by_serial_.find(s);
    if (it!=by_serial_.end()) return (int)it->second;
    it = by_hex_.find(h);
    if (it!=by_hex_.end()) return (int)it->second;
    it = by_dotted_.find(d);
    if (it!=by_dotted_.end()) return (int)it->second;
    return -1;
}

void CameraManager::collect_stats(std::function<void(const CameraStats&)> consumer) const {
    for (auto& n : nodes_) {
        n->collect_stats(consumer);
    }
}
