#pragma once
#include <string>

struct AlarmEvent {
    std::string camera_id;      // Used for config lookup (mapped from serial_id)
    std::string status;         // "Start" or "Stop"
    std::string event_type;     // "MotionDetect", "HumanDetect", etc.
    int channel = 0;
    std::string serial_id;      // Original SerialID from JSON
    std::string address;        // Hex IP address from JSON
    
    // Additional fields for macro expansion
    std::string description;    // Descrip field from JSON
    std::string start_time;     // StartTime field from JSON
    std::string alarm_type;     // Type field from JSON (Alarm/Log/etc)
};