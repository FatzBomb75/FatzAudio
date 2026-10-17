cmake_minimum_required(VERSION 3.21)
project(fatzaudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
# One C++ file (src/stretch.cpp, around the header-only Signalsmith Stretch).
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# Sanitizer builds pass their flags in CMAKE_C_FLAGS; the C++ file (and the C++
# link) need them too.
if(CMAKE_C_FLAGS MATCHES "-fsanitize" AND NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    string(APPEND CMAKE_CXX_FLAGS " ${CMAKE_C_FLAGS}")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep the DLL next to test executables so Windows finds it without PATH tweaks.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(FA_BUILD_TESTS "Build FatzAudio tests" ON)
option(FA_BUILD_TOOLS "Build the listening and comparison tools (tools/)" ON)

set(FA_SOURCES
    src/engine.c
    src/device.c
    src/stream.c
    src/source.c
    src/probe.c
    src/decoder.c
    src/dec_miniaudio.c
    src/dec_mp3.c
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
    src/ac3.c
    src/dec_ac3.c
    src/dec_midi.c
    src/tinysoundfont_impl.c
    src/mdct.c
    src/ffmpeg.c
    src/push.c
    src/demux.c
    src/demux_mp4.c
    src/demux_adts.c
    src/demux_mkv.c
    src/demux_ogg.c
    src/codec_xiph.c
    src/codec_flac.c
    src/demux_asf.c
    src/stubs.c
    src/miniaudio_impl.c
    src/net_url.c
    src/net_http.c
    src/net_source.c
    src/net_hls.c
    src/cdg.c
    src/zip.c
    src/tempo.c
    src/resample.c
    src/stretch.cpp
    src/encode.c
    src/enc_wav.c
    src/enc_flac.c
    src/enc_mp3.c
    src/dynlib.c
    src/cdg.h
    src/disc.c
    src/disc_mmc.c
    src/disc_image.c
    src/encode.h
    src/disc_cdg.c
    src/tempo.h
    src/ac3.h
    src/mdct.h
    src/fa_internal.h
    src/net.h
    src/disc.h
)
# HTTPS through the platform's own stack, CD drives through its drive interface:
# nothing is bundled.
if(WIN32)
    # codec_mf.c: Windows Media Audio through Media Foundation (mfplat.dll is loaded
    # at run time, so the library still loads on Windows N without it).
    list(APPEND FA_SOURCES src/net_https_winhttp.c src/disc_win.c src/codec_mf.c)
elseif(APPLE)
    enable_language(OBJC)
    list(APPEND FA_SOURCES src/net_https_apple.m src/disc_mac.c)
else()
    list(APPEND FA_SOURCES src/net_https_curl.c src/disc_linux.c)
endif()

include(third_party/xiph.cmake)
add_library(fatzaudio SHARED ${FA_SOURCES})
target_link_libraries(fatzaudio PRIVATE fa_opus fa_vorbis)
set_source_files_properties(src/stretch.cpp PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith)
# One self-contained binary: no compiler runtime DLLs next to fatzaudio.dll (the
# C API hands no CRT objects across, so the app may use any runtime).
if(MSVC)
    set_property(TARGET fatzaudio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
    target_link_options(fatzaudio PRIVATE -static-libstdc++ -static-libgcc)
endif()

target_include_directories(fatzaudio
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fatzaudio PRIVATE FA_BUILDING)
set_target_properties(fatzaudio PROPERTIES
    OUTPUT_NAME fatzaudio
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)

find_package(Threads REQUIRED)
target_link_libraries(fatzaudio PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    # Also the system libraries of codec_mf.c: mfuuid (GUIDs, static) and ole32.
    set(FA_NET_LIBS ws2_32 winhttp mfuuid ole32)
elseif(APPLE)
    set(FA_NET_LIBS "-framework Foundation" "-framework CFNetwork")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        list(APPEND FA_NET_LIBS "-framework IOKit" "-framework CoreFoundation")
    endif()
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
    set_source_files_properties(src/miniaudio_impl.c src/tinysoundfont_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
    set(FA_WARNINGS /W3)
else()
    target_compile_options(fatzaudio PRIVATE -Wall -Wextra)
    # -fwrapv: dr_flac's predictors overflow int on damaged files; wrapping is what
    # they expect, and it keeps that defined behaviour.
    set_source_files_properties(src/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "-w;-fwrapv")
    set(FA_WARNINGS -Wall -Wextra)
endif()

if(FA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FA_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
