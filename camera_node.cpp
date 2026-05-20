#include "camera_node.hpp"
#include "utils.hpp"
#include "packet_pool.hpp"

CameraNode::CameraNode(std::shared_ptr<const CameraConfig> cfg, int pre, int post)
    : cfg_(cfg),
      stream_(RTSPStream::Config{
          .url=cfg->rtsp_url,
          .timeout_ms=2000,
          .tcp_only=true,
          .reconnect_delay_ms=cfg->reconnect_delay_ms>0?cfg->reconnect_delay_ms:3000,
          .reconnect_max_delay_ms=cfg->reconnect_max_delay_ms>0?cfg->reconnect_max_delay_ms:30000,
          .read_timeout_ms=10000,
          .on_rtsp_lost=cfg->on_rtsp_lost,
          .on_rtsp_found=cfg->on_rtsp_found,
          .camera_config=cfg
      }) {
    RecordingSession::Config session_cfg{
        .camera_cfg = cfg,
        .pre_buffer_iframes = pre,
        .post_buffer_iframes = post,
        .on_video_save = [cfg](const std::string& path) {
            if (!cfg->on_video_save.empty()) {
                LOG_DEBUG("NODE", "Calling on_video_save for: ", path);
                execute_script_async(cfg->on_video_save, *cfg, nullptr, path);
            }
        }
    };
    session_ = std::make_unique<RecordingSession>(std::move(session_cfg));
}

CameraNode::~CameraNode() { stop(); }

bool CameraNode::initialize() {
    if (initialized_.load(std::memory_order_acquire)) return true;
    initialized_.store(true, std::memory_order_release);
    return true;
}

void CameraNode::start() {
    if (!initialized_.load(std::memory_order_acquire)) return;
    stream_.start([this](AVPacket* p){ on_packet(p); });
}

void CameraNode::stop() {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) return;
    stream_.stop();
    session_->shutdown();
}

void CameraNode::on_alarm(const AlarmEvent& evt) {
    if (!initialized_.load(std::memory_order_acquire)) return;
    
    if (evt.isStop()) {
        LOG_INFO("NODE", "ALARM STOP: ", evt.event_type, " [", cfg_->name, "]");
        if (!cfg_->on_event_stop.empty()) {
            execute_script_async(cfg_->on_event_stop, *cfg_, &evt);
        }
        session_->stop_event();
        return;
    }
    
    if (!stream_.is_alive()) {
        LOG_WARN("NODE", "Ignoring ALARM START - stream not alive [", cfg_->name, "]");
        return;
    }
    
    if (evt.isStart()) {
        auto current_state = session_->current_state();
        
        if (current_state == RecordingSession::State::RECORDING || 
            current_state == RecordingSession::State::POST_BUFFER) {
            LOG_INFO("NODE", "ALARM START during active recording: ", evt.event_type, 
                     " [", cfg_->name, "] - CONTINUING event");
            if (!cfg_->on_event_start.empty()) {
                execute_script_async(cfg_->on_event_start, *cfg_, &evt);
            }
            session_->continue_event(evt);
        } else {
            int current_event_id = event_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_INFO("NODE", "ALARM START #", current_event_id, ": ", evt.event_type, 
                     " [", cfg_->name, "]");
            if (!cfg_->on_event_start.empty()) {
                execute_script_async(cfg_->on_event_start, *cfg_, &evt);
            }
            auto codec_params = stream_.get_codec_params();
            auto stream_info = stream_.get_stream_info();
            if (codec_params && stream_info.valid) {
                session_->start_event(evt, codec_params.get(), stream_info);
            } else {
                LOG_ERROR("NODE", "Cannot start recording: missing codec params or stream info");
            }
        }
    }
}

bool CameraNode::is_alive() const { return stream_.is_alive(); }
const std::string& CameraNode::name() const { return cfg_->name; }
const std::string& CameraNode::serial_id() const { return cfg_->serialid; }

void CameraNode::collect_stats(std::function<void(const CameraStats&)> consumer) const {
    auto session_stats = session_->get_stats();
    SessionState state;
    switch (session_stats.state) {
        case RecordingSession::State::IDLE: state = SessionState::IDLE; break;
        case RecordingSession::State::PRE_BUFFER: state = SessionState::PRE_BUFFER; break;
        case RecordingSession::State::RECORDING: state = SessionState::RECORDING; break;
        case RecordingSession::State::POST_BUFFER: state = SessionState::POST_BUFFER; break;
        default: state = SessionState::IDLE;
    }
    double fps = session_stats.detected_fps;
    if (fps == 0.0) {
        auto stream_info = stream_.get_stream_info();
        if (stream_info.valid && stream_info.frame_rate.num > 0) {
            fps = av_q2d(stream_info.frame_rate);
        }
    }
    CameraStats stats{
        .name = cfg_->name,
        .serial_id = cfg_->serialid,
        .state = state,
        .buffer_iframes = session_stats.buffer_iframes,
        .buffer_kb = session_stats.buffer_kb,
        .total_pushed_frames = session_stats.total_pushed_frames,
        .total_written_kb = session_stats.total_written_kb,
        .detected_fps = fps,
        .rtsp_connected = stream_.is_alive()
    };
    consumer(stats);
}

void CameraNode::on_packet(AVPacket* pkt) { session_->push_packet(pkt); }
