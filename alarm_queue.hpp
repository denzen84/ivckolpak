#pragma once
#include "alarm_event.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

class AlarmQueue {
public:
    void push(AlarmEvent evt);
    std::optional<AlarmEvent> pop();
    bool try_pop(AlarmEvent& out, std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    void shutdown();
private:
    std::queue<AlarmEvent> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool is_shutdown_ = false;
};