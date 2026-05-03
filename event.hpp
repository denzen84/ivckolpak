#pragma once
#include <string>

struct AlarmEvent {
    std::string camera_id;
    std::string status;           // "Start" or "Stop"
    std::string event_type;       // "MotionDetect", "HumanDetect", etc.
    int channel = 0;
    std::string serial_id;
    std::string address;
    
    std::string description;      // Descrip field
    std::string start_time;       // StartTime field
    std::string alarm_type;       // Type field (Alarm/Log/etc)
};