#pragma once
#include <iostream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <atomic>

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static void set_enabled(bool enabled) {
        logs_enabled_.store(enabled, std::memory_order_relaxed);
    }
    template<typename... Args>
    static void log(LogLevel level, std::string_view module, Args&&... args) {
        if (!logs_enabled_.load(std::memory_order_relaxed)) return;
#ifdef NDEBUG
        if (level == LogLevel::DEBUG) return;
#endif
        std::lock_guard lock(mtx_);
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm;
        localtime_r(&time_t_now, &tm);
        std::cerr << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
                  << '.' << std::setfill('0') << std::setw(3) << ms.count()
                  << " [" << level_str(level) << "][" << module << "] ";
        (std::cerr << ... << std::forward<Args>(args)) << '\n';
    }
private:
    static inline std::mutex mtx_;
    static inline std::atomic<bool> logs_enabled_{true};
    static constexpr std::string_view level_str(LogLevel l) noexcept {
        switch(l) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
        }
        return "UNKNOWN";
    }
};

#define LOG_DEBUG(mod, ...) Logger::log(LogLevel::DEBUG, mod, __VA_ARGS__)
#define LOG_INFO(mod, ...)  Logger::log(LogLevel::INFO,  mod, __VA_ARGS__)
#define LOG_WARN(mod, ...)  Logger::log(LogLevel::WARN,  mod, __VA_ARGS__)
#define LOG_ERROR(mod, ...) Logger::log(LogLevel::ERROR, mod, __VA_ARGS__)
