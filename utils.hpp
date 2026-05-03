#pragma once
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <memory>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
}

extern bool g_debug_mode;
#define DEBUG_LOG(msg) do { if (g_debug_mode) { std::cerr << "[DEBUG] " << msg << std::endl; } } while(0)

static inline std::string getTimestamp() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

static inline std::string sanitizeFilename(const std::string& name) {
    std::string result = name;
    const std::string forbidden = "/\\:*?\"<>|";
    bool changed = false;
    for (char& c : result) {
        if (forbidden.find(c) != std::string::npos || c < 32) {
            c = '_';
            changed = true;
        }
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
        changed = true;
    }
    if (changed) {
        std::cerr << "[" << getTimestamp() << "] [Utils] Sanitized filename: " << result << "\n";
    }
    return result;
}

struct SafePacket {
    std::shared_ptr<AVPacket> ptr;
    SafePacket() = default;
    explicit SafePacket(AVPacket* raw) { if (raw) ptr.reset(raw, [](AVPacket* p) { av_packet_free(&p); }); }
    SafePacket(const SafePacket& other) { if (other.ptr) ptr.reset(av_packet_clone(other.ptr.get()), [](AVPacket* p) { av_packet_free(&p); }); }
    SafePacket(SafePacket&&) noexcept = default;
    SafePacket& operator=(const SafePacket&) = default;
    SafePacket& operator=(SafePacket&&) noexcept = default;
    AVPacket* get() const { return ptr.get(); }
    bool empty() const { return !ptr; }
};

class ScriptRunner {
public:
    static void run(const std::string& cmd) {
        DEBUG_LOG("ScriptRunner::run: " << cmd);
        static std::atomic<uint64_t> call_count{0};
        uint64_t cnt = ++call_count;
        std::lock_guard<std::mutex> lock(mutex_);
        threads_.emplace_back([cmd, cnt]() {
            DEBUG_LOG("Script thread #" << cnt << " started");
            int ret = std::system(cmd.c_str());
            DEBUG_LOG("Script thread #" << cnt << " exited with code " << ret);
            if (ret != 0) std::cerr << "[" << getTimestamp() << "] [ScriptRunner] Failed (code " << ret << "): " << cmd << "\n";
        });
    }
    static void joinAll() {
        DEBUG_LOG("ScriptRunner::joinAll called");
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_) if (t.joinable()) t.join();
        threads_.clear();
        DEBUG_LOG("ScriptRunner::joinAll done");
    }
private:
    static std::mutex mutex_;
    static std::vector<std::thread> threads_;
};

// 🔧 UNIFIED MACRO ENGINE
struct MacroContext {
    std::string camera_id;
    std::string camera_name;
    std::string event_type;
    std::string status;
    std::string address;
    int channel = 0;
    std::string description;
    std::string serial_id;
    std::string start_time;
    std::string alarm_type;
    std::string filepath;
    std::time_t timestamp = 0;
    std::string rtsp_status; // "connected" / "disconnected"
    std::string rtsp_event;  // "rtsp_found" / "rtsp_lost"
};

inline std::string applyMacros(const std::string& tmpl, const MacroContext& ctx) {
    if (tmpl.empty()) return "";
    std::string result = tmpl;

    auto replace = [&](const std::string& key, const std::string& value) {
        if (value.empty()) return; // Skip empty values to preserve irrelevant macros (e.g. %{event} in event scripts)
        size_t pos = 0;
        while ((pos = result.find(key, pos)) != std::string::npos) {
            result.replace(pos, key.length(), value);
            pos += value.length();
        }
    };

    if (ctx.timestamp > 0) {
        auto tm = *std::localtime(&ctx.timestamp);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y", &tm); replace("%Y", buf);
        std::strftime(buf, sizeof(buf), "%m", &tm); replace("%m", buf);
        std::strftime(buf, sizeof(buf), "%d", &tm); replace("%d", buf);
        std::strftime(buf, sizeof(buf), "%H", &tm); replace("%H", buf);
        std::strftime(buf, sizeof(buf), "%M", &tm); replace("%M", buf);
        std::strftime(buf, sizeof(buf), "%S", &tm); replace("%S", buf);
        std::strftime(buf, sizeof(buf), "%T", &tm); replace("%T", buf);
    }

    replace("%t", ctx.camera_id);
    replace("%$", ctx.camera_name);
    replace("%e", ctx.event_type);
    replace("%f", ctx.filepath);
    replace("%v", "1");

    replace("%{json_addr}", ctx.address);
    replace("%{json_chan}", std::to_string(ctx.channel));
    replace("%{json_desc}", ctx.description);
    replace("%{json_event}", ctx.event_type);
    replace("%{json_serialid}", ctx.serial_id);
    replace("%{json_starttime}", ctx.start_time);
    replace("%{json_status}", ctx.status);
    replace("%{json_alarm}", ctx.alarm_type);

    replace("%{status}", ctx.rtsp_status);
    replace("%{event}", ctx.rtsp_event);

    return result;
}