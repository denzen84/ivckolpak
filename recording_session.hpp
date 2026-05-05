#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <memory>
#include "utils.hpp"
#include "event.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoRecorder;

class RecordingSession {
public:
    using OnFinishedCallback = std::function<void(const std::string& cam_id)>;
    
    RecordingSession(const std::string& cam_id, const std::string& cam_name, const AlarmEvent& evt,
                     AVCodecParameters* codec_params, const std::string& filepath,
                     int max_duration_sec, int max_chunks, int post_buffer_iframes,
                     const std::string& on_start_script, const std::string& on_stop_script,
                     const std::string& on_save_script, OnFinishedCallback on_finished);
    ~RecordingSession();

    void start(std::vector<SafePacket> prebuffer);
    void requestStop();
    void updateEventStatus(const std::string& new_status);
    bool isFinished() const { return finished_.load(); }
    void pushPacket(const SafePacket& pkt);
    const std::string& getCamId() const { return cam_id_; }

private:
    void runLoop();
    void executeScriptAsync(const std::string& tmpl, const std::string& filepath);
    bool isIFrame(const AVPacket* pkt) const;

    std::string cam_id_;
    std::string cam_name_;
    AlarmEvent event_data_;
    std::unique_ptr<AVCodecParameters> codec_params_;
    std::string filepath_;
    int max_duration_sec_;
    int max_chunks_;
    int post_buffer_iframes_;
    std::string on_start_, on_stop_, on_save_;
    OnFinishedCallback on_finished_;

    std::unique_ptr<VideoRecorder> recorder_;
    std::thread worker_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> post_buffer_active_{false};
    std::atomic<int> post_buffer_remaining_{0};
    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
};