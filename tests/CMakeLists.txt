add_executable(fa_smoke smoke.c)
target_link_libraries(fa_smoke PRIVATE fatzaudio)
if(UNIX)
    target_link_libraries(fa_smoke PRIVATE m)
endif()
add_test(NAME smoke COMMAND fa_smoke)

if(WIN32)
    add_executable(fa_test_deps test_deps.c)
    if(MSVC)
        target_compile_definitions(fa_test_deps PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME deps COMMAND fa_test_deps $<TARGET_FILE:fatzaudio>)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}" MATCHES "-fsanitize")
    # Sanitizer builds link their runtimes dynamically on purpose.
    add_test(NAME deps COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_deps_linux.sh $<TARGET_FILE:fatzaudio>)
endif()

add_executable(fa_test_core test_core.c)
target_link_libraries(fa_test_core PRIVATE fatzaudio)
if(UNIX)
    target_link_libraries(fa_test_core PRIVATE m)
endif()
add_test(NAME core COMMAND fa_test_core WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(fa_test_analysis test_analysis.c)
target_link_libraries(fa_test_analysis PRIVATE fatzaudio)
if(UNIX)
    target_link_libraries(fa_test_analysis PRIVATE m)
endif()
add_test(NAME analysis COMMAND fa_test_analysis WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# White-box tests: the library sources linked straight into the test, so it can
# drive the mixer by hand and check exact sample values.
list(TRANSFORM FA_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/)
set_source_files_properties(${PROJECT_SOURCE_DIR}/src/stretch.cpp PROPERTIES
    INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/third_party/signalsmith)
# Every white-box test compiles the MIDI decoder, so its vendored headers are found
# from the sources themselves.
set_source_files_properties(${PROJECT_SOURCE_DIR}/src/dec_midi.c ${PROJECT_SOURCE_DIR}/src/tinysoundfont_impl.c PROPERTIES
    INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
add_executable(fa_test_internal test_internal.c)
target_include_directories(fa_test_internal PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_internal PRIVATE FA_STATIC)
target_compile_options(fa_test_internal PRIVATE ${FA_WARNINGS})
if(MSVC)
    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/miniaudio_impl.c ${PROJECT_SOURCE_DIR}/src/tinysoundfont_impl.c
        PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "-w;-fwrapv")
    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/surface_fx.c ${PROJECT_SOURCE_DIR}/src/cdg_scale.c
        ${PROJECT_SOURCE_DIR}/src/cdg_contour.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
if(APPLE)
    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/net_https_apple.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
if(IOS)
    # As for the library: miniaudio sets up the AVAudioSession on iOS, in Objective-C
    # (source file properties do not carry over from the top-level directory).
    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/miniaudio_impl.c PROPERTIES LANGUAGE OBJC)
endif()
target_link_libraries(fa_test_internal PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_internal PRIVATE m)
endif()
add_test(NAME internal COMMAND fa_test_internal)

add_executable(fa_test_mixer test_mixer.c)
target_include_directories(fa_test_mixer PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_mixer PRIVATE FA_STATIC)
target_compile_options(fa_test_mixer PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_mixer PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_mixer PRIVATE m)
endif()
add_test(NAME mixer COMMAND fa_test_mixer)

add_executable(fa_test_sync test_sync.c)
target_include_directories(fa_test_sync PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_sync PRIVATE FA_STATIC)
target_compile_options(fa_test_sync PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_sync PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_sync PRIVATE m)
endif()
add_test(NAME sync COMMAND fa_test_sync)

# The system's real audio output and input (skips unless FA_TEST_REAL_SYSTEM=1).
add_executable(fa_test_device test_device.c)
target_include_directories(fa_test_device PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_device PRIVATE FA_STATIC)
target_compile_options(fa_test_device PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_device PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_device PRIVATE m)
endif()
add_test(NAME device COMMAND fa_test_device)

add_executable(fa_test_mic test_mic.c)
target_include_directories(fa_test_mic PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_mic PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_mic PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_mic PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_mic PRIVATE m)
endif()
add_test(NAME mic COMMAND fa_test_mic)

add_executable(fa_test_loudness test_loudness.c)
target_include_directories(fa_test_loudness PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_loudness PRIVATE FA_STATIC)
target_compile_options(fa_test_loudness PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_loudness PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_loudness PRIVATE m)
endif()
add_test(NAME loudness COMMAND fa_test_loudness WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(fa_test_crossfade test_crossfade.c)
target_include_directories(fa_test_crossfade PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_crossfade PRIVATE FA_STATIC)
target_compile_options(fa_test_crossfade PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_crossfade PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_crossfade PRIVATE m)
endif()
add_test(NAME crossfade COMMAND fa_test_crossfade)

add_executable(fa_test_handoff test_handoff.c)
target_include_directories(fa_test_handoff PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_handoff PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_handoff PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_handoff PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_handoff PRIVATE m)
endif()
add_test(NAME handoff COMMAND fa_test_handoff)

add_executable(fa_test_demux test_demux.c)
target_include_directories(fa_test_demux PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_demux PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_demux PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_demux PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_demux PRIVATE m)
endif()
add_test(NAME demux COMMAND fa_test_demux)

add_executable(fa_test_dolby test_dolby.c)
target_include_directories(fa_test_dolby PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_dolby PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_dolby PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_dolby PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_dolby PRIVATE m)
endif()
add_test(NAME dolby COMMAND fa_test_dolby)

add_executable(fa_test_aac test_aac.c)
target_include_directories(fa_test_aac PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_aac PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_aac PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_aac PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_aac PRIVATE m)
endif()
add_test(NAME aac COMMAND fa_test_aac)

# fatzaudio_ffmpeg.h against a real FFmpeg, the way an app uses it. Only built when
# FFmpeg's development files are found (the library itself never needs them).
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FA_TEST_AV QUIET IMPORTED_TARGET libavformat libavcodec libavutil)
endif()
if(FA_TEST_AV_FOUND)
    add_executable(fa_test_ffmpeg_helper test_ffmpeg_helper.c)
    target_compile_definitions(fa_test_ffmpeg_helper PRIVATE FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    target_compile_options(fa_test_ffmpeg_helper PRIVATE ${FA_WARNINGS})
    target_link_libraries(fa_test_ffmpeg_helper PRIVATE fatzaudio PkgConfig::FA_TEST_AV)
    if(UNIX)
        target_link_libraries(fa_test_ffmpeg_helper PRIVATE m)
    endif()
    add_test(NAME ffmpeg_helper COMMAND fa_test_ffmpeg_helper)
endif()

add_executable(fa_test_net test_net.c)
target_include_directories(fa_test_net PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_net PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_net PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_net PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_net PRIVATE m)
endif()
if(ANDROID)
    target_sources(fa_test_net PRIVATE net_tls_android.c)
endif()
add_test(NAME net COMMAND fa_test_net)

add_executable(fa_test_cdg test_cdg.c)
target_include_directories(fa_test_cdg PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_cdg PRIVATE FA_STATIC)
target_compile_options(fa_test_cdg PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_cdg PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_cdg PRIVATE m)
endif()
add_test(NAME cdg COMMAND fa_test_cdg WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(fa_test_font test_font.c)
target_include_directories(fa_test_font PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_font PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_font PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_font PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_font PRIVATE m)
endif()
add_test(NAME font COMMAND fa_test_font)

add_executable(fa_test_lyric test_lyric.c)
target_include_directories(fa_test_lyric PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_lyric PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_lyric PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_lyric PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_lyric PRIVATE m)
endif()
add_test(NAME lyric COMMAND fa_test_lyric)

add_executable(fa_test_disc test_disc.c)
target_include_directories(fa_test_disc PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_disc PRIVATE FA_STATIC)
target_compile_options(fa_test_disc PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_disc PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_disc PRIVATE m)
endif()
add_test(NAME disc COMMAND fa_test_disc WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(fa_test_tempo test_tempo.c)
target_include_directories(fa_test_tempo PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_tempo PRIVATE FA_STATIC)
target_compile_options(fa_test_tempo PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_tempo PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_tempo PRIVATE m)
endif()
add_test(NAME tempo COMMAND fa_test_tempo)

add_executable(fa_test_asf test_asf.c)
target_include_directories(fa_test_asf PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(fa_test_asf PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_asf PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_asf PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_asf PRIVATE m)
endif()
add_test(NAME asf COMMAND fa_test_asf)

add_executable(fa_test_tags test_tags.c)
target_include_directories(fa_test_tags PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(fa_test_tags PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_tags PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_tags PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_tags PRIVATE m)
endif()
add_test(NAME tags COMMAND fa_test_tags)

add_executable(fa_test_encode test_encode.c)
target_include_directories(fa_test_encode PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(fa_test_encode PRIVATE FA_STATIC)
target_compile_options(fa_test_encode PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_encode PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_encode PRIVATE m)
endif()
add_test(NAME encode COMMAND fa_test_encode WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(fa_test_midi test_midi.c)
target_include_directories(fa_test_midi PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_midi PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_midi PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_midi PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_midi PRIVATE m)
endif()
add_test(NAME midi COMMAND fa_test_midi WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# White-box: it plays the audio thread itself for FA_EVENT_TRACKER_ROW.
add_executable(fa_test_tracker test_tracker.c)
target_include_directories(fa_test_tracker PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_tracker PRIVATE FA_STATIC)
target_compile_options(fa_test_tracker PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_tracker PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_tracker PRIVATE m)
endif()
add_test(NAME tracker COMMAND fa_test_tracker)

add_executable(fa_test_ogg test_ogg.c)
target_include_directories(fa_test_ogg PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_test_ogg PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_ogg PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_ogg PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_ogg PRIVATE m)
endif()
add_test(NAME ogg COMMAND fa_test_ogg)

add_executable(fa_test_flac test_flac.c)
target_include_directories(fa_test_flac PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio)
target_compile_definitions(fa_test_flac PRIVATE FA_STATIC FA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(fa_test_flac PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_test_flac PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_test_flac PRIVATE m)
endif()
add_test(NAME flac COMMAND fa_test_flac)

# The white-box tests link the library's sources as objects compiled once for all
# of them; the vendored codecs come prebuilt.
add_library(fa_whitebox OBJECT ${FA_SOURCES})
target_include_directories(fa_whitebox PUBLIC
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/miniaudio
    ${PROJECT_SOURCE_DIR}/third_party/tinysoundfont)
target_compile_definitions(fa_whitebox PUBLIC FA_STATIC)
target_compile_options(fa_whitebox PRIVATE ${FA_WARNINGS})
target_link_libraries(fa_whitebox PUBLIC fa_opus fa_vorbis fa_openmpt Threads::Threads ${CMAKE_DL_LIBS} ${FA_NET_LIBS})
if(UNIX)
    target_link_libraries(fa_whitebox PUBLIC m)
endif()
if(MSVC)
    set_property(TARGET fa_whitebox PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Fuzz targets (tests/fuzz/CMakeLists.txt): the library sources get edge coverage for
# the driver. Build only the fuzz_* targets in such a tree; the other tests lack the driver.
option(FA_FUZZ "Build the fuzz targets (tests/fuzz)" OFF)
if(FA_FUZZ)
    target_compile_options(fa_whitebox PRIVATE -fsanitize-coverage=trace-pc)
endif()
add_subdirectory(fuzz)

foreach(t fa_test_internal fa_test_loudness fa_test_crossfade fa_test_handoff fa_test_demux
          fa_test_dolby fa_test_aac fa_test_net fa_test_cdg fa_test_disc fa_test_tempo fa_test_ogg
          fa_test_encode fa_test_midi fa_test_tracker fa_test_flac fa_test_asf fa_test_tags fa_test_font
          fa_test_lyric fa_test_mixer fa_test_sync fa_test_mic fa_test_device)
    target_link_libraries(${t} PRIVATE fa_whitebox)
    # The same static C runtime as those libraries: libopenmpt is C++, and MSVC
    # refuses to link C++ objects built for different runtimes.
    if(MSVC)
        set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endforeach()

# DEFLATE and ZIP against zlib, where CMake finds it (test only).
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    add_executable(fa_test_inflate test_inflate.c)
    target_compile_options(fa_test_inflate PRIVATE ${FA_WARNINGS})
    target_link_libraries(fa_test_inflate PRIVATE fa_whitebox ZLIB::ZLIB)
    if(MSVC)
        set_property(TARGET fa_test_inflate PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME inflate COMMAND fa_test_inflate WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# These tests write scratch files with fixed names into the shared working
# directory, so ctest -j must not run two of them at once. Everything else runs
# in parallel.
set_tests_properties(core loudness cdg disc encode midi internal tags analysis PROPERTIES RESOURCE_LOCK fa_scratch_files)
