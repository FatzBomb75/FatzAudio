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

- MIDI song credits: `fa_stream_get_midi_credits` (.NET `AudioStream.MidiCredits`) gives title, artist, writer,
  lyricist, year, copyright, key, tempo, time signature and other header lines, gathered from `.kar` `@T`/`@I` lines
  and labelled text events. See [docs/features/midi.md](docs/features/midi.md#song-credits).
- MIDI text code page: Shift-JIS (Japanese) text is detected and decoded; `fa_open_options.text_code_page`
  (.NET `StreamOptions.TextCodePage`) forces UTF-8, Windows-1252 or Shift-JIS.
- .NET: `AudioStream.GetLevels(Span<float> peak, Span<float> rms)` and `OutputDevice.GetLevels(peak, rms)`, which
  write into your own buffers and allocate nothing (a VU meter at 60 Hz made about 180 small objects a second).
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
- MIDI note velocity now follows the SoundFont 2 default curve, 40·log10(velocity/127) dB (velocity 64 is 11.9 dB
  down, was 6 dB), which is what FluidSynth and DLS synths play. Songs keep their loudest notes and gain dynamics.
- MIDI: a program change to a program the soundfont lacks (in the bank and in bank 0) plays program 0 instead of
  keeping the channel's previous instrument. DLS instruments are no longer cut at 128 regions.
- .NET: `AudioStream.Info` no longer allocates on every read: the last `StreamInfo` is reused while the native values
  are unchanged (a changed duration, length or bitrate still gives a fresh one), and `GetSpectrum(int)` on streams
  and devices shares one code path with the `Span<float>` overload. For library scans, `Tags.Read(path,
  TagReadOptions.NoPictures)` skips the cover art (the default still copies it).

- Tracker modules no longer use the Amiga resampler by default (`FA_TRACKER_AMIGA_RESAMPLER` 0, .NET
  `TrackerAmigaResampler` false), as in libopenmpt: it cost about 19 times the CPU on MOD files. MOD files now sound
  cleaner and brighter; set it to 1 for the Paula sound.
- Less CPU and memory, same sound (memory and speed review): device-rate conversion (44.1 kHz on a 48 kHz device)
  works its kernels out once per ratio, about five times cheaper (a resampled stream went from about 3.4% to
  0.6% of one core); the output limiter measures true peaks several frames at a time and skips quiet blocks
  (bit-identical, about 2x); the equalizer runs all bands frame by frame, left and right together (bit-identical,
  about 2x); Ogg Opus streams no longer hold a 184 KB buffer; FLAC in Matroska, MP4 and Ogg holds output for its
  own channels (not 8) and stops allocating per packet (about 190 KB less per stream); a resampled stream keeps
  32 KB less history; web streams no longer keep a 256 KB read-ahead on top of their download window; idle
  engine workers wake 20 times less; Windows (MSVC) builds read atomics with plain loads instead of locked
  compare-exchanges. .NET: `HasCdg` and the `Tags` lookups (`Cover`, the indexer) no longer allocate.
- Less memory per stream, same sound (memory review, part 2): a web stream that can't seek (radio, HLS, a server
  without byte ranges) keeps only the 2 MB ahead of playback, not a 4 MB window (about 2.1 MB less per radio
  stream); push and other forward-only streams reserve the tempo chain's half second of history only once tempo,
  pitch or rate is used (188 KB at 48 kHz stereo); decoded audio is converted straight into the decode-ahead
  buffer (16 KB less); a stream's own state is 14 KB instead of 24 KB (its tempo map, equalizer, push timestamps
  and the unused true-peak meter are made only when used); levels and spectrum keep 1 s plus the device's latency
  and callbacks instead of about 2.7 s (512 KB instead of 1 MiB at 44.1 and 48 kHz on most devices); HE-AAC keeps
  about 75 KB less SBR state per stereo element and 190 KB less for HE-AACv2 (parametric stereo); CD+G checkpoints
  take half the memory.
- HE-AAC decodes about 35% faster (its QMF analysis bank uses a fast DCT-IV; 16 KB less per decoder) and ALAC
  up to 30% faster (faster bit reading). The output is unchanged.
- Less engine CPU, same output: the mixer mixes a stream whose gains hold steady in one pass instead of every 16
  frames (bit-identical; 16 playing streams mix in about half the time); idle decode workers no longer each poll
  every 5 ms (one polls and wakes the others when there is work: about 320 wake-ups a second instead of 200 per
  worker); device-rate conversion of 5.1 and 7.1 sums its channels with SSE2, about 3x faster (per channel now
  cheaper than stereo); CD+G smooth scaling redraws 1080p 2.5 to 6 times faster (pixel-identical).
- Faster codecs, same output (bit-identical on x86-64; on ARM, where compilers fuse multiply-adds differently, within
  float rounding): the inverse MDCT's rotations run four at a time (SSE on x86, NEON on ARM), about 13%
  off AC-3 and AAC decoding; HE-AAC's high band works out its linear prediction in one pass and keeps its filter
  bank delay lines as rings instead of moving them every slot (with the MDCT, HE-AAC about 25% faster); ALAC sums
  its predictor in 32 bits where that gives the same result, as Apple's decoder does (about 10%); the FLAC encoder
  is about 1.7x faster with byte-identical files (32-bit residuals, a table CRC, all autocorrelation lags at once,
  the 5-bit Rice method skipped where it cannot win); the MIDI chorus turns its LFO instead of calling `sin` four
  times a frame (about 10% off MIDI rendering, within 4e-9 of before).
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

- CD+G: copies of a memory preset (repeat count above 0) no longer clear the screen again when a border or tiles
  were drawn between the copies (reported by FatzBomb.MediaControl).
- CD+G from discs: packs that fail their P/Q parity are corrected (one damaged symbol) or dropped, instead of
  drawing garbage tiles from a scratched disc. Drives that return no parity are unchanged.
- CD+G: `fa_cdg_surface_update`, `fa_cdg_surface_update_at` and `fa_cdg_surface_destroy` could touch freed memory
  when the surface's stream was closed on another thread at the same moment; the close now waits for them.
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
