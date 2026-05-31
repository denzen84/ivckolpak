#include "alarm_server.hpp"
#include "logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

extern "C" {
#include "json.h"
}

AlarmServer::AlarmServer(int port) : port_(port) {}
AlarmServer::~AlarmServer() { stop(); }

void AlarmServer::setCallback(AlarmCallback cb) { callback_ = std::move(cb); }

[[nodiscard]] bool AlarmServer::start() {
    if (listener_thread_.joinable()) return false;
    LOG_INFO("ALARM", "Starting TCP server on port ", port_);
    listener_thread_ = std::jthread([this](std::stop_token s) { runListener(s); });
    return true;
}

void AlarmServer::stop() {
    if (!listener_thread_.joinable()) return;
    LOG_INFO("ALARM", "Stopping server...");
    listener_thread_.request_stop();
    if (int fd = server_fd_.load(); fd >= 0) shutdown(fd, SHUT_RDWR);
    listener_thread_.join();
    std::lock_guard lock(clients_mtx_);
    for (auto& t : client_threads_) t.request_stop();
    client_threads_.clear();
    LOG_INFO("ALARM", "Server stopped cleanly.");
}

void AlarmServer::runListener(std::stop_token st) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG_ERROR("ALARM", "socket() failed: ", strerror(errno)); return; }
    server_fd_.store(fd);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(fd, 10) < 0) {
        LOG_ERROR("ALARM", "bind/listen failed: ", strerror(errno));
        close(fd); server_fd_.store(-1); return;
    }
    LOG_INFO("ALARM", "Listening for connections...");
    while (!st.stop_requested()) {
        sockaddr_in ca{};
        socklen_t al = sizeof(ca);
        int cfd = accept(fd, reinterpret_cast<sockaddr*>(&ca), &al);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (st.stop_requested()) break;
            LOG_ERROR("ALARM", "accept() failed: ", strerror(errno));
            continue;
        }
        LOG_DEBUG("ALARM", "New client connected. FD: ", cfd);
        std::jthread client([this, cfd, st] { runClient(cfd, st); });
        {
            std::lock_guard lock(clients_mtx_);
            client_threads_.push_back(std::move(client));
            std::erase_if(client_threads_, [](const std::jthread& t){ return !t.joinable(); });
        }
    }
    close(fd);
    server_fd_.store(-1);
}

void AlarmServer::runClient(int client_fd, std::stop_token st) {
    struct timeval tv{0, 200000};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string buffer;
    buffer.reserve(4096);
    char tmp[4096];
    while (!st.stop_requested()) {
        ssize_t n = recv(client_fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buffer.append(tmp, n);
            if (callback_) processBuffer(buffer);
        } else if (n == 0) {
            LOG_DEBUG("ALARM", "Client ", client_fd, " disconnected.");
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_WARN("ALARM", "Client ", client_fd, " recv error: ", strerror(errno));
            break;
        }
    }
    close(client_fd);
}

void AlarmServer::processBuffer(std::string& buffer) {
    constexpr size_t MAX_BUF = 32768;
    while (!buffer.empty()) {
        size_t start = buffer.find('{');
        if (start == std::string::npos) {
            if (buffer.size() > MAX_BUF) {
                LOG_WARN("ALARM", "Buffer overflow, clearing.");
                buffer.clear();
            }
            return;
        }
        if (start > 0) buffer.erase(0, start);
        size_t end = findJsonEnd(buffer);
        if (end == 0) return;
        std::string json_str = buffer.substr(0, end);
        buffer.erase(0, end);
        
        json_value_s* root = json_parse(json_str.c_str(), json_str.length());
        if (!root) {
            LOG_WARN("ALARM", "JSON parse error");
            continue;
        }
        
        json_object_s* obj = json_value_as_object(root);
        if (obj) {
            AlarmEvent evt;
            
            for (json_object_element_s* elem = obj->start; elem; elem = elem->next) {
                std::string key(elem->name->string, elem->name->string_size);
                json_string_s* str_val = json_value_as_string(elem->value);
                json_number_s* num_val = json_value_as_number(elem->value);
                
                if (key == "Type" && str_val) {
                    evt.type.assign(str_val->string, str_val->string_size);
                } else if (key == "Address" && str_val) {
                    evt.address.assign(str_val->string, str_val->string_size);
                } else if (key == "Channel" && num_val) {
                    evt.channel = std::stoi(std::string(num_val->number, num_val->number_size));
                } else if (key == "Descrip" && str_val) {
                    evt.description.assign(str_val->string, str_val->string_size);
                } else if (key == "Event" && str_val) {
                    evt.event_type.assign(str_val->string, str_val->string_size);
                } else if (key == "SerialID" && str_val) {
                    evt.serial_id.assign(str_val->string, str_val->string_size);
                } else if (key == "StartTime" && str_val) {
                    evt.start_time.assign(str_val->string, str_val->string_size);
                } else if (key == "Status" && str_val) {
                    evt.status.assign(str_val->string, str_val->string_size);
                }
            }
            
            if (evt.type == "Alarm") {
                if (!evt.address.empty() && evt.address.length() >= 10) {
                    try {
                        uint32_t v_le = std::stoul(evt.address.substr(2), nullptr, 16);
                        uint32_t v_be = ((v_le & 0xFF) << 24) | 
                                       ((v_le & 0xFF00) << 8) | 
                                       ((v_le & 0xFF0000) >> 8) | 
                                       ((v_le & 0xFF000000) >> 24);
                        std::ostringstream oss;
                        oss << "0x" << std::hex << std::uppercase 
                            << std::setw(8) << std::setfill('0') << v_be;
                        evt.address = oss.str();
                        
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                                     (v_be >> 24) & 0xFF,
                                     (v_be >> 16) & 0xFF,
                                     (v_be >> 8) & 0xFF,
                                     v_be & 0xFF);
                        evt.address_ip = buf;
                        LOG_DEBUG("ALARM", "Converted LE address to BE: ", evt.address, " -> ", evt.address_ip);
                    } catch (...) {
                        LOG_WARN("ALARM", "Failed to convert address: ", evt.address);
                    }
                }
                
                LOG_DEBUG("ALARM", "Parsed: Type=", evt.type, " | Status=", evt.status, 
                          " | Serial=", evt.serial_id, " | IP=", evt.address_ip);
                callback_(std::move(evt));
            }
        }
        
        free(root);
    }
}

size_t AlarmServer::findJsonEnd(std::string_view data) {
    int depth = 0; bool in_str = false, esc = false;
    for (size_t i = 0; i < data.size(); ++i) {
        char c = data[i];
        if (esc) { esc = false; continue; }
        if (c == '\\' && in_str) { esc = true; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (!in_str) {
            if (c == '{') ++depth;
            else if (c == '}') { if (--depth == 0) return i + 1; }
        }
    }
    return 0;
}
