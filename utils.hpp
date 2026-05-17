#pragma once
#include "config.h"
#include "script_executor.hpp"
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include "logger.hpp"

inline std::string timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::ostringstream o;
    o << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S")
      << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return o.str();
}

inline std::string hexToIp(const std::string& hex) {
    if (hex.length() < 10) return "0.0.0.0";
    try {
        uint32_t v = std::stoul(hex.substr(2), nullptr, 16);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      v & 0xFF, (v >> 8) & 0xFF,
                      (v >> 16) & 0xFF, (v >> 24) & 0xFF);
        return buf;
    } catch (...) {
        return "0.0.0.0";
    }
}

inline void execute_script_async(const std::string& cmd, const CameraConfig& cfg, const AlarmEvent* evt=nullptr, const std::string& fp="") {
    if (cmd.empty()) return;
    std::string r = cmd;
    auto rep = [&](const std::string& t, const std::string& v) {
        size_t p = 0;
        while ((p = r.find(t, p)) != std::string::npos) {
            r.replace(p, t.length(), v);
            p += v.length();
        }
    };
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