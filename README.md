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

---

## Key Features

### 1. Keyframe-Accurate Pre-Buffer

> **This is the most important architectural advantage over conventional solutions.**

Traditional NVRs store a fixed number of seconds or packets in their buffer. This causes recordings to start with B/P-frames that cannot be decoded without a preceding I-frame, resulting in corrupted video.

ivckolpak measures its buffer in **I-frame count** (configurable via `pre_buffer_iframes`):

```cpp
while (kf > buffer_frames_) {
    if (buffer_.front().is_key) kf--;
    buffer_.pop_front();
}
while (!buffer_.empty() && !buffer_.front().is_key) {
    buffer_.pop_front();  // Removes non-key frames before the first I-frame
}
```

**Result:** Recording always starts with a keyframe, ensuring compatibility with any media player and artifact-free playback.

### 2. Intelligent Post-Buffer on Stop

When a "Stop" command is received, recording does not stop immediately. The program waits for a configurable number of I-frames (`post_buffer_iframes`), capturing event development after the trigger ends. This is critical for security applications where activity may continue after the alarm clears.

### 3. RTSP Reconnection Resilience

On connection loss, the stream automatically reconnects every second. Recording sessions are unaffected — they store a copy of codec parameters (`AVCodecParameters`) rather than a pointer to the live context, preventing crashes when creating new chunks after reconnection.

### 4. Minimal Packet Copying via SafePacket

A custom `SafePacket` wrapper based on `std::shared_ptr<AVPacket>` with a custom deleter (`av_packet_free`) enables efficient packet transfer through the pipeline via move semantics:

```
RtspCameraStream → CameraNode → RecordingSession → VideoRecorder
```

Deep copying only occurs when writing the pre-buffer to file, where PTS modification is required.

### 5. Chunked Recording

Each event can be split into multiple chunks (files):
- `max_event_duration` — duration of a single chunk in seconds
- `max_event_chunks` — maximum number of chunks per event

Chunk switching is atomic: the old recorder stops, a new one is created, and recording continues without frame loss.

### 6. Flexible File Naming with Macro Expansion

The filename format supports extensive macro substitution:

| Macro | Description |
|-------|-------------|
| `%t` | Camera ID |
| `%e` | Event type |
| `%$` | Camera serial/code |
| `%Y`, `%m`, `%d` | Year, month, day |
| `%H`, `%M`, `%S` | Hours, minutes, seconds |
| `%T` | Full time (HH:MM:SS) |
| `%v` | Chunk version number |
| `%{json_addr}` | Address from JSON payload |
| `%{json_chan}` | Channel from JSON payload |
| `%{json_desc}` | Description from JSON payload |
| `%{json_event}` | Event type from JSON payload |
| `%{json_serialid}` | Serial ID from JSON payload |
| `%{json_starttime}` | Start time from JSON payload |
| `%{json_status}` | Status from JSON payload |
| `%{json_alarm}` | Alarm type from JSON payload |

### 7. Cascade Configuration

Directories and filename formats are resolved in cascade order:
```
[camera] target_dir → [alarmserver] target_dir → /tmp
[camera] filename_format → [alarmserver] filename_format → "rec_%t_%Y%m%d_%H%M%S.mp4"
```

Each camera can have its own storage directory and naming convention. Directories are automatically created on startup.

### 8. RTSP Connection Status Notifications

The program monitors RTSP connection state and can execute custom scripts on connection loss or recovery via `on_rtsp_lost` and `on_rtsp_found` configuration options. Macros in these scripts support `%{status}` (connected/disconnected) and `%{event}` (rtsp_found/rtsp_lost).

### 9. Centralized Script Execution

`ScriptRunner` manages all external scripts (`on_event_start`, `on_event_stop`, `on_video_save`, `on_rtsp_lost`, `on_rtsp_found`) in dedicated threads without blocking recording. On shutdown, all background scripts are gracefully terminated via `joinAll()`.

### 10. Automatic FPS Detection

`VideoRecorder` dynamically calculates frame rate from I-frame intervals, adapting to the actual stream:
```cpp
double fps = std::clamp(packet_count_since_iframe_ / el, 5.0, 120.0);
out_frame_duration_ = static_cast<int64_t>(1000000.0 / fps + 0.5);
```

### 11. Deadlock-Free Thread Safety

All shared resources are protected by mutexes with no circular dependencies:
- `sessions_mutex_` — active sessions map
- `sessions_mtx_` — per-node session list
- `ctx_mutex_` (`shared_mutex`) — format context and stream index
- `buf_mtx_` — packet ring buffer
- `queue_mutex_` — VideoRecorder and AlarmServer queues

### 12. Graceful Shutdown

Clean termination on SIGINT/SIGTERM:
- AlarmServer sends a dummy connection to unblock `accept()`
- All sessions receive `requestStop()`
- All writer threads complete current chunks and finalize MP4 files
- Background scripts are joined via `ScriptRunner::joinAll()`

### 13. Debug/Production Logging

Two verbosity levels:
- **Production** (`-q` or `--no-debug`): key events only (start/stop recording, errors)
- **Debug** (`--debug`): every method call, variable values, hex dumps of incoming data

