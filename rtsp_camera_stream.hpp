#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>
#include <vector>
#include <shared_mutex>
#include "utils.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class RtspCameraStream {
public:
    using OnPacketCallback = std::function<void(const SafePacket& pkt)>;
    using OnStatusCallback = std::function<void(bool connected)>;
    
    explicit RtspCameraStream(const std::string& url, int buffer_frames = 3);
    ~RtspCameraStream();
    
    bool start(); 
    void stop(); 
    void join();
    
    void setOnPacket(OnPacketCallback cb) { on_packet_ = std::move(cb); }
    void setOnStatusChanged(OnStatusCallback cb) { on_status_changed_ = std::move(cb); }
    
    std::vector<SafePacket> getBufferSafe() const;
    
    std::unique_ptr<AVCodecParameters> getCodecParamsCopy() const;
    
    bool isConnected() const { return connected_.load(); }
    
    static constexpr size_t MAX_BUFFER_PACKETS = 500;

private:
    void run(); 
    bool open(); 
    void trimBuffer();
    void notifyStatusChanged(bool connected);
    static int interruptCallback(void* ctx);

    struct Pkt { SafePacket data; bool is_key; double dur; };
    
    std::string url_; 
    int buffer_frames_;
    AVFormatContext* fmt_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    AVRational time_base_{};
    
    std::deque<Pkt> buffer_;
    double buffer_dur_ = 0.0;
    
    mutable std::mutex buf_mtx_;
    mutable std::shared_mutex ctx_mutex_;
    
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> connected_{false};
    
    OnPacketCallback on_packet_;
    OnStatusCallback on_status_changed_;
};