#pragma once
#include <string>
#include <thread>
#include <stop_token>
#include <atomic>
#include <mutex>
#include <functional>
#include <memory>
#include <chrono>
#include "logger.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

struct CameraConfig;
class RTSPStream {
public:
    struct Config {
        std::string url;
        int timeout_ms = 2000;
        bool tcp_only = true;
        int reconnect_delay_ms = 3000;
        int reconnect_max_delay_ms = 30000;
        int max_reconnect_attempts = 0;
        int read_timeout_ms = 10000;
        std::string on_rtsp_lost;
        std::string on_rtsp_found;
        std::shared_ptr<const CameraConfig> camera_config;
        std::function<void()> internal_lost_callback;    
        std::function<void()> internal_found_callback;   
    };
    struct StreamInfo {
        AVRational time_base;
        AVRational frame_rate;
        bool valid = false;
    };
    using PacketCallback = std::function<void(AVPacket*)>;
    explicit RTSPStream(Config cfg);
    ~RTSPStream();
    RTSPStream(const RTSPStream&) = delete;
    RTSPStream& operator=(const RTSPStream&) = delete;
    void start(PacketCallback cb);
    void stop();
    [[nodiscard]] bool is_alive() const noexcept;
    [[nodiscard]] std::unique_ptr<AVCodecParameters, std::function<void(AVCodecParameters*)>> get_codec_params() const;
    [[nodiscard]] StreamInfo get_stream_info() const;
    [[nodiscard]] uint64_t get_total_bytes_read() const noexcept {  
        return total_bytes_read_.load(std::memory_order_relaxed);
    }
private:
    void run(std::stop_token st);
    void watchdog_loop(std::stop_token st);
    bool open_input();
    void close_input();
    static int interrupt_cb(void* ctx) {
        return static_cast<std::atomic<bool>*>(ctx)->load(std::memory_order_relaxed) ? 1 : 0;
    }
    Config cfg_;
    std::jthread worker_;
    std::jthread watchdog_;
    mutable std::mutex io_mtx_;
    std::atomic<bool> alive_{false};
    AVFormatContext* fmt_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    std::unique_ptr<AVCodecParameters, std::function<void(AVCodecParameters*)>> codec_params_;
    AVRational stream_time_base_{1, 90000};
    AVRational stream_frame_rate_{0, 1};
    bool stream_info_valid_ = false;
    PacketCallback packet_cb_;
    std::atomic<bool> interrupt_flag_{false};
    std::atomic<uint64_t> last_packet_time_ms_{0};
    std::atomic<bool> watchdog_enabled_{false};
    std::atomic<uint64_t> total_bytes_read_{0}; 
};