---

## Architecture

```
┌──────────────┐     JSON/TCP     ┌──────────────┐
│  External    │ ────────────────→ │ AlarmServer  │
│  VMS/Sensor  │                   │  (port 15002)│
└──────────────┘                   └──────┬───────┘
                                          │ event queue
                                          ▼
                                   ┌──────────────┐
                                   │CameraManager │
                                   │ (orchestrator)│
                                   └──────┬───────┘
                                          │
                          ┌───────────────┼───────────────┐
                          ▼               ▼               ▼
                   ┌────────────┐ ┌────────────┐ ┌────────────┐
                   │ CameraNode │ │ CameraNode │ │ CameraNode │
                   │  (cam 1)   │ │  (cam 2)   │ │  (cam N)   │
                   └─────┬──────┘ └─────┬──────┘ └─────┬──────┘
                         │              │              │
                    ┌────▼────┐   ┌────▼────┐   ┌────▼────┐
                    │Session 1│   │Session 2│   │Session N│
                    │Session 2│   └─────────┘   └─────────┘
                    └────┬────┘
                         │
                    ┌────▼────┐
                    │VideoRec │
                    │  (MP4)  │
                    └─────────┘
```

---

## Configuration File

### Full Example (`ivckolpak.ini`)

```ini
[alarmserver]
# TCP port for receiving alarm commands
port=15002

# Global recording directory (fallback for cameras without their own target_dir)
target_dir=/var/recordings

# Global filename format (fallback for cameras without their own filename_format)
# Supported macros: %t (camera id), %e (event type), %$ (serial), %v (version),
#                   %Y %m %d %H %M %S %T (date/time),
#                   %{json_addr}, %{json_chan}, %{json_desc}, %{json_event},
#                   %{json_serialid}, %{json_starttime}, %{json_status}, %{json_alarm}
filename_format=rec_%t_%Y%m%d_%H%M%S.mp4


[camera]
# Unique camera identifier (must match SerialID field in JSON commands)
serialid=cam_front_door

# Human-readable description
description=Front Door Camera

# RTSP stream URL
rtsp=rtsp://192.168.64.34:554/user=admin_password=tlJwpbo6_channel=0_stream=0.sdp

# Number of I-frames to keep before event trigger (pre-buffer)
# Higher values = more context before recording starts
pre_buffer_iframes=3

# Number of I-frames to keep after Stop command (post-buffer)
# Higher values = more context after recording ends
# Set to 0 to disable post-buffering (immediate stop)
post_buffer_iframes=2

# Maximum duration of a single chunk in seconds
max_event_duration=30

# Maximum number of chunks per event
max_event_chunks=5

# Camera-specific recording directory (overrides [alarmserver] target_dir)
# target_dir=/mnt/storage/cam_front_door

# Camera-specific filename format (overrides [alarmserver] filename_format)
# filename_format=cam_front_%e_%Y%m%d_%H%M%S.mp4

# Script executed when recording starts
# Macros: %t (camera id), %e (event type), %f (filename), %{json_*} (JSON fields)
on_event_start=/opt/ivckolpak/notify.sh "start" "%t" "%e" "%{json_desc}"

# Script executed when recording stops
on_event_stop=/opt/ivckolpak/notify.sh "stop" "%t" "%e"

# Script executed when a video file is saved
on_video_save=/opt/ivckolpak/upload.sh "%f" "%t"

# Script executed when RTSP connection is lost
# Macros: %t (camera id), %{status} (disconnected), %{event} (rtsp_lost)
on_rtsp_lost=/opt/ivckolpak/alert.sh "rtsp_lost" "%t" "%{status}"

# Script executed when RTSP connection is restored
# Macros: %t (camera id), %{status} (connected), %{event} (rtsp_found)
on_rtsp_found=/opt/ivckolpak/alert.sh "rtsp_found" "%t" "%{status}"


# You can define multiple cameras by repeating the [camera] section
[camera]
serialid=cam_back_yard
description=Back Yard Camera
rtsp=rtsp://192.168.64.35:554/user=admin_password=pass123_channel=0_stream=0.sdp
pre_buffer_iframes=5
post_buffer_iframes=3
max_event_duration=60
max_event_chunks=10
on_event_start=/opt/ivckolpak/notify.sh "start" "%t" "%e"
on_event_stop=/opt/ivckolpak/notify.sh "stop" "%t" "%e"
on_video_save=/opt/ivckolpak/upload.sh "%f" "%t"
```

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

## Build Requirements

| Component | Required Version |
|-----------|------------------|
| C++ Standard | C++17 or higher |
| CMake | 3.14+ |
| FFmpeg | libavformat, libavcodec, libavutil |
| nlohmann/json | 3.10.5+ |
| POSIX Threads | Standard |

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
- ✅ Supports flexible macro expansion in filenames and scripts
- ✅ Automatically creates recording directories
- ✅ Provides configurable logging levels
- ✅ Monitors RTSP connection status with scriptable notifications

The program is ready for 24/7 operation in video surveillance systems of any scale.
