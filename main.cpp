// main.cpp
#include "camera_manager.hpp"
#include "alarm_server.hpp"
#include "alarm_queue.hpp"
#include "config.h"
#include "logger.hpp"
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

std::atomic<bool> g_shutdown{false};

void signal_handler(int s) {
    LOG_INFO("MAIN", "Signal ", s, ". Shutdown...");
    g_shutdown.store(true, std::memory_order_release);
}

void print_help() {
    std::cout << 
        "-----------------------------------------------------------------------------\n"
        "| ivckolpak :: enhanced videorecorder with AlarmServer support        v1.0.0|\n"
        "-----------------------------------------------------------------------------\n"
        "(c) 2026 Flangeneer, Saint-Petersburg, Russia\n\n"
        "A lightweight, high-performance RTSP camera recording system designed as a\n"
        "direct alternative to motion. It utilizes full hardware passthrough mode to\n"
        "record directly to disk without decoding, relying on native camera alarms.\n\n"
        "Command-line options:\n"
        "  -c, --config <file>   Specify path to configuration file (default: ivckolpak.ini)\n"
        "  -s, --silent          Disable console output (equivalent to disable_logs=yes)\n"
        "  --help-full           Show detailed INI format, options, and macros reference\n"
        "  -h, --help            Show this help message and exit\n"
        << std::endl;
}

void print_full_help() {
    std::cout << 
        "================================================================================\n"
        " ivckolpak :: Full Configuration & Macros Reference\n"
        "================================================================================\n\n"
        
        "1. INI FILE FORMAT & INHERITANCE\n"
        "--------------------------------\n"
        "The configuration uses standard INI format with three sections: [global], [camera],\n"
        "and [alarm_server]. The [global] section defines DEFAULT values for ALL cameras.\n"
        "Any parameter specified in a [camera] section OVERRIDES the corresponding global\n"
        "value. If a camera parameter is left empty, 0, or 0.0, the system automatically\n"
        "falls back to the [global] setting. This enables centralized defaults with\n"
        "per-camera customization without duplication.\n\n"

        "2. CONFIGURATION SECTIONS & OPTIONS\n"
        "-----------------------------------\n"
        "[global]\n"
        "  pre_buffer_iframes=3          : I-frames to keep in memory before alarm triggers.\n"
        "  post_buffer_iframes=2         : I-frames to record after alarm stops.\n"
        "  max_chunk_duration_time_s=30  : Max duration (seconds) per video chunk.\n"
        "  max_event_total_duration_s=150: Hard timeout for recording (forces STOP).\n"
        "  max_chunk_kbytes=51200        : Max size (KB) per video chunk.\n"
        "  max_event_chunks=5            : Max number of chunks per alarm event.\n"
        "  reconnect_delay_ms=3000       : Initial RTSP reconnect delay (ms).\n"
        "  reconnect_max_delay_ms=30000  : Max RTSP reconnect backoff delay (ms).\n"
        "  target_dir=\"/mnt/recordings\"  : Directory to save recorded video files.\n"
        "  filename_format=...           : Output filename template (see macros below).\n"
        "  on_event_start=\"\"             : Shell command executed on recording start.\n"
        "  on_event_stop=\"\"              : Shell command executed on recording stop.\n"
        "  on_video_save=\"\"              : Shell command executed when chunk is finalized.\n"
        "  on_rtsp_lost=\"\"               : Shell command executed when RTSP drops.\n"
        "  on_rtsp_found=\"\"              : Shell command executed when RTSP connects.\n"
        "  disable_logs=false            : Set to yes/true/1/on to disable console output.\n\n"

        "[camera] (Required fields: name, serialid, rtsp)\n"
        "  name=\"FrontDoor\"              : Human-readable camera name.\n"
        "  serialid=\"CAM001\"             : Unique ID (must match AlarmServer SerialID).\n"
        "  rtsp=\"rtsp://user:pass@ip:554/\" : Full RTSP stream URL.\n"
        "  rtsp_over_udp=false           : Force UDP transport for RTSP (TCP is default).\n"
        "  [Plus ALL [global] options listed above, which override defaults per camera.]\n\n"

        "[alarm_server]\n"
        "  listen_port=15002             : TCP port for receiving JSON alarm events.\n\n"

        "3. AVAILABLE MACROS\n"
        "-------------------\n"
        "Macros can be used in 'filename_format' and external script commands.\n\n"
        "[Time & Date] (Applies to: Filename, Script)\n"
        "  %Y, %m, %d, %H, %M, %S        : Current year, month, day, hour, minute, second.\n"
        "  %T                            : Full time in HH:MM:SS format.\n\n"
        
        "[Event & Chunk] (Applies to: Filename)\n"
        "  %v                            : Chunk version/index within an event.\n"
        "  %e                            : Sequential Event ID.\n\n"
        
        "[Camera Info] (Applies to: Filename, Script)\n"
        "  %{cam_id}                     : Camera serial ID.\n"
        "  %{cam_name}                   : Human-readable camera name.\n"
        "  %{cam_ip}                     : IP extracted from RTSP URL (hex format).\n\n"
        
        "[Alarm JSON Payload] (Applies to: Filename, Script)\n"
        "  %{json_addr}                  : Address field from incoming alarm JSON.\n"
        "  %{json_chan}                  : Channel field.\n"
        "  %{json_desc}                  : Description field.\n"
        "  %{json_event}                 : Event type.\n"
        "  %{json_serialid}              : Serial ID from JSON.\n"
        "  %{json_starttime}             : Start time from JSON.\n"
        "  %{json_status}                : Status field (start/stop/1/0).\n"
        "  %{json_alarm}                 : Alarm category/type.\n\n"
        
        "[File Path] (Applies to: Script ONLY)\n"
        "  %f                            : Absolute path to the finalized video file.\n"
        
        "================================================================================\n"
        << std::endl;
}

