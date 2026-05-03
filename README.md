# ivckolpak — Event-Driven NVR with Keyframe-Accurate Buffering

## Overview

**ivckolpak** is a high-reliability Network Video Recorder (NVR) designed for IP cameras operating on an event-triggered model. The program continuously reads RTSP streams from cameras and saves video to disk only when external events occur (motion detection, alarm triggers, etc.). Recording is controlled via TCP commands in JSON format.

---

## Manual Building

### Debian / Ubuntu

```bash
# Install build dependencies
sudo apt install build-essential pkg-config cmake \
                 libavformat-dev libavcodec-dev libavutil-dev \
                 nlohmann-json3-dev

# Clone the repository
git clone https://github.com/denzen84/ivckolpak.git
cd ivckolpak

# Build
mkdir Build
cd Build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)
```

The compiled binary will be located at `Build/ivckolpak`.

### Other Linux Distributions

Ensure the following dependencies are installed:
- **CMake** ≥ 3.14
- **GCC** or **Clang** with C++17 support
- **FFmpeg** development libraries (`libavformat`, `libavcodec`, `libavutil`)
- **nlohmann/json** ≥ 3.10.5
- **POSIX Threads**

Then follow the same build steps as above.

---

## Macro System Reference

ivckolpak uses a unified macro engine (`MacroContext` + `applyMacros()`) for filename formatting and script command substitution. Macros are replaced at runtime with values from the current event context, camera configuration, or RTSP connection state.

### Supported Macros by Category

#### Identification Macros

| Macro | Substitutes | on_event_start | on_event_stop | on_video_save | on_rtsp_lost | on_rtsp_found | filename_format |
|-------|-------------|:---:|:---:|:---:|:---:|:---:|:---:|
| `%t` | Camera ID (`cam_id`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%$` | Camera name from config (`cfg.id`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

#### Event Data Macros (from JSON payload)

| Macro | Substitutes | JSON Field | on_event_start | on_event_stop | on_video_save | on_rtsp_lost | on_rtsp_found | filename_format |
|-------|-------------|------------|:---:|:---:|:---:|:---:|:---:|:---:|
| `%e` | Event type | `Event` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_addr}` | IP address or location | `Address` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_chan}` | Channel number | `Channel` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_desc}` | Human-readable description | `Descrip` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_event}` | Event type (same as `%e`) | `Event` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_serialid}` | Camera serial identifier | `SerialID` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_starttime}` | Event start timestamp | `StartTime` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_status}` | Recording status | `Status` | ✅ (`"Start"`) | ✅ (`"Stop"`) | ✅ | ❌ | ❌ | ✅ |
| `%{json_alarm}` | Alarm/Log type | `Type` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |

#### RTSP Connection Macros

| Macro | Substitutes | on_event_start | on_event_stop | on_video_save | on_rtsp_lost | on_rtsp_found | filename_format |
|-------|-------------|:---:|:---:|:---:|:---:|:---:|:---:|
| `%{status}` | Connection state | ❌ | ❌ | ❌ | ✅ (`"disconnected"`) | ✅ (`"connected"`) | ❌ |
| `%{event}` | RTSP event type | ❌ | ❌ | ❌ | ✅ (`"rtsp_lost"`) | ✅ (`"rtsp_found"`) | ❌ |

#### Date/Time Macros

| Macro | Substitutes | Example | All Events | filename_format |
|-------|-------------|---------|:----------:|:---:|
| `%Y` | Year (4 digits) | `2026` | ✅ | ✅ |
| `%m` | Month (2 digits) | `05` | ✅ | ✅ |
| `%d` | Day (2 digits) | `03` | ✅ | ✅ |
| `%H` | Hour (24-hour, 2 digits) | `14` | ✅ | ✅ |
| `%M` | Minute (2 digits) | `30` | ✅ | ✅ |
| `%S` | Second (2 digits) | `00` | ✅ | ✅ |
| `%T` | Full time (`%H:%M:%S`) | `14:30:00` | ✅ | ✅ |
| `%v` | Version/chunk number | `1` | ✅ | ✅ |

#### File Path Macros

