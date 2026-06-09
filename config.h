#pragma once
#include <string>
#include <vector>
#include <memory>
#include "alarm_event.hpp"

struct GlobalConfig {
    int pre_buffer_iframes=3, post_buffer_iframes=2, max_chunk_kbytes=51200, max_event_chunks=5, reconnect_delay_ms=3000, reconnect_max_delay_ms=30000, alarm_server_port=15002;
    double max_chunk_duration_time_s=30.0, max_event_total_duration_s=150.0;
    int stats_every_sec=2;
    std::string target_dir="/mnt/recordings", filename_format="rec_%{cam_id}_%Y%m%d_%T_v%v_%e.mp4", on_event_start, on_event_stop, on_video_save, on_rtsp_lost, on_rtsp_found;
    bool disable_logs = false;
};

struct CameraConfig {
    std::string name, description, serialid, rtsp_url, target_dir, filename_format, on_event_start, on_event_stop, on_video_save, on_rtsp_lost, on_rtsp_found;
    int pre_buffer_iframes=0, post_buffer_iframes=0, max_chunk_kbytes=0, max_event_chunks=0, reconnect_delay_ms=0, reconnect_max_delay_ms=0;
    double max_chunk_duration_time_s=0.0, max_event_total_duration_s=0.0;
    bool rtsp_over_udp=false;
};

struct ParsedConfig {
    std::shared_ptr<const GlobalConfig> global;
    std::vector<std::shared_ptr<const CameraConfig>> cameras;
};

ParsedConfig load_config(const std::string& path);
std::string extract_ip_hex_from_rtsp(const std::string& url);
std::string format_filename(const std::string& fmt, const CameraConfig& cfg, int chunk_idx, const AlarmEvent* evt=nullptr);
