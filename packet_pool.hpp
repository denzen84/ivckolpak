#pragma once
#include <vector>
#include <mutex>
#include <memory>
#include "logger.hpp"

extern "C" {
#include <libavcodec/packet.h>
}

class PacketPool {
public:
    static PacketPool& instance() {
        static PacketPool pool(300);
        return pool;
    }
    AVPacket* acquire() {
        std::lock_guard lock(mtx_);
        if (free_list_.empty()) {
            ++allocations_;
            return av_packet_alloc();
        }
        AVPacket* pkt = free_list_.back();
        free_list_.pop_back();
        ++reuses_;
        return pkt;
    }
    void release(AVPacket* pkt) {
        if (!pkt) return;
        av_packet_unref(pkt);
        std::lock_guard lock(mtx_);
        if (free_list_.size() < capacity_) {
            free_list_.push_back(pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
    ~PacketPool() {
        for (auto* pkt : free_list_) {
            av_packet_free(&pkt);
        }
        LOG_INFO("PACKET_POOL", "Stats: allocations=", allocations_, " reuses=", reuses_);
    }
private:
    explicit PacketPool(size_t capacity) : capacity_(capacity) {
        free_list_.reserve(capacity);
    }
    std::vector<AVPacket*> free_list_;
    std::mutex mtx_;
    size_t capacity_;
    uint64_t allocations_ = 0;
    uint64_t reuses_ = 0;
};

struct PacketPoolDeleter {
    void operator()(AVPacket* p) const {
        PacketPool::instance().release(p);
    }
};
using PooledPacketPtr = std::unique_ptr<AVPacket, PacketPoolDeleter>;