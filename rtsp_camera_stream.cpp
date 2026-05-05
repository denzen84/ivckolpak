#include "rtsp_camera_stream.hpp"
#include <iostream>
#include <algorithm>

extern "C" {
#include <libavutil/mathematics.h>
}

RtspCameraStream::RtspCameraStream(const std::string& url, int buffer_frames)
    : url_(url), buffer_frames_(std::clamp(buffer_frames, 1, 10)) {
    DEBUG_LOG("RtspCameraStream ctor: url=" << url_ << " buffer_frames=" << buffer_frames_);
}

RtspCameraStream::~RtspCameraStream() { 
    DEBUG_LOG("RtspCameraStream dtor");
    stop(); 
    join(); 
}

bool RtspCameraStream::start() {
    DEBUG_LOG("RtspCameraStream::start() called");
    if (running_.load()) {
        DEBUG_LOG("Already running, returning false");
        return false;
    }
    should_stop_ = false; 
    running_ = true;
    worker_ = std::thread(&RtspCameraStream::run, this);
    DEBUG_LOG("Worker thread created");
    return true;
}

void RtspCameraStream::stop() { 
    DEBUG_LOG("RtspCameraStream::stop() called");
    should_stop_ = true; 
    running_ = false; 
}

void RtspCameraStream::join() { 
    DEBUG_LOG("RtspCameraStream::join() called");
    if (worker_.joinable()) {
        DEBUG_LOG("Joining worker thread...");
        worker_.join();
        DEBUG_LOG("Worker thread joined");
    }
    running_ = false; 
}

int RtspCameraStream::interruptCallback(void* ctx) {
    auto* self = static_cast<RtspCameraStream*>(ctx);
    return self->should_stop_.load() ? 1 : 0;
}

void RtspCameraStream::notifyStatusChanged(bool connected) {
    bool old_state = connected_.exchange(connected);
    if (old_state != connected && on_status_changed_) {
        DEBUG_LOG("RTSP status changed: connected=" << connected);
        if (!connected) {
            std::lock_guard<std::mutex> lock(buf_mtx_);
            buffer_.clear();
            buffer_dur_ = 0;
            DEBUG_LOG("Buffer cleared on RTSP loss");
        }
        on_status_changed_(connected);
    }
}

std::unique_ptr<AVCodecParameters> RtspCameraStream::getCodecParamsCopy() const {
    std::shared_lock<std::shared_mutex> lock(ctx_mutex_);
    if (!fmt_ctx_ || video_stream_idx_ < 0 || 
        video_stream_idx_ >= static_cast<int>(fmt_ctx_->nb_streams)) {
        return nullptr;
    }
    auto params = std::unique_ptr<AVCodecParameters>(avcodec_parameters_alloc());
    if (!params || avcodec_parameters_copy(params.get(), 
            fmt_ctx_->streams[video_stream_idx_]->codecpar) < 0) {
        return nullptr;
    }
    return params;
}

bool RtspCameraStream::open() {
    DEBUG_LOG("RtspCameraStream::open() called for url=" << url_);
    
    AVDictionary* o = nullptr;
    av_dict_set(&o, "rtsp_transport", "tcp", 0);
    av_dict_set(&o, "stimeout", "3000000", 0);

    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        DEBUG_LOG("avformat_alloc_context failed!");
        av_dict_free(&o);
        return false;
    }
    
    fmt_ctx_->interrupt_callback.callback = interruptCallback;
    fmt_ctx_->interrupt_callback.opaque = this;

    DEBUG_LOG("Calling avformat_open_input...");
    int ret = avformat_open_input(&fmt_ctx_, url_.c_str(), nullptr, &o);
    
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DEBUG_LOG("avformat_open_input failed: " << errbuf);
        fmt_ctx_ = nullptr;
        av_dict_free(&o);
        notifyStatusChanged(false);
        return false;
    }
    av_dict_free(&o);
    
    DEBUG_LOG("Calling avformat_find_stream_info...");
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        DEBUG_LOG("avformat_find_stream_info failed");
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
        notifyStatusChanged(false);
        return false;
    }
    
    DEBUG_LOG("Finding best video stream...");
    video_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        DEBUG_LOG("No video stream found");
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
        notifyStatusChanged(false);
        return false;
    }
    
    time_base_ = fmt_ctx_->streams[video_stream_idx_]->time_base;
    notifyStatusChanged(true);
    DEBUG_LOG("open() succeeded, time_base = " << time_base_.num << "/" << time_base_.den);
    return true;
}

void RtspCameraStream::run() {
    DEBUG_LOG("RtspCameraStream::run() thread started");
    
    while (!should_stop_) {
        if (!open()) { 
            DEBUG_LOG("run(): open() failed, sleeping 1s");
            std::this_thread::sleep_for(std::chrono::seconds(1)); 
            continue; 
        }
        DEBUG_LOG("run(): open() succeeded");
        
        if (!fmt_ctx_ || video_stream_idx_ < 0) {
            DEBUG_LOG("run(): invalid state after open(), skipping");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            DEBUG_LOG("av_packet_alloc failed!");
            break;
        }

        DEBUG_LOG("Entering read loop...");
        
        while (!should_stop_ && av_read_frame(fmt_ctx_, pkt) >= 0) {
            if (pkt->stream_index == video_stream_idx_) {
                bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                double dur = (pkt->duration > 0) ? pkt->duration * av_q2d(time_base_) : 0.05;
                
                // 🔧 FIX: Clone ONCE and reuse via shared_ptr (cheap copy)
                SafePacket cloned(av_packet_clone(pkt));
                if (!cloned.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(buf_mtx_);
                        buffer_.push_back({cloned, is_key, dur});
                        buffer_dur_ += dur;
                        if (is_key) trimBuffer();
                    }
                    if (on_packet_) {
                        on_packet_(cloned);  // 🔧 Pass the SAME cloned packet
                    }
                }
            }
            av_packet_unref(pkt);
        }
        
        if (!should_stop_) {
            DEBUG_LOG("Read loop exited with error, notifying disconnect");
            notifyStatusChanged(false);
        }
        
        av_packet_free(&pkt);
        DEBUG_LOG("AVPacket freed");
        
        if (fmt_ctx_) { 
            DEBUG_LOG("Closing input...");
            avformat_close_input(&fmt_ctx_); 
            fmt_ctx_ = nullptr;
            DEBUG_LOG("Input closed");
        }
        
        if (!should_stop_) {
            DEBUG_LOG("Sleeping 1s before reconnect...");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    if (connected_.load()) {
        notifyStatusChanged(false);
    }
    
    DEBUG_LOG("RtspCameraStream::run() thread exiting");
}

void RtspCameraStream::trimBuffer() {
    int kf = 0; 
    for (auto& p : buffer_) if (p.is_key) kf++;
    
    while (kf > buffer_frames_) {
        if (buffer_.front().is_key) kf--;
        buffer_dur_ -= buffer_.front().dur;
        buffer_.pop_front();
    }
    while (!buffer_.empty() && !buffer_.front().is_key) {
        buffer_dur_ -= buffer_.front().dur;
        buffer_.pop_front();
    }
    
    // 🔧 Additional limit by total packet count
    while (buffer_.size() > MAX_BUFFER_PACKETS) {
        buffer_dur_ -= buffer_.front().dur;
        buffer_.pop_front();
    }
}

std::vector<SafePacket> RtspCameraStream::getBufferSafe() const {
    std::lock_guard<std::mutex> lock(buf_mtx_);
    std::vector<SafePacket> res;
    for (auto& p : buffer_) res.push_back(p.data);
    return res;
}