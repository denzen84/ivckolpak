#pragma once
#include <string>
#include <cstdint>

enum class SessionState { IDLE, PRE_BUFFER, RECORDING, FINALIZING, POST_BUFFER };

struct CameraStats {
    std::string name;
    std::string serial_id;
    SessionState state;
    int buffer_iframes;
    uint64_t buffer_kb;
    uint64_t total_pushed_frames;
    uint64_t total_written_kb;
    uint64_t total_read_bytes;
    double detected_fps;
    bool rtsp_connected;
    
    const char* state_str() const {
        switch (state) {
            case SessionState::IDLE: return "IDLE";
            case SessionState::PRE_BUFFER: return "WAIT";
            case SessionState::RECORDING: return "REC";
            case SessionState::FINALIZING: return "FIN"; 
            case SessionState::POST_BUFFER: return "POST";
        }
        return "UNKNOWN";
    }
};
