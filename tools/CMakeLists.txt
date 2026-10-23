# Listening and comparison tools (tools/tempo-compare/README.md).
add_executable(fa_tempo_render tempo-compare/fa_tempo_render.c)
target_link_libraries(fa_tempo_render PRIVATE fatzaudio)
target_compile_options(fa_tempo_render PRIVATE ${FA_WARNINGS})

if(WIN32)
    # Loads bass.dll and bass_fx.dll at run time (64-bit, like the legacy DLLs).
    add_executable(bassfx_render tempo-compare/bassfx_render.c)
    target_compile_options(bassfx_render PRIVATE ${FA_WARNINGS})
    if(MSVC)
        target_compile_definitions(bassfx_render PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# Runs the DJ mixer's setup check on a real rig (tools/mic-check/fa_mic_check.c).
add_executable(fa_mic_check mic-check/fa_mic_check.c)
target_link_libraries(fa_mic_check PRIVATE fatzaudio)
target_compile_options(fa_mic_check PRIVATE ${FA_WARNINGS})
if(MSVC)
    target_compile_definitions(fa_mic_check PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# BASS comparison harness (tools/bass-compare/README.md).
add_executable(fa_bench bass-compare/fa_bench.c)
target_link_libraries(fa_bench PRIVATE fatzaudio)
target_compile_options(fa_bench PRIVATE ${FA_WARNINGS})
# Records what reaches the sound card, for the play commands; part of neither
# library (miniaudio on its own).
add_executable(loop_rec bass-compare/loop_rec.c bass-compare/loop_rec_miniaudio.c)
target_include_directories(loop_rec PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniaudio)
target_compile_options(loop_rec PRIVATE ${FA_WARNINGS})
if(MSVC)
    set_source_files_properties(bass-compare/loop_rec_miniaudio.c PROPERTIES COMPILE_OPTIONS "/W0")
    target_compile_definitions(loop_rec PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    set_source_files_properties(bass-compare/loop_rec_miniaudio.c PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(loop_rec PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(NOT APPLE AND NOT WIN32)
        target_link_libraries(loop_rec PRIVATE m)
    endif()
endif()
if(WIN32)
    target_link_libraries(loop_rec PRIVATE psapi)
endif()

if(WIN32)
    target_link_libraries(fa_bench PRIVATE psapi)
    # Loads bass.dll and its add-ons at run time (64-bit, like the legacy DLLs).
    add_executable(bass_bench bass-compare/bass_bench.c)
    target_link_libraries(bass_bench PRIVATE psapi)
    target_compile_options(bass_bench PRIVATE ${FA_WARNINGS})
    if(MSVC)
        target_compile_definitions(fa_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_definitions(bass_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
