# 📹 ivckolpak — Lightweight Passthrough NVR for RTSP Cameras

## 🚀 Overview

**ivckolpak** is a high-reliability, low-overhead Network Video Recorder (NVR) designed specifically for RTSP IP cameras operating in **true passthrough mode**. The application continuously reads RTSP streams and writes video packets directly to MP4 containers **without any decoding, re-encoding, or frame processing**. Recording is triggered exclusively by external JSON events received via TCP (AlarmServer), making it ideal for integration with camera-native analytics, VMS systems, or custom bots.

### ✨ Key Advantages

| Feature | Benefit |
|---------|---------|
| **True Hardware Passthrough** | Zero CPU load from video processing — packets flow RTSP → MP4 unchanged |
| **Camera-Native Event Detection** | No software motion analysis; relies on camera's own PIR/AI analytics via AlarmServer |
| **I-Frame Accurate Buffering** | Pre/post-buffers measured in keyframes guarantee clean, playable recordings |
| **Modern C++20 Architecture** | `std::jthread`, `std::stop_token`, move semantics, packet pooling for efficiency |
| **ARM-Optimized Build System** | CMake flags for RK3308/RK3566/S905Y2/RK3399 with LTO and aggressive tuning |
| **Config Inheritance Model** | `[global]` defaults automatically overridden by per-`[camera]` settings |
| **Thread-Safe Zero-Copy Stats** | Real-time monitoring without locking overhead or memory copies |
| **Graceful Degradation** | RTSP reconnect with exponential backoff, timeout protection, clean shutdown |

---

## 🛠️ Building & Installation

### Dependencies (Debian/Ubuntu)
```bash
sudo apt install build-essential pkg-config cmake \
                 libavformat-dev libavcodec-dev libavutil-dev \
                 nlohmann-json3-dev
```

### Standard Build
```bash
git clone https://github.com/denzen84/ivckolpak.git
cd ivckolpak
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```
Binary output: `build/bin/ivckolpak`

### 🎯 ARM Optimization Options (Optional)

For maximum performance on embedded ARM SoCs, enable aggressive optimization flags:

```bash
# Native build (auto-detect host CPU)
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_AGGRESSIVE_OPT=ON -DTARGET_CPU=native ..

# Cross-compile for specific SoC
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_AGGRESSIVE_OPT=ON -DTARGET_CPU=rk3566 ..

# Universal ARMv8-A compatible build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_AGGRESSIVE_OPT=ON -DTARGET_CPU=armv8_generic ..
```

| TARGET_CPU | Recommended For | Compiler Flags Applied |
|------------|----------------|------------------------|
| `native` | Build on target device | `-march=native -mtune=native` |
| `rk3308` / `s905y2` | Cortex-A35 devices | `-mcpu=cortex-a35` |
| `rk3566` | Cortex-A55 devices (universal baseline) | `-mcpu=cortex-a55` |
| `rk3399` | big.LITTLE (tune for A72 cores) | `-mcpu=cortex-a72` |
| `armv8_generic` | Maximum compatibility | `-march=armv8-a -mtune=generic-aarch64` |

**Release builds automatically:**
- Enable `-O3`, `-ffast-math`, `-flto` (Link-Time Optimization)
- Strip debug symbols (`-s`) for minimal binary size
- Apply architecture-specific tuning via `-mcpu`

> ⚠️ **Note:** `-march=native` works **only** for native compilation. Use specific `TARGET_CPU` values for cross-compilation.

---

## ⚙️ Command-Line Options

| Option | Description |
|--------|-------------|
| `-c <file>`, `--config <file>` | Path to configuration file (default: `ivckolpak.ini`) |
| `-s`, `--silent` | Disable all console logging (equivalent to `disable_logs=yes` in config) |
| `--help-full` | Display detailed INI format, all options, and macro reference |
| `-h`, `--help` | Show brief help and exit |

**Examples:**
```bash
# Run with custom config
./ivckolpak --config /etc/ivckolpak/custom.ini

# Silent mode (no console output)
./ivckolpak --silent

# View full documentation
./ivckolpak --help-full
```

---

## 📄 Configuration File Format (INI)

The application uses a standard INI-style configuration with three sections: `[global]`, `[camera]`, and `[alarm_server]`.

