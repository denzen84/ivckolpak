#include "rtsp_stream.hpp"
#include "utils.hpp"
#include "packet_pool.hpp"
#include "config.h"
#include <chrono>
#include <algorithm>
#include <cstring>

static auto codec_params_deleter = [](AVCodecParameters* p) { if (p) avcodec_parameters_free(&p); };

RTSPStream::RTSPStream(Config cfg) : cfg_(std::move(cfg)) {}
RTSPStream::~RTSPStream() { stop(); }

void RTSPStream::start(PacketCallback cb) {
    if (worker_.joinable()) { LOG_WARN("RTSP", "Already running."); return; }
    packet_cb_ = std::move(cb);
    LOG_INFO("RTSP", "Starting stream: ", cfg_.url);
    worker_ = std::jthread([this](std::stop_token st) { run(st); });
}

void RTSPStream::stop() {
    if (!worker_.joinable()) return;
    LOG_INFO("RTSP", "Stopping stream...");
    interrupt_flag_.store(true, std::memory_order_relaxed);
    worker_.request_stop();
    worker_.join();
    { std::lock_guard lock(io_mtx_); close_input(); }
    interrupt_flag_.store(false, std::memory_order_relaxed);
    LOG_INFO("RTSP", "Stream stopped.");
}

bool RTSPStream::is_alive() const noexcept { return alive_.load(std::memory_order_acquire); }

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

void RTSPStream::run(std::stop_token st) {
    int reconnect_attempts = 0;
    int64_t current_delay = cfg_.reconnect_delay_ms;
    bool first_connection = true;
    while (!st.stop_requested()) {
        if (!open_input()) {
            close_input();
            alive_.store(false, std::memory_order_release);
            if (cfg_.max_reconnect_attempts > 0 && reconnect_attempts >= cfg_.max_reconnect_attempts) {
                LOG_ERROR("RTSP", "Max reconnect attempts reached.");
                return;
            }
            if (!cfg_.on_rtsp_lost.empty() && cfg_.camera_config) {
                LOG_DEBUG("RTSP", "Calling on_rtsp_lost callback");
                execute_script_async(cfg_.on_rtsp_lost, *cfg_.camera_config);
            }
            LOG_WARN("RTSP", "Reconnecting in ", current_delay, "ms (attempt ", reconnect_attempts + 1, ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(current_delay));
            current_delay = std::min(current_delay * 2, static_cast<int64_t>(cfg_.reconnect_max_delay_ms));
            reconnect_attempts++;
            continue;
        }
        reconnect_attempts = 0;
        current_delay = cfg_.reconnect_delay_ms;
        alive_.store(true, std::memory_order_release);
        if (!cfg_.on_rtsp_found.empty() && cfg_.camera_config) {
            if (first_connection) {
                LOG_DEBUG("RTSP", "Calling on_rtsp_found callback (FIRST connection)");
            } else {
                LOG_DEBUG("RTSP", "Calling on_rtsp_found callback (reconnect)");
            }
            execute_script_async(cfg_.on_rtsp_found, *cfg_.camera_config);
        }
        first_connection = false;
        LOG_INFO("RTSP", "Connected to stream.");
        AVPacket* pkt = PacketPool::instance().acquire();
        while (!st.stop_requested()) {
            int ret = av_read_frame(fmt_ctx_, pkt);
            if (ret < 0) {
                if (ret == AVERROR_EXIT || st.stop_requested()) break;
                char err[128];
                av_strerror(ret, err, sizeof(err));
                LOG_WARN("RTSP", "Read failed: ", err);
                break;
            }
            if (pkt->stream_index == video_stream_idx_) {
                AVPacket* clone = av_packet_clone(pkt);
                if (clone && packet_cb_) packet_cb_(clone);
                else if (clone) av_packet_free(&clone);
            }
            av_packet_unref(pkt);
        }
        PacketPool::instance().release(pkt);
        close_input();
        alive_.store(false, std::memory_order_release);
        if (!st.stop_requested()) LOG_WARN("RTSP", "Connection lost. Reconnecting...");
    }
}

bool RTSPStream::open_input() {
    std::lock_guard lock(io_mtx_);
    if (fmt_ctx_) return true;
    AVFormatContext* raw_ctx = avformat_alloc_context();
    if (!raw_ctx) {
        LOG_ERROR("RTSP", "Alloc context failed.");
        return false;
    }
    raw_ctx->interrupt_callback = { interrupt_cb, &interrupt_flag_ };
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", cfg_.tcp_only ? "tcp" : "udp", 0);
    av_dict_set_int(&opts, "stimeout", cfg_.timeout_ms * 1000LL, 0);
    int ret = avformat_open_input(&raw_ctx, cfg_.url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        LOG_ERROR("RTSP", "Open failed: ", e);
        avformat_free_context(raw_ctx);
        return false;
    }
    if (avformat_find_stream_info(raw_ctx, nullptr) < 0) {
        LOG_ERROR("RTSP", "Stream info failed.");
        avformat_close_input(&raw_ctx);
        return false;
    }
    video_stream_idx_ = av_find_best_stream(raw_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        LOG_ERROR("RTSP", "No video stream.");
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
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
    video_stream_idx_ = -1;
    codec_params_.reset();
    stream_info_valid_ = false;
}