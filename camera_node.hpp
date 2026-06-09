#pragma once
#include "rtsp_stream.hpp"
#include "recording_session.hpp"
#include "stats.hpp"
#include "config.h"
#include "logger.hpp"
#include <memory>
#include <atomic>
#include <functional>

class CameraNode {
public:
    explicit CameraNode(std::shared_ptr<const CameraConfig> cfg, int pre_iframes, int post_iframes);
    ~CameraNode();
    CameraNode(const CameraNode&) = delete;
    CameraNode& operator=(const CameraNode&) = delete;
    bool initialize();
    void start();
    void stop();
    void on_alarm(const AlarmEvent& evt);
    void on_rtsp_connection_lost();      
    void on_rtsp_connection_restored();  
    [[nodiscard]] bool is_alive() const;
    [[nodiscard]] const std::string& name() const;
    [[nodiscard]] const std::string& serial_id() const;
    [[nodiscard]] uint64_t get_total_bytes_read() const; 
    void collect_stats(std::function<void(const CameraStats&)> consumer) const;
private:
    void on_packet(AVPacket* pkt);
    std::shared_ptr<const CameraConfig> cfg_;
    RTSPStream stream_;
    std::unique_ptr<RecordingSession> session_;
    std::atomic<bool> initialized_{false};
    std::atomic<int> event_counter_{0};
};
