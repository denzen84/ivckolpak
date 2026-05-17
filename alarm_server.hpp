#pragma once
#include "alarm_event.hpp"
#include <thread>
#include <stop_token>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <string>

class AlarmServer {
public:
    using AlarmCallback = std::function<void(AlarmEvent)>;
    explicit AlarmServer(int port);
    ~AlarmServer();
    AlarmServer(const AlarmServer&) = delete;
    AlarmServer& operator=(const AlarmServer&) = delete;
    [[nodiscard]] bool start();
    void stop();
    void setCallback(AlarmCallback cb);
private:
    void runListener(std::stop_token st);
    void runClient(int fd, std::stop_token st);
    void processBuffer(std::string& buf);
    static size_t findJsonEnd(std::string_view data);
    int port_;
    AlarmCallback callback_;
    std::atomic<int> server_fd_{-1};
    std::jthread listener_thread_;
    mutable std::mutex clients_mtx_;
    std::vector<std::jthread> client_threads_;
};