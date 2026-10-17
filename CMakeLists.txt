cmake_minimum_required(VERSION 3.21)
project(fatzaudio VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep the DLL next to test executables so Windows finds it without PATH tweaks.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(FA_BUILD_TESTS "Build FatzAudio tests" ON)

set(FA_SOURCES
    src/engine.c
    src/device.c
    src/stream.c
    src/source.c
    src/probe.c
    src/decoder.c
    src/dec_miniaudio.c
    src/fa_platform.c
    src/ring.c
    src/curve.c
    src/lane.c
    src/bus.c
    src/event.c
    src/loudness.c
    src/tags.c
    src/scan.c
    src/normalize.c
    src/limiter.c
    src/clock.c
    src/codec.c
    src/ffmpeg.c
    src/push.c
    src/demux.c
    src/demux_mp4.c
    src/demux_mkv.c
    src/stubs.c
    src/miniaudio_impl.c
    src/net_url.c
    src/net_http.c
    src/net_source.c
    src/net_hls.c
    src/fa_internal.h
    src/net.h
)
# HTTPS through the platform's own stack: nothing is bundled.
if(WIN32)
    list(APPEND FA_SOURCES src/net_https_winhttp.c)
elseif(APPLE)
    enable_language(OBJC)
    list(APPEND FA_SOURCES src/net_https_apple.m)
else()
    list(APPEND FA_SOURCES src/net_https_curl.c)
endif()

add_library(fatzaudio SHARED ${FA_SOURCES})

target_include_directories(fatzaudio
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(fatzaudio PRIVATE FA_BUILDING)
set_target_properties(fatzaudio PROPERTIES
    OUTPUT_NAME fatzaudio
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)

find_package(Threads REQUIRED)
target_link_libraries(fatzaudio PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    set(FA_NET_LIBS ws2_32 winhttp)
elseif(APPLE)
    set(FA_NET_LIBS "-framework Foundation" "-framework CFNetwork")
    set_source_files_properties(src/net_https_apple.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
    set(FA_NET_LIBS "")
endif()
target_link_libraries(fatzaudio PRIVATE ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fatzaudio PRIVATE m)
endif()

if(MSVC)
    target_compile_options(fatzaudio PRIVATE /W3)
    set_source_files_properties(src/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
    set(FA_WARNINGS /W3)
else()
    target_compile_options(fatzaudio PRIVATE -Wall -Wextra)
    set_source_files_properties(src/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "-w")
    set(FA_WARNINGS -Wall -Wextra)
endif()

if(FA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
