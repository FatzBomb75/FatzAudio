# FatzAudio

FatzAudio is a cross-platform audio engine with a plain C API (`include/fatzaudio/fatzaudio.h`),
built as a shared library (`fatzaudio.dll` / `libfatzaudio.so` / `libfatzaudio.dylib`).
The current backend uses [miniaudio](https://github.com/mackron/miniaudio) (vendored in
`third_party/miniaudio`) for device I/O, and its embedded dr_wav/dr_flac/dr_mp3 behind
FatzAudio's own decoder registry. Time stretching uses
[Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (MIT, vendored in
`third_party/signalsmith`, wrapped in `src/stretch.cpp`, the library's only C++ file).
Opus and Vorbis decode with [libopus](https://opus-codec.org/) 1.5.2 and
[libvorbis](https://xiph.org/vorbis/) 1.3.7 (with the later fixes on master) and libogg 1.3.6 (BSD, vendored in `third_party/opus`,
`third_party/vorbis` and `third_party/ogg`, see `third_party/xiph.cmake`), statically linked.
MIDI is synthesized with [TinySoundFont](https://github.com/schellingb/TinySoundFont)
(TinySoundFont MIT, TinyMidiLoader zlib, vendored with hardening patches in
`third_party/tinysoundfont`). No SoundFont is bundled: the application loads its own.
Tracker modules (MOD, XM, S3M, IT, MPTM and more) play through [libopenmpt](https://lib.openmpt.org) 0.8.9
(BSD-3-Clause, vendored in `third_party/libopenmpt`, see `third_party/libopenmpt.cmake`), statically linked.

## Status

The engine is feature complete apart from checks that need real devices and an app that uses it. Everything the legacy BASS stack gave the app (bass.dll and its add-ons) is covered by this one library, and more:

- **Playback:** streams from files, memory, your own reader, URLs (HTTP/HTTPS, Icecast/Shoutcast radio, HLS), pushed PCM, packets or frames from FFmpeg, your own callback, a microphone or an audio CD, with format detection from content (WAV/AIFF/RF64/W64, FLAC, MP3, Ogg Opus/Vorbis/FLAC, AAC, AC-3/E-AC-3, ALAC, MP4/MKV/WebM audio, CAF, WMA on Windows, MIDI with a SoundFont, tracker modules).
- **Control:** ramps on seven curves, sample-accurate scheduled starts on the device clock, crossfades, gapless queues, markers, buses, a channel matrix, tempo/pitch/rate and reverse playback on every stream, an equalizer and user DSP, and typed events.
- **Loudness and analysis:** EBU R128 normalization on every stream, loudness meters, levels and spectrum, beat grid and BPM, key, waveform and silence detection.
- **Karaoke and DJ:** CD+G and MIDI/.kar lyric surfaces for any renderer, a two-deck DJ mixer with beat sync, auto-mix, microphones and headphone cue.
- **Recording and encoding:** WAV and FLAC built in, MP3 through LAME loaded at runtime; capture from a device, including Windows loopback.
- **Platforms:** Windows, macOS, Linux, iOS and Android, with a .NET binding for `net8.0` and `net10.0`.

Decoding runs ahead on worker threads into lock-free ring buffers; the audio callback only mixes. A few features are deliberately not built (3D audio, Dolby passthrough, Opus and AAC encoders, MIDI device input, more than two DJ decks). The [docs index](docs/README.md) says which, with the status of every feature, and the [CHANGELOG](CHANGELOG.md) lists every version. To start using it: [Getting started](docs/getting-started.md).

## Build

Requires CMake 3.21+ and a C11 and C++17 compiler (libopenmpt needs C++17).

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Windows with Visual Studio 2026 add `-G "Visual Studio 18 2026" -A x64` to the first command.
Use `-DFA_BUILD_TESTS=OFF` to skip the tests. Outputs land in `build/bin`.

## Design

See the [documentation](docs/README.md). [fatzaudio-design.md](fatzaudio-design.md) is the early design draft, kept as background.

## Licenses

Third-party code built into the library, and what is loaded at run time, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
