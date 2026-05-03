#include "camera_manager.hpp"
#include "utils.hpp"
#include <iostream>
#include <algorithm>
#include <filesystem>

// 🔧 Unified path builder using applyMacros
std::string buildRecordingPath(const CameraConfig& cfg, const AlarmEvent& evt, std::time_t timestamp) {
    MacroContext ctx;
    ctx.camera_id = evt.camera_id;
    ctx.camera_name = cfg.id;
    ctx.event_type = evt.event_type;
    ctx.status = evt.status;
    ctx.address = evt.address;
    ctx.channel = evt.channel;
    ctx.description = evt.description;
    ctx.serial_id = evt.serial_id;
    ctx.start_time = evt.start_time;
    ctx.alarm_type = evt.alarm_type;
    ctx.timestamp = timestamp;

    std::string fmt = cfg.filename_format.empty() ? "rec_%t_%Y%m%d_%H%M%S.mp4" : cfg.filename_format;
    std::string filename = sanitizeFilename(applyMacros(fmt, ctx));

    std::string target = cfg.target_dir.empty() ? "/tmp" : cfg.target_dir;
    if (!target.empty() && target.back() != '/') target += '/';
    return target + filename;
}

void CameraManager::addCamera(const CameraConfig& cfg) {
    if (!cfg.target_dir.empty() && !std::filesystem::exists(cfg.target_dir)) {
        std::error_code ec;
        std::filesystem::create_directories(cfg.target_dir, ec);
    }
    
    rtsp_connection_state_[cfg.id] = false;
    cameras_[cfg.id] = std::make_unique<CameraNode>(cfg.id, cfg.url, cfg.serial_id, cfg.ip_address, cfg.pre_buffer_iframes);
    
    cameras_[cfg.id]->setOnRtspStatusChanged([this, id = cfg.id](bool connected) {
        this->onRtspStatusChanged(id, connected);
    });
    
    camera_configs_[cfg.id] = cfg;
    std::cout << "[" << getTimestamp() << "] [Manager] Camera " << cfg.id << " registered.\n";
}

void CameraManager::start(AlarmServer& server) {
    for (auto& p : cameras_) p.second->start();
    running_.store(true);
    active_server_ = &server;
    worker_thread_ = std::thread(&CameraManager::eventLoop, this);
    std::cout << "[" << getTimestamp() << "] [Manager] Event loop started.\n";
}

void CameraManager::stop() {
    running_.store(false);
    if (active_server_) active_server_->wakeUp();
    if (worker_thread_.joinable()) worker_thread_.join();

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& p : active_sessions_) p.second->requestStop();
    active_sessions_.clear();
    
    for (auto& p : cameras_) p.second->stop();
    ScriptRunner::joinAll();
    std::cout << "[" << getTimestamp() << "] [Manager] Stopped.\n";
}

void CameraManager::onSessionFinished(const std::string& cam_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    active_sessions_.erase(cam_id);
    std::cout << "[" << getTimestamp() << "] [Manager] Session removed for " << cam_id << ".\n";
}

void CameraManager::onRtspStatusChanged(const std::string& cam_id, bool connected) {
    auto cfg_it = camera_configs_.find(cam_id);
    if (cfg_it == camera_configs_.end()) return;
    const std::string& script = connected ? cfg_it->second.on_rtsp_found : cfg_it->second.on_rtsp_lost;
    if (script.empty()) return;
    executeRtspScript(cam_id, script, connected);
}

void CameraManager::executeRtspScript(const std::string& cam_id, const std::string& tmpl, bool connected) {
    if (tmpl.empty()) return;
    MacroContext ctx;
    ctx.camera_id = cam_id;
    ctx.camera_name = cam_id;
    ctx.timestamp = std::time(nullptr);
    ctx.rtsp_status = connected ? "connected" : "disconnected";
    ctx.rtsp_event = connected ? "rtsp_found" : "rtsp_lost";
    // Event/JSON fields remain empty -> ignored for RTSP scripts

    std::string cmd = applyMacros(tmpl, ctx);
    std::cout << "[" << getTimestamp() << "] [Manager] Executing RTSP script: " << cmd << "\n";
    ScriptRunner::run(cmd);
}

void CameraManager::eventLoop() {
    AlarmEvent evt;
    while (running_.load()) {
        if (!active_server_->popEvent(evt)) {
            if (!running_.load()) break;
            continue;
        }
        handleEvent(evt);
    }
}

void CameraManager::handleEvent(const AlarmEvent& evt) {
    DEBUG_LOG("handleEvent: " << evt.camera_id << " " << evt.status);
    auto cfg_it = camera_configs_.find(evt.camera_id);
    auto node_it = cameras_.find(evt.camera_id);
    if (cfg_it == camera_configs_.end() || node_it == cameras_.end()) return;
    
    auto* node = node_it->second.get();
    const auto& cfg = cfg_it->second;

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto sess_it = active_sessions_.find(evt.camera_id);

    if (evt.status == "Start") {
        if (sess_it != active_sessions_.end()) return; // Already recording
        
        std::string filename = buildRecordingPath(cfg, evt, std::time(nullptr));
        auto codec_params = node->getCodecParamsCopy();
        if (!codec_params) return;
        
        // 🔧 Pass original event & config name to session for accurate macro expansion
        auto sess = std::make_shared<RecordingSession>(
            evt.camera_id, cfg.id, evt,
            codec_params.get(), filename, cfg.max_duration, cfg.max_chunks,
            cfg.post_buffer_iframes, cfg.on_start, cfg.on_stop, cfg.on_save,
            [this](const std::string& id){ this->onSessionFinished(id); }
        );
        
        sess->start(node->getPreBuffer());
        active_sessions_[evt.camera_id] = sess;
        node->addSession(sess);
        std::cout << "[" << getTimestamp() << "] [Manager] Recording started for " << evt.camera_id << "\n";
    } 
	else if (evt.status == "Stop") {
		DEBUG_LOG("Processing STOP event for " << evt.camera_id);
		
		if (sess_it != active_sessions_.end()) {
			// 🔧 Обновляем статус в сессии для корректных макросов в on_stop/on_save
			sess_it->second->updateEventStatus(evt.status);
			sess_it->second->requestStop();
			std::cout << "[" << getTimestamp() << "] [Manager] Stop requested for " << evt.camera_id << "\n";
		} else {
			DEBUG_LOG("⚠️ No active session for " << evt.camera_id);
		}
		DEBUG_LOG("=== CameraManager::handleEvent END (STOP) ===");
	}
}