### 🔁 Inheritance Model
All parameters in `[global]` act as **defaults** for every camera. When a parameter is specified inside a `[camera]` section, it **overrides** the global value. If a camera parameter is left empty, `0`, or `0.0`, the system automatically falls back to the `[global]` setting.

### 📋 [global] Section
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `pre_buffer_iframes` | int | `3` | I-frames to retain in memory before alarm triggers |
| `post_buffer_iframes` | int | `2` | I-frames to record after alarm stops |
| `max_chunk_duration_time_s` | double | `30.0` | Max duration (seconds) per video chunk |
| `max_event_total_duration_s` | double | `150.0` | Hard timeout for recording event (forces STOP) |
| `max_chunk_kbytes` | int | `51200` | Max size (KB) per video chunk |
| `max_event_chunks` | int | `5` | Max number of chunks per alarm event |
| `reconnect_delay_ms` | int | `3000` | Initial RTSP reconnect delay (ms) |
| `reconnect_max_delay_ms` | int | `30000` | Max backoff delay for RTSP reconnection |
| `target_dir` | string | `/mnt/recordings` | Directory for recorded video files |
| `filename_format` | string | `rec_%{cam_id}_%Y%m%d_%T_v%v_%e.mp4` | Output filename template (supports macros) |
| `on_event_start` | string | `""` | Shell command executed on recording start |
| `on_event_stop` | string | `""` | Shell command executed on recording stop |
| `on_video_save` | string | `""` | Shell command executed when chunk is finalized |
| `on_rtsp_lost` | string | `""` | Shell command executed on RTSP disconnect |
| `on_rtsp_found` | string | `""` | Shell command executed on RTSP reconnect |
| `disable_logs` | bool | `false` | Set to `yes`/`true`/`1`/`on` to disable console output |

### 📷 [camera] Section (repeatable; required: `name`, `serialid`, `rtsp`)
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `name` | string | — | Human-readable camera name |
| `serialid` | string | — | Unique ID (must match `SerialID` in AlarmServer JSON) |
| `rtsp` | string | — | Full RTSP stream URL |
| `rtsp_over_udp` | bool | `false` | Force UDP transport for RTSP (TCP is default) |
| *[All `[global]` parameters listed above]* | — | — | Override global defaults per camera |

### 🔔 [alarm_server] Section
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `listen_port` | int | `15002` | TCP port for receiving JSON alarm events |

---

## 🔌 AlarmServer: JSON Command Format

Send TCP packets to `localhost:15002` (or configured port) with raw JSON (no binary prefix required):

### Start Recording
```json
{
  "Type": "Alarm",
  "Status": "Start",
  "Event": "MotionDetect",
  "Channel": 0,
  "SerialID": "cam_front_door",
  "Address": "192.168.1.100",
  "Descrip": "Motion detected at entrance",
  "StartTime": "2026-05-18 14:30:00"
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
  "Descrip": "Motion cleared",
  "StartTime": "2026-05-18 14:30:00"
}
```

**Required fields:** `Type` (must be `"Alarm"`), `Status` (`"Start"`/`"Stop"`), `SerialID` (matches camera `serialid`).

---

## 🧩 Macro System Reference

Macros can be used in `filename_format` and external script commands (`on_*` hooks).

### ✅ Availability Matrix

| Macro | Filename | Script (on_*) | Description |
|-------|:--------:|:-------------:|-------------|
| `%Y`, `%m`, `%d`, `%H`, `%M`, `%S` | ✅ | ✅ | Date/time components (Year, Month, Day, Hour, Minute, Second) |
| `%T` | ✅ | ✅ | Full time in `HH:MM:SS` format |
| `%v` | ✅ | ❌ | Chunk version/index within an event |
| `%e` | ✅ | ❌ | Sequential Event ID |
| `%{cam_id}` | ✅ | ✅ | Camera `serialid` |
| `%{cam_name}` | ✅ | ✅ | Human-readable camera name |
| `%{cam_ip}` | ✅ | ✅ | IP address from RTSP URL (hex format, e.g., `0xC0A80164`) |
| `%{json_addr}` | ✅ | ✅ | `Address` field from alarm JSON |
| `%{json_chan}` | ✅ | ✅ | `Channel` field from alarm JSON |
| `%{json_desc}` | ✅ | ✅ | `Descrip` field from alarm JSON |
| `%{json_event}` | ✅ | ✅ | `Event` field from alarm JSON |
| `%{json_serialid}` | ✅ | ✅ | `SerialID` field from alarm JSON |
| `%{json_starttime}` | ✅ | ✅ | `StartTime` field from alarm JSON |
| `%{json_status}` | ✅ | ✅ | `Status` field (`Start`/`Stop`) from alarm JSON |
| `%{json_alarm}` | ✅ | ✅ | `Type`/alarm category from alarm JSON |
| `%f` | ❌ | ✅ | Absolute path to finalized video file (script-only) |

