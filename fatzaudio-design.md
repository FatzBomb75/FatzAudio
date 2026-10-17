# FatzAudio design (draft 2)

Goal: one native library per platform (Windows, macOS, iOS, Android, Linux), one source tree, one C API, each capability implemented once. It must match or beat the legacy BASS stack (bass.dll 2.4.18 + 9 add-ons, see `bass-dll-analysis.md`) on measured behaviour.

Code location: `C:\development\nuget\fatzaudio` (not under git yet; it will be initialised when the project is scaffolded).

## 1. Requirements (from Keith, 2026-10-17)

| # | Requirement | Priority |
|---|---|---|
| R1 | Decoding, the most robust part: everything legacy covered **plus E-AC-3 and Dolby/DTS family** | Must |
| R2 | Encoding, limited set, **including MP3** | Should |
| R3 | Audio from video files natively, and **direct hand-off from an app's own FFmpeg pipeline with no extra conversion and no manual sync** | Must |
| R4 | Web streams | Must |
| R5 | Multiple streams with crossfade: **matrix style, marker points (start/end), linear/bezier/other curves** | Must |
| R6 | Normalization on all streams, **fast on large video files** | Must |
| R7 | One binary per platform, no overlapping features | Must |
| R8 | Fast; never worse than BASS on the comparison suite | Must |
| R9 | **CD+G** karaoke graphics from files and disc, synced natively, rendered to any surface | Should |
| R10 | **Disc playback** (audio CD) | Should |
| R11 | FFmpeg is **never bundled**; it stays its own library, FatzAudio is only compatible with it | Decided |
| R12 | **Tempo (pitch kept) and pitch shift of clearly better quality** than NAudio/SoundTouch, at least BASS quality | Must |
| R13 | **Native multi-threaded processing**; buffering like BASS (decode-ahead) for smooth, glitch-free output | Must |
| R14 | Input and output device selection, always defaulting to the system's primary device | Must |

Keith can point to his current implementations (FFmpeg+BASS sync, normalization analysis, CDG processing) when each part is built.

## 2. Shape of the library

```
            C API (fa_*.h)   ← .NET (P/Invoke), Swift, Kotlin/JNI, C/C++
                 │
   ┌─────────────┼─────────────────────────────────────────┐
   │  Engine: devices · mixer · voices · master clock      │
   │  ┌──────────┐ ┌──────────┐ ┌──────────────────┐        │
   │  │ Source   │→│ Decoder  │→│ Voice chain      │→ Mixer → Limiter → Device
   │  │ file/mem │ │ registry │ │ resample         │        │
   │  │ HTTP/HLS │ │ (one per │ │ normalize gain   │        │
   │  │ pipe     │ │  format) │ │ envelopes/xfade  │        │
   │  │ FFmpeg   │ │          │ │ channel matrix   │        │
   │  │  frames  │ │          │ │ FX · user DSP    │        │
   │  │ CD drive │ │          │ │ meters           │        │
   │  └──────────┘ └──────────┘ └──────────────────┘        │
   │  Side streams on the same clock: CD+G renderer        │
   └───────────────────────────────────────────────────────┘
```

Rules:
- **One `fa_open()`** for every source. Format and container detected by content.
- **One decoder per format per platform**, picked at build time.
- **One processing chain** per voice: normalization, envelopes, matrix, FX, DSP.
- **One master clock**; video and CD+G follow the audio, never the other way round.
- No per-thread "current device" or last-error state; explicit handles, returned error codes.

## 3. Feature design

### R1 Decoding (robustness first)

| Format | Implementation |
|---|---|
| WAV/AIFF/W64/RF64, MP3, FLAC | dr_wav, dr_mp3, dr_flac (PD/MIT-0), hardened (see below) |
| Ogg Vorbis, Opus | libvorbis/stb_vorbis, libopus + opusfile (BSD) |
| AAC-LC/HE-AAC/xHE, ALAC | OS codec (Media Foundation, AudioToolbox, MediaCodec); FFmpeg bridge on Linux |
| **AC-3, E-AC-3 (DD+), incl. Atmos-in-DD+ core** | Built-in decoder written from the public specs (src/ac3.c), up to 7.1; FFmpeg only for the rare AHT/enhanced-coupling frames; **passthrough** (IEC 61937 bitstream over HDMI/S/PDIF) planned |
| **TrueHD, DTS / DTS-HD** | Passthrough + FFmpeg bridge. No OS decodes these reliably |
| WMA | Media Foundation on Windows, FFmpeg bridge elsewhere |
| MIDI | TinySoundFont + TinyMidiLoader (SF2) |
| Tracker modules | libopenmpt (build option) |

