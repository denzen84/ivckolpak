cmake_minimum_required(VERSION 3.14)
project(ivckolpak LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.10.5 REQUIRED)

add_executable(ivckolpak
    main.cpp
    config_parser.cpp
    rtsp_camera_stream.cpp
    video_recorder.cpp
    recording_session.cpp
    camera_node.cpp
    camera_manager.cpp
    alarm_server.cpp
)

# Strip executable only in Release mode (avoids CMake parsing warnings)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET ivckolpak POST_BUILD
        COMMAND ${CMAKE_STRIP} "$<TARGET_FILE:ivckolpak>"
        COMMENT "Stripping debug symbols from binary"
        VERBATIM
    )
endif()

target_include_directories(ivckolpak PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(ivckolpak PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(ivckolpak PRIVATE 
    ${FFMPEG_LIBRARIES} 
    Threads::Threads 
    nlohmann_json::nlohmann_json
)