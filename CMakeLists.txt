cmake_minimum_required(VERSION 3.16)
project(ivckolpak VERSION 1.0.0 LANGUAGES CXX)

# Enforce C++20 standard without compiler-specific extensions
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Locate required dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(Threads REQUIRED)

# Define the main executable
add_executable(ivckolpak
    main.cpp
    camera_manager.cpp
    camera_node.cpp
    recording_session.cpp
    rtsp_stream.cpp
    alarm_server.cpp
    alarm_queue.cpp
    config.cpp
)

# Configure include paths and link libraries
target_include_directories(ivckolpak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(ivckolpak PRIVATE ${FFMPEG_LIBRARIES} nlohmann_json::nlohmann_json Threads::Threads)

# Base warning flags and configuration-specific defaults
target_compile_options(ivckolpak PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Debug>:-g -O0 -DDEBUG>
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
)

# --- Aggressive ARM Optimization Configuration ---
option(ENABLE_AGGRESSIVE_OPT "Enable aggressive optimization for ARM SoCs" ON)
set(TARGET_CPU "native" CACHE STRING "Target CPU: native, rk3308, rk3566, s905y2, rk3399, armv8_generic")

# Apply advanced flags only for Release builds
if(ENABLE_AGGRESSIVE_OPT AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # Common performance flags for ARMv8-A
    set(ARM_COMMON_FLAGS "-fno-math-errno" "-ftree-vectorize" "-fomit-frame-pointer" "-ffast-math")

    # CPU-specific tuning
    if(TARGET_CPU STREQUAL "native")
        # Auto-detect host CPU features (native compilation only)
        list(APPEND ARM_COMMON_FLAGS "-march=native" "-mtune=native")
    elseif(TARGET_CPU STREQUAL "rk3308" OR TARGET_CPU STREQUAL "s905y2")
        # Cortex-A35 baseline (low-power embedded)
        list(APPEND ARM_COMMON_FLAGS "-mcpu=cortex-a35")
    elseif(TARGET_CPU STREQUAL "rk3566")
        # Cortex-A55 baseline (recommended universal cross-compile target)
        list(APPEND ARM_COMMON_FLAGS "-mcpu=cortex-a55")
    elseif(TARGET_CPU STREQUAL "rk3399")
        # Tune for performance cores (Cortex-A72) in big.LITTLE configuration
        list(APPEND ARM_COMMON_FLAGS "-mcpu=cortex-a72")
    else()
        # Universal ARMv8-A fallback for maximum compatibility
        list(APPEND ARM_COMMON_FLAGS "-march=armv8-a" "-mtune=generic-aarch64")
    endif()

    # Apply optimization flags for GCC and Clang
    target_compile_options(ivckolpak PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${ARM_COMMON_FLAGS}>
    )

    # Enable Link-Time Optimization (LTO) for improved inlining and reduced binary size
    target_compile_options(ivckolpak PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-flto=auto>
        $<$<CXX_COMPILER_ID:Clang>:-flto>
    )
    target_link_options(ivckolpak PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-flto=auto>
        $<$<CXX_COMPILER_ID:Clang>:-flto>
    )

    # Strip debug symbols and metadata for Release builds
    target_link_options(ivckolpak PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-s>
    )
endif()

# Set the output directory for the compiled binary
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)