Done: the built-in AC-3/E-AC-3 decoder makes Dolby Digital work everywhere without FFmpeg ([dolby.md](docs/features/dolby.md)). The patent check for E-AC-3 is still open (AC-3 patents have expired). Atmos object rendering and TrueHD stay passthrough/FFmpeg, since they're proprietary.

Robustness means:
- Every decoder is fuzzed (libFuzzer/AFL corpora per format) in CI.
- Corrupt frames are skipped and resynced instead of failing the stream; errors become events, not stops.
- Encoder delay/padding is honoured (LAME/iTunes/Opus pre-skip) so lengths and gapless are exact.
- Sample-accurate seeking with index building for VBR files without seek tables.

### R2 Encoding (limited)

WAV, FLAC (libFLAC), Opus (libopus) built in. AAC via OS encoders. **MP3:** MP3 patents have expired, but LAME (the reference-quality encoder) is LGPL, so it follows the FFmpeg model: loaded at runtime if the app ships `lame` (libmp3lame), never bundled. Windows also gets Media Foundation's MP3 encoder as a fallback.

### R3 Audio from video files, and the FFmpeg hand-off

**Native:** MP4/MOV/M4V and Matroska/WebM demuxed in the library, skipping video packets without decoding them. Track enumeration and selection, positions in presentation timestamps.

**FFmpeg compatibility, no bundling.** The goal is to replace "decode to PCM → convert → push to BASS → fight the sync" with a direct path:

1. **Packet hand-off (fastest).** The app's FFmpeg demuxer already reads the file for video. It passes the *compressed audio packets* (data, size, pts, time base, codec parameters) to `fa_stream_push_packet()`. FatzAudio decodes them itself where it has a decoder, or through the app's libavcodec when it doesn't. The file is read once, nothing gets converted twice, and FatzAudio owns the audio clock.
2. **Frame hand-off.** If the app already decodes audio, it passes the decoded frame (planar or interleaved, any sample format, pts) to `fa_stream_push_frame()`. FatzAudio takes FFmpeg's native layouts as-is (planar float etc.), so there's no swr conversion in the app.
3. **Clock out.** `fa_stream_get_clock()` returns the presentation time **of what's leaving the speaker right now** (device latency included). The video renderer shows the frame for that time, so sync is automatic. Speed changes, pauses and seeks stay consistent because there's only one clock.

ABI safety: the push APIs take a small FatzAudio struct (pointers, sizes, format, pts, time base), not FFmpeg structs, so FFmpeg can update independently. An optional thin helper maps `AVPacket`/`AVFrame`/`AVCodecParameters` for the FFmpeg major versions in use, detected at runtime.

### R4 Web streams

HTTP/HTTPS with range seeking, prebuffer, stall/resume events, auto-reconnect; Icecast/Shoutcast metadata; HLS (m3u8, TS and fMP4 segments). Transport through OS HTTP stacks (WinHTTP, NSURLSession, Android via JNI); libcurl loaded at runtime on Linux.

### R5 Crossfade: matrix, markers, curves

- **Markers per stream:** cue-in, cue-out, fade-in start/end, fade-out start/end, mix point (when the next track starts). Set by the app, read from tags/cue sheets, or auto-detected (silence/level thresholds).
- **Curves:** linear, equal-power, logarithmic, exponential, S-curve, and **cubic bezier** with two control points; custom curves as point lists. Any attribute (volume, pan, rate, filter, send level) can be automated with these envelopes, not just volume.
- **Matrix:** each voice has an input→output channel matrix (routing, downmix/upmix, per-channel gain), and the mixer supports N decks/buses mixing into M outputs, so DJ-style "deck A → bus 1, deck B → bus 2, crossfader across buses" layouts are configuration, not code.
- **Scheduling:** everything is sample-accurate on the master clock. Queue mode starts the next item at its mix point automatically; gapless is a zero-length fade.

### R6 Normalization (fast on large video files)

