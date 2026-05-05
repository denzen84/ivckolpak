#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include "utils.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h> 
}

class VideoRecorder {
public:
    explicit VideoRecorder(const std::string& output_path);
    ~VideoRecorder();
    
    bool start(AVCodecParameters* params, std::vector<SafePacket>& prebuffer);
    void push(SafePacket pkt);
    bool stop();
    bool isRecording() const { return is_recording_.load(); }

private:
    void writeLoop(); 
    bool writeHeader(); 
    bool writeTrailer(); 
    void rebasePacket(AVPacket* pkt);
    
    std::string output_path_, temp_path_;
    AVFormatContext* out_fmt_ctx_ = nullptr;  
    int out_video_stream_idx_ = -1;
    std::deque<SafePacket> write_queue_;
    mutable std::mutex queue_mutex_; 
    std::condition_variable queue_cv_;
    std::condition_variable empty_cv_;
    std::thread writer_thread_;
    std::atomic<bool> is_recording_{false}, should_stop_{false};
    int64_t next_pts_ = 0, out_frame_duration_ = 50000;
    AVRational out_time_base_rec_{};
    int64_t last_iframe_pts_ = -1, packet_count_since_iframe_ = 0;
    
    static constexpr size_t MAX_QUEUE_SIZE = 500;
};