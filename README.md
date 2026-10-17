# FatzAudio

FatzAudio is a cross-platform audio engine with a plain C API (`include/fatzaudio/fatzaudio.h`),
built as a shared library (`fatzaudio.dll` / `libfatzaudio.so` / `libfatzaudio.dylib`).
The current backend uses [miniaudio](https://github.com/mackron/miniaudio) (vendored in
`third_party/miniaudio`) for device I/O, and its embedded dr_wav/dr_flac/dr_mp3 behind
FatzAudio's own decoder registry. Time stretching uses
[Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (MIT, vendored in
`third_party/signalsmith`, wrapped in `src/stretch.cpp`, the library's only C++ file).

Status: core engine (phase 1), loudness normalization (phase 2) and crossfades (phase 3) done. Decoding runs ahead on worker threads into lock-free ring
buffers; the audio callback only mixes. Implemented: engine (with a silent null output for tests),
device enumeration/open/start/stop/volume/latency, streams from files, memory and custom I/O
with format detection from content (WAV/AIFF/RF64/W64, FLAC, MP3), attach/detach/move between
devices keeping position, play/pause/stop/seek/position/clock/state, volume and pan with
ramps on seven curves (including your own points), sample-accurate scheduled starts on the device clock,
crossfades, queues with gapless playback, cue/fade/mix-point markers, mix buses, a per-stream channel
matrix, ended/ramp-done/marker/started events, decode-only reads, and EBU R128
loudness normalization on every stream (tags, content-keyed cache, parallel audio-only background
scans, gain that glides to the exact value, true-peak output limiter), tempo, pitch and rate on every stream, and recording and encoding to WAV, FLAC (both built in) and MP3 (LAME
loaded at runtime) from a device, a playing stream, a file or your own samples. Functions not implemented yet return `FA_ERROR_NOT_IMPLEMENTED`; the
[docs index](docs/README.md) tracks the status of each feature.

## Build

Requires CMake 3.21+ and a C11 and C++11 compiler.

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
