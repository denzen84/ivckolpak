#pragma once
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include "camera_node.hpp"
#include "recording_session.hpp"
#include "alarm_server.hpp"
#include "event.hpp"

struct CameraConfig {
    std::string id;
    std::string url;
    std::string serial_id;
    std::string ip_address;
    
    int pre_buffer_iframes = 3;
    int post_buffer_iframes = 0;
    
    int max_duration = 30;
    int max_chunks = 1;
    
    // Scripts for recording events
    std::string on_start;
    std::string on_stop;
    std::string on_save;
    
    // 🔧 New scripts for RTSP connection status
    std::string on_rtsp_lost;
    std::string on_rtsp_found;
    
    std::string target_dir;
    std::string filename_format;
};

class CameraManager {
public:
    void addCamera(const CameraConfig& cfg);
    void start(AlarmServer& server);
    void stop();
    void onSessionFinished(const std::string& cam_id);
    void onRtspStatusChanged(const std::string& cam_id, bool connected);

private:
    void eventLoop();
    void handleEvent(const AlarmEvent& evt);
    void executeRtspScript(const std::string& cam_id, const std::string& script, bool connected);
    
    std::map<std::string, std::unique_ptr<CameraNode>> cameras_;
    std::map<std::string, CameraConfig> camera_configs_;
    std::map<std::string, std::shared_ptr<RecordingSession>> active_sessions_;
    std::map<std::string, bool> rtsp_connection_state_;
    std::mutex sessions_mutex_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    AlarmServer* active_server_ = nullptr;
};