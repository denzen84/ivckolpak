#include "video_recorder.hpp"
#include "utils.hpp"
#include <iostream>
#include <filesystem>
#include <algorithm>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
}

VideoRecorder::VideoRecorder(const std::string& p) : output_path_(p), temp_path_(p + ".tmp") {}
VideoRecorder::~VideoRecorder() { stop(); }

bool VideoRecorder::start(AVCodecParameters* params, std::vector<SafePacket>& pre) {
    if (is_recording_.load() || !params) return false;
    if (avformat_alloc_output_context2(&out_fmt_ctx_, nullptr, "mp4", temp_path_.c_str()) < 0) return false;
    AVStream* out = avformat_new_stream(out_fmt_ctx_, nullptr);
    if (!out || avcodec_parameters_copy(out->codecpar, params) < 0) return false;
    out->codecpar->codec_tag = 0;
    out_video_stream_idx_ = out->index;
    out_time_base_rec_ = AVRational{1, 1000000}; out->time_base = out_time_base_rec_;
    if (!writeHeader()) return false;
    
    should_stop_ = false; is_recording_ = true;
    writer_thread_ = std::thread(&VideoRecorder::writeLoop, this);
    
    for (auto& sp : pre) {
        if (sp.get()) {
            AVPacket* copy = av_packet_clone(sp.get());
            rebasePacket(copy);
            av_interleaved_write_frame(out_fmt_ctx_, copy);
            av_packet_free(&copy);
        }
    }
    pre.clear();
    std::cout << "[" << getTimestamp() << "] [Recorder] Started: " << temp_path_ << "\n";
    return true;
}

void VideoRecorder::push(SafePacket pkt) {
    if (!is_recording_.load() || pkt.empty()) return;
    { 
        std::lock_guard<std::mutex> l(queue_mutex_); 
        write_queue_.push_back(std::move(pkt)); 
    }
    queue_cv_.notify_one();
}

bool VideoRecorder::stop() {
    if (!is_recording_.load()) return true;
    should_stop_ = true; queue_cv_.notify_all();
    if (writer_thread_.joinable()) writer_thread_.join();
    writeTrailer();
    std::error_code ec; std::filesystem::rename(temp_path_, output_path_, ec);
    if (!ec) std::cout << "[" << getTimestamp() << "] [Recorder] Saved: " << output_path_ << "\n";
    is_recording_ = false; return true;
}

void VideoRecorder::writeLoop() {
    while (true) {
        SafePacket pkt;
        {
            std::unique_lock<std::mutex> l(queue_mutex_);
            // 🔧 Wait until data arrives OR stop is requested
            queue_cv_.wait(l, [this]{ return !write_queue_.empty() || should_stop_.load(); });
            
            // 🔧 Clear exit logic: if queue is empty, we're done (stop was signaled)
            if (write_queue_.empty()) {
                break;
            }
            
            pkt = std::move(write_queue_.front());
            write_queue_.pop_front();
        }
        // 🔧 pkt is guaranteed to be non-empty here
        rebasePacket(pkt.get());
        av_interleaved_write_frame(out_fmt_ctx_, pkt.get());
        // pkt auto-destructs (shared_ptr cleanup)
    }
}

bool VideoRecorder::writeHeader() {
    if (avio_open(&out_fmt_ctx_->pb, temp_path_.c_str(), AVIO_FLAG_WRITE) < 0) return false;
    AVDictionary* o = nullptr;
    av_dict_set(&o, "movflags", "faststart", 0);
    return avformat_write_header(out_fmt_ctx_, &o) >= 0;
}
bool VideoRecorder::writeTrailer() {
    if (out_fmt_ctx_ && out_fmt_ctx_->pb) { av_write_trailer(out_fmt_ctx_); avio_closep(&out_fmt_ctx_->pb); }
    return true;
}

void VideoRecorder::rebasePacket(AVPacket* pkt) {
    pkt->pts = next_pts_; pkt->dts = next_pts_;
    pkt->stream_index = out_video_stream_idx_; pkt->time_base = out_time_base_rec_;
    bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    pkt->flags = is_key ? (pkt->flags | AV_PKT_FLAG_KEY) : (pkt->flags & ~AV_PKT_FLAG_KEY);
    packet_count_since_iframe_++;
    if (is_key) {
        if (last_iframe_pts_ >= 0 && packet_count_since_iframe_ > 1) {
            double el = (next_pts_ - last_iframe_pts_) / 1000000.0;
            if (el > 0.01) {
                double fps = std::clamp(packet_count_since_iframe_ / el, 5.0, 120.0);
                out_frame_duration_ = static_cast<int64_t>(1000000.0 / fps + 0.5);
            }
        }
        last_iframe_pts_ = next_pts_; packet_count_since_iframe_ = 0;
    }
    pkt->duration = out_frame_duration_;
    next_pts_ += out_frame_duration_;
}