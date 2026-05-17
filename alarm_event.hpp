#pragma once
#include <string>
#include <algorithm>
#include <cctype>

struct AlarmEvent {
    std::string address;
    int channel = 0;
    std::string description;
    std::string event_type;
    std::string serial_id;
    std::string start_time;
    std::string status;
    std::string type;
    std::string address_ip;
    std::string camera_id;
    std::string alarm_type;
    int event_id = 0;
    [[nodiscard]] bool isStart() const noexcept {
        std::string s = status; std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s == "start" || s == "1" || s == "on";
    }
    [[nodiscard]] bool isStop() const noexcept {
        std::string s = status; std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s == "stop" || s == "end" || s == "0" || s == "off";
    }
};