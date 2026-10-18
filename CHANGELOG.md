# Changelog

FatzAudio follows [semantic versioning](https://semver.org) for its C ABI and the .NET package, which share one
version number (`FA_VERSION_*` in `include/fatzaudio/fatzaudio.h`; `fa_version()` at run time;
`AudioEngine.NativeVersion` in .NET).

- Before 1.0, a **minor** bump (0.2 to 0.3) may break the API or ABI; every break is listed below under
  "Breaking". A **patch** bump never does.
- From 1.0 on, only a major bump breaks.
- The .NET binding refuses to start against a native library of another major.minor version
  (the first call into the library throws `InvalidOperationException`; `AudioEngine.NativeVersion` still
  works), so a mismatched `fatzaudio.dll` is caught at once instead of crashing later. On Linux the SONAME
  carries the minor version too (`libfatzaudio.so.0.2`).
- Additions (new functions, new enum values at the end, new fields at the end of a struct that has a
  `struct_size`) are not breaking.

## 0.2.0 (2026-10-18)

The first packaged version (NuGet `FatzAudio` with `win-x64` and `linux-x64` natives).

### Breaking (against 0.1.0 builds from main)

- `fa_dsp_callback` gained a `sample_rate` argument: `(frames, count, channels, sample_rate, user)`.
  .NET: `DspProcessor(Span<float> samples, int channels, int sampleRate)`. User DSP never worked before
  this version (it returned `FA_ERROR_NOT_IMPLEMENTED`).
- `fa_ffmpeg_load` / `fa_ffmpeg_get_info` and .NET `FFmpeg.Load` / `FFmpeg.Info` were removed: FatzAudio
  never loads FFmpeg. Formats only FFmpeg decodes (DTS, TrueHD, WMA off Windows) return
  `FA_ERROR_UNSUPPORTED_FORMAT`; decode them with your own FFmpeg and use the packet hand-off.
- The MinGW build is now named `fatzaudio.dll` (was `libfatzaudio.dll`), like the MSVC build.
- The Linux `.so` exports only `fa_*` symbols and links the C++ runtime in.
- The Linux SONAME is now `libfatzaudio.so.0.2` (was `libfatzaudio.so.0`).

### Added

- Equalizer: up to 16 peak or shelf bands per stream, 13 presets, bypass (`fa_stream_set_eq`,
  `fa_eq_get_preset`; .NET `SetEqualizer`).
- User DSP: `fa_stream_add_dsp` / `fa_stream_remove_dsp` (.NET `AddDsp`).
- Looping on or off while playing (`fa_stream_set_loop`, `FA_EVENT_LOOPED`; .NET `Loop`, `Looped`).
- Mute for streams and devices that keeps the volume (`fa_stream_set_mute`, `fa_device_set_mute`; .NET `Muted`).
- `fa_stream_get_clock` is lock-free for streams on a device and file streams, safe from a render loop.
- Tempo and rate on push and packet streams; the clock keeps the app's timestamps and runs at the speed heard.
- Seeking without waiting: `fa_stream_seek_async` (.NET `RequestSeek`), for seek bars and web streams.
- Tag reader with cover art and file properties (`fa_tags_*`; .NET `Tags`); bitrate, file size and audio
  size in `fa_stream_info`.
- MIDI: DLS soundfonts and the operating system's own soundfont as the default.
- MIDI: SF2 instrument attenuation follows the SoundFont spec (quiet instruments in some SF2 banks were
  up to 10x too loud).
- Levels and spectrum (FFT) for streams and devices (`fa_stream_get_levels`, `fa_stream_get_spectrum`,
  `fa_device_get_levels`, `fa_device_get_spectrum`; .NET `GetLevels`, `GetSpectrum`).
- Everything else built so far: decoding (MP3, AAC, ALAC, FLAC, Opus, Vorbis, AC-3/E-AC-3, WMA on Windows,
  MIDI), web streams and HLS, crossfades, markers, buses, R128 normalization, tempo and pitch, CD+G,
  disc playback, encoding (WAV, FLAC, MP3 via LAME) and the FFmpeg packet hand-off. See
  [docs/features](docs/features/README.md).

## 0.1.0

Development builds from main before packaging. Not released.
