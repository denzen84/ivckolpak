#include "recording_session.hpp"
#include "packet_pool.hpp"
#include <filesystem>
#include <algorithm>
#include <numeric>
#include <cmath>

static auto codec_params_deleter = [](AVCodecParameters* p) { if (p) avcodec_parameters_free(&p); };
static auto fmt_ctx_deleter = [](AVFormatContext* f) { if (f) avformat_free_context(f); };

RecordingSession::RecordingSession(Config cfg) : cfg_(std::move(cfg)) {
    std::filesystem::create_directories(cfg_.camera_cfg->target_dir);
    LOG_INFO("SESSION", "Created for camera: ", cfg_.camera_cfg->name);
}

RecordingSession::~RecordingSession() { shutdown(); }

void RecordingSession::push_packet(AVPacket* pkt) {
    if (!pkt || shutdown_requested_.load(std::memory_order_acquire)) {
        if (pkt) av_packet_free(&pkt);
        return;
    }
    local_pushed_frames_++;
    if (local_pushed_frames_ % 30 == 0) {
        total_pushed_frames_.store(local_pushed_frames_, std::memory_order_relaxed);
    }
    
    Packet packet(pkt);
    bool should_finalize = false;
    
    {
        std::lock_guard lock(buffer_mtx_);
        State current = state_.load(std::memory_order_acquire);
        
        switch (current) {
            case State::IDLE:
                av_packet_free(&packet.pkt);
                packet.pkt = nullptr;
                break;
                
            case State::PRE_BUFFER:
                add_to_pre_buffer(std::move(packet));
                break;
                
            case State::RECORDING:
                analyzer_.analyze_packet(pkt);
                write_packet_to_file(packet.pkt);
                packet.pkt = nullptr;
                local_written_bytes_ += packet.size;
                if (local_pushed_frames_ % 30 == 0) {
                    total_written_bytes_.store(local_written_bytes_, std::memory_order_relaxed);
                }
                chunk_bytes_ += packet.size;
                
                if (cfg_.camera_cfg->max_chunk_duration_time_s > 0 ||
                    cfg_.camera_cfg->max_chunk_kbytes > 0) {
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - event_start_time_).count();
                    bool time_hit = cfg_.camera_cfg->max_chunk_duration_time_s > 0 &&
                                    elapsed >= cfg_.camera_cfg->max_chunk_duration_time_s;
                    bool size_hit = cfg_.camera_cfg->max_chunk_kbytes > 0 &&
                                    chunk_bytes_ >= (uint64_t)cfg_.camera_cfg->max_chunk_kbytes * 1024;
                    bool count_hit = cfg_.camera_cfg->max_event_chunks > 0 &&
                                     chunk_idx_ >= cfg_.camera_cfg->max_event_chunks;
                    
                    if ((time_hit || size_hit) && !count_hit) {
                        LOG_INFO("SESSION", "Chunk limit reached, scheduling rotation");
                        should_finalize = true;
                    }
                }
                break;
                
            case State::POST_BUFFER:
                if (add_to_post_buffer(std::move(packet))) {
                    should_finalize = true;
                }
                break;
        }
    }
    
    if (should_finalize) {
        finalize_and_rotate();
    }
}