int main(int argc, char* argv[]) {
    std::string config_path = "ivckolpak.ini";
    bool cli_silent = false;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "--help-full") {
            print_full_help();
            return 0;
        } else if (arg == "-s" || arg == "--silent") {
            cli_silent = true;
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg.find("--config=") == 0) {
            config_path = arg.substr(9);
        } else if (arg.find("-c=") == 0) {
            config_path = arg.substr(3);
        } else {
            std::cerr << "Unknown argument: " << arg << "\nUse --help for usage information." << std::endl;
            return 1;
        }
    }

    // Apply CLI silent flag early
    if (cli_silent) {
        Logger::set_enabled(false);
    }

    avformat_network_init();
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    ParsedConfig cfg;
    try {
        cfg = load_config(config_path);
        if (cfg.cameras.empty()) {
            LOG_ERROR("MAIN", "No cameras configured.");
            return 1;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("MAIN", "Config error: ", e.what());
        return 1;
    }

    // Apply config-based silent flag (CLI takes precedence if already set)
    if (cfg.global->disable_logs) {
        Logger::set_enabled(false);
    }

    AlarmQueue queue;
    AlarmServer server(cfg.global->alarm_server_port);
    CameraManager mgr(cfg, queue);

    server.setCallback([&queue](AlarmEvent e) { queue.push(std::move(e)); });

    if (!mgr.initialize()) {
        LOG_ERROR("MAIN", "Camera manager initialization failed.");
        return 1;
    }

    mgr.start();
    if (!server.start()) {
        LOG_ERROR("MAIN", "Alarm server start failed.");
        return 1;
    }

    LOG_INFO("MAIN", "System running on port ", cfg.global->alarm_server_port);

    // Statistics thread (runs every 2 seconds)
    std::jthread stats([&mgr](std::stop_token st) {
        while (!st.stop_requested()) {
            for (int i = 0; i < 20 && !st.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (st.stop_requested()) break;
            try {
                mgr.collect_stats([](const CameraStats& s) {
                    LOG_INFO("STATS",
                             std::left, std::setw(10), s.name, " | ",
                             std::setw(5), s.state_str(), " | ",
                             "Buf:", s.buffer_iframes, "I/", s.buffer_kb, "KB | ",
                             "Pushed:", s.total_pushed_frames, " | ",
                             "Written:", s.total_written_kb, "KB | ",
                             "FPS:", std::fixed, std::setprecision(1), s.detected_fps, " | ",
                             "RTSP:", (s.rtsp_connected ? "OK" : "OFF"));
                });
            } catch (const std::exception& e) {
                LOG_ERROR("STATS", "Failed to get stats: ", e.what());
            }
        }
    });

    // Main event loop
    while (!g_shutdown.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    LOG_INFO("MAIN", "=== Graceful Shutdown ===");
    stats.request_stop();
    stats.join();
    mgr.stop();
    server.stop();
    queue.shutdown();
    avformat_network_deinit();
    LOG_INFO("MAIN", "Terminated.");
    return 0;
}