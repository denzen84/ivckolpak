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

extern "C" {
#include <libavcodec/avcodec.h>
}

// 🔧 Global debug flag (defined in main.cpp)
extern bool g_debug_mode;

#define DEBUG_LOG(msg) do { if (g_debug_mode) { std::cerr << "[DEBUG] " << msg << std::endl; } } while(0)
#define DEBUG_LOG_PTR(ptr, name) do { if (g_debug_mode) { std::cerr << "[DEBUG] " << name << " = " << (void*)(ptr) << std::endl; } } while(0)
	
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
        
    explicit SafePacket(const AVPacket* raw) {
        if (raw) {
            ptr.reset(av_packet_clone(raw), [](AVPacket* p) { 
                if (p) av_packet_free(&p); 
            });
        }
    }
    
    explicit SafePacket(AVPacket* raw, bool take_ownership) {
        if (raw && take_ownership) {
            ptr.reset(raw, [](AVPacket* p) { 
                if (p) av_packet_free(&p); 
            });
        } else if (raw) {        
            ptr.reset(av_packet_clone(raw), [](AVPacket* p) { 
                if (p) av_packet_free(&p); 
            });
        }
    }
    
    SafePacket(const SafePacket& other) {
        if (other.ptr && other.ptr.get()) {
            ptr.reset(av_packet_clone(other.ptr.get()), [](AVPacket* p) { av_packet_free(&p); });
        }
    }
    
    SafePacket(SafePacket&&) noexcept = default;
    SafePacket& operator=(const SafePacket&) = default;
    SafePacket& operator=(SafePacket&&) noexcept = default;

    AVPacket* get() const { return ptr.get(); }
    bool empty() const { return !ptr; }
};

class ScriptRunner {
public:
    static void run(const std::string& cmd) {
        DEBUG_LOG("ScriptRunner::run called: " << cmd);
                
        static std::atomic<uint64_t> call_count{0};
        uint64_t cnt = ++call_count;
        if (g_debug_mode && cnt % 10 == 0) {
            std::cerr << "[DEBUG] [ScriptRunner] Call #" << cnt << ": " << cmd << "\n";
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        threads_.emplace_back([cmd, cnt]() { 
            DEBUG_LOG("Script thread #" << cnt << " started: " << cmd);
            auto start = std::chrono::steady_clock::now();
            int ret = std::system(cmd.c_str());
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            DEBUG_LOG("Script thread #" << cnt << " exited with code " << ret << " in " << dur << "ms");
            if (ret != 0) {
                std::cerr << "[" << getTimestamp() << "] [ScriptRunner] Command failed (code " << ret << "): " << cmd << "\n";
            }
        });
    }
    
    static void joinAll() {
        DEBUG_LOG("ScriptRunner::joinAll called, threads count: " << threads_.size());
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_) {
            if (t.joinable()) {
                DEBUG_LOG("Joining script thread...");
                t.join();
            }
        }
        threads_.clear();
        DEBUG_LOG("ScriptRunner::joinAll done");
    }
    
private:
    static std::mutex mutex_;
    static std::vector<std::thread> threads_;
};