void RecordingSession::finalize_and_rotate() {
    std::string path_to_finalize;
    std::function<void(const std::string&)> callback_copy;
    
    {
        std::lock_guard lock(buffer_mtx_);
        State current = state_.load(std::memory_order_acquire);
        
        if (current == State::RECORDING) {
            if (!out_fmt_ctx_) return;
            path_to_finalize = current_filename_;
            callback_copy = cfg_.on_video_save;
            
            if (analyzer_.detected_fps.num > 0 && out_stream_) {
                out_stream_->avg_frame_rate = analyzer_.detected_fps;
            }
        } else if (current == State::POST_BUFFER) {
            analyzer_.finalize();
            if (!out_fmt_ctx_) {
                post_buffer_.clear();
                post_buffer_iframe_count_ = 0;
                state_.store(State::PRE_BUFFER, std::memory_order_release);
                return;
            }
            path_to_finalize = current_filename_;
            callback_copy = cfg_.on_video_save;
            
            if (analyzer_.detected_fps.num > 0 && out_stream_) {
                out_stream_->avg_frame_rate = analyzer_.detected_fps;
            }
        } else {
            return;
        }
    }
    
    bool success = finalize_file_io(path_to_finalize);
    
    {
        std::lock_guard lock(buffer_mtx_);
        State current = state_.load(std::memory_order_acquire);
        
        out_fmt_ctx_.reset();
        out_stream_ = nullptr;
        chunk_start_pts_ = AV_NOPTS_VALUE;
        
        if (current == State::RECORDING) {
            chunk_idx_++;
            chunk_bytes_ = 0;
            event_start_time_ = std::chrono::steady_clock::now();
            if (stored_codec_params_) {
                init_muxer(stored_codec_params_.get());
            }
        } else if (current == State::POST_BUFFER) {
            post_buffer_.clear();
            post_buffer_iframe_count_ = 0;
            state_.store(State::PRE_BUFFER, std::memory_order_release);
            LOG_INFO("SESSION", "State: POST_BUFFER -> PRE_BUFFER");
        }
    }
    
    if (success && callback_copy) {
        LOG_DEBUG("SESSION", "Calling on_video_save callback asynchronously");
        callback_copy(path_to_finalize);
    }
}

bool RecordingSession::finalize_file_io(const std::string& path) {
    if (path.empty()) return false;
    
    LOG_DEBUG("SESSION", "Finalizing file (outside lock): ", path);
    
    AVFormatContext* fmt_ptr = nullptr;
    {
        std::lock_guard lock(buffer_mtx_);
        if (out_fmt_ctx_) {
            fmt_ptr = out_fmt_ctx_.get();
        }
    }
    
    if (!fmt_ptr) return false;
    
    int ret = av_write_trailer(fmt_ptr);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        LOG_ERROR("SESSION", "av_write_trailer failed: ", err);
    }
    
    if (fmt_ptr->pb) {
        avio_closep(&fmt_ptr->pb);
    }
    
    bool success = (ret >= 0);
    
    if (success) {
        LOG_INFO("SESSION", "Saved: ", path);
    } else {
        std::filesystem::remove(path);
        LOG_WARN("SESSION", "Deleted corrupted file: ", path);
    }
    
    return success;
}

void RecordingSession::start_event(const AlarmEvent& evt,
                                   const AVCodecParameters* codec_params,
                                   const RTSPStream::StreamInfo& stream_info) {
    std::lock_guard lock(buffer_mtx_);
    State current = state_.load(std::memory_order_acquire);
    
    if (current == State::RECORDING || current == State::POST_BUFFER) {
        LOG_INFO("SESSION", "Event already in progress, will be handled by continue_event()");
        return;
    }
    
    current_event_id_++;
    current_event_ = evt;
    LOG_INFO("SESSION", "Starting event #", current_event_id_, ": ", evt.event_type);
    
    if (codec_params) {
        stored_codec_params_ = std::unique_ptr<AVCodecParameters, std::function<void(AVCodecParameters*)>>(
            avcodec_parameters_alloc(), codec_params_deleter);
        avcodec_parameters_copy(stored_codec_params_.get(), codec_params);
    }
    if (stream_info.valid) {
        stored_time_base_ = stream_info.time_base;
        stored_frame_rate_ = stream_info.frame_rate;
        analyzer_.time_base = stream_info.time_base;
        LOG_INFO("SESSION", "Using stream params: time_base=",
                 stored_time_base_.num, "/", stored_time_base_.den,
                 " fps=", stored_frame_rate_.num, "/", stored_frame_rate_.den);
    } else {
        LOG_WARN("SESSION", "Stream info invalid, using defaults");
    }
    
    chunk_idx_ = 0;
    chunk_bytes_ = 0;
    event_start_time_ = std::chrono::steady_clock::now();
    analyzer_ = StreamAnalyzer{};
    analyzer_.time_base = stored_time_base_;
    init_muxer(codec_params);
    
    LOG_DEBUG("SESSION", "Draining ", pre_buffer_.size(), " packets from pre-buffer");
    for (auto& pkt : pre_buffer_) {
        if (pkt.pkt) {
            analyzer_.analyze_packet(pkt.pkt);
            write_packet_to_file(pkt.pkt);
            local_written_bytes_ += pkt.size;
            pkt.pkt = nullptr;
        }
    }
    pre_buffer_.clear();
    pre_buffer_iframe_count_ = 0;
    current_buffer_bytes_ = 0;
    state_.store(State::RECORDING, std::memory_order_release);
    LOG_INFO("SESSION", "State: PRE_BUFFER -> RECORDING");
}

