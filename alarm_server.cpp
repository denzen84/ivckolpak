#include "alarm_server.hpp"
#include "utils.hpp"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

AlarmServer::AlarmServer(int port) : port_(port) {
    DEBUG_LOG("AlarmServer ctor: port=" << port_);
}

AlarmServer::~AlarmServer() { 
    DEBUG_LOG("AlarmServer dtor");
    stop(); 
}

void AlarmServer::start() {
    DEBUG_LOG("AlarmServer::start() called");
    if (running_.load()) return;
    running_.store(true);
    server_thread_ = std::thread(&AlarmServer::run, this);
    DEBUG_LOG("Server thread started");
}

void AlarmServer::stop() {
    DEBUG_LOG("AlarmServer::stop() called");
    if (!running_.load()) return;
    running_.store(false);
    cv_.notify_all();

    if (server_fd_ >= 0) {
        DEBUG_LOG("Waking up accept() with dummy connection...");
        int dummy = socket(AF_INET, SOCK_STREAM, 0);
        if (dummy >= 0) {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port_);
            connect(dummy, (struct sockaddr*)&addr, sizeof(addr));
            close(dummy);
        }
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
        DEBUG_LOG("Server socket closed");
    }
    if (server_thread_.joinable()) {
        DEBUG_LOG("Joining server thread...");
        server_thread_.join();
        DEBUG_LOG("Server thread joined");
    }
    DEBUG_LOG("AlarmServer::stop() done");
}

bool AlarmServer::popEvent(AlarmEvent& evt) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    bool notified = cv_.wait_for(lock, std::chrono::milliseconds(200), [this]{ 
        return !event_queue_.empty() || !running_.load(); 
    });
    
    if (!event_queue_.empty()) {
        DEBUG_LOG("popEvent: event available (queue_size=" << event_queue_.size() << ")");
        evt = event_queue_.front();
        event_queue_.pop();
        DEBUG_LOG("popEvent: returning event {camera_id='" << evt.camera_id << "', status='" << evt.status << "'}");
        return true;
    }
    
    if (!running_.load()) {
        DEBUG_LOG("popEvent: shutting down");
        return false;
    }
    
    if (notified) {
        DEBUG_LOG("popEvent: spurious wakeup");
    }
    
    return false;
}

void AlarmServer::wakeUp() {
    DEBUG_LOG("AlarmServer::wakeUp() called");
    cv_.notify_all();
}

void AlarmServer::run() {
    DEBUG_LOG("AlarmServer::run() thread started");
    
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        DEBUG_LOG("ERROR: socket() failed");
        return;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(server_fd_, 5) < 0) {
        close(server_fd_);
        return;
    }
    DEBUG_LOG("Listening on port " << port_);

    while (running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &addrlen);
        
        if (client_fd < 0) {
            if (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char buf[4096];
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        
        if (n > 0) {
            buf[n] = '\0';
            DEBUG_LOG("Received " << n << " bytes");
            recv_buffer_ += std::string(buf, n);
            processBuffer();
        } else if (n == 0) {
            DEBUG_LOG("Client disconnected");
        }
        
        close(client_fd);
    }
    close(server_fd_);
    DEBUG_LOG("AlarmServer::run() thread exiting");
}

void AlarmServer::processBuffer() {
    DEBUG_LOG("processBuffer() called, buffer size=" << recv_buffer_.size());
    
    constexpr size_t BINARY_PREFIX_SIZE = 20;
    
    while (recv_buffer_.size() >= BINARY_PREFIX_SIZE) {
        std::string_view json_part(recv_buffer_.data() + BINARY_PREFIX_SIZE, 
                                   recv_buffer_.size() - BINARY_PREFIX_SIZE);
        
        size_t json_start = json_part.find('{');
        if (json_start == std::string_view::npos) {
            recv_buffer_.clear();
            return;
        }
        json_part.remove_prefix(json_start);
        
        try {
            int depth = 0;
            size_t json_end = 0;
            bool in_string = false, escape = false;
            
            for (; json_end < json_part.size(); ++json_end) {
                char c = json_part[json_end];
                if (escape) { escape = false; continue; }
                if (c == '\\' && in_string) { escape = true; continue; }
                if (c == '"') { in_string = !in_string; continue; }
                if (!in_string) {
                    if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        if (depth == 0) { json_end++; break; }
                    }
                }
            }
            
            if (depth != 0) return;
            
            std::string json_str(json_part.data(), json_end);
            DEBUG_LOG("Extracted JSON: " << json_str);
            
            auto j = nlohmann::json::parse(json_str);
            std::string type = j.value("Type", "");
            
            if (type != "Alarm") {
                DEBUG_LOG("Ignoring non-Alarm event (Type='" << type << "')");
            } else {
                AlarmEvent evt;
                evt.status = j.value("Status", "");
                evt.event_type = j.value("Event", "");
                evt.channel = j.value("Channel", 0);
                evt.serial_id = j.value("SerialID", "");
                evt.address = j.value("Address", "");
                evt.camera_id = evt.serial_id;  // 🔧 Critical: map SerialID to camera_id
                
                // 🔧 New fields from JSON for macros
                evt.description = j.value("Descrip", "");
                evt.start_time = j.value("StartTime", "");
                evt.alarm_type = j.value("Type", "");
                
                DEBUG_LOG("Created AlarmEvent: camera_id='" << evt.camera_id << "', status='" << evt.status << "'");
                
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    event_queue_.push(evt);
                    DEBUG_LOG("Event pushed to queue");
                }
                cv_.notify_one();
            }
            
            size_t consumed = BINARY_PREFIX_SIZE + json_start + json_end;
            recv_buffer_.erase(0, consumed);
            
        } catch (const nlohmann::json::parse_error& e) {
            DEBUG_LOG("JSON parse error: " << e.what());
            if (!recv_buffer_.empty()) recv_buffer_.erase(0, 1);
        } catch (const std::exception& e) {
            DEBUG_LOG("Exception in processBuffer: " << e.what());
            recv_buffer_.clear();
            return;
        }
    }
}