- Loudness to **ITU-R BS.1770-4 / EBU R128** (libebur128, MIT): integrated loudness, LRA, true peak.
- **Why it will be much faster than doing it by hand:**
  - only the audio track is read and decoded; video packets are skipped at the demuxer;
  - the file is split into chunks scanned **in parallel on all cores**, with gating histograms merged at the end (R128 integrated loudness can be combined exactly this way);
  - results are **cached** by content hash, so a file is only analysed once;
  - playback doesn't wait: it starts with a quick estimate (first seconds, or stored ReplayGain/R128 tags) and smoothly moves to the exact gain as soon as the background scan finishes.
- Gain priority: stored tags → cache → background scan → adaptive running loudness (live/web streams).
- Applied per voice before the mix; a **true-peak limiter** on the master keeps crossfades from clipping. Target configurable (−16 LUFS default, −23 LUFS broadcast).
- Scan results exportable for the app's own database.

### R9 CD+G

- **Sources:** `.cdg` + audio pairs (MP3+G, ZIP), and **from disc** (R–W subchannel read alongside the audio sectors).
- **Decoder:** full CD+G instruction set (memory preset, border, tile normal/XOR, scroll preset/copy, transparent colour, palette load), and **CD+EG** if needed.
- **Sync:** the CD+G stream is a side stream of the audio voice on the master clock (300 packs per second, tied to sample position), so seeking, pausing, tempo/pitch changes and crossfades keep graphics in sync with no app-side work.
- **Output surface, platform-independent:**
  1. the base output is a CPU framebuffer (indexed 300×216 + palette, or RGBA32/BGRA32 at native or scaled size) with **dirty rectangles** and a "frame changed" callback;
  2. the app uploads it to any surface: DirectX/Direct3D texture, Metal, OpenGL/Vulkan, WPF/WinUI bitmap, Android Bitmap/SurfaceTexture, SkiaSharp etc.;
  3. optional helpers for the common ones (e.g. a D3D11 texture updater on Windows, a CVPixelBuffer on Apple) built on top, so the core stays universal.
- Options: border on/off, transparent background (for overlaying on video), scaling filter.

### R10 Disc playback

- Audio CD on Windows, macOS and Linux (iOS/Android have no practical optical drive support): TOC, CD-Text, MusicBrainz/freedb disc ID, read offset correction, error-tolerant reading with retries and C2 error detection, drive speed control.
- Disc tracks are just another source for `fa_open()`, so everything else (normalization, crossfade, CD+G) works on them unchanged.
- Analog CD playback (legacy basscd feature) is dropped.

### R12 Tempo and pitch quality

- Why NAudio/SoundTouch sound bad: they use time-domain WSOLA (overlap-add of short segments). It's cheap, but produces echo/phasing on sustained tones, smeared drums, and "warbly" vocals as soon as the ratio moves away from 1.0.
- FatzAudio uses a **frequency-domain stretcher with phase locking and transient handling**: Signalsmith Stretch (MIT) as the first candidate, Bungee (MPL-2.0) as the second. Both are modern and permissively licensed, so they can be compiled into the single binary. Rubber Band would be excellent but is GPL/commercial, so it's out.
- Pitch shift = stretch + **high-quality resample** (windowed-sinc polyphase, e.g. r8brain-free, MIT). The same resampler is used everywhere sample rates differ, so rate changes and device-rate conversion are clean too.
- Formant preservation option for vocals (karaoke key change without the "chipmunk" sound).
- Quality is measured, not assumed: the comparison suite renders the same material at ratios 0.5–2.0 and ±12 semitones through BASS (bass_fx), SoundTouch and FatzAudio, scores it with an objective metric (ViSQOL, Apache-2.0) and produces files for blind listening.

### R13 Threads and buffering

- **Decode-ahead per stream.** Decoding, stretching, resampling and normalization run on worker threads into a **lock-free ring buffer** per stream (default ~500 ms, configurable). That's what BASS's playback buffer does, and it's why BASS sounds smooth: the audio thread never waits on disk, network or heavy DSP. It also gives the stretcher lookahead, which improves its quality.
- **The audio callback only mixes:** no locks, no allocation, no I/O, no decoding. It runs at real-time priority (MMCSS "Pro Audio" on Windows, audio workgroups on Apple, AAudio performance mode on Android).
- **Work scheduler** with a thread pool sized to the CPU: decode jobs, loudness scans and seek-index builds run in parallel. Opening, scanning and seeking are **asynchronous** (completion events), so the UI thread never blocks.
- Optional "process in my thread" mode for apps that want to drive rendering themselves (offline export, video encoders).

### R14 Devices