### 💡 Usage Examples

**Filename format:**
```ini
filename_format=rec_%{cam_name}_%Y%m%d_%T_v%v_%e.mp4
# Output: rec_FrontDoor_20260518_143022_v0_42.mp4
```

**Script command:**
```ini
on_video_save=/usr/local/bin/upload.sh -f %f -n %{cam_name} -t %{json_event}
```

---

## 🏗️ Architecture Overview

```
┌─────────────────┐     TCP:15002      ┌─────────────────┐
│ Camera / VMS /  │  JSON Alarm Events │   AlarmServer   │
│ Telegram Bot    │ ─────────────────► │ (TCP Listener)  │
└─────────────────┘                    └────────┬────────┘
                                                │
                                                ▼
                                     ┌─────────────────────┐
                                     │   AlarmQueue        │
                                     │ (thread-safe FIFO)  │
                                     └────────┬────────────┘
                                              │
                                              ▼
                                   ┌─────────────────────┐
                                   │  CameraManager      │
                                   │ (orchestrator)      │
                                   └────────┬────────────┘
                                            │
              ┌─────────────────────────────┼─────────────────────────────┐
              ▼                             ▼                             ▼
    ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
    │  CameraNode #1  │    │  CameraNode #2  │    │  CameraNode #N  │
    │ • RTSP reader   │    │ • RTSP reader   │    │ • RTSP reader   │
    │ • Passthrough   │    │ • Passthrough   │    │ • Passthrough   │
    │ • RecordingSession│  │ • RecordingSession│  │ • RecordingSession│
    └────────┬────────┘    └────────┬────────┘    └────────┬────────┘
             │                      │                      │
             ▼                      ▼                      ▼
    ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
    │ MP4 Chunk #1    │  │ MP4 Chunk #1    │  │ MP4 Chunk #1    │
    │ (passthrough)   │  │ (passthrough)   │  │ (passthrough)   │
    └─────────────────┘  └─────────────────┘  └─────────────────┘
```

**Critical design principle:** Video packets flow **unchanged** from RTSP stream to MP4 muxer. No decoding, no pixel buffers, no re-encoding — only PTS rebasing and container muxing.

---

## 🧪 Technical Stack

| Component | Technology |
|-----------|------------|
| Language | **C++20** (`std::jthread`, `std::stop_token`, concepts-ready) |
| Multimedia | FFmpeg (`libavformat`, `libavcodec`, `libavutil`) |
| JSON | `nlohmann/json` ≥ 3.2.0 |
| Build System | CMake ≥ 3.16 |
| Threading | `std::mutex`, `std::condition_variable`, `std::atomic`, lock-free patterns |
| Network | POSIX sockets (TCP), non-blocking I/O with timeouts |
| Recording Mode | **Pure passthrough** — zero transcoding, bit-exact preservation |

---

## 🎯 Why Choose ivckolpak?

✅ **Zero CPU video processing** — ideal for Raspberry Pi, Orange Pi, Rockchip, Amlogic SBCs  
✅ **Bit-perfect recordings** — no quality loss, no generation loss from re-encoding  
✅ **Camera-native intelligence** — leverage built-in PIR, AI detection, not software heuristics  
✅ **Telegram/VMS ready** — simple JSON/TCP interface, scriptable hooks for notifications  
✅ **I-frame aligned** — recordings always start/end on decodable keyframes  
✅ **Resilient by design** — exponential reconnect backoff, timeout protection, clean shutdown  
✅ **Minimal memory footprint** — packet pooling, zero-copy stats, no frame buffers  
✅ **Production-hardened** — deadlock-free threading, audited mutex usage, graceful degradation  
✅ **Flexible naming** — 22 macros across filename and script contexts  
✅ **Open & auditable** — clean modern C++20, no hidden dependencies, MIT-style clarity  

*For questions, issues, or contributions, please open an issue or submit a pull request on GitHub.*
