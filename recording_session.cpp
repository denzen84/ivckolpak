#include "recording_session.hpp"
#include "video_recorder.hpp"
#include "utils.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>

RecordingSession::RecordingSession(const std::string& cam_id, AVCodecParameters* params,
                                   const std::string& filepath, int max_dur, int max_chunks,
                                   int post_buf_iframes, 
                                   const std::string& on_start, const std::string& on_stop,
                                   const std::string& on_save, OnFinishedCallback finished_cb)
    : cam_id_(cam_id), filepath_(filepath),
      max_duration_sec_(max_dur), max_chunks_(max_chunks),
      post_buffer_iframes_(post_buf_iframes), 
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
    for (auto& t : script_threads_) if (t.joinable()) t.join();
}

void RecordingSession::start(std::vector<SafePacket> prebuffer) {
    std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Recording started.\n";
    
    if (!codec_params_) {
        std::cerr << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Codec params not available.\n";
        finished_.store(true);
        if (on_finished_) on_finished_(cam_id_);
        return;
    }
    
    if (!recorder_->start(codec_params_.get(), prebuffer)) {
        std::cerr << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Failed to start recorder.\n";
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
        std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ 
                  << " -> Post-buffering activated (" << post_buffer_iframes_ << " I-frames).\n";
        return;  
    }
    

    stop_requested_.store(true);
    timer_cv_.notify_all();
}

bool RecordingSession::isIFrame(const AVPacket* pkt) const {
    return pkt && (pkt->flags & AV_PKT_FLAG_KEY);
}

void RecordingSession::pushPacket(const SafePacket& pkt) {
    if (stop_requested_.load() && !post_buffer_active_.load()) return;
    if (!recorder_ || !recorder_->isRecording()) return;
    if (pkt.empty()) return;
    

    if (post_buffer_active_.load()) {
        if (isIFrame(pkt.get())) {
            int remaining = --post_buffer_remaining_;
            DEBUG_LOG("Post-buffer: I-frame received, remaining=" << remaining);
            if (remaining <= 0) {
                std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ 
                          << " -> Post-buffer complete, stopping recording.\n";
                post_buffer_active_.store(false);
                stop_requested_.store(true);
                timer_cv_.notify_all();

            }
        }
    }
    
    recorder_->push(pkt);
}

void RecordingSession::runLoop() {
    int chunk_idx = 1;
    int event_counter = 1;
    
    AlarmEvent dummy_evt;
    dummy_evt.event_type = "MotionDetect";
    dummy_evt.status = "Start";
    
    executeScriptAsync(on_start_, dummy_evt, filepath_);

    while (chunk_idx <= max_chunks_ && !stop_requested_.load()) {
        std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Writing chunk " << chunk_idx << "\n";
        
        {
            std::unique_lock<std::mutex> lock(timer_mutex_);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(max_duration_sec_);
            if (timer_cv_.wait_until(lock, deadline, [this]{ return stop_requested_.load(); })) break;
        }
        
        chunk_idx++;
        if (chunk_idx > max_chunks_) break;

        // Auto-split
        std::cout << "[" << getTimestamp() << "] [Session] " << cam_id_ << " -> Splitting to next chunk.\n";
        recorder_->stop();
        
        std::string new_path = filepath_;
        size_t dot = new_path.find_last_of('.');
        if (dot != std::string::npos) new_path.insert(dot, "_part" + std::to_string(chunk_idx));
        
        recorder_ = std::make_unique<VideoRecorder>(new_path);
        std::vector<SafePacket> dummy;
        recorder_->start(codec_params_.get(), dummy);
        
        executeScriptAsync(on_save_, dummy_evt, new_path);
    }

    if (recorder_) recorder_->stop();
    executeScriptAsync(on_stop_, dummy_evt, filepath_);
    executeScriptAsync(on_save_, dummy_evt, filepath_);
    
    finished_.store(true);
    if (on_finished_) on_finished_(cam_id_);
}

void RecordingSession::executeScriptAsync(const std::string& tmpl, const AlarmEvent& evt, const std::string& path) {
    if (tmpl.empty()) return;
    
    std::string cmd = tmpl;
    auto replace = [&](const std::string& key, const std::string& value) {
        size_t pos = 0;
        while ((pos = cmd.find(key, pos)) != std::string::npos) {
            cmd.replace(pos, key.length(), value);
            pos += value.length();
        }
    };
    
    replace("%t", cam_id_);
    replace("%e", evt.event_type);
    replace("%f", path);
    replace("%{json_addr}", evt.address);
    replace("%{json_chan}", std::to_string(evt.channel));
    replace("%{json_desc}", evt.description);
    replace("%{json_event}", evt.event_type);
    replace("%{json_serialid}", evt.serial_id);
    replace("%{json_starttime}", evt.start_time);
    replace("%{json_status}", evt.status);
    replace("%{json_alarm}", evt.alarm_type);
    
    std::cout << "[" << getTimestamp() << "] [Session] Executing async: " << cmd << "\n";
    ScriptRunner::run(cmd);
}