| Macro | Substitutes | on_event_start | on_event_stop | on_video_save | on_rtsp_lost | on_rtsp_found | filename_format |
|-------|-------------|:---:|:---:|:---:|:---:|:---:|:---:|
| `%f` | Full file path | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%v` | Chunk version | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |

### Summary Table (All Macros × All Contexts)

| Macro | on_event_start | on_event_stop | on_video_save | on_rtsp_lost | on_rtsp_found | filename_format |
|-------|:---:|:---:|:---:|:---:|:---:|:---:|
| `%t` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%$` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%e` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%f` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%v` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%Y` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%m` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%d` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%H` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%M` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%S` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%T` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `%{json_addr}` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_chan}` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_desc}` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_event}` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_serialid}` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_starttime}` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_status}` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{json_alarm}` | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| `%{status}` | ❌ | ❌ | ❌ | ✅ | ✅ | ❌ |
| `%{event}` | ❌ | ❌ | ❌ | ✅ | ✅ | ❌ |

**Legend:** ✅ = macro is substituted with actual value | ❌ = macro is left as literal text (not applicable for this event)

---

## Configuration Parameters Reference

### `[alarmserver]` Section

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `port` | int | `15002` | TCP port for receiving alarm commands |
| `target_dir` | string | `/tmp` | Global recording directory (fallback) |
| `filename_format` | string | `rec_%t_%Y%m%d_%H%M%S.mp4` | Global filename format with macro support |

### `[camera]` Section (repeatable)

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `serialid` | string | `cam` + index | Camera unique identifier (must match JSON `SerialID`) |
| `description` | string | — | Human-readable camera description |
| `rtsp` | string | — | RTSP stream URL |
| `pre_buffer_iframes` | int | `3` | Number of I-frames in pre-buffer |
| `post_buffer_iframes` | int | `0` | Number of I-frames to capture after Stop command |
| `max_event_duration` | int | `30` | Maximum chunk duration (seconds) |
| `max_event_chunks` | int | `1` | Maximum number of chunks per event |
| `target_dir` | string | from `[alarmserver]` | Camera-specific recording directory |
| `filename_format` | string | from `[alarmserver]` | Camera-specific filename format |
| `on_event_start` | string | — | Script to run on recording start |
| `on_event_stop` | string | — | Script to run on recording stop |
| `on_video_save` | string | — | Script to run when file is saved |
| `on_rtsp_lost` | string | — | Script to run on RTSP disconnect |
| `on_rtsp_found` | string | — | Script to run on RTSP reconnect |

---

## JSON Command Format (sent to AlarmServer)

**Note:** Commands are prefixed with 20 bytes of binary data.

### Start Recording
```json
{
    "Type": "Alarm",
    "Status": "Start",
    "Event": "MotionDetect",
    "Channel": 0,
    "SerialID": "cam_front_door",
    "Address": "192.168.1.100",
    "Descrip": "Motion detected at front door",
    "StartTime": "2026-05-03 14:30:00"
}
```

### Stop Recording
```json
{
    "Type": "Alarm",
    "Status": "Stop",
    "Event": "MotionDetect",
    "Channel": 0,
    "SerialID": "cam_front_door",
    "Address": "192.168.1.100",
    "Descrip": "Motion cleared at front door",
    "StartTime": "2026-05-03 14:30:00"
}
```

---

## Running the Application

```bash
# Production mode (minimal logging)
./ivckolpak /path/to/ivckolpak.ini -q

# Debug mode (verbose logging)
./ivckolpak /path/to/ivckolpak.ini --debug

# Default config path (looks for ivckolpak.ini in current directory)
./ivckolpak
```

---

## Summary

**ivckolpak** is a professionally engineered, production-ready NVR solution that:

- ✅ Always starts and ends recordings on keyframes
- ✅ Captures event context through pre- and post-buffering
- ✅ Survives RTSP connection drops without crashes
- ✅ Minimizes CPU usage through zero-copy packet handling
- ✅ Never blocks on external scripts
- ✅ Is completely deadlock-free
- ✅ Gracefully shuts down all threads
- ✅ Supports 22 macros across 6 event contexts
- ✅ Automatically creates recording directories
- ✅ Provides configurable logging levels
- ✅ Monitors RTSP connection status with scriptable notifications

The program is ready for 24/7 operation in video surveillance systems of any scale.
