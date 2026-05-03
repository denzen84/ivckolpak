# ivckolpak — Lightweight Passthrough NVR for RTSP Cameras

## Overview

**ivckolpak** is a high-reliability, low-overhead Network Video Recorder (NVR) designed specifically for RTSP IP cameras operating in **passthrough mode**. The program continuously reads RTSP streams from cameras and saves video to disk only when external events occur (motion detection, alarm triggers, Telegram bot notifications, etc.). Recording is controlled via TCP commands in JSON format.

Unlike traditional NVR solutions that re-encode video streams, ivckolpak works in pure **passthrough mode** — it writes video packets directly to MP4 containers without any transcoding. This means **zero CPU load** from video processing, making it ideal for single-board computers (SBCs) like Raspberry Pi, Orange Pi, and other ARM-based devices where every CPU cycle counts.

## Why ivckolpak Was Created

The author had been using the popular **Motion** package for years to handle IP camera recordings integrated with Telegram bots. While Motion is a capable tool, it has several fundamental limitations that prevented it from fully utilizing the potential of modern SBC computers:

- **Motion re-encodes video streams**, consuming significant CPU resources even when no processing is needed
- **No native passthrough mode** — every frame goes through unnecessary decoding and re-encoding
- **Limited I-frame aware buffering** — recordings often start mid-GOP, producing corrupted or unplayable videos
- **Heavy resource footprint** — struggles on ARM-based SBCs with multiple camera streams
- **Complex configuration** for simple event-triggered recording scenarios
- **No built-in support for JSON-based external triggers** from systems like Telegram bots

**ivckolpak** was developed through **vibe-coding** to address these shortcomings. It provides:

- ✅ **True passthrough recording** — zero transcoding, near-zero CPU usage
- ✅ **Keyframe-accurate buffering** — recordings always start and end on I-frames
- ✅ **Minimal resource footprint** — runs comfortably on low-power ARM SBCs
- ✅ **Native JSON/TCP trigger interface** — perfect for Telegram bot integration
- ✅ **Clean, modern C++17 codebase** — easy to audit, modify, and extend

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

## Key Features

### 1. True Passthrough Recording (Zero Transcoding)

> **This is the defining architectural advantage.**

ivckolpak never re-encodes video. It reads H.264/H.265 packets from the RTSP stream and writes them directly to MP4 containers, only adjusting PTS timestamps to maintain proper playback timing. This results in:
- **Near-zero CPU usage** — limited to network I/O and disk writes
- **No quality loss** — the recorded video is bit-for-bit identical to the camera's original stream
- **Minimal latency** — no encoding pipeline delay

### 2. Keyframe-Accurate Pre-Buffer

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

### 3. Intelligent Post-Buffer on Stop

When a "Stop" command is received, recording does not stop immediately. The program waits for a configurable number of I-frames (`post_buffer_iframes`), capturing event development after the trigger ends. This is critical for security applications where activity may continue after the alarm clears, and for Telegram bot workflows where delayed notifications may need video context.

### 4. Ideal for Telegram Bot Integration

The TCP/JSON command interface is designed specifically for easy integration with Telegram bots:

```python
# Example: Trigger recording from a Telegram bot
import socket
import json

def start_recording(camera_id):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('localhost', 15002))
    prefix = b'\x00' * 20  # 20-byte binary prefix
    payload = json.dumps({
        "Type": "Alarm",
        "Status": "Start",
        "Event": "MotionDetect",
        "SerialID": camera_id
    }).encode()
    sock.send(prefix + payload)
    sock.close()
```

With notification scripts (`on_event_start`, `on_event_stop`, `on_video_save`), you can:
- Send Telegram messages when recording starts/stops
- Upload video files directly to Telegram chats
- Notify about RTSP connection status changes

### 5. RTSP Reconnection Resilience

On connection loss, the stream automatically reconnects with configurable scripts for `on_rtsp_lost` and `on_rtsp_found` events. Recording sessions are unaffected — they store a copy of codec parameters rather than a pointer to the live context.

### 6. Minimal Packet Copying via SafePacket

A custom `SafePacket` wrapper based on `std::shared_ptr<AVPacket>` enables efficient packet transfer through the pipeline via move semantics. Deep copying only occurs when writing the pre-buffer to file.

### 7. Chunked Recording

Each event can be split into multiple chunks with configurable duration and count. Chunk switching is atomic with no frame loss.

### 8. Unified Macro Engine

