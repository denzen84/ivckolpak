#pragma once
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <cstdlib>
#include "logger.hpp"

class ScriptExecutor {
public:
    static ScriptExecutor& instance() {
        static ScriptExecutor executor(4);
        return executor;
    }
    void submit(std::string cmd) {
        {
            std::lock_guard lock(mtx_);
            tasks_.push(std::move(cmd));
        }
        cv_.notify_one();
    }
    ~ScriptExecutor() {
        stop_.store(true, std::memory_order_release);
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }
private:
    explicit ScriptExecutor(size_t num_workers) {
        for (size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        LOG_INFO("SCRIPT_POOL", "Initialized with ", num_workers, " workers");
    }
    void worker_loop() {
        while (!stop_.load(std::memory_order_acquire)) {
            std::string cmd;
            {
                std::unique_lock lock(mtx_);
                cv_.wait(lock, [this] {
                    return !tasks_.empty() || stop_.load(std::memory_order_acquire);
                });
                if (tasks_.empty()) continue;
                cmd = std::move(tasks_.front());
                tasks_.pop();
            }
            int ret = std::system(cmd.c_str());
            if (ret != 0) {
                LOG_ERROR("SCRIPT", "Failed (exit code ", ret, "): ", cmd);
            }
        }
    }
    std::queue<std::string> tasks_;
    std::vector<std::thread> workers_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};