#include "alarm_queue.hpp"
#include "logger.hpp"

void AlarmQueue::push(AlarmEvent evt) {
    {
        std::lock_guard lock(mtx_);
        queue_.push(std::move(evt));
    }
    cv_.notify_one();
}

std::optional<AlarmEvent> AlarmQueue::pop() {
    std::unique_lock lock(mtx_);
    cv_.wait(lock, [this] { return !queue_.empty() || is_shutdown_; });
    if (queue_.empty()) return std::nullopt;
    AlarmEvent evt = std::move(queue_.front());
    queue_.pop();
    return evt;
}

bool AlarmQueue::try_pop(AlarmEvent& out, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mtx_);
    bool result = cv_.wait_for(lock, timeout, [this] {
        return !queue_.empty() || is_shutdown_;
    });
    if (!result || queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    return true;
}

void AlarmQueue::shutdown() {
    {
        std::lock_guard lock(mtx_);
        if (is_shutdown_) return;
        is_shutdown_ = true;
    }
    LOG_INFO("QUEUE", "Shutdown requested. Waking consumers.");
    cv_.notify_all();
}