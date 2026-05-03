cmake_minimum_required(VERSION 3.14)
project(ivckolpak LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 🔧 Глобальные флаги оптимизации для Release
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Максимальная оптимизация под конкретный CPU (-march=native)
    add_compile_options(-march=native -mtune=native)
    
    # Агрессивная оптимизация
    add_compile_options(-O3)
    
    # Link-Time Optimization (LTO)
    add_compile_options(-flto)
    add_link_options(-flto)
    
    # Отключение assert'ов
    add_compile_options(-DNDEBUG)
    
    # Дополнительные флаги безопасности
    add_compile_options(-fstack-protector-strong)
    
    # Оптимизация размера (опционально, вместо -O3)
    # add_compile_options(-Os)
endif()

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

target_include_directories(ivckolpak PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(ivckolpak PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(ivckolpak PRIVATE 
    ${FFMPEG_LIBRARIES} 
    Threads::Threads 
    nlohmann_json::nlohmann_json
)

# Strip executable only in Release mode
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET ivckolpak POST_BUILD
        COMMAND ${CMAKE_STRIP} "$<TARGET_FILE:ivckolpak>"
        COMMENT "Stripping debug symbols from binary"
        VERBATIM
    )
endif()