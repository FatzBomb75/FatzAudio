# libopenmpt (BSD) for tracker modules (MOD, S3M, XM, IT and the other formats it
# reads), linked into fatzaudio as a static library of position-independent
# C++17. No external libraries: no zlib, mpg123 or Vorbis (so no MO3 files with
# compressed samples), nothing exported from the shared library
# (FA_OPENMPT_STATIC, see third_party/libopenmpt/README.md).

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/libopenmpt/sources.txt FA_OPENMPT_FILES)
list(TRANSFORM FA_OPENMPT_FILES PREPEND ${CMAKE_CURRENT_LIST_DIR}/libopenmpt/)
add_library(fa_openmpt STATIC ${FA_OPENMPT_FILES})
set_target_properties(fa_openmpt PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(fa_openmpt PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/libopenmpt
    ${CMAKE_CURRENT_LIST_DIR}/libopenmpt/src
    ${CMAKE_CURRENT_LIST_DIR}/libopenmpt/common)
target_compile_definitions(fa_openmpt PRIVATE LIBOPENMPT_BUILD
    PUBLIC FA_OPENMPT_STATIC)
if(MSVC)
    target_compile_options(fa_openmpt PRIVATE /W0 /EHsc /GR /bigobj /utf-8)
    target_compile_definitions(fa_openmpt PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    set_property(TARGET fa_openmpt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # Its own warnings are not ours to fix; it needs exceptions and RTTI (the
    # Android NDK turns both off by default).
    target_compile_options(fa_openmpt PRIVATE -w -fexceptions -frtti)
    # Size over speed (it renders far faster than real time either way), and each
    # function in its own section so the linker drops what fatzaudio never calls.
    target_compile_options(fa_openmpt PRIVATE $<$<NOT:$<CONFIG:Debug>>:-Os> -ffunction-sections -fdata-sections)
    if(MINGW)
        target_compile_options(fa_openmpt PRIVATE -Wa,-mbig-obj)
    endif()
endif()
# The C API headers for src/dec_tracker.c (libopenmpt/libopenmpt.h).
target_include_directories(fa_openmpt INTERFACE ${CMAKE_CURRENT_LIST_DIR}/libopenmpt)
if(UNIX)
    target_link_libraries(fa_openmpt INTERFACE m)
endif()
