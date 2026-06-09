#include "rtsp_stream.hpp"
#include "utils.hpp"
#include "packet_pool.hpp"
#include "config.h"
#include <chrono>
#include <algorithm>
#include <cstring>

static auto codec_params_deleter = [](AVCodecParameters* p) { if (p) avcodec_parameters_free(&p); };

RTSPStream::RTSPStream(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.read_timeout_ms <= 0) {
        cfg_.read_timeout_ms = 10000;
    }
}

RTSPStream::~RTSPStream() { stop(); }

void RTSPStream::start(PacketCallback cb) {
    if (worker_.joinable()) { 
        LOG_WARN("RTSP", "Already running."); 
        return; 
    }
    packet_cb_ = std::move(cb);
    LOG_INFO("RTSP", "Starting stream: ", cfg_.url);
    LOG_INFO("RTSP", "Read timeout configured: ", cfg_.read_timeout_ms, "ms");
    
    worker_ = std::jthread([this](std::stop_token st) { run(st); });
    watchdog_ = std::jthread([this](std::stop_token st) { watchdog_loop(st); });
}

void RTSPStream::stop() {
    if (!worker_.joinable()) return;
    LOG_INFO("RTSP", "Stopping stream...");
    
    watchdog_enabled_.store(false, std::memory_order_release);
    interrupt_flag_.store(true, std::memory_order_release);
    
    if (watchdog_.joinable()) {
        watchdog_.request_stop();
        watchdog_.join();
    }
    
    worker_.request_stop();
    worker_.join();
    
    { 
        std::lock_guard lock(io_mtx_); 
        close_input(); 
    }
    
    interrupt_flag_.store(false, std::memory_order_release);
    LOG_INFO("RTSP", "Stream stopped.");
}

bool RTSPStream::is_alive() const noexcept { 
    return alive_.load(std::memory_order_acquire); 
}

auto RTSPStream::get_codec_params() const -> std::unique_ptr<AVCodecParameters, std::function<void(AVCodecParameters*)>> {
    std::lock_guard lock(io_mtx_);
    if (!codec_params_) return nullptr;
    auto clone = std::unique_ptr<AVCodecParameters, std::function<void(AVCodecParameters*)>>(
        avcodec_parameters_alloc(), codec_params_deleter);
    if (avcodec_parameters_copy(clone.get(), codec_params_.get()) < 0) return nullptr;
    return clone;
}

RTSPStream::StreamInfo RTSPStream::get_stream_info() const {
    std::lock_guard lock(io_mtx_);
    return StreamInfo{
        .time_base = stream_time_base_,
        .frame_rate = stream_frame_rate_,
        .valid = stream_info_valid_
    };
}

void RTSPStream::watchdog_loop(std::stop_token st) {
    LOG_DEBUG("RTSP_WD", "Watchdog started with timeout: ", cfg_.read_timeout_ms, "ms");
    
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        if (!watchdog_enabled_.load(std::memory_order_acquire)) {
            continue;
        }
        
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto last_packet_ms = last_packet_time_ms_.load(std::memory_order_acquire);
        uint64_t elapsed = static_cast<uint64_t>(now_ms - last_packet_ms);  
        
        if (elapsed > static_cast<uint64_t>(cfg_.read_timeout_ms)) { 
            LOG_ERROR("RTSP_WD", "TIMEOUT! No data received for ", elapsed, "ms (limit: ", 
                     cfg_.read_timeout_ms, "ms). Forcing disconnect.");
            interrupt_flag_.store(true, std::memory_order_release);
            watchdog_enabled_.store(false, std::memory_order_release);
        }
    }
    
    LOG_DEBUG("RTSP_WD", "Watchdog stopped");
}


