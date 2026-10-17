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

# BASS comparison harness (tools/bass-compare/README.md).
add_executable(fa_bench bass-compare/fa_bench.c)
target_link_libraries(fa_bench PRIVATE fatzaudio)
target_compile_options(fa_bench PRIVATE ${FA_WARNINGS})
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
