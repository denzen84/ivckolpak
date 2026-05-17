#pragma once
#include "camera_node.hpp"
#include "alarm_queue.hpp"
#include "stats.hpp"
#include "config.h"
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <thread>
#include <stop_token>
#include <functional>

class CameraManager {
public:
    explicit CameraManager(const ParsedConfig& cfg, AlarmQueue& queue);
    ~CameraManager();
    bool initialize();
    void start();
    void stop();
    void check_timeouts();
    void collect_stats(std::function<void(const CameraStats&)> consumer) const;
private:
    struct ActiveEvent {
        std::chrono::steady_clock::time_point start_time;
        int node_idx;
        double timeout_override;
    };
    void alarm_loop(std::stop_token st);
    void monitor_loop(std::stop_token st);
    int find_idx(const std::string& s, const std::string& h, const std::string& d) const;
    const ParsedConfig& cfg_;
    AlarmQueue& queue_;
    std::vector<std::unique_ptr<CameraNode>> nodes_;
    std::jthread alarm_thr_, monitor_thr_;
    std::unordered_map<std::string, size_t> by_serial_, by_hex_, by_dotted_;
    mutable std::mutex state_mtx_;
    std::unordered_map<std::string, ActiveEvent> active_;
};