void RTSPStream::run(std::stop_token st) {
    int reconnect_attempts = 0;
    int64_t current_delay = cfg_.reconnect_delay_ms;
    bool was_connected = false;
    
    while (!st.stop_requested()) {
        interrupt_flag_.store(false, std::memory_order_release);
        watchdog_enabled_.store(false, std::memory_order_release);
        
        if (!open_input()) {
            close_input();
            alive_.store(false, std::memory_order_release);
            
            if (was_connected) {

                if (cfg_.internal_lost_callback) {
                    LOG_DEBUG("RTSP", "Calling internal_lost_callback");
                    cfg_.internal_lost_callback();
                }
                
                if (!cfg_.on_rtsp_lost.empty() && cfg_.camera_config) {
                    LOG_WARN("RTSP", "Calling on_rtsp_lost callback");
                    execute_script_async(cfg_.on_rtsp_lost, *cfg_.camera_config);
                }
                was_connected = false;
            }
            
            if (cfg_.max_reconnect_attempts > 0 && reconnect_attempts >= cfg_.max_reconnect_attempts) {
                LOG_ERROR("RTSP", "Max reconnect attempts reached.");
                return;
            }
            
            LOG_WARN("RTSP", "Reconnecting in ", current_delay, "ms (attempt ", reconnect_attempts + 1, ")");
            
            for (int i = 0; i < current_delay / 100 && !st.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            current_delay = std::min(current_delay * 2, static_cast<int64_t>(cfg_.reconnect_max_delay_ms));
            reconnect_attempts++;
            continue;
        }
        
        reconnect_attempts = 0;
        current_delay = cfg_.reconnect_delay_ms;
        alive_.store(true, std::memory_order_release);
        
        if (!was_connected) {
      
            if (cfg_.internal_found_callback) {
                LOG_DEBUG("RTSP", "Calling internal_found_callback");
                cfg_.internal_found_callback();
            }
            
            if (!cfg_.on_rtsp_found.empty() && cfg_.camera_config) {
                LOG_INFO("RTSP", "Calling on_rtsp_found callback");
                execute_script_async(cfg_.on_rtsp_found, *cfg_.camera_config);
            }
            was_connected = true;
        }
        
        LOG_INFO("RTSP", "Connected to stream, starting watchdog");
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        last_packet_time_ms_.store(now_ms, std::memory_order_release);
        watchdog_enabled_.store(true, std::memory_order_release);
        
        AVPacket* pkt = PacketPool::instance().acquire();
        bool connection_lost = false;
        
        while (!st.stop_requested()) {
            if (interrupt_flag_.load(std::memory_order_acquire)) {
                LOG_ERROR("RTSP", "Interrupt flag set, breaking read loop");
                connection_lost = true;
                break;
            }
            
            int ret = av_read_frame(fmt_ctx_, pkt);
            
            if (ret < 0) {
                if (ret == AVERROR_EXIT) {
                    LOG_INFO("RTSP", "Received AVERROR_EXIT");
                    break;
                }
                if (ret == AVERROR(EAGAIN) || ret == AVERROR(EWOULDBLOCK)) {
                    LOG_DEBUG("RTSP", "Temporary unavailable, retrying");
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                if (st.stop_requested()) {
                    break;
                }
                
                char err[128];
                av_strerror(ret, err, sizeof(err));
                
                if (ret == AVERROR(ETIMEDOUT) || ret == AVERROR(EIO) || 
                    ret == AVERROR(ECONNRESET) || ret == AVERROR(EPIPE)) {
                    LOG_ERROR("RTSP", "Connection error: ", err, " (code: ", ret, ")");
                    connection_lost = true;
                } else {
                    LOG_WARN("RTSP", "Read error: ", err, " (code: ", ret, ")");
                    connection_lost = true;
                }
                break;
            }
            
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            last_packet_time_ms_.store(now_ms, std::memory_order_release);
            
            if (pkt->stream_index == video_stream_idx_) {

                total_bytes_read_.fetch_add(pkt->size, std::memory_order_relaxed);
                
                AVPacket* clone = av_packet_clone(pkt);
                if (clone && packet_cb_) {
                    packet_cb_(clone);
                } else if (clone) {
                    av_packet_free(&clone);
                }
            }
            av_packet_unref(pkt);
        }
        
        watchdog_enabled_.store(false, std::memory_order_release);
        PacketPool::instance().release(pkt);
        close_input();
        alive_.store(false, std::memory_order_release);
        
        if (connection_lost && !st.stop_requested()) {
            if (was_connected) {

                if (cfg_.internal_lost_callback) {
                    LOG_DEBUG("RTSP", "Connection lost during operation, calling internal_lost_callback");
                    cfg_.internal_lost_callback();
                }
                
                if (!cfg_.on_rtsp_lost.empty() && cfg_.camera_config) {
                    LOG_WARN("RTSP", "Connection lost during operation, calling on_rtsp_lost callback");
                    execute_script_async(cfg_.on_rtsp_lost, *cfg_.camera_config);
                }
                was_connected = false;
            }
            LOG_WARN("RTSP", "Connection lost. Attempting to reconnect...");
        } else if (st.stop_requested()) {
            LOG_INFO("RTSP", "Stop requested, exiting");
            break;
        }
    }
}

bool RTSPStream::open_input() {
    std::lock_guard lock(io_mtx_);
    if (fmt_ctx_) return true;
    
    AVFormatContext* raw_ctx = avformat_alloc_context();
    if (!raw_ctx) {
        LOG_ERROR("RTSP", "Alloc context failed");
        return false;
    }
    
    raw_ctx->interrupt_callback = { interrupt_cb, &interrupt_flag_ };
    
    AVDictionary* opts = nullptr;

    av_dict_set(&opts, "rtsp_transport", cfg_.tcp_only ? "tcp" : "udp", 0);
    av_dict_set_int(&opts, "stimeout", cfg_.timeout_ms * 1000LL, 0);
    av_dict_set_int(&opts, "rw_timeout", cfg_.timeout_ms * 1000LL, 0);

    av_dict_set(&opts, "fflags", "nobuffer+discardcorrupt", 0);  
    av_dict_set(&opts, "err_detect", "ignore_err", 0);           
    av_dict_set_int(&opts, "max_delay", 200000, 0);              

    av_dict_set(&opts, "reorder_queue_size", "0", 0);
    av_dict_set(&opts, "buffer_size", "1024000", 0);
    av_dict_set(&opts, "rtsp_flags", "prefer_tcp", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    
    int ret = avformat_open_input(&raw_ctx, cfg_.url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        LOG_ERROR("RTSP", "Open failed: ", e, " (code: ", ret, ")");
        avformat_free_context(raw_ctx);
        return false;
    }
    
    raw_ctx->max_analyze_duration = 2 * AV_TIME_BASE;
    raw_ctx->probesize = 1024 * 1024;
    
    if (avformat_find_stream_info(raw_ctx, nullptr) < 0) {
        LOG_ERROR("RTSP", "Stream info failed");
        avformat_close_input(&raw_ctx);
        return false;
    }
    
    video_stream_idx_ = av_find_best_stream(raw_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        LOG_ERROR("RTSP", "No video stream found");
        avformat_close_input(&raw_ctx);
        return false;
    }
    
    AVStream* stream = raw_ctx->streams[video_stream_idx_];
    stream_time_base_ = stream->time_base;
    stream_frame_rate_ = stream->avg_frame_rate;
    stream_info_valid_ = true;
    
    LOG_INFO("RTSP", "Stream parameters: time_base=", stream_time_base_.num, "/", stream_time_base_.den,
             " fps=", stream_frame_rate_.num, "/", stream_frame_rate_.den);
    
    codec_params_ = std::unique_ptr<AVCodecParameters, std::function<void(AVCodecParameters*)>>(
        avcodec_parameters_alloc(), codec_params_deleter);
    
    if (avcodec_parameters_copy(codec_params_.get(), stream->codecpar) < 0) {
        avformat_close_input(&raw_ctx);
        codec_params_.reset();
        stream_info_valid_ = false;
        return false;
    }
    
    fmt_ctx_ = raw_ctx;
    return true;
}

void RTSPStream::close_input() {
    if (!fmt_ctx_) return;
    
    LOG_DEBUG("RTSP", "Closing input context");
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
    video_stream_idx_ = -1;
    codec_params_.reset();
    stream_info_valid_ = false;
}
