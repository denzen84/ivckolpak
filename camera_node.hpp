#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>
#include "utils.hpp"
#include "rtsp_camera_stream.hpp"
#include "recording_session.hpp"

class CameraNode {
public:
    using OnRtspStatusCallback = std::function<void(bool connected)>;
    
    CameraNode(const std::string& id, const std::string& url, const std::string& serial, 
               const std::string& ip, int buffer_frames);
    void start(); 
    void stop();
    
    void addSession(std::shared_ptr<RecordingSession> s);
    void setOnRtspStatusChanged(OnRtspStatusCallback cb) { on_rtsp_status_ = std::move(cb); } 
    
    std::vector<SafePacket> getPreBuffer() const;
    std::unique_ptr<AVCodecParameters> getCodecParamsCopy() const;
    
    const std::string& getSerialId() const { return serial_id_; }
    const std::string& getIpAddress() const { return ip_address_; }
    const std::string& getId() const { return id_; }

private:
    std::string id_, serial_id_, ip_address_;
    RtspCameraStream stream_;
    std::vector<std::shared_ptr<RecordingSession>> sessions_;
    mutable std::mutex sessions_mtx_;
    OnRtspStatusCallback on_rtsp_status_;  
};