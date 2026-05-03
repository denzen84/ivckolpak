#include "camera_manager.hpp"
#include "utils.hpp"
#include <iostream>
#include <algorithm>
#include <filesystem>

std::string buildRecordingPath(const CameraConfig& cfg, const AlarmEvent& evt, std::time_t timestamp) {
    std::string format = cfg.filename_format.empty() ? "rec_%t_%Y%m%d_%H%M%S.mp4" : cfg.filename_format;
    
    auto tm = *std::localtime(&timestamp);
    char buf[512];
    
    auto replace = [&](const std::string& key, const std::string& value) {
        size_t pos = 0;
        while ((pos = format.find(key, pos)) != std::string::npos) {
            format.replace(pos, key.length(), value);
            pos += value.length();
        }
    };
    
    std::strftime(buf, sizeof(buf), "%Y", &tm); replace("%Y", buf);
    std::strftime(buf, sizeof(buf), "%m", &tm); replace("%m", buf);
    std::strftime(buf, sizeof(buf), "%d", &tm); replace("%d", buf);
    std::strftime(buf, sizeof(buf), "%H", &tm); replace("%H", buf);
    std::strftime(buf, sizeof(buf), "%M", &tm); replace("%M", buf);
    std::strftime(buf, sizeof(buf), "%S", &tm); replace("%S", buf);
    std::strftime(buf, sizeof(buf), "%T", &tm); replace("%T", buf);
    
    replace("%t", evt.camera_id);
    replace("%$", cfg.id);
    replace("%e", evt.event_type);
    replace("%v", "1");
    
    replace("%{json_addr}", evt.address);
    replace("%{json_chan}", std::to_string(evt.channel));
    replace("%{json_desc}", evt.description);
    replace("%{json_event}", evt.event_type);
    replace("%{json_serialid}", evt.serial_id);
    replace("%{json_starttime}", evt.start_time);
    replace("%{json_status}", evt.status);
    replace("%{json_alarm}", evt.alarm_type);
    
    format = sanitizeFilename(format);
    
    std::string target = cfg.target_dir.empty() ? "/tmp" : cfg.target_dir;
    if (!target.empty() && target.back() != '/') target += '/';
    
    return target + format;
}

void CameraManager::addCamera(const CameraConfig& cfg) {
    if (!cfg.target_dir.empty() && !std::filesystem::exists(cfg.target_dir)) {
        std::error_code ec;
        std::filesystem::create_directories(cfg.target_dir, ec);
        if (ec) {
            std::cerr << "[" << getTimestamp() << "] [Manager] WARNING: Could not create " << cfg.target_dir << "\n";
        }
    }
    
    rtsp_connection_state_[cfg.id] = false;
    
    cameras_[cfg.id] = std::make_unique<CameraNode>(cfg.id, cfg.url, cfg.serial_id, cfg.ip_address, cfg.pre_buffer_iframes);

    cameras_[cfg.id]->setOnRtspStatusChanged(
        [this, id = cfg.id](bool connected) {
            this->onRtspStatusChanged(id, connected);
        }
    );
    
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

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& p : active_sessions_) p.second->requestStop();
        active_sessions_.clear();
    }
    
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
    DEBUG_LOG("CameraManager::onRtspStatusChanged: cam_id=" << cam_id << ", connected=" << connected);
    
    auto cfg_it = camera_configs_.find(cam_id);
    if (cfg_it == camera_configs_.end()) {
        DEBUG_LOG("Config not found for " << cam_id);
        return;
    }
    
    const auto& cfg = cfg_it->second;
    const std::string& script = connected ? cfg.on_rtsp_found : cfg.on_rtsp_lost;
    
    if (script.empty()) {
        DEBUG_LOG("No script configured for RTSP status change on " << cam_id);
        return;
    }
    
    executeRtspScript(cam_id, script, connected);
}


void CameraManager::executeRtspScript(const std::string& cam_id, const std::string& tmpl, bool connected) {
    if (tmpl.empty()) return;
    

    std::string cmd = tmpl;
    auto replace = [&](const std::string& key, const std::string& value) {
        size_t pos = 0;
        while ((pos = cmd.find(key, pos)) != std::string::npos) {
            cmd.replace(pos, key.length(), value);
            pos += value.length();
        }
    };
    
    replace("%t", cam_id);
    replace("%$", cam_id);  
    replace("%{status}", connected ? "connected" : "disconnected");
    replace("%{event}", "rtsp_" + std::string(connected ? "found" : "lost"));
    
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
    DEBUG_LOG("=== CameraManager::handleEvent START ===");
    DEBUG_LOG("Event: camera_id='" << evt.camera_id << "', status='" << evt.status << "'");
    
    auto cfg_it = camera_configs_.find(evt.camera_id);
    if (cfg_it == camera_configs_.end()) {
        DEBUG_LOG("❌ Config NOT FOUND for camera_id='" << evt.camera_id << "'");
        return;
    }
    
    auto node_it = cameras_.find(evt.camera_id);
    if (node_it == cameras_.end()) {
        DEBUG_LOG("❌ Node NOT FOUND for camera_id='" << evt.camera_id << "'");
        return;
    }
    
    auto* node = node_it->second.get();
    const auto& cfg = cfg_it->second;

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto sess_it = active_sessions_.find(evt.camera_id);

    if (evt.status == "Start") {
        DEBUG_LOG("Processing START event for " << evt.camera_id);
        
        if (sess_it != active_sessions_.end()) {
            DEBUG_LOG("⚠️ Already recording, ignoring duplicate Start");
            return;
        }
        
        std::string filename = buildRecordingPath(cfg, evt, std::time(nullptr));
        DEBUG_LOG("Generated filename: " << filename);
        
        auto codec_params = node->getCodecParamsCopy();
        if (!codec_params) {
            DEBUG_LOG("❌ Failed to get codec params");
            return;
        }
        
        auto sess = std::make_shared<RecordingSession>(
            evt.camera_id, codec_params.get(),
            filename, cfg.max_duration, cfg.max_chunks,
            cfg.post_buffer_iframes,
            cfg.on_start, cfg.on_stop, cfg.on_save,
            [this](const std::string& id){ this->onSessionFinished(id); }
        );
        
        auto prebuffer = node->getPreBuffer();
        sess->start(prebuffer);
        active_sessions_[evt.camera_id] = sess;
        node->addSession(sess);
        
        std::cout << "[" << getTimestamp() << "] [Manager] Recording started for " << evt.camera_id << "\n";
        DEBUG_LOG("=== CameraManager::handleEvent END (START) ===");
    } 
    else if (evt.status == "Stop") {
        DEBUG_LOG("Processing STOP event for " << evt.camera_id);
        
        if (sess_it != active_sessions_.end()) {
            sess_it->second->requestStop();
            std::cout << "[" << getTimestamp() << "] [Manager] Stop requested for " << evt.camera_id << "\n";
        } else {
            DEBUG_LOG("⚠️ No active session for " << evt.camera_id);
        }
        DEBUG_LOG("=== CameraManager::handleEvent END (STOP) ===");
    }
}