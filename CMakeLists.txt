cmake_minimum_required(VERSION 3.18)
project(pizero2cam_app
    VERSION     1.0.0
    DESCRIPTION "Pi Zero 2 W cross-compiled cam application"
    LANGUAGES   CXX C
)

# ── Standards ──────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ── Default build type ─────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler:    ${CMAKE_CXX_COMPILER}")
message(STATUS "System:      ${CMAKE_SYSTEM_NAME}")
message(STATUS "Processor:   ${CMAKE_SYSTEM_PROCESSOR}")

# ── Compile options ────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g3 -fno-omit-frame-pointer)
    add_compile_definitions(DEBUG=1)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-O2 -g -fno-omit-frame-pointer)
endif()

# ── Export compile commands (for IntelliSense) ─────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Main executable ────────────────────────────────────
add_executable(pizero2cam_app
    src/main.cpp
    src/power/ina219.cpp
    src/camera/camera_manager.cpp
    src/record/recorder.cpp
    src/record/h264_hw_encoder.cpp
    src/stream/mjpeg_server.cpp
    src/stream/jpeg_hw_encoder.cpp
    src/stream/jpeg_mt_encoder.cpp
    src/stream/gst_camera_source.cpp
)

# ── Include directories ────────────────────────────────
target_include_directories(pizero2cam_app PRIVATE
    src
    third_party
)

target_link_libraries(pizero2cam_app
    PRIVATE pthread rt atomic
)

# ── libcamera (required — camera_manager.cpp depends on it) ───────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)

message(STATUS "libcamera: ${LIBCAMERA_VERSION}")
target_include_directories(pizero2cam_app PRIVATE
    ${LIBCAMERA_INCLUDE_DIRS})
target_link_libraries(pizero2cam_app PRIVATE
    ${LIBCAMERA_LINK_LIBRARIES})
target_compile_definitions(pizero2cam_app PRIVATE
    HAVE_LIBCAMERA=1)

# ── GStreamer (required — recorder.cpp depends on it) ──────────────────
pkg_check_modules(GST REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0)

message(STATUS "GStreamer: ${GST_VERSION}")
target_include_directories(pizero2cam_app PRIVATE ${GST_INCLUDE_DIRS})
target_link_libraries(pizero2cam_app PRIVATE ${GST_LINK_LIBRARIES})

# ── libjpeg — jpeg_mt_encoder.cpp uses jpeglib.h + jpeg_write_raw_data ────
target_link_libraries(pizero2cam_app PRIVATE jpeg)

# ── Install ────────────────────────────────────────────
install(TARGETS pizero2cam_app RUNTIME DESTINATION bin)