void RecordingSession::continue_event(const AlarmEvent& evt) {
    std::lock_guard lock(buffer_mtx_);
    State current = state_.load(std::memory_order_acquire);
    
    if (current == State::IDLE || current == State::PRE_BUFFER) {
        LOG_WARN("SESSION", "continue_event called but not recording, ignoring");
        return;
    }
    
    if (current == State::POST_BUFFER) {
        LOG_INFO("SESSION", "Continuing event: canceling post-buffer, returning to RECORDING");
        post_buffer_.clear();
        post_buffer_iframe_count_ = 0;
        state_.store(State::RECORDING, std::memory_order_release);
        LOG_INFO("SESSION", "State: POST_BUFFER -> RECORDING (continued)");
    } else {
        LOG_INFO("SESSION", "Continuing event: already RECORDING, extending duration");
    }
    
    current_event_ = evt;
    LOG_DEBUG("SESSION", "Event continued with type: ", evt.event_type);
}

void RecordingSession::stop_event() {
    std::lock_guard lock(buffer_mtx_);
    State current = state_.load(std::memory_order_acquire);
    
    if (current == State::IDLE || current == State::PRE_BUFFER) {
        LOG_WARN("SESSION", "STOP event ignored, not in active recording state");
        return;
    }
    
    if (current == State::POST_BUFFER) {
        LOG_INFO("SESSION", "Already in POST_BUFFER, will force-finalize outside lock");
        return;
    }
    
    LOG_INFO("SESSION", "Stopping event, collecting ", cfg_.post_buffer_iframes, " post-frames");
    post_buffer_.clear();
    post_buffer_iframe_count_ = 0;
    state_.store(State::POST_BUFFER, std::memory_order_release);
    LOG_INFO("SESSION", "State: RECORDING -> POST_BUFFER");
}

void RecordingSession::force_stop() {
    std::string path_to_finalize;
    std::function<void(const std::string&)> callback_copy;
    
    {
        std::lock_guard lock(buffer_mtx_);
        State current = state_.load(std::memory_order_acquire);
        
        if (current == State::IDLE || current == State::PRE_BUFFER) {
            return;
        }
        
        LOG_WARN("SESSION", "FORCE_STOP: Scheduling finalization from state ", (int)current);
        
        if (out_fmt_ctx_) {
            path_to_finalize = current_filename_;
            callback_copy = cfg_.on_video_save;
        }
        
        pre_buffer_.clear();
        post_buffer_.clear();
        pre_buffer_iframe_count_ = 0;
        post_buffer_iframe_count_ = 0;
    }
    
    if (!path_to_finalize.empty()) {
        finalize_file_io(path_to_finalize);
    }
    
    {
        std::lock_guard lock(buffer_mtx_);
        out_fmt_ctx_.reset();
        out_stream_ = nullptr;
        state_.store(State::PRE_BUFFER, std::memory_order_release);
        LOG_INFO("SESSION", "State: FORCE_STOP -> PRE_BUFFER");
    }
}

