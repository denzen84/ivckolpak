#pragma once
#include <string>
#include <deque>
#include <vector>
#include <mutex>
#include <atomic>
#include <optional>
#include <functional>
#include <memory>
#include <chrono>
#include "config.h"
#include "alarm_event.hpp"
#include "logger.hpp"
#include "rtsp_stream.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
}

class RecordingSession {
public:

    enum class State { IDLE, PRE_BUFFER, RECORDING, FINALIZING, POST_BUFFER };
    
    struct Config {
        std::shared_ptr<const CameraConfig> camera_cfg;
        int pre_buffer_iframes;
        int post_buffer_iframes;
        std::function<void(const std::string&)> on_video_save;
    };
    struct Stats {
        State state;
        int buffer_iframes;
        uint64_t buffer_kb;
        uint64_t total_pushed_frames;
        uint64_t total_written_kb;
        double detected_fps;
        std::string current_file;
    };
    explicit RecordingSession(Config cfg);
    ~RecordingSession();
    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;
    void push_packet(AVPacket* pkt);
    void start_event(const AlarmEvent& evt,
                     const AVCodecParameters* codec_params,
                     const RTSPStream::StreamInfo& stream_info);
    void continue_event(const AlarmEvent& evt);
    void stop_event();
    void force_stop();
    void shutdown();
    [[nodiscard]] State current_state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] Stats get_stats() const;
private:
    struct Packet {
        AVPacket* pkt;
        bool is_keyframe;
        int64_t pts;
        size_t size;
        ~Packet() { if (pkt) av_packet_free(&pkt); }
        Packet(AVPacket* p) : pkt(p),
                              is_keyframe(p && (p->flags & AV_PKT_FLAG_KEY)),
                              pts(p ? p->pts : AV_NOPTS_VALUE),
                              size(p ? p->size : 0) {}
        Packet(Packet&& o) noexcept : pkt(o.pkt), is_keyframe(o.is_keyframe), pts(o.pts), size(o.size) { o.pkt = nullptr; }
        Packet& operator=(Packet&& o) noexcept {
            if (this != &o) {
                if (pkt) av_packet_free(&pkt);
                pkt = o.pkt; is_keyframe = o.is_keyframe; pts = o.pts; size = o.size;
                o.pkt = nullptr;
            }
            return *this;
        }
    };
    struct StreamAnalyzer {
        int64_t first_pts = AV_NOPTS_VALUE;
        int64_t last_pts = AV_NOPTS_VALUE;
        int64_t total_frames = 0;
        std::vector<int64_t> pts_diffs;
        AVRational detected_fps{0, 1};
        AVRational time_base{1, 90000};
        void analyze_packet(const AVPacket* pkt);
        void finalize();
        double get_fps() const;
    };
    void add_to_pre_buffer(Packet&& pkt);
    void drop_oldest_gop_from_pre_buffer();
    bool add_to_post_buffer(Packet&& pkt);
    void init_muxer(const AVCodecParameters* codec_params);
    void write_packet_to_file(AVPacket* pkt);
    void finalize_and_rotate();
    bool finalize_file_io(const std::string& path);
    void emergency_clear_pre_buffer(uint64_t target_bytes);  
    void emergency_clear_pre_buffer_by_count(size_t target_count); 
    Config cfg_;
    
    std::atomic<State> state_{State::PRE_BUFFER};
    
    mutable std::mutex buffer_mtx_;
    std::deque<Packet> pre_buffer_;
    std::deque<Packet> post_buffer_;
    int pre_buffer_iframe_count_ = 0;
    int post_buffer_iframe_count_ = 0;
    std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext*)>> out_fmt_ctx_;
    AVStream* out_stream_ = nullptr;
    std::string current_filename_;
    int current_event_id_ = 0;
    std::optional<AlarmEvent> current_event_;
    StreamAnalyzer analyzer_;
    std::unique_ptr<AVCodecParameters, std::function<void(AVCodecParameters*)>> stored_codec_params_;
    AVRational stored_time_base_{1, 90000};
    AVRational stored_frame_rate_{25, 1};
    int chunk_idx_ = 0;
    uint64_t chunk_bytes_ = 0;
    int64_t chunk_start_pts_ = AV_NOPTS_VALUE;
    std::chrono::steady_clock::time_point event_start_time_;
    uint64_t local_pushed_frames_ = 0;
    uint64_t local_written_bytes_ = 0;
    std::atomic<uint64_t> total_pushed_frames_{0};
    std::atomic<uint64_t> total_written_bytes_{0};
    uint64_t current_buffer_bytes_ = 0;
    std::atomic<bool> shutdown_requested_{false};
};
