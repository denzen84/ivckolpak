#include "camera_node.hpp"
#include "utils.hpp"
#include <iostream>
#include <algorithm>

CameraNode::CameraNode(const std::string& id, const std::string& url, const std::string& serial, 
                       const std::string& ip, int bf)
    : id_(id), serial_id_(serial), ip_address_(ip), stream_(url, bf) {
    
    stream_.setOnPacket([this](const SafePacket& pkt) {
        if (!pkt.get()) return;
        
        std::lock_guard<std::mutex> l(sessions_mtx_);
        for (auto& s : sessions_) {
            s->pushPacket(pkt);
        }
        // Cleanup finished sessions on every packet (also done periodically)
        sessions_.erase(
            std::remove_if(sessions_.begin(), sessions_.end(),
                [](const auto& s){ return s->isFinished(); }),
            sessions_.end()
        );
    });
    
    stream_.setOnStatusChanged([this](bool connected) {
        DEBUG_LOG("CameraNode: RTSP status changed for " << id_ << ", connected=" << connected);
        if (on_rtsp_status_) {
            on_rtsp_status_(connected);
        }
    });
}

void CameraNode::start() { 
    std::cout << "[" << getTimestamp() << "] [Node] " << id_ << " -> Starting\n"; 
    stream_.start(); 
}

void CameraNode::stop() { 
    std::cout << "[" << getTimestamp() << "] [Node] " << id_ << " -> Stopping\n"; 
    stream_.stop(); 
    stream_.join(); 
}

void CameraNode::addSession(std::shared_ptr<RecordingSession> s) { 
    DEBUG_LOG("CameraNode::addSession called for " << id_);
    std::lock_guard<std::mutex> l(sessions_mtx_); 
    sessions_.push_back(s);
    DEBUG_LOG("Session added, count=" << sessions_.size());
}

void CameraNode::cleanupFinishedSessions() {
    std::lock_guard<std::mutex> l(sessions_mtx_);
    auto before = sessions_.size();
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
            [](const auto& s){ return s->isFinished(); }),
        sessions_.end()
    );
    if (sessions_.size() < before) {
        DEBUG_LOG("CameraNode::cleanupFinishedSessions: removed " 
                  << (before - sessions_.size()) << " finished sessions");
    }
}

std::vector<SafePacket> CameraNode::getPreBuffer() const { 
    return stream_.getBufferSafe(); 
}

std::unique_ptr<AVCodecParameters> CameraNode::getCodecParamsCopy() const {
    return stream_.getCodecParamsCopy();
}