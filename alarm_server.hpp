#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <nlohmann/json.hpp>
#include "event.hpp"

class AlarmServer {
public:
    explicit AlarmServer(int port);
    ~AlarmServer();

    void start();
    void stop();
    bool popEvent(AlarmEvent& evt);
    void wakeUp();  

private:
    void run();
    void processBuffer();
    int port_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    int server_fd_ = -1;
    std::string recv_buffer_;

    std::queue<AlarmEvent> event_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
};