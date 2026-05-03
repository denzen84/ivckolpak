#include <iostream>
#include <thread>
#include <csignal>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include "camera_manager.hpp"
#include "alarm_server.hpp"
#include "config_parser.hpp"
#include "utils.hpp"

bool g_debug_mode = true;
std::atomic<bool> g_running{true};

void signalHandler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string cfg_path = "ivckolpak.ini";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-debug") == 0 || std::strcmp(argv[i], "-q") == 0) {
            g_debug_mode = false;
        } else if (argv[i][0] != '-') {
            cfg_path = argv[i];
        }
    }

    DEBUG_LOG("=== Application started ===");
    DEBUG_LOG("Config path: " << cfg_path);

    ConfigParser cfg;
    if (!cfg.load(cfg_path)) {
        std::cerr << "[" << getTimestamp() << "] [Main] Failed to load config: " << cfg_path << "\n";
        return 1;
    }
    DEBUG_LOG("Config loaded successfully");

    avformat_network_init();
    DEBUG_LOG("FFmpeg network initialized");

    std::string global_target_dir = cfg.getString("alarmserver", "target_dir", "/tmp");
    std::string global_filename_format = cfg.getString("alarmserver", "filename_format", "rec_%t_%Y%m%d_%H%M%S.mp4");

    if (!std::filesystem::exists(global_target_dir)) {
        std::error_code ec;
        std::filesystem::create_directories(global_target_dir, ec);
        if (ec) {
            std::cerr << "[" << getTimestamp() << "] [Main] WARNING: Could not create " << global_target_dir << "\n";
        }
    }

    CameraManager manager;
    AlarmServer server(cfg.getInt("alarmserver", "port", 15002));

    for (size_t i = 0; i < cfg.countSections("camera"); ++i) {
        CameraConfig cc;
        cc.id = cfg.getString("camera", "serialid", "cam" + std::to_string(i), i);
        cc.url = cfg.getString("camera", "rtsp", "", i);
        cc.serial_id = cc.id;
        cc.ip_address = cfg.getIp("camera", "ip", "", i);
        
        cc.pre_buffer_iframes = cfg.getInt("camera", "pre_buffer_iframes", 3, i);
        cc.post_buffer_iframes = cfg.getInt("camera", "post_buffer_iframes", 0, i);
        
        cc.max_duration = cfg.getInt("camera", "max_event_duration", 30, i);
        cc.max_chunks = cfg.getInt("camera", "max_event_chunks", 1, i);
        
        cc.on_start = cfg.getString("camera", "on_event_start", "", i);
        cc.on_stop = cfg.getString("camera", "on_event_stop", "", i);
        cc.on_save = cfg.getString("camera", "on_video_save", "", i);
        

        cc.on_rtsp_lost = cfg.getString("camera", "on_rtsp_lost", "", i);
        cc.on_rtsp_found = cfg.getString("camera", "on_rtsp_found", "", i);
        
        cc.target_dir = cfg.getString("camera", "target_dir", "", i);
        if (cc.target_dir.empty()) {
            cc.target_dir = global_target_dir;
        }
        cc.filename_format = cfg.getString("camera", "filename_format", "", i);
        if (cc.filename_format.empty()) {
            cc.filename_format = global_filename_format;
        }
        
        DEBUG_LOG("Adding camera: id=" << cc.id << " url=" << cc.url);
        if (!cc.url.empty()) {
            manager.addCamera(cc);
            DEBUG_LOG("Camera added: " << cc.id);
        }
    }

    manager.start(server);
    server.start();

    std::cout << "[" << getTimestamp() << "] [Main] System running. Press Ctrl+C to stop.\n";
    
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "[" << getTimestamp() << "] [Main] Shutting down...\n";
    server.stop();
    manager.stop();
    
    avformat_network_deinit();
    DEBUG_LOG("=== Application exited ===");
    return 0;
}

// ============================================================================
// Definition of static members for ScriptRunner
// ============================================================================
std::mutex ScriptRunner::mutex_;
std::vector<std::thread> ScriptRunner::threads_;