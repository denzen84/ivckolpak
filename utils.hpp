#pragma once
#include "config.h"
#include "script_executor.hpp"
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include "logger.hpp"

inline std::string timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm;
    localtime_r(&t, &tm);
    std::ostringstream o;
    o << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
      << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return o.str();
}

inline std::string hexToIp(const std::string& hex) {
    if (hex.length() < 10) return "0.0.0.0";
    try {
        uint32_t v = std::stoul(hex.substr(2), nullptr, 16);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      (v >> 24) & 0xFF,
                      (v >> 16) & 0xFF,
                      (v >> 8) & 0xFF,
                      v & 0xFF);
        return buf;
    } catch (...) {
        return "0.0.0.0";
    }
}

inline void execute_script_async(const std::string& cmd, const CameraConfig& cfg, const AlarmEvent* evt=nullptr, const std::string& fp="") {
    if (cmd.empty()) return;
    std::string r = cmd;
    
    std::time_t now = std::time(nullptr);
    std::tm tm;
    localtime_r(&now, &tm);
    char time_buf[32];
    
    auto rep = [&](const std::string& t, const std::string& v) {
        size_t p = 0;
        while ((p = r.find(t, p)) != std::string::npos) {
            r.replace(p, t.length(), v);
            p += v.length();
        }
    };
    
    std::strftime(time_buf, sizeof(time_buf), "%Y", &tm);
    rep("%Y", time_buf);
    
    std::strftime(time_buf, sizeof(time_buf), "%m", &tm);
    rep("%m", time_buf);
    
    std::strftime(time_buf, sizeof(time_buf), "%d", &tm);
    rep("%d", time_buf);
    
    std::strftime(time_buf, sizeof(time_buf), "%H", &tm);
    rep("%H", time_buf);
    
    std::strftime(time_buf, sizeof(time_buf), "%M", &tm);
    rep("%M", time_buf);
    
    std::strftime(time_buf, sizeof(time_buf), "%S", &tm);
    rep("%S", time_buf);
    
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);
    rep("%T", time_buf);
    
    rep("%{cam_name}", cfg.name);
    rep("%{cam_id}", cfg.serialid);
    rep("%{cam_ip}", extract_ip_hex_from_rtsp(cfg.rtsp_url));
    if (evt) {
        rep("%{json_addr}", evt->address);
        rep("%{json_chan}", std::to_string(evt->channel));
        rep("%{json_desc}", evt->description);
        rep("%{json_event}", evt->event_type);
        rep("%{json_serialid}", evt->serial_id);
        rep("%{json_starttime}", evt->start_time);
        rep("%{json_status}", evt->status);
        rep("%{json_alarm}", evt->alarm_type);
    }
    rep("%f", fp);
    LOG_DEBUG("SCRIPT", "Submitting to pool: ", r);
    ScriptExecutor::instance().submit(std::move(r));
}