22 macros across 6 event contexts for flexible filename formatting and script command construction. See the [Macro System Reference](#macro-system-reference) for complete documentation.

### 9. Deadlock-Free Thread Safety

All shared resources are protected by mutexes with no circular dependencies, verified through multiple audit iterations.

### 10. Graceful Shutdown

Clean termination on SIGINT/SIGTERM with proper cleanup of all threads, completion of current recordings, and joining of background scripts.

---

## Architecture

```
┌──────────────┐     JSON/TCP     ┌──────────────┐
│  Telegram    │ ────────────────→ │ AlarmServer  │
│  Bot / VMS   │   (port 15002)   │              │
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
                   │ RTSP→Passthru│ │ RTSP→Passthru│ │ RTSP→Passthru│
                   └─────┬──────┘ └─────┬──────┘ └─────┬──────┘
                         │              │              │
                    ┌────▼────┐   ┌────▼────┐   ┌────▼────┐
                    │Session 1│   │Session 2│   │Session N│
                    │(MP4 out)│   │(MP4 out)│   │(MP4 out)│
                    └─────────┘   └─────────┘   └─────────┘
```

**Key principle:** RTSP packets flow through the pipeline **without modification**, only receiving PTS rebasing before being written to the MP4 muxer. No decoding, no encoding, no pixel manipulation.

---

## Macro System Reference

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

### Detailed Macro Descriptions

| Macro | Description | Substitutes |
|-------|-------------|-------------|
| `%t` | Camera ID | Camera identifier (e.g., `cam_front_door`) |
| `%$` | Camera name | Camera name from config (`cfg.id`) |
| `%e` | Event type | Event type from JSON (`MotionDetect`, `HumanDetect`, etc.) |
| `%f` | File path | Full path to the recording file |
| `%v` | Version | Chunk version number |
| `%Y` | Year | 4-digit year (e.g., `2026`) |
| `%m` | Month | 2-digit month (`01`–`12`) |
| `%d` | Day | 2-digit day (`01`–`31`) |
| `%H` | Hour | 2-digit hour, 24h format (`00`–`23`) |
| `%M` | Minute | 2-digit minute (`00`–`59`) |
| `%S` | Second | 2-digit second (`00`–`59`) |
| `%T` | Full time | `%H:%M:%S` format (`14:30:00`) |
| `%{json_addr}` | Address | `Address` field from JSON |
| `%{json_chan}` | Channel | `Channel` field from JSON |
| `%{json_desc}` | Description | `Descrip` field from JSON |
| `%{json_event}` | Event | `Event` field from JSON |
| `%{json_serialid}` | Serial ID | `SerialID` field from JSON |
| `%{json_starttime}` | Start time | `StartTime` field from JSON |
| `%{json_status}` | Status | `Status` field from JSON (`"Start"` or `"Stop"`) |
| `%{json_alarm}` | Alarm type | `Type` field from JSON |
| `%{status}` | RTSP state | `"connected"` or `"disconnected"` (RTSP scripts only) |
| `%{event}` | RTSP event | `"rtsp_found"` or `"rtsp_lost"` (RTSP scripts only) |

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

## Technical Stack

| Component | Technology |
|-----------|------------|
| Language | C++17 |
| Multimedia | FFmpeg (libavformat, libavcodec, libavutil) |
| JSON | nlohmann/json 3.10.5+ |
| Build System | CMake 3.14+ |
| Threading | `std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic` |
| Network | POSIX sockets (TCP) |
| Recording Mode | **Passthrough** (no transcoding) |

---

## Summary

**ivckolpak** is a purpose-built, production-ready NVR solution designed for resource-constrained environments where CPU efficiency, video quality preservation, and external system integration are paramount. It is:

- ✅ **True passthrough** — zero transcoding, bit-perfect video preservation
- ✅ **SBC-optimized** — runs comfortably on Raspberry Pi, Orange Pi, DietPi, and similar ARM devices
- ✅ **Telegram bot ready** — native JSON/TCP interface with scriptable notifications
- ✅ **I-frame accurate** — recordings always start and end on decodable keyframes
- ✅ **Reconnection resilient** — survives RTSP drops without crashing or corrupting files
- ✅ **Minimal overhead** — no decoding, no encoding, no pixel buffer allocations
- ✅ **Deadlock-free** — verified through multiple professional code audits
- ✅ **Graceful shutdown** — clean termination with proper cleanup of all resources
- ✅ **22 macros across 6 event contexts** — flexible naming and script composition
- ✅ **Open source** — clean, modern C++17 codebase, easy to audit and extend

The program is ready for 24/7 operation in video surveillance systems of any scale, from single-home setups to multi-camera SBC deployments.
