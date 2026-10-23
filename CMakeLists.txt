cmake_minimum_required(VERSION 3.21)
# The version lives in the public header only (FA_VERSION_*); everything else reads it.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/fatzaudio/fatzaudio.h" fa_version_lines REGEX "^#define FA_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(part MAJOR MINOR PATCH)
    string(REGEX MATCH "FA_VERSION_${part} ([0-9]+)" _ "${fa_version_lines}")
    set(fa_version_${part} ${CMAKE_MATCH_1})
endforeach()
project(fatzaudio VERSION ${fa_version_MAJOR}.${fa_version_MINOR}.${fa_version_PATCH} LANGUAGES C CXX)

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

# iOS executables need an app bundle and a signed device or simulator to run, so
# an iOS build makes only the library (docs/building.md).
if(IOS)
    set(FA_EXECUTABLES_DEFAULT OFF)
else()
    set(FA_EXECUTABLES_DEFAULT ON)
endif()
option(FA_BUILD_TESTS "Build FatzAudio tests" ${FA_EXECUTABLES_DEFAULT})
option(FA_BUILD_TOOLS "Build the listening and comparison tools (tools/)" ${FA_EXECUTABLES_DEFAULT})

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
    src/eq.c
    src/lane.c
    src/bus.c
    src/event.c
    src/loudness.c
    src/tags.c
    src/tags_read.c
    src/scan.c
    src/normalize.c
    src/limiter.c
    src/analysis.c
    src/clock.c
    src/codec.c
    src/ac3.c
    src/dec_ac3.c
    src/aac.c
    src/aac_sbr.c
    src/aac_ps.c
    src/aac_tables.c
    src/dec_midi.c
    src/dec_tracker.c
    src/dls.c
    src/sf3.c
    src/midi_fx.c
    src/tinysoundfont_impl.c
    src/mdct.c
    src/push.c
    src/demux.c
    src/demux_mp4.c
    src/demux_adts.c
    src/demux_mkv.c
    src/demux_ogg.c
    src/codec_xiph.c
    src/codec_flac.c
    src/codec_alac.c
    src/codec_os.c
    src/demux_asf.c
    src/stubs.c
    src/miniaudio_impl.c
    src/net_url.c
    src/net_http.c
    src/net_source.c
    src/net_hls.c
    src/cdg.c
    src/lyric.c
    src/lyric_time.c
    src/cdg_scale.c
    src/cdg_contour.c
    src/surface.c
    src/surface_fx.c
    src/font.c
    src/raster.c
    src/font_find.c
    src/zip.c
    src/tempo.c
    src/resample.c
    src/resample_device.c
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
    # codec_mf.c: Windows Media Audio, AAC and ALAC through Media Foundation
    # (mfplat.dll is loaded at run time, so the library still loads on Windows N).
    list(APPEND FA_SOURCES src/net_https_winhttp.c src/disc_win.c src/codec_mf.c)
elseif(APPLE)
    enable_language(OBJC)
    # codec_at.c: AAC through AudioToolbox.
    list(APPEND FA_SOURCES src/net_https_apple.m src/disc_mac.c src/codec_at.c)
    # Objective-C files (and miniaudio on iOS) export only fa_*, like the C ones.
    set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
    if(IOS)
        # miniaudio sets up the AVAudioSession on iOS, which is Objective-C.
        set_source_files_properties(src/miniaudio_impl.c PROPERTIES LANGUAGE OBJC)
    endif()
else()
    list(APPEND FA_SOURCES src/disc_linux.c)
    if(ANDROID)
        # net_https_android.c: HTTPS through the app's Java VM (HttpURLConnection,
        # found at run time). codec_mediacodec.c: AAC through MediaCodec
        # (libmediandk.so is loaded at run time).
        list(APPEND FA_SOURCES src/net_https_android.c src/codec_mediacodec.c)
    else()
        list(APPEND FA_SOURCES src/net_https_curl.c)
    endif()
endif()

