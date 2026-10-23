# Mic latency spike: a standalone tool, not part of the library build.
#   cmake -S tools/mic-latency -B build-mic && cmake --build build-mic --config Release
cmake_minimum_required(VERSION 3.16)
project(mic_latency C)
add_executable(mic_latency mic_latency.c)
target_include_directories(mic_latency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/miniaudio)
if(MSVC)
  target_compile_options(mic_latency PRIVATE /W3)
  target_compile_definitions(mic_latency PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(mic_latency PRIVATE Threads::Threads m ${CMAKE_DL_LIBS})
endif()