void RecordingSession::shutdown() {
    shutdown_requested_.store(true, std::memory_order_release);
    
    std::string path_to_finalize;
    {
        std::lock_guard lock(buffer_mtx_);
        if (state_.load(std::memory_order_acquire) == State::RECORDING ||
            state_.load(std::memory_order_acquire) == State::POST_BUFFER) {
            if (out_fmt_ctx_) {
                path_to_finalize = current_filename_;
            }
        }
        pre_buffer_.clear();
        post_buffer_.clear();
        state_.store(State::IDLE, std::memory_order_release);
        total_pushed_frames_.store(local_pushed_frames_, std::memory_order_relaxed);
        total_written_bytes_.store(local_written_bytes_, std::memory_order_relaxed);
    }
    
    if (!path_to_finalize.empty()) {
        finalize_file_io(path_to_finalize);
    }
    
    {
        std::lock_guard lock(buffer_mtx_);
        out_fmt_ctx_.reset();
        out_stream_ = nullptr;
    }
    
    LOG_INFO("SESSION", "Shutdown complete");
}

auto RecordingSession::get_stats() const -> Stats {
    std::lock_guard lock(buffer_mtx_);
    return Stats{
        .state = state_.load(std::memory_order_acquire),
        .buffer_iframes = pre_buffer_iframe_count_,
        .buffer_kb = current_buffer_bytes_ / 1024,
        .total_pushed_frames = total_pushed_frames_.load(std::memory_order_relaxed),
        .total_written_kb = total_written_bytes_.load(std::memory_order_relaxed) / 1024,
        .detected_fps = analyzer_.get_fps(),
        .current_file = current_filename_
    };
}

void RecordingSession::add_to_pre_buffer(Packet&& pkt) {
    if (!pkt.pkt) return;
    current_buffer_bytes_ += pkt.size;
    if (pkt.is_keyframe) {
        pre_buffer_iframe_count_++;
    }
    pre_buffer_.push_back(std::move(pkt));
    while (pre_buffer_iframe_count_ > cfg_.pre_buffer_iframes) {
        drop_oldest_gop_from_pre_buffer();
    }
}

void RecordingSession::drop_oldest_gop_from_pre_buffer() {
    if (pre_buffer_.empty()) return;
    auto it = pre_buffer_.begin();
    while (it != pre_buffer_.end() && !it->is_keyframe) {
        current_buffer_bytes_ -= it->size;
        ++it;
    }
    pre_buffer_.erase(pre_buffer_.begin(), it);
    if (it == pre_buffer_.end()) return;
    bool keyframe_removed = false;
    it = pre_buffer_.begin();
    while (it != pre_buffer_.end()) {
        if (it->is_keyframe) {
            if (keyframe_removed) break;
            keyframe_removed = true;
        }
        current_buffer_bytes_ -= it->size;
        it = pre_buffer_.erase(it);
    }
    if (keyframe_removed) {
        pre_buffer_iframe_count_--;
    }
}

bool RecordingSession::add_to_post_buffer(Packet&& pkt) {
    if (!pkt.pkt) return false;
    if (pkt.is_keyframe) {
        post_buffer_iframe_count_++;
    }
    analyzer_.analyze_packet(pkt.pkt);
    write_packet_to_file(pkt.pkt);
    local_written_bytes_ += pkt.size;
    pkt.pkt = nullptr;
    return post_buffer_iframe_count_ >= cfg_.post_buffer_iframes;
}