include(third_party/xiph.cmake)
include(third_party/libopenmpt.cmake)
add_library(fatzaudio SHARED ${FA_SOURCES})
target_link_libraries(fatzaudio PRIVATE fa_opus fa_vorbis fa_openmpt)
set_source_files_properties(src/stretch.cpp PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith)
# One self-contained binary: no compiler runtime DLLs next to fatzaudio.dll (the
# C API hands no CRT objects across, so the app may use any runtime).
if(MSVC)
    set_property(TARGET fatzaudio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
    target_link_options(fatzaudio PRIVATE -static-libstdc++ -static-libgcc -Wl,--gc-sections)
    # fatzaudio.dll, the same name as the MSVC build (MinGW would add "lib").
    set_target_properties(fatzaudio PROPERTIES PREFIX "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The same on Linux: libstdc++ and libgcc go into libfatzaudio.so, so it needs
    # only libc and libm. --exclude-libs keeps their symbols out of the export
    # table, and the version script also hides the std:: templates stretch.cpp
    # instantiates (libstdc++ declares them with default visibility).
    target_link_options(fatzaudio PRIVATE -static-libstdc++ -static-libgcc "-Wl,--exclude-libs,ALL" -Wl,--gc-sections
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/fatzaudio.map")
    set_property(TARGET fatzaudio APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fatzaudio.map)
endif()

# What nothing calls is left out (libopenmpt is built with a section per
# function); MSVC does this in release builds already (/OPT:REF).
if(APPLE)
    target_link_options(fatzaudio PRIVATE -Wl,-dead_strip)
elseif(ANDROID)
    target_link_options(fatzaudio PRIVATE -Wl,--gc-sections)
endif()

target_include_directories(fatzaudio
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fatzaudio PRIVATE FA_BUILDING)
# Before 1.0 a minor version may change the ABI (CHANGELOG.md), so it is part of the SONAME.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(fa_soversion ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
else()
    set(fa_soversion ${PROJECT_VERSION_MAJOR})
endif()
set_target_properties(fatzaudio PROPERTIES
    OUTPUT_NAME fatzaudio
    VERSION ${PROJECT_VERSION}
    SOVERSION ${fa_soversion})

find_package(Threads REQUIRED)
target_link_libraries(fatzaudio PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    # Also the system libraries of codec_mf.c: mfuuid (GUIDs, static) and ole32;
    # advapi32: the registry's font list (font_find.c).
    set(FA_NET_LIBS ws2_32 winhttp mfuuid ole32 advapi32)
elseif(APPLE)
    # AudioToolbox: codec_at.c (and miniaudio on iOS).
    # CoreText: font_find.c.
    set(FA_NET_LIBS "-framework Foundation" "-framework CFNetwork" "-framework AudioToolbox" "-framework CoreText")
    if(IOS)
        # miniaudio links Core Audio directly on iOS (no runtime loading there).
        list(APPEND FA_NET_LIBS "-framework CoreFoundation" "-framework CoreAudio" "-framework AVFoundation")
    else()
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
if(ANDROID)
    # The C++ runtime is linked in (the NDK's default c++_static), so the .so needs
    # no libc++_shared.so; keep its symbols out of the export table so they cannot
    # clash with the app's own C++ runtime. 16 KB pages: required by Android 15+
    # devices with 16 KB pages and by Google Play (NDK r28 and later do it anyway).
    # Release builds drop the NDK's debug info (10 MB) but keep the symbol table,
    # so crash reports still name functions.
    target_link_options(fatzaudio PRIVATE "-Wl,--exclude-libs,ALL" "-Wl,-z,max-page-size=16384"
        "$<$<CONFIG:Release,MinSizeRel>:-Wl,--strip-debug>")
elseif(IOS)
    set(FA_IOS_BUNDLE_ID "com.fatzaudio.fatzaudio" CACHE STRING "CFBundleIdentifier of fatzaudio.framework")
    # iOS apps embed dynamic code only as frameworks: fatzaudio.framework, combined
    # into fatzaudio.xcframework by tools/build-xcframework.sh.
    # CMake's own Info.plist template has no MinimumOSVersion, which the App Store
    # requires of embedded frameworks.
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(FA_IOS_PLATFORM iPhoneSimulator)
    else()
        set(FA_IOS_PLATFORM iPhoneOS)
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(FA_IOS_MIN ${CMAKE_OSX_DEPLOYMENT_TARGET})
    else()
        set(FA_IOS_MIN 13.0)
    endif()
    configure_file(cmake/ios-framework-Info.plist.in ${CMAKE_BINARY_DIR}/ios-framework-Info.plist.in @ONLY)
    set_target_properties(fatzaudio PROPERTIES
        FRAMEWORK TRUE
        MACOSX_FRAMEWORK_INFO_PLIST ${CMAKE_BINARY_DIR}/ios-framework-Info.plist.in
        MACOSX_FRAMEWORK_IDENTIFIER ${FA_IOS_BUNDLE_ID}
        MACOSX_FRAMEWORK_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_FRAMEWORK_BUNDLE_VERSION ${PROJECT_VERSION}
        PUBLIC_HEADER "include/fatzaudio/fatzaudio.h;include/fatzaudio/fatzaudio_ffmpeg.h")
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
