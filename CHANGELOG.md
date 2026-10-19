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

## Unreleased

### Added

- Transient mode for tempo and pitch: `FA_STRETCH_TRANSIENTS` (.NET `StretchQuality.Transients`) plays the original
  audio around each onset, so drum hits have no pre-echo (-50 dB instead of -25 dB) and stay on time. See
  [docs/features/tempo-pitch.md](docs/features/tempo-pitch.md#transient-mode).
- Tracker modules: MOD, XM, S3M, IT, MPTM and the other formats libopenmpt reads (MTM, UMX, 669, AMF, DBM, MED,
  PSM and more) open with the ordinary `fa_stream_open_*` calls and play once (loop with the stream's loop
  toggle). libopenmpt 0.8.9 is built in (`third_party/libopenmpt`, BSD-3-Clause), so a C++17 compiler is
  now required to build and the Linux `.so` grows by about 2 MiB. See [docs/features/trackers.md](docs/features/trackers.md).
- Module information and names (`fa_stream_get_tracker_info`, `fa_stream_get_tracker_text`; .NET `TrackerInfo`),
  the time of an order and row and seeking to it (`fa_stream_get_tracker_time`, `fa_stream_seek_tracker`; .NET
  `GetTrackerTime`, `SeekTracker`), live interpolation, stereo separation, volume ramping and Amiga resampler
  settings (`fa_stream_set_tracker_param`; .NET `TrackerInterpolation`, `TrackerStereoSeparation`,
  `TrackerVolumeRamping`, `TrackerAmigaResampler`) and channel mute (`fa_stream_set_tracker_channel_mute`;
  .NET `SetTrackerChannelMuted`).
- Tracker subsongs: a module's subsongs are the stream's audio tracks. `fa_open_options.audio_track` (.NET
  `StreamOptions.AudioTrack`) plays subsong n, `fa_stream_info.audio_track_count` is the count, a subsong past the
  last gives `FA_ERROR_NOT_FOUND`; `fa_tracker_info.subsong` (at the end, older shorter structs are still filled up
  to `duration`; .NET `TrackerInfo.Subsong`). The restart position and duration are the playing subsong's, and
  `fa_stream_get_tracker_time` / `fa_stream_seek_tracker` return `FA_ERROR_NOT_FOUND` for rows of another subsong.
  This needed a second local libopenmpt patch (`third_party/libopenmpt/README.md`).
- `fa_stream_get_tracker_position`: the order and row heard now, within about 2 ms of the row start, lock-free
  (.NET `AudioStream.TrackerPosition`).
- Tracker row events: `FA_TRACKER_ROW_EVENTS` (0 off, 1 every row, 2 first row of each order) raises
  `FA_EVENT_TRACKER_ROW` (value = order << 16 | row), best effort like markers (.NET `TrackerRowEvents`,
  `TrackerRowReached`).
- Tags: a module's title, artist and song message (`FA_TAG_FORMAT_TRACKER`; .NET `TagFormat.Tracker`).

### Changed

- A tempo, pitch or rate ramp's `FA_EVENT_RAMP_DONE` now comes when the ramp's end is heard, not up to the
  decode-ahead latency (150 ms) earlier when it was decoded, matching volume ramps. See
  [docs/features/tempo-pitch.md](docs/features/tempo-pitch.md).
- Playback at another rate than the device's (a 44.1 kHz file on a 48 kHz device) is resampled with the windowed-sinc
  resampler that rate and pitch use, instead of miniaudio's linear one: about -90 dB of aliasing and imaging, and
  high frequencies no longer dulled. See [docs/features/playback.md](docs/features/playback.md).
- Frames pushed at another rate than their push stream's (`fa_stream_push_frame`), and tracks whose rate changes
  partway (chained Ogg), are resampled with the same windowed-sinc resampler (a 17 kHz tone pushed at 44.1 kHz into
  a 48 kHz stream: its image went from 8 dB to 126 dB below the tone).

### Fixed

- Transient mode (`FA_STRETCH_TRANSIENTS`) now also removes the pre-echo of a hit in the last ~100 ms of a track
  (-49 dB instead of -20 dB).

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
- Device changes: an unplugged device is reopened on the default with its streams playing on, and
  `FA_EVENT_DEVICE_CHANGED` (.NET `DeviceChanged`) is raised for reroutes and reopens.
- `fa_stream_get_clock` is lock-free for streams on a device and file streams, safe from a render loop.
- Tempo and rate on push and packet streams; the clock keeps the app's timestamps and runs at the speed heard.
- Seeking without waiting: `fa_stream_seek_async` (.NET `RequestSeek`), for seek bars and web streams.
- Tag reader with cover art and file properties (`fa_tags_*`; .NET `Tags`); bitrate, file size and audio
  size in `fa_stream_info`.
- MIDI: DLS soundfonts and the operating system's own soundfont as the default.
- MIDI: SF2 instrument attenuation follows the SoundFont spec (quiet instruments in some SF2 banks were
  up to 10x too loud).
- MIDI: GM/GM2/GS/XG SysEx (resets, drum parts on any channel, XG and GM2 drum banks, master volume and
  tuning); notes held across a seek start again at the target; tempo changes and time and key signatures
  (`fa_stream_get_midi_marks`; .NET `MidiMarks`).
- MIDI: SF3 soundfonts (SoundFont 2 with Ogg Vorbis samples) load with `fa_soundfont_open_path` /
  `fa_soundfont_open_memory` (.NET `SoundFont.FromFile` / `FromMemory`); samples are decoded to 16-bit at load.
- MIDI: built-in reverb and chorus, on by default and driven by CC91, CC93 and the soundfont's effect sends;
  `fa_stream_set_midi_effects` / `fa_stream_get_midi_effects` (.NET `AudioStream.MidiEffects`) turn them off.
- Levels and spectrum (FFT) for streams and devices (`fa_stream_get_levels`, `fa_stream_get_spectrum`,
  `fa_device_get_levels`, `fa_device_get_spectrum`; .NET `GetLevels`, `GetSpectrum`).
- ZIP64 archives (over 4 GB, or more than 65,535 entries) open like any other karaoke ZIP.
- Everything else built so far: decoding (MP3, AAC, ALAC, FLAC, Opus, Vorbis, AC-3/E-AC-3, WMA on Windows,
  MIDI), web streams and HLS, crossfades, markers, buses, R128 normalization, tempo and pitch, CD+G,
  disc playback, encoding (WAV, FLAC, MP3 via LAME) and the FFmpeg packet hand-off. See
  [docs/features](docs/features/README.md).

## 0.1.0

Development builds from main before packaging. Not released.
