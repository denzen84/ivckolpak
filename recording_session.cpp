#include "recording_session.hpp"
#include "video_recorder.hpp"
#include "utils.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>

RecordingSession::RecordingSession(const std::string& cam_id, const std::string& cam_name, const AlarmEvent& evt,
                                   AVCodecParameters* params, const std::string& filepath, int max_dur, int max_chunks,
                                   int post_buf_iframes, const std::string& on_start, const std::string& on_stop,
                                   const std::string& on_save, OnFinishedCallback finished_cb)
    : cam_id_(cam_id), cam_name_(cam_name), event_data_(evt), filepath_(filepath),
      max_duration_sec_(max_dur), max_chunks_(max_chunks), post_buffer_iframes_(post_buf_iframes),
      on_start_(on_start), on_stop_(on_stop), on_save_(on_save), on_finished_(std::move(finished_cb)) {
    if (params) {
        codec_params_.reset(avcodec_parameters_alloc());
        avcodec_parameters_copy(codec_params_.get(), params);
    }
    recorder_ = std::make_unique<VideoRecorder>(filepath);
}

RecordingSession::~RecordingSession() {
    requestStop();
    if (worker_.joinable()) worker_.join();
}

void RecordingSession::start(std::vector<SafePacket> prebuffer) {
    std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Recording started.\n";
    if (!codec_params_ || !recorder_->start(codec_params_.get(), prebuffer)) {
        std::cerr << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Failed to start.\n";
        finished_.store(true);
        if (on_finished_) on_finished_(cam_id_);
        return;
    }
    worker_ = std::thread(&RecordingSession::runLoop, this);
}

void RecordingSession::requestStop() {
    if (post_buffer_iframes_ > 0 && !post_buffer_active_.load()) {
        post_buffer_active_.store(true);
        post_buffer_remaining_.store(post_buffer_iframes_);
        std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Post-buffering activated.\n";
        return;
    }
    stop_requested_.store(true);
    timer_cv_.notify_all();
}

void RecordingSession::updateEventStatus(const std::string& new_status) {
    event_data_.status = new_status;
}

bool RecordingSession::isIFrame(const AVPacket* pkt) const {
    return pkt && (pkt->flags & AV_PKT_FLAG_KEY);
}

void RecordingSession::pushPacket(const SafePacket& pkt) {
    if (stop_requested_.load() && !post_buffer_active_.load()) return;
    if (!recorder_ || !recorder_->isRecording() || pkt.empty()) return;
    
    if (post_buffer_active_.load() && isIFrame(pkt.get())) {
        if (--post_buffer_remaining_ <= 0) {
            std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Post-buffer complete.\n";
            post_buffer_active_.store(false);
            stop_requested_.store(true);
            timer_cv_.notify_all();
        }
    }
    recorder_->push(pkt);
}

void RecordingSession::runLoop() {
    int chunk_idx = 1;
    executeScriptAsync(on_start_, filepath_);

    while (chunk_idx <= max_chunks_ && !stop_requested_.load()) {
        std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Writing chunk " << chunk_idx << "\n";
        {
            std::unique_lock<std::mutex> lock(timer_mutex_);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(max_duration_sec_);
            if (timer_cv_.wait_until(lock, deadline, [this]{ return stop_requested_.load(); })) break;
        }
        chunk_idx++;
        if (chunk_idx > max_chunks_) break;

        recorder_->stop();
        std::string new_path = filepath_;
        size_t dot = new_path.find_last_of('.');
        if (dot != std::string::npos) new_path.insert(dot, "_part" + std::to_string(chunk_idx));
        
        recorder_ = std::make_unique<VideoRecorder>(new_path);
        std::vector<SafePacket> dummy;
        recorder_->start(codec_params_.get(), dummy);
        executeScriptAsync(on_save_, new_path);
    }

    if (recorder_) recorder_->stop();
    executeScriptAsync(on_stop_, filepath_);
    executeScriptAsync(on_save_, filepath_);  // 🔧 Final on_save call
    finished_.store(true);
    if (on_finished_) on_finished_(cam_id_);
}

void RecordingSession::executeScriptAsync(const std::string& tmpl, const std::string& path) {
    if (tmpl.empty()) return;

    MacroContext ctx;
    ctx.camera_id = cam_id_;
    ctx.camera_name = cam_name_;
    ctx.event_type = event_data_.event_type;
    ctx.status = event_data_.status;
    ctx.address = event_data_.address;
    ctx.channel = event_data_.channel;
    ctx.description = event_data_.description;
    ctx.serial_id = event_data_.serial_id;
    ctx.start_time = event_data_.start_time;
    ctx.alarm_type = event_data_.alarm_type;
    ctx.filepath = path;
    ctx.timestamp = std::time(nullptr);

    std::string cmd = applyMacros(tmpl, ctx);
    std::cout << "[" << getTimestamp() << "] [Session] Executing async: " << cmd << "\n";
    ScriptRunner::run(cmd);
}