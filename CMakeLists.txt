cmake_minimum_required(VERSION 3.16)
project(ivckolpak VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ✅ FIX: вынесено ДО add_executable
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)
find_package(Threads REQUIRED)

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

target_include_directories(ivckolpak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(ivckolpak PRIVATE
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)

target_compile_options(ivckolpak PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Debug>:-g -O0 -DDEBUG>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# --- Aggressive ARM Optimization Configuration ---
option(ENABLE_AGGRESSIVE_OPT "Enable aggressive optimization for ARM SoCs" ON)

# TARGET_CPU values:
#   native        - auto-detect build host (only for native builds)
#   rk3308        - Rockchip RK3308 (Cortex-A35, ARMv8.0-A, in-order, no crypto)
#   s905y2        - Amlogic S905Y2  (Cortex-A53, ARMv8-A+crypto, 8-stage in-order dual-issue)
#   allwinner-h5  - Allwinner H5    (Cortex-A53, ARMv8.0-A, in-order, crypto optional/absent)
#   rk3566        - Rockchip RK3566 (Cortex-A55, ARMv8.2-A, in-order, LSE)
#   rk3399        - Rockchip RK3399 (Cortex-A72, ARMv8.0-A, out-of-order, no LSE)
#   armv8_generic - Generic ARMv8-A fallback
set(TARGET_CPU "native" CACHE STRING
    "Target CPU: native, rk3308, s905y2, allwinner-h5, rk3566, rk3399, armv8_generic")

if(ENABLE_AGGRESSIVE_OPT AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(ARM_COMMON_FLAGS
        "-ffast-math"                 
        "-fno-semantic-interposition" 
        "-ffunction-sections"         
        "-fdata-sections"  
    )

    # 🧠 SoC-specific tuning
    if(TARGET_CPU STREQUAL "native")
        list(APPEND ARM_COMMON_FLAGS
            "-march=native"
        )

    elseif(TARGET_CPU STREQUAL "rk3308")
        list(APPEND ARM_COMMON_FLAGS
            "-mcpu=cortex-a35"
            "-moutline-atomics"      
            "-funroll-loops"        
            "-falign-functions=4"    
        )

    elseif(TARGET_CPU STREQUAL "s905y2")
        list(APPEND ARM_COMMON_FLAGS
            "-mcpu=cortex-a53"            
            "-march=armv8-a+crypto"
            "-moutline-atomics"        
            "-funroll-loops"            
            "-falign-functions=4"        
        )

    elseif(TARGET_CPU STREQUAL "allwinner-h5")
        list(APPEND ARM_COMMON_FLAGS
            "-mcpu=cortex-a53"
            "-moutline-atomics"
            "-funroll-loops"
            "-falign-functions=4"
        )

    elseif(TARGET_CPU STREQUAL "rk3566")
        list(APPEND ARM_COMMON_FLAGS
            "-mcpu=cortex-a55"
            "-march=armv8.2-a+lse+fp16"
            "-moutline-atomics"          
            "-funroll-loops"              
            "-ftree-loop-distribution"    
        )

    elseif(TARGET_CPU STREQUAL "rk3399")

        list(APPEND ARM_COMMON_FLAGS
            "-mcpu=cortex-a72"
            "-moutline-atomics"         
            "-ftree-loop-distribution"   
        )

    else()
        list(APPEND ARM_COMMON_FLAGS
            "-march=armv8-a+crc"
        )
    endif()


    target_compile_options(ivckolpak PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${ARM_COMMON_FLAGS}>
    )

    # 📦 Link-Time Optimization (LTO)
    target_compile_options(ivckolpak PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-flto=auto>
        $<$<CXX_COMPILER_ID:Clang>:-flto>
    )
    target_link_options(ivckolpak PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-flto=auto>
        $<$<CXX_COMPILER_ID:Clang>:-flto>
        $<$<CXX_COMPILER_ID:GNU,Clang>:-s>     
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--gc-sections>  
    )
endif()

# --- Partial Static Linking ---
option(BUILD_STATIC "Link libstdc++ and libgcc statically to ensure portability" OFF)
if(BUILD_STATIC)
    message(STATUS "=== ENABLING PARTIAL STATIC LINKING (libstdc++/libgcc only) ===")
    target_link_options(ivckolpak PRIVATE
        -static-libgcc
        -static-libstdc++

    )
endif()
