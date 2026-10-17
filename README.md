# FatzAudio

FatzAudio is a cross-platform audio engine with a plain C API (`include/fatzaudio/fatzaudio.h`),
built as a shared library (`fatzaudio.dll` / `libfatzaudio.so` / `libfatzaudio.dylib`).
The current backend uses [miniaudio](https://github.com/mackron/miniaudio) (vendored in
`third_party/miniaudio`) for device I/O and WAV/MP3/FLAC decoding.

Status: early scaffold. Implemented: engine, device enumeration/open/start/stop/volume/latency,
file and memory streams, attach/play/pause/stop/seek/position/state, volume and pan, decode-only
reads. All other API functions are exported and return `FA_ERROR_NOT_IMPLEMENTED`.

## Build

Requires CMake 3.21+ and a C11 compiler.

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Windows with Visual Studio 2026 add `-G "Visual Studio 18 2026" -A x64` to the first command.
Use `-DFA_BUILD_TESTS=OFF` to skip the tests. Outputs land in `build/bin`.

## Design

See [fatzaudio-design.md](fatzaudio-design.md) for the architecture and roadmap.
`legacy/` holds third-party reference binaries and is not part of the repository.
