# libopus, libvorbis and libogg (BSD), linked into fatzaudio as static libraries
# of position-independent code: plain C, float build, no SIMD dispatch, nothing
# exported from the shared library.

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/opus/sources.txt FA_OPUS_FILES)
list(TRANSFORM FA_OPUS_FILES PREPEND ${CMAKE_CURRENT_LIST_DIR}/opus/)
add_library(fa_opus STATIC ${FA_OPUS_FILES})
target_include_directories(fa_opus PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/opus/include
    ${CMAKE_CURRENT_LIST_DIR}/opus/celt
    ${CMAKE_CURRENT_LIST_DIR}/opus/silk
    ${CMAKE_CURRENT_LIST_DIR}/opus/silk/float
    ${CMAKE_CURRENT_LIST_DIR}/opus/src)
# OPUS_EXPORT empty: the library's own functions stay internal to fatzaudio.
target_compile_definitions(fa_opus PRIVATE OPUS_BUILD "OPUS_EXPORT=")
# Scratch buffers on the stack: variable-length arrays, or _alloca where the
# compiler has no VLAs (MSVC).
if(MSVC)
    target_compile_definitions(fa_opus PRIVATE USE_ALLOCA)
else()
    target_compile_definitions(fa_opus PRIVATE VAR_ARRAYS HAVE_LRINTF HAVE_LRINT)
endif()

add_library(fa_vorbis STATIC
    ${CMAKE_CURRENT_LIST_DIR}/ogg/src/bitwise.c
    ${CMAKE_CURRENT_LIST_DIR}/ogg/src/framing.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/mdct.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/smallft.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/block.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/envelope.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/window.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/lsp.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/lpc.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/analysis.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/synthesis.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/psy.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/info.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/floor1.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/floor0.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/res0.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/mapping0.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/registry.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/codebook.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/sharedbook.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/lookup.c
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib/bitrate.c)
target_include_directories(fa_vorbis PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/lib
    ${CMAKE_CURRENT_LIST_DIR}/vorbis/include
    ${CMAKE_CURRENT_LIST_DIR}/ogg/include)
if(MSVC)
    target_compile_definitions(fa_vorbis PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_compile_definitions(fa_vorbis PRIVATE HAVE_ALLOCA_H)   # Windows: malloc.h has it
endif()

foreach(t fa_opus fa_vorbis)
    # Third-party code: its own warnings are not ours to fix.
    if(MSVC)
        target_compile_options(${t} PRIVATE /W0)
        set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(${t} PRIVATE -w)
    endif()
endforeach()

target_include_directories(fa_opus INTERFACE ${CMAKE_CURRENT_LIST_DIR}/opus/include)
target_include_directories(fa_vorbis INTERFACE ${CMAKE_CURRENT_LIST_DIR}/vorbis/include ${CMAKE_CURRENT_LIST_DIR}/ogg/include)
if(UNIX)
    target_link_libraries(fa_opus INTERFACE m)
    target_link_libraries(fa_vorbis INTERFACE m)
endif()