- Output and input default to the system's primary device and **follow it** when it changes (headset plugged in, Bluetooth connects). An explicit device can be chosen and is kept.
- Unplugged device → event, and playback moves to the new default instead of stopping (configurable).
- Windows: WASAPI shared by default, **WASAPI exclusive** and **ASIO** optional for low latency/bit-perfect output.

### Features worth taking from other BASS add-ons

From the official add-on list (un4seen.com/bass.html, read 2026-10-17), the ones that fit FatzAudio:

| BASS add-on | Idea for FatzAudio |
|---|---|
| BASSmix | Matrix mixing (already in R5) and **splitters**: one source to several outputs, e.g. main speakers + a cue/headphone device for a karaoke host or DJ |
| BASSenc (+ MP3/FLAC/OGG/OPUS) | Record/encode any stream or the master mix to file, and **broadcast to Icecast/Shoutcast** |
| BASSloud | Loudness metering (part of R6) |
| BASSHLS | HLS (already in R4) |
| BASSWASAPI / BASSASIO | Exclusive-mode and ASIO output (R14) |
| BASSWV, BASSAPE, BASSALAC, BASS_MPC, BASS_TTA, BASSDSD | Extra lossless/hi-res formats: WavPack, Monkey's Audio, ALAC, Musepack, TTA, DSD (DSF/DFF). All have permissive or small decoders; cheap to add to the registry |
| BASSMIDI | SFZ and SF3 soundfonts besides SF2, MIDI input |
| BASS_VST | Hosting effect plugins (VST3/CLAP) for user effects — later phase, optional |
| Tags | Formatted tag text (e.g. "%artist - %title") as a helper |
| BASS_FFMPEG / BASS_DSHOW | Covered by the FFmpeg compatibility bridge (R3) |

Not taken: Winamp/Sonique plugin hosting, WMA encoding, ADX/AIX/OptimFROG (niche), BASS_SSL (OS TLS is used instead).

## 4. Build and packaging

CMake, C11 core (C++17 only where a dependency needs it), permissive dependencies vendored and statically linked, LGPL ones (FFmpeg, LAME) only loaded at runtime. Outputs: `fatzaudio.dll` (Windows x64/arm64), `libfatzaudio.dylib` (macOS universal), `fatzaudio.xcframework` (iOS), `libfatzaudio.so` (Android ABIs, Linux x64/arm64). CI matrix builds all of them, runs fuzzers and the BASS comparison suite. One NuGet with `runtimes/<rid>/native/`.

## 5. Comparison suite (vs BASS)

Decode null-test, seek accuracy, length/end accuracy, gapless/crossfade timing, web stream start and reconnect, CPU/memory with N streams, latency, normalization accuracy against a reference R128 meter and **scan speed on large video files**, **A/V sync drift** in the FFmpeg hand-off vs the current PCM-push approach, and **CD+G sync error**. BASS's result is the floor for each metric.

## 6. Open decisions

1. Dolby Digital everywhere: rely on OS codecs + passthrough + FFmpeg (recommended to start), or also write our own AC-3/E-AC-3 decoder later?
2. MP3 encoding via runtime-loaded LAME (recommended), same model as FFmpeg.
3. Build order (proposed): core engine + devices + decoders → normalization → crossfade/markers → FFmpeg hand-off → web/HLS → CD+G → disc.

## 7. API fixes found while writing the .NET binding (to do in the core-engine phase)

Status: ✅ = done in the core-engine phase.

1. ✅ Document that `fa_stream_read` returns `FA_ERROR_END_OF_STREAM` with 0 frames at the end.
2. ✅ `fa_stream_attach` must not reopen the decoder or reset position/format (resampling belongs in the voice chain). Attaching to another device now moves the stream; `fa_stream_detach` added.
3. ✅ Add a duration field to `fa_stream_info` (`duration`, nanoseconds, -1 unknown).
4. ✅ Make `fa_device_start`/`stop` idempotent (`fa_device_is_started` added).
5. ✅ Define an "unset" value for markers (`FA_FRAMES_UNSET`).
6. ✅ Add a ramp-finished event so `RampAsync` can complete on the real end (`FA_EVENT_RAMP_DONE`, value = `fa_ramp.tag`, exactly once per ramp).
7. ✅ Specify `fa_io_callbacks.close` and the lifetime of `user` (owned by the stream only after a successful open; `close` called exactly once by `fa_stream_close`).
8. ✅ Document `fa_event.frame`/`value` per event type (table above `fa_event_type` in the header, and docs/features/events.md).