void RecordingSession::init_muxer(const AVCodecParameters* codec_params) {
    if (!codec_params) {
        LOG_ERROR("SESSION", "Cannot init muxer without codec params");
        return;
    }
    AlarmEvent temp_evt;
    if (current_event_.has_value()) {
        temp_evt = current_event_.value();
        temp_evt.event_id = current_event_id_;
    }
    current_filename_ = cfg_.camera_cfg->target_dir + "/" +
                        format_filename(cfg_.camera_cfg->filename_format,
                                        *cfg_.camera_cfg,
                                        chunk_idx_,
                                        current_event_.has_value() ? &temp_evt : nullptr);
    LOG_DEBUG("SESSION", "Initializing muxer: ", current_filename_);
    AVFormatContext* fmt = nullptr;
    int ret = avformat_alloc_output_context2(&fmt, nullptr, "mp4", current_filename_.c_str());
    if (ret < 0 || !fmt) {
        LOG_ERROR("SESSION", "Failed to allocate output context");
        return;
    }
    ret = avio_open(&fmt->pb, current_filename_.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        LOG_ERROR("SESSION", "Failed to open file: ", err);
        avformat_free_context(fmt);
        return;
    }
    out_stream_ = avformat_new_stream(fmt, nullptr);
    if (!out_stream_) {
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        LOG_ERROR("SESSION", "Failed to create stream");
        return;
    }
    avcodec_parameters_copy(out_stream_->codecpar, codec_params);
    out_stream_->time_base = stored_time_base_;
    out_stream_->avg_frame_rate = stored_frame_rate_;
    LOG_DEBUG("SESSION", "Set stream time_base=", out_stream_->time_base.num, "/", out_stream_->time_base.den);
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "faststart+frag_keyframe", 0);
    av_dict_set(&opts, "frag_duration", "1000000", 0);
    ret = avformat_write_header(fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        LOG_ERROR("SESSION", "Failed to write header: ", err);
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return;
    }
    out_fmt_ctx_ = std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext*)>>(fmt, fmt_ctx_deleter);
    LOG_DEBUG("SESSION", "Muxer initialized successfully with faststart");
}

void RecordingSession::write_packet_to_file(AVPacket* pkt) {
    if (!out_fmt_ctx_ || !pkt) {
        if (pkt) av_packet_free(&pkt);
        return;
    }
    av_packet_rescale_ts(pkt, analyzer_.time_base, out_stream_->time_base);
    if (pkt->pts != AV_NOPTS_VALUE && chunk_start_pts_ == AV_NOPTS_VALUE) {
        chunk_start_pts_ = pkt->pts;
    }
    pkt->stream_index = 0;
    pkt->pos = -1;
    int ret = av_interleaved_write_frame(out_fmt_ctx_.get(), pkt);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        LOG_WARN("SESSION", "Write frame failed: ", err);
    }
    av_packet_free(&pkt);
}

void RecordingSession::StreamAnalyzer::analyze_packet(const AVPacket* pkt) {
    if (!pkt || pkt->pts == AV_NOPTS_VALUE) return;
    if (first_pts == AV_NOPTS_VALUE) {
        first_pts = pkt->pts;
    }
    if (last_pts != AV_NOPTS_VALUE && pkt->pts > last_pts) {
        int64_t diff = pkt->pts - last_pts;
        if (diff > 0 && diff < 1000000) {
            pts_diffs.push_back(diff);
        }
    }
    last_pts = pkt->pts;
    total_frames++;
}

void RecordingSession::StreamAnalyzer::finalize() {
    if (pts_diffs.empty() || first_pts == AV_NOPTS_VALUE || last_pts == AV_NOPTS_VALUE) {
        return;
    }
    std::sort(pts_diffs.begin(), pts_diffs.end());
    int64_t median_diff = pts_diffs[pts_diffs.size() / 2];
    if (median_diff > 0) {
        double fps = static_cast<double>(time_base.den) / (median_diff * time_base.num);
        if (std::abs(fps - 25.0) < 1.0) fps = 25.0;
        else if (std::abs(fps - 30.0) < 1.0) fps = 30.0;
        else if (std::abs(fps - 15.0) < 1.0) fps = 15.0;
        else if (std::abs(fps - 20.0) < 1.0) fps = 20.0;
        detected_fps = AVRational{static_cast<int>(fps * 1000), 1000};
        LOG_DEBUG("ANALYZER", "Using time_base ", time_base.num, "/", time_base.den,
                  " | Median PTS diff: ", median_diff, " -> FPS: ", fps);
    }
}

double RecordingSession::StreamAnalyzer::get_fps() const {
    if (detected_fps.num == 0) return 0.0;
    return av_q2d(detected_fps);
}
