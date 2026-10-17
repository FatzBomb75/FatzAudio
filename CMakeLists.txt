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

add_library(fatzaudio SHARED
    src/engine.c
    src/device.c
    src/stream.c
    src/stubs.c
    src/miniaudio_impl.c
    src/fa_internal.h)

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
if(UNIX)
    target_link_libraries(fatzaudio PRIVATE m)
endif()

if(MSVC)
    target_compile_options(fatzaudio PRIVATE /W3)
    set_source_files_properties(src/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    target_compile_options(fatzaudio PRIVATE -Wall -Wextra)
    set_source_files_properties(src/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(FA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
