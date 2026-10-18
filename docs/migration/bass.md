# Migrating from BASS

This guide maps every BASS 2.4.18 function, and every add-on in the legacy app (bass_aac, bass_ac3, bass_fx, bassalac, basscd, bassflac, bassmidi, bassopus, basswebm, basswma), plus BASSenc for recording and encoding, to FatzAudio. It's updated as each feature lands. Status symbols are explained in the [docs index](../README.md#status-legend).

**Goal:** after migration the app ships one native library per platform (`fatzaudio.dll`/`.dylib`/`.so`) and none of the BASS DLLs.

On Android and iOS BASS also comes as the core library plus one library per add-on (`libbass.so`, `libbass_fx.so` and so on in each Android ABI folder). FatzAudio replaces them all with one `libfatzaudio.so` per ABI and one `fatzaudio.xcframework` ([Building](../building.md)). Delete the add-on libraries and their `BASS_PluginLoad` calls there too.

## 1. What changes conceptually

| BASS | FatzAudio | Why it matters when porting |
|---|---|---|
| `BASS_Init` sets a per-thread "current device"; later calls act on it implicitly | You hold an `fa_device*` and pass it explicitly | Calls on another thread no longer silently target the wrong device. Remove all `BASS_SetDevice` juggling |
| Functions return `FALSE`/`0`, then you call `BASS_ErrorGetCode()` | Every function returns an `fa_result` error code directly; .NET throws `FatzAudioException` | Replace every `if (!x) err = BASS_ErrorGetCode()` with a direct check |
| `HSTREAM`/`HCHANNEL`/`HMUSIC`/`HRECORD` are all `DWORD` and interchangeable by accident | Typed handles (`fa_stream*`, `fa_device*`, `fa_bus*`) | The compiler catches wrong-handle bugs |
| Each add-on has its own `BASS_XXX_StreamCreateFile` + `BASS_PluginLoad` | One `fa_stream_open_*` detects the format; no plugins | Delete all plugin loading and per-format create calls |
| Flags are one big `DWORD` bitfield shared across unrelated functions | Small option structs per call (`fa_open_options`) | No more guessing which `BASS_SAMPLE_*`/`BASS_STREAM_*` flags apply |
| Positions in bytes, converted with `Bytes2Seconds` | Positions in frames (sample-exact) or nanoseconds | No byte arithmetic that breaks when the format is float or multichannel |
| `BASS_ChannelSetSync` with numeric `BASS_SYNC_*` types and a `SYNCPROC` | One event callback per stream with typed `fa_event`; .NET uses normal C# events | One handler instead of a sync per event type |
| Volume/pan via `BASS_ChannelSetAttribute(h, BASS_ATTRIB_VOL, v)` | `fa_stream_set_param(s, FA_PARAM_VOLUME, v)`; .NET: `stream.Volume = v` | |
| Fades via `BASS_ChannelSlideAttribute` (linear or log only) | `fa_stream_ramp_param` with linear, equal-power, log, exp, S-curve, bezier or your own points, optionally starting at an exact time | |
| Crossfades built by hand from two slides and a `BASS_SYNC_POS` | `fa_crossfade`, or `fa_stream_set_next` to crossfade (or go gapless) automatically at the mix point | No sync juggling; both sides start on the same frame |
| Tempo/pitch need bass_fx and a separate "tempo stream" wrapping the source | `FA_PARAM_TEMPO` and `FA_PARAM_PITCH` on any stream | No wrapper streams to create and free |
| Audio of a video played with FFmpeg means decoding it to PCM yourself, pushing it, and correcting drift by hand | Push FFmpeg's packets as they are (`fa_stream_push_packet`) or its frames in their own format (`fa_stream_push_frame`); `fa_stream_get_clock` reports their pts as it leaves the speaker ([FFmpeg hand-off](../features/ffmpeg.md)) | No audio decoding or resampling in the app, and the video follows one clock: no manual resync |

## 2. Function map: bass.dll

### Library and devices

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_GetVersion | `fa_version` | `AudioEngine.NativeVersion` | ✅ |
| BASS_ErrorGetCode | Return value of every call; `fa_result_string` | `FatzAudioException.ErrorCode` | ✅ |
| BASS_ERROR_WMA_LICENSE (protected WMA) | `FA_ERROR_PROTECTED_CONTENT`, also for encrypted MP4/MKV and SAMPLE-AES HLS | `ErrorCode.ProtectedContent` | ✅ |
| BASS_Init | `fa_engine_create` + `fa_device_open` | `AudioEngine.Create()` + `engine.OpenOutput()` | ✅ |
| BASS_Init with device 0 ("no sound") | `FA_ENGINE_NULL_OUTPUT` in `fa_engine_config.flags`: a silent output that still runs in real time ([devices](../features/devices.md#silent-output-for-tests-and-servers)) | `new AudioEngineOptions { NullOutput = true }` | ✅ |
| BASS_Free | `fa_device_close` + `fa_engine_destroy` | `Dispose()` | ✅ |
| BASS_GetDeviceInfo | `fa_device_count` + `fa_device_get_info` | `engine.OutputDevices` | ✅ |
| BASS_SetDevice / BASS_GetDevice | Not needed: pass the `fa_device*` | — | ➖ (by design) |
| BASS_GetInfo | `fa_device_get_info`, `fa_device_get_latency` | `device.Latency` | ✅ (latency, rate, channels) |
| BASS_Start / BASS_Stop | `fa_device_start` / `fa_device_stop` (both idempotent) | `device.Start()` / `Stop()` | ✅ |
| BASS_Pause | `fa_device_stop` (streams keep their position) | `device.Stop()` | ✅ |
| BASS_IsStarted | `fa_device_is_started` | `device.IsStarted` | ✅ |
| BASS_SetVolume / BASS_GetVolume | `fa_device_set_volume` / `get_volume` | `device.Volume` | ✅ |
| BASS_SetConfig / GetConfig (+Ptr) | `fa_engine_config` fields at creation; per-stream options | `AudioEngineOptions` | ✅ partially (rate, period, threads, cache dir) |
| BASS_CONFIG_BUFFER | `fa_open_options.buffer_ms` per stream, default 500 ms ([playback](../features/playback.md#buffer-length)) | `StreamOptions.BufferLength` | ✅ |
| BASS_CONFIG_NET_TIMEOUT / NET_READTIMEOUT | `fa_net_options.timeout_ms` per stream: connecting, and the longest the server may stay silent ([web streams](../features/web-streams.md#connection-settings)) | `NetworkOptions.Timeout` | ✅ |
| BASS_CONFIG_NET_BUFFER | Not needed: each web stream keeps up to 4 MB, at most 2 MB ahead of playback | — | ➖ (by design) |
| BASS_CONFIG_NET_PREBUF / NET_PREBUF_WAIT | `fa_net_options.prebuffer_ms` (default 1 s), used at the start and after every stall; opening never waits for it | `NetworkOptions.Prebuffer` | ✅ |
| BASS_CONFIG_NET_AGENT | `fa_net_options.user_agent` | `NetworkOptions.UserAgent` | ✅ |
| BASS_CONFIG_NET_PROXY | `fa_net_options.proxy`: `NULL` = system settings (Windows, macOS, Android) and environment, `""` = direct, or `"user:pass@host:port"` | `NetworkOptions.Proxy` | ✅ |
| BASS_CONFIG_NET_PLAYLIST | Always on: `.pls` and `.m3u` are followed to their first entry, `.m3u8` plays as HLS | automatic | ✅ |
| BASS_CONFIG_NET_META | Always asks for ICY titles; `FA_NET_NO_ICY` turns it off | `NetworkOptions.SongTitles = false` | ✅ |
| BASS_CONFIG_NET_RESTRATE | Not needed: the download only runs 2 MB ahead of playback, so it's paced by playback anyway | — | ➖ (by design) |
| BASS_CONFIG_NET_PASSIVE (FTP) | FTP isn't supported | — | ➖ |
| BASS_CONFIG_DEV_PERIOD / BASS_CONFIG_DEV_BUFFER | `fa_engine_config.period_ms` (0 = the system's default, 10 ms on Windows); the device buffer is two periods on Windows, the system's default elsewhere. `fa_device_get_latency` reports what the system gave | `AudioEngineOptions.Period` | ✅ |
| BASS_CONFIG_UPDATETHREADS / UPDATEPERIOD | `fa_engine_config.worker_threads`; workers refill the emptiest buffer first, nothing to tune | `AudioEngineOptions.WorkerThreads` | ✅ |
| BASS_Update / BASS_ChannelUpdate | Not needed: worker threads keep buffers full | — | ➖ (by design) |
| BASS_GetCPU | Planned `fa_engine_get_stats` | | 🗓️ |
| BASS_GetDSoundObject | Not provided (Windows-only escape hatch) | — | ➖ |

### Streams

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_StreamCreateFile (path) | `fa_stream_open_path`; format detected from content ([streams](../features/streams.md)) | `engine.OpenFile(path)` | ✅ WAV/AIFF/MP3/FLAC/OGG/Opus so far (and the add-on formats below: MP4, MKV, AAC, AC-3, WMA) |
| BASS_STREAM_PRESCAN | `FA_OPEN_PRESCAN` in `fa_open_options.flags`: MP3 reads every frame header at open. Seeks are exact and fast without it; other formats are always exact ([MP3](../features/streams.md#mp3)) | `new StreamOptions { Prescan = true }` | ✅ |
| BASS_StreamCreateFile (memory) | `fa_stream_open_memory` | `engine.OpenMemory(bytes)` | ✅ |
| BASS_StreamCreateFileUser | `fa_stream_open_io`; `close` is called once by `fa_stream_close`, never on a failed open ([custom I/O](../features/streams.md#custom-io-your-own-reader)) | `engine.OpenStream(System.IO.Stream)` | ✅ |
| BASS_StreamCreateURL | `fa_stream_open_url`: HTTP/HTTPS files with range seeking, Icecast/Shoutcast radio, HLS, `.pls`/`.m3u`; reconnects by itself ([web streams](../features/web-streams.md)) | `engine.OpenUrl(url)` | ✅ WAV/FLAC/MP3/OGG/Opus/AAC/AC-3 built in |
| BASS_StreamCreateURL with `https://` on Android | The system's HTTPS (`HttpURLConnection` on the app's Java VM): no SSL library to ship. Android 12+ and .NET for Android need nothing; Java/Kotlin apps on Android 7 to 11 call `fa_android_set_java_vm` once ([web streams](../features/web-streams.md#android)) | automatic (`AudioEngine.SetAndroidJavaVM` if needed) | ✅ |
| BASS_StreamCreateURL `DOWNLOADPROC` (raw bytes to save a station) | Planned | | 🗓️ |
| BASS_StreamCreateURL `BASS_STREAM_BLOCK` | Not needed: memory per web stream is bounded (4 MB) and seeking still works | — | ➖ (by design) |
| BASS_StreamCreateURL `BASS_STREAM_STATUS` (+ `BASS_TAG_HTTP` during connect) | `fa_stream_get_http_header` and `fa_stream_get_net_status` after the open | `GetHttpHeader`, `NetworkStatus` | ✅ |
| BASS_StreamCreate (STREAMPROC_PUSH) + BASS_StreamPutData | `fa_stream_open_push` + `fa_stream_push_pcm`, with an optional pts per push ([FFmpeg hand-off](../features/ffmpeg.md)) | `engine.CreatePushStream(format)`, `Push(samples, pts)` | ✅ |
| BASS_StreamPutData of PCM converted from another format (swresample first) | `fa_stream_push_frame`: any sample format, planar or interleaved, any rate; converted on a worker thread | `PushFrame(format, (nint)frame->extended_data, ...)` | ✅ |
| BASS_StreamPutData(h, NULL, 0) to read the queued amount | `fa_stream_get_buffered` (queued + decoded ahead + device buffer, as a duration) | `Buffered` | ✅ |
| BASS_StreamPutData with BASS_STREAMPROC_END | `fa_stream_push_end` | `End()` | ✅ |
| BASS_ChannelSetPosition(h, 0, ...) to clear a push stream after the app seeks | `fa_stream_push_flush`: drops everything queued at once; the clock holds until the new audio plays | `Flush()` | ✅ |
| Decode compressed audio yourself (FFmpeg) and push PCM | `fa_stream_open_packets` + `fa_stream_push_packet`: packets straight from `av_read_frame`; PCM, MP3, AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC, ALAC (and WMA on Windows) built in; for other codecs decode with your FFmpeg and push frames (`fa_stream_push_frame`) | `engine.OpenPackets(params)`, `PushPacket(...)` | ✅ |
| BASS_StreamCreate (STREAMPROC callback) | Planned pull-callback source | | 🗓️ |
| BASS_StreamPutFileData | `fa_stream_open_io` with `seek = NULL` (forward-only source fed from your own buffer) | `engine.OpenStream(stream)` with a non-seekable stream | ✅ |
| BASS_StreamGetFilePosition (web streams) | `fa_stream_get_net_status`: `BASS_FILEPOS_DOWNLOAD` → `downloaded`, `BUFFER` → `buffered_bytes` (and `buffered` as time), `END`/`SIZE` → `content_length`, `CONNECTED` → `connected`, `BUFFERING` → `FA_STATE_BUFFERING` ([web streams](../features/web-streams.md#status-and-headers)) | `NetworkStatus` | ✅ |
| BASS_StreamGetFilePosition (files) | Positions are in frames: use `fa_stream_get_position`. `BASS_FILEPOS_END`/`SIZE` (file size) → `fa_stream_info.file_size`; `START` (where the audio begins, after tags) is not needed: `audio_bytes` already leaves tags and headers out | `stream.Info.FileSize`, `AudioBytes` | ✅ |
| BASS_StreamCancel | Async open with cancellation | | 🗓️ |
| BASS_StreamFree / BASS_ChannelFree | `fa_stream_close` | `Dispose()` | ✅ |
| `BASS_STREAM_DECODE` flag + BASS_ChannelGetData (PCM) | `FA_OPEN_DECODE_ONLY` + `fa_stream_read`; returns `FA_ERROR_END_OF_STREAM` with 0 frames at the end ([decoding](../features/streams.md#decoding-without-playback)) | `Read(Span<float>)`, 0 at the end | ✅ |

### Channel control

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_ChannelPlay / BASS_ChannelStart | `fa_stream_attach` + `fa_stream_play` | `stream.PlayOn(device)` | ✅ |
| BASS_ChannelPlay at a precise time | `fa_stream_play_at` on the device clock (`fa_device_get_time`) ([crossfade](../features/crossfade.md#the-device-clock)) | `stream.PlayAt(output.Time + delay)` | ✅ |
| BASS_ChannelPause / BASS_ChannelStop | `fa_stream_pause` / `fa_stream_stop` | `Pause()` / `Stop()` | ✅ |
| BASS_ChannelIsActive | `fa_stream_get_state` | `stream.State` | ✅ |
| BASS_ChannelGetInfo | `fa_stream_get_info` (the source's own format; unchanged by attaching), plus `bits_per_sample` (`origres`), `file_size`, `audio_bytes` and `bitrate` ([stream info](../features/streams.md#stream-info)) | `stream.Info` | ✅ |
| BASS_ChannelGetAttribute `BASS_ATTRIB_BITRATE` | `fa_stream_info.bitrate` (average bits per second; also from `fa_tags_get_info` without opening a stream) | `Info.Bitrate`, `Tags.Bitrate` | ✅ |
| BASS_ChannelGetLength + bitrate + file size for a media library (a decode stream per file just to read them) | `fa_tags_read_path`: tags, cover and properties in one call, no engine or stream ([tags](../features/tags.md)) | `Tags.Read(path)` | ✅ |
| BASS_ChannelGetLength | `fa_stream_info.length_frames` or `.duration` (ns) | `stream.Info.Duration` | ✅ |
| BASS_ChannelGetPosition / SetPosition | `fa_stream_get_position` / `fa_stream_seek` (frames); seek refills 100 ms before returning | `stream.Position` (TimeSpan) | ✅ |
| BASS_ChannelGetPosition with BASS_POS_DECODE | Not needed: the position always counts what was played, never what was decoded ahead | — | ➖ (by design) |
| BASS_ChannelBytes2Seconds / Seconds2Bytes | Not needed: frames ÷ sample rate, or use TimeSpan in .NET | — | ➖ (by design) |
| BASS_ChannelSetAttribute VOL / PAN | `fa_stream_set_param` VOLUME / PAN, de-zippered over 5 ms ([ramps](../features/ramps.md)) | `Volume` / `Pan` | ✅ |
| BASS_ChannelSetAttribute FREQ (playback speed and pitch together) | `FA_PARAM_RATE` as a multiplier: `new_freq / original_freq`, e.g. 1.02 for +2 %; sinc resampling, exact ([tempo and pitch](../features/tempo-pitch.md)) | `Rate` | ✅ (not on push streams) |
| BASS_ChannelSetAttributeEx / GetAttributeEx | Typed functions per feature instead | | ➖ (by design) |
| BASS_ChannelSlideAttribute | `fa_stream_ramp_param` with 7 curves and an optional exact start time ([ramps](../features/ramps.md)) | `Ramp(...)` / `await RampAsync(...)` | ✅ volume, pan |
| BASS_ChannelIsSliding | `FA_EVENT_RAMP_DONE` (one per ramp, tagged) instead of polling | `RampAsync` task, `RampCompleted` event | ✅ |
| BASS_ChannelSetSync / RemoveSync | `fa_stream_set_event_callback`; one callback receives every event type ([events](../features/events.md)) | `Ended`, `RampCompleted`, … events | ✅ |
| BASS_SYNC_END | `FA_EVENT_ENDED` | `Ended` | ✅ |
| BASS_SYNC_SLIDE | `FA_EVENT_RAMP_DONE` | `RampCompleted` / `RampAsync` | ✅ |
| BASS_SYNC_META | `FA_EVENT_METADATA`, raised when the new song is heard rather than when it is downloaded ([song titles](../features/web-streams.md#song-titles-icy-metadata)) | `MetadataChanged` (carries the new `Metadata`) | ✅ |
| BASS_SYNC_STALL | `FA_EVENT_STALLED` and `FA_EVENT_RESUMED` (two events instead of one with a flag) | `Stalled`, `Resumed` | ✅ |
| BASS_SYNC_DOWNLOAD | `fa_net_status.finished` | `NetworkStatus.Finished` | ✅ (polled, no event) |
| BASS_SYNC_HLS_SEGMENT | Not provided | — | ➖ |
| BASS_SYNC_POS | `fa_stream_add_position_event` + `FA_EVENT_POSITION` | `AddPositionEvent`, `PositionReached` | 🧩 |
| BASS_SYNC_POS used to start a crossfade or the next track | Markers (`FA_MARKER_MIX_POINT`, fades) and `fa_stream_set_next` ([crossfade](../features/crossfade.md)) | `stream.Markers`, `QueueNext` | ✅ |
| BASS_SYNC_POS used to end playback early (trim) | `FA_MARKER_CUE_OUT` (exact end) and `FA_MARKER_CUE_IN` | `Markers.CueOut`, `Markers.CueIn` | ✅ |
| `FA_EVENT_MARKER` has no BASS equivalent | Raised when playback passes a marker | `MarkerReached` | ✅ |
| BASS_SYNC_MIXTIME | Not needed: callbacks never run on the audio thread, so any call is safe in them. Sample-accurate actions are scheduled instead: `fa_stream_play_at`, ramps with a start time, and queues (`fa_stream_set_next`) that start the next track on the exact frame | — | ➖ (by design) |
| BASS_ChannelSetDSP (+Ex) / RemoveDSP | `fa_stream_add_dsp` / `fa_stream_remove_dsp` | | 🧩 |
| BASS_ChannelGetLevel (+Ex) | `fa_stream_get_levels` | `GetLevels()` | 🧩 |
| BASS_ChannelGetData (FFT) | Planned `fa_stream_get_spectrum` | | 🗓️ |
| BASS_ChannelGetTags | `fa_stream_get_tags` for an open stream, or `fa_tags_read_path`/`_memory` without one: one reader for every format, common fields with `fa_tags_get(tags, FA_TAG_ARTIST)`, every item with `fa_tags_get_item` ([tags](../features/tags.md)) | `stream.GetTags()`, `Tags.Read(path)`: `tags.Artist`, `tags.Title`, ... | ✅ |
| BASS_ChannelGetTags `BASS_TAG_ID3` (ID3v1) / `BASS_TAG_ID3V2` / `BASS_TAG_APE` / `BASS_TAG_MP4` / `BASS_TAG_RIFF_INFO` | Parsed for you: no raw blocks to decode. Items keep their format's own keys (`TIT2`, `©nam`, `INAM`, ...) and `format` says which tag they came from; ID3v2 wins over APEv2 over ID3v1 for the common fields | `tags.Items` (`Key`, `Value`, `Format`), `tags["KEY"]` | ✅ |
| BASS_ChannelGetTags `BASS_TAG_APE_BINARY` / ID3v2 `APIC` / MP4 `covr` (cover art) | `fa_tags_get_cover`, or every picture with `fa_tags_get_picture` (type, MIME, description, data) | `tags.Cover`, `tags.Pictures` | ✅ |
| Tags add-on (tags.dll) `TAGS_Read(h, "%ARTI - %TITL")` | No format strings: build the text from `fa_tags_get` fields | String interpolation on `Tags` | ➖ (formatting is the app's) |
| BASS_ChannelGetTags `BASS_TAG_META` (ICY StreamTitle) | `fa_stream_get_metadata`: the title heard now, split into artist and title ([song titles](../features/web-streams.md#song-titles-icy-metadata)) | `stream.Metadata` | ✅ |
| BASS_ChannelGetTags `BASS_TAG_ICY` / `BASS_TAG_HTTP` | `fa_stream_get_metadata` (station, genre, bitrate, ...) or any header by name with `fa_stream_get_http_header` | `stream.Metadata`, `GetHttpHeader(name)` | ✅ |
| BASS_ChannelGetTags `BASS_TAG_HLS_*` | `fa_stream_get_info` (container `hls`, codec) and `fa_stream_get_net_status` (`is_hls`, media playlist URL) | `Info`, `NetworkStatus` | ✅ |
| BASS_ChannelFlags (loop) | `FA_OPEN_LOOP`, looping between the cue-in and cue-out markers; runtime toggle planned | | ✅ open-time only |
| BASS_POS_LOOP / BASS_POS_END (loop start, end position) | `FA_MARKER_CUE_IN` / `FA_MARKER_CUE_OUT` | `Markers.CueIn` / `Markers.CueOut` | ✅ |
| BASS_ChannelSetLink / RemoveLink | `fa_stream_play_at` with the same time starts streams on the same frame; a bus ([buses](../features/buses.md)) groups their volume and pan | `PlayAt`, `MixBus` | ✅ |
| BASS_ChannelLock | Not needed: parameter changes are applied atomically per buffer | — | ➖ (by design) |
| BASS_ChannelSetDevice / GetDevice | `fa_stream_attach` to the new device moves the stream and keeps its position; `fa_stream_detach` removes it | `stream.PlayOn(other)`, `stream.Detach()`, `stream.Device` | ✅ |
| BASS_ChannelRef | Not needed: handles stay valid until you close them | — | ➖ (by design) |

### Device clock

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_ChannelGetPosition on a playing channel used as a timeline | `fa_device_get_time`: frames the device mixed since it opened, in ns; never jumps on seek or track change | `output.Time` | ✅ |

### Video sync

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_ChannelGetPosition minus the latency from BASS_GetInfo, converted with Bytes2Seconds, to find which video frame to show | `fa_stream_get_clock`: the time leaving the speaker, latency included, interpolated between callbacks; holds while paused, stalled or ended ([FFmpeg hand-off](../features/ffmpeg.md#following-the-clock-with-the-video)) | `stream.Clock` | ✅ |
| Keeping your own offset from the first pushed pts | Not needed: push streams report the pts you pushed, timestamp jumps included | `Clock` | ➖ (by design) |
| BASS_SYNC_STALL (stalled / resumed) | `FA_EVENT_STALLED` / `FA_EVENT_RESUMED` on push and packet streams | `Stalled` / `Resumed` | ✅ (web streams 🗓️) |
| BASS_ChannelIsActive == BASS_ACTIVE_STALLED | `FA_STATE_BUFFERING`; resumes by itself at about 100 ms queued | `StreamState.Buffering` | ✅ |
| BASS_FFMPEG / BASS_DSHOW add-ons (third-party) to play the audio of video files | `fa_stream_open_packets` fed from the FFmpeg demuxer your app already has | `OpenPackets` | ✅ |

### Effects

| BASS | FatzAudio | Status |
|---|---|---|
| BASS_ChannelSetFX / RemoveFX, BASS_FXSetParameters / GetParameters / Reset / SetPriority / SetBypass / Free | One effects chain per stream (EQ, compressor, reverb, echo, chorus, flanger, distortion) | 🗓️ |

### Loudness and normalization

BASS has no normalization: apps measure each file themselves with a decode pass and then set the volume. FatzAudio does it for every stream ([normalization](../features/normalization.md)).

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| Your own analysis: decode stream (`BASS_STREAM_DECODE`) + `BASS_ChannelGetData`/`GetLevel` loop, compute a gain, then `BASS_ChannelSetAttribute(h, BASS_ATTRIB_VOL, gain)` | `fa_engine_set_normalization(engine, FA_NORMALIZE_TRACK, -16)` once. Tags, cache and a parallel audio-only background scan; playback starts at once and glides to the exact gain | `engine.SetNormalization(NormalizeMode.Track, -16)` | ✅ |
| Storing your analysis result and reapplying it next time | `fa_stream_set_loudness` (wins over everything); results cached automatically in `fa_engine_config.cache_directory` | `stream.SetLoudness(lufs)`, `AudioEngineOptions.CacheDirectory` | ✅ |
| Analysing files ahead of playback (library import) | `fa_scan_loudness_path`: audio only, all cores, cached | `engine.ScanLoudness(path)` | ✅ |
| `BASS_ATTRIB_VOLDSP` to boost above 1.0 | Normalization gain is separate from `FA_PARAM_VOLUME` and boosts up to +12 dB; volume stays free for fades | automatic | ✅ |
| Reading ReplayGain tags yourself (`BASS_ChannelGetTags` + parsing) | Read automatically: ID3v2 `TXXX` (also in WAV and AIFF), FLAC and Ogg Vorbis comments, OpusTags, APEv2, MP4 freeform atoms, ASF attributes; RG2 and R128. The raw values are also in `fa_tags_get_item` | automatic | ✅ |
| BASSloud: `BASS_Loudness_Start` / `GetLevel` (integrated, range, true peak) | `fa_stream_get_loudness`, `fa_scan_loudness_path` (BS.1770-4 / R128) | `GetLoudness()`, `ScanLoudness()` | ✅ integrated, range, true peak; momentary/short-term meters 🗓️ |
| bass_fx `BASS_FX_BFX_DAMP` (dynamic amplification) on live streams | Running loudness estimate drives the gain for streams that can't be scanned | automatic | ✅ |
| bass_fx `BASS_FX_BFX_COMPRESSOR2` used to stop clipping | `fa_engine_set_limiter`: true-peak, look-ahead, on every device | `engine.SetLimiter(...)` | ✅ |

### Samples (in-memory sounds)

| BASS | FatzAudio | Status |
|---|---|---|
| BASS_SampleLoad / Create / SetData / GetData / GetInfo / SetInfo / GetChannel / GetChannels / Stop / Free | `fa_stream_open_memory` covers playback of a loaded sound; a voice pool for many simultaneous copies is planned | 🗓️ |

### Tracker music

| BASS | FatzAudio | Status |
|---|---|---|
| BASS_MusicLoad / BASS_MusicFree | `fa_stream_open_*` once libopenmpt is added | 🗓️ |

### Recording

| BASS | FatzAudio | Status |
|---|---|---|
| BASS_RecordGetDeviceInfo | `fa_device_count/get_info(FA_DEVICE_CAPTURE)` | ✅ (listing) |
| BASS_RecordInit / Free / SetDevice / GetDevice / GetInfo / Start | Capture devices as stream sources | 🗓️ |
| BASS_RecordGetInput / SetInput / GetInputName | Not provided (old WinMM mixer lines) | ➖ |

### Encoding and recording (BASSenc, BASSenc_MP3, BASSenc_FLAC)

The legacy app ships no encoder DLLs; this is how BASSenc-style code maps. One encoder object replaces the encoder add-ons, and format conversion, threading and file headers are built in ([recording and encoding](../features/recording-encoding.md)).

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_Encode_Start / BASS_Encode_StartUser on a playing channel | `fa_encoder_create_path` / `fa_encoder_create_output` + `fa_encoder_attach_stream` | `AudioEncoder.Create(path or Stream)` + `Record(stream)` | ✅ |
| BASS_Encode_Start on a decoding channel, then a `BASS_ChannelGetData` loop | `fa_encoder_encode_stream`: the encoder's thread reads the stream to its end; `fa_encoder_wait` | `Encode(stream)` + `Wait`, or `AudioEncoder.ConvertFile` | ✅ |
| Recording the final mix (a mixer or the device output) | `fa_encoder_attach_device` | `Record(output)` | ✅ |
| BASS_Encode_Write (your own PCM) | `fa_encoder_write` (interleaved float, any rate and channel count) | `Write(span, rate, channels)` | ✅ |
| BASS_Encode_MP3_Start / StartFile (bassenc_mp3, LAME built in) | `format = FA_ENCODE_MP3`; LAME is loaded at runtime from your app (`fa_lame_load`), never bundled | `EncodeFormat.Mp3`, `Lame.Load` | ✅ |
| `--preset`, `-b`, `-V` LAME options | `bitrate_kbps` (CBR) or `quality` (VBR 0 … 9) | `BitrateKbps`, `Quality` | ✅ |
| BASS_Encode_FLAC_Start / StartFile (bassenc_flac, libFLAC) | `format = FA_ENCODE_FLAC` (built in, levels 0 … 8, 16 or 24 bit) | `EncodeFormat.Flac` | ✅ |
| BASS_ENCODE_PCM (WAV writer), BASS_ENCODE_FP_16BIT/24BIT/32BIT, BASS_ENCODE_RF64 | `format = FA_ENCODE_WAV`, `bits_per_sample` 16/24/32; RF64 is automatic past 4 GB | `EncodeFormat.Wav`, `BitsPerSample` | ✅ |
| Tags via encoder command lines (`--tt`, `-T TITLE=`) | `title`, `artist`, `album`, `genre`, `date`, `comment`, `track_number` | `Title` … `TrackNumber` | ✅ |
| BASS_Encode_SetPaused | `fa_encoder_set_paused` | `IsPaused` | ✅ |
| BASS_Encode_IsActive | `fa_encoder_get_status` (state, error, frames, bytes, dropped) | `Status` | ✅ |
| BASS_Encode_GetCount (BASS_ENCODE_COUNT_IN/OUT/QUEUE...) | `fa_encoder_status.frames`, `bytes`, `dropped_frames` | `Status.Frames`, `Bytes`, `DroppedFrames` | ✅ |
| BASS_Encode_Stop / StopEx | `fa_encoder_close` (completes the file, returns the first error) | `Close()` / `Dispose()` | ✅ |
| BASS_Encode_SetChannel / GetChannel | Create a new encoder (the file is completed when the old one closes) | | ➖ (by design) |
| BASS_ENCODE_QUEUE, BASS_ENCODE_LIMIT, BASS_CONFIG_ENCODE_PRIORITY | Always queued on the encoder's own thread (`queue_ms`); the audio thread never waits | `QueueLength` | ✅ |
| BASS_Encode_SetNotify (encoder died) | `FA_ENCODER_FAILED` in the status, the error from `fa_encoder_close` | `Status.State`, `Close()` throws | ✅ |
| Sample rate / channel conversion with a resampling mixer in front of the encoder | `sample_rate` and `channels` in the options | `SampleRate`, `Channels` | ✅ |
| BASS_Encode_OPUS_* (bassenc_opus), BASS_Encode_OGG_* (bassenc_ogg), BASS_Encode_AAC (bassenc_aac) | Opus and AAC encoding | | 🗓️ |
| BASS_Encode_CastInit / CastSetTitle / CastGetStats (Icecast/Shoutcast) | Broadcasting through `fa_encoder_create_output` to a server connection | | 🗓️ |
| BASS_Encode_ServerInit (built-in server) | Not provided | | ➖ |
| BASS_Encode_StartCA / StartACM (OS codecs) | AAC through the OS encoders is planned; ACM is obsolete | | 🗓️ / ➖ |
| BASS_WMA_Encode* (basswma) | Not provided (Windows-only, WMA is obsolete) | | ➖ |

### 3D and EAX

| BASS | FatzAudio | Status |
|---|---|---|
| BASS_Set3DFactors / Get3DFactors / Set3DPosition / Get3DPosition / Apply3D, BASS_ChannelSet3D* / Get3D* | Optional spatial module | 🗓️ (low priority) |
| BASS_SetEAXParameters / GetEAXParameters | Not provided (hardware EAX no longer exists) | ➖ |

### Plugins

| BASS | FatzAudio | Status |
|---|---|---|
| BASS_PluginLoad / Free / Enable | Not needed: all formats are built in and detected from content | ➖ (by design) |
| BASS_PluginGetInfo | `fa_format_count` + `fa_format_get_info`; .NET `AudioEngine.SupportedFormats` | ✅ |

### BASSmix (mixer streams)

The legacy app doesn't ship bassmix.dll, but mixer-style code built on `BASS_ChannelSetLink` and slides maps here too.

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_Mixer_StreamCreate + BASS_Mixer_StreamAddChannel | `fa_bus_create` + `fa_stream_attach(s, device, bus)`; no extra stream or buffer ([buses](../features/buses.md)) | `output.CreateBus()`, `stream.PlayOn(bus)` | ✅ |
| BASS_Mixer_ChannelRemove | `fa_stream_attach(s, device, NULL)` (master mix) or `fa_stream_detach` | `PlayOn(output)`, `Detach()` | ✅ |
| BASS_Mixer_ChannelSetMatrix (+Ex) | `fa_stream_set_matrix` | `SetMatrix(float[,])`, `ResetMatrix()` | ✅ |
| BASS_Mixer_ChannelSetEnvelope (volume/pan envelope) | Ramps with start times, point curves, marker fades | `Ramp(..., startAt)`, `Curve.Points`, `Markers` | ✅ |
| BASS_Mixer_ChannelSetPosition / GetPosition | `fa_stream_seek` / `fa_stream_get_position` | `Position` | ✅ |
| BASS_ATTRIB_VOL on the mixer stream | `fa_bus_set_param` / `fa_bus_ramp_param` | `bus.Volume`, `bus.Ramp` | ✅ |
| BASS_MIXER_CHAN_NORAMPIN | Not needed: starts are exact and fades are explicit | — | ➖ (by design) |
| BASS_Split_* (one source to several outputs) | Not provided yet | | 🗓️ |

### bass_aac (AAC)

AAC plays through the ordinary open calls, in MP4/M4A files, video files, `.aac` (ADTS) files, AAC radio and HLS. FatzAudio's own decoder handles AAC-LC, HE-AAC, HE-AAC v2, AAC Main and AAC-LTP, mono to 7.1, the same on every platform, with no FFmpeg and no system codec ([AAC](../features/streams.md#aac)). Only xHE-AAC goes to the system's decoder (Media Foundation, AudioToolbox, MediaCodec, where the system has one); without one it returns `FA_ERROR_UNSUPPORTED_FORMAT` (an app can still decode it with its own FFmpeg and push the frames). ALAC in M4A and Matroska is decoded by the library's own decoder on every platform, to the bit.

| bass_aac | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_AAC_StreamCreateFile / BASS_MP4_StreamCreateFile | `fa_stream_open_path` / `fa_stream_open_memory` | `engine.OpenFile` / `engine.OpenMemory` | ✅ |
| BASS_AAC_StreamCreateURL / BASS_MP4_StreamCreateURL | `fa_stream_open_url` (AAC radio with song titles, `.aac` and MP4 over HTTP, AAC in HLS) | `engine.OpenUrl` | ✅ |
| BASS_AAC_StreamCreateFileUser / BASS_MP4_StreamCreateFileUser | `fa_stream_open_io` | `engine.OpenStream` | ✅ |
| BASS_PluginLoad("bass_aac.dll") | Not needed: built in | — | ➖ (by design) |

### bass_ac3 (Dolby Digital and Dolby Digital Plus)

AC-3 and E-AC-3 are built-in formats: the ordinary open calls play them, in raw files, video files, web streams and HLS, with no plugin and no FFmpeg ([Dolby Digital](../features/dolby.md)).

| bass_ac3 | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_AC3_StreamCreateFile | `fa_stream_open_path` / `fa_stream_open_memory` | `engine.OpenFile` / `engine.OpenMemory` | ✅ |
| BASS_AC3_StreamCreateURL | `fa_stream_open_url` (raw AC-3/E-AC-3 over HTTP, and AC-3 in HLS) | `engine.OpenUrl` | ✅ |
| BASS_AC3_StreamCreateFileUser | `fa_stream_open_io` | `engine.OpenStream` | ✅ |
| BASS_PluginLoad("bass_ac3.dll") | Not needed: built in | — | ➖ (by design) |
| BASS_CONFIG_AC3_DYNRNG / BASS_AC3_DYNAMIC_RANGE | `fa_open_options.dynamic_range` per stream (`FA_DYNAMIC_RANGE_FULL`, `NORMAL` or `NIGHT`), changeable while playing with `fa_stream_set_dynamic_range` | `StreamOptions.DynamicRange`, `stream.DynamicRange` | ✅ |
| BASS_AC3_DOWNMIX_2 / BASS_AC3_DOWNMIX_4 | The engine downmixes to the device automatically; for a custom downmix, `fa_stream_set_matrix` | `stream.SetMatrix` | ✅ |
| BASS_AC3_DOWNMIX_DOLBY (Pro Logic Lt/Rt) | Not provided: the plain stereo downmix, or your own matrix | — | ➖ (rarely needed today) |
| E-AC-3 (Dolby Digital Plus), 7.1 | Built in with every coding tool (dependent substreams, spectral extension, enhanced coupling, the adaptive hybrid transform); Atmos streams play their bed | Built in | ✅ |
| AC-3 packets from your own FFmpeg demuxer | `fa_stream_open_packets` with codec `ac3` / `eac3`, decoded natively | `engine.OpenPackets` | ✅ |
| Passthrough to an AV receiver (S/PDIF, HDMI) | Not provided | — | 🗓️ |

### bassopus and OGG Vorbis

Opus and Vorbis are built-in formats: `.opus` and `.ogg` files, chained files, Ogg web radio, and Opus or Vorbis tracks in WebM, Matroska and MP4 all open with the ordinary calls, with no plugin and no FFmpeg ([Ogg Opus and Ogg Vorbis](../features/streams.md#ogg-opus-and-ogg-vorbis)).

| BASS / bassopus | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_StreamCreateFile on `.ogg` (built into bass.dll) | `fa_stream_open_path` / `fa_stream_open_memory` | `engine.OpenFile` / `engine.OpenMemory` | ✅ |
| BASS_OPUS_StreamCreateFile | `fa_stream_open_path` / `fa_stream_open_memory` | `engine.OpenFile` / `engine.OpenMemory` | ✅ |
| BASS_OPUS_StreamCreateURL | `fa_stream_open_url`; Ogg radio song titles come from each song's comments | `engine.OpenUrl` | ✅ |
| BASS_OPUS_StreamCreateFileUser | `fa_stream_open_io` | `engine.OpenStream` | ✅ |
| BASS_OPUS_StreamCreate (`BASS_OPUS_HEAD`) + BASS_OPUS_StreamPutData (raw packets) | `fa_stream_open_packets` with codec `opus`, the `OpusHead` as extradata (or none for plain mono/stereo), then `fa_stream_push_packet` ([FFmpeg hand-off](../features/ffmpeg.md)) | `engine.OpenPackets`, `PushPacket` | ✅ |
| BASS_PluginLoad("bassopus.dll") | Not needed: built in | — | ➖ (by design) |
| BASS_CONFIG_OGG_PRESCAN (exact length and seeking of chained/VBR files) | Always exact: the length comes from the last page, chained files are read once at open | automatic | ✅ |
| BASS_ChannelSetPosition on Opus and Vorbis | `fa_stream_seek`: lands on the frame asked for. Opus decodes 300 ms ahead of the target, so the audio after a seek matches a straight decode (bassopus: about 30 dB off in Ogg, 55 dB in WebM); Vorbis is bit exact. About a millisecond per seek ([Ogg](../features/streams.md#ogg-opus-and-ogg-vorbis)) | `stream.Position` | ✅ |
| BASS_ATTRIB_OPUS_GAIN / the header's output gain | The header gain is applied; add your own with the stream volume | `stream.Volume` | ✅ |
| BASS_ATTRIB_OPUS_ORIGFREQ (the encoder's input rate) | Not exposed: Opus always decodes at 48 kHz | — | ➖ |
| BASS_ChannelGetTags(BASS_TAG_OGG) | `fa_stream_get_tags` / `fa_tags_read_path`: Vorbis comments of Ogg Vorbis, Opus and Ogg FLAC, including `METADATA_BLOCK_PICTURE` covers ([tags](../features/tags.md)); ReplayGain / R128 also feed normalization, and radio titles come through `fa_stream_get_metadata` | `GetTags()`, `Tags.Read` | ✅ |

### bassflac

FLAC is a built-in format wherever it appears: `.flac` files, Ogg FLAC, FLAC tracks in MP4 and Matroska, and FLAC frames your app pushes. One decoder (miniaudio's dr_flac) handles all of them, so a track decodes to the same samples whatever it is packed in ([streams](../features/streams.md)).

| bassflac | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_FLAC_StreamCreateFile (`.flac` and Ogg FLAC) | `fa_stream_open_path` / `fa_stream_open_memory` | `engine.OpenFile` / `engine.OpenMemory` | ✅ |
| BASS_FLAC_StreamCreateURL | `fa_stream_open_url` (FLAC and Ogg FLAC streams, Ogg FLAC radio titles from the comment block) | `engine.OpenUrl` | ✅ |
| BASS_FLAC_StreamCreateFileUser | `fa_stream_open_io` | `engine.OpenStream` | ✅ |
| BASS_ChannelSetPosition on FLAC | `fa_stream_seek`: sample exact, under a millisecond, with or without a SEEKTABLE in the file ([FLAC](../features/streams.md#flac)) | `stream.Position` | ✅ |
| FLAC in MP4 / Matroska (bassflac via basswebm or the MP4 demuxer) | Built in, no FFmpeg; `audio_track` picks the track | `StreamOptions.AudioTrack` | ✅ |
| Raw FLAC frames (decoding packets from your own demuxer) | `fa_stream_open_packets` with codec `flac` and the 34-byte STREAMINFO as extradata (or none: then the rate, channels and `bits_per_coded_sample` are used) ([FFmpeg hand-off](../features/ffmpeg.md)) | `engine.OpenPackets`, `PushPacket` | ✅ |
| BASS_PluginLoad("bassflac.dll") | Not needed: built in | — | ➖ (by design) |
| BASS_ChannelGetTags(BASS_TAG_VENDOR / BASS_TAG_OGG on FLAC) | `fa_tags_read_path` / `fa_stream_get_tags`: every Vorbis comment as an item (`FA_TAG_FORMAT_VORBIS`); the vendor string is not exposed, `FA_TAG_ENCODER` gives the `ENCODER` comment ([tags](../features/tags.md)) | `Tags.Items`, `Tags.Encoder` | ✅ |
| BASS_ChannelGetTags(BASS_TAG_FLAC_PICTURE) | `fa_tags_get_picture` / `fa_tags_get_cover` (type, MIME, description, data) | `Tags.Pictures`, `Tags.Cover` | ✅ |
| BASS_ChannelGetTags(BASS_TAG_FLAC_CUE / BASS_TAG_FLAC_METADATA) | Not exposed: embedded cue sheets and application blocks are rare in music libraries | — | ➖ |

### bassalac (Apple Lossless)

ALAC is a built-in format: ALAC tracks in `.m4a`/`.mp4`/`.mov` and Matroska files open with the ordinary calls, decoded by FatzAudio's own decoder on every platform, bit exact, every bit depth and channel layouts up to 7.1, with no plugin, no system codec and no FFmpeg ([streams](../features/streams.md#video-files-and-audio-tracks)).

| bassalac | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_ALAC_StreamCreateFile (ALAC in MP4/M4A) | `fa_stream_open_path` / `fa_stream_open_memory` | `engine.OpenFile` / `engine.OpenMemory` | ✅ |
| BASS_ALAC_StreamCreateURL | `fa_stream_open_url` (M4A over HTTP) | `engine.OpenUrl` | ✅ |
| BASS_ALAC_StreamCreateFileUser | `fa_stream_open_io` | `engine.OpenStream` | ✅ |
| ALAC in CAF (Core Audio Format) files | Not supported: there is no CAF demuxer, so `.caf` files return `FA_ERROR_UNSUPPORTED_FORMAT` | — | 🗓️ |
| BASS_ChannelSetPosition on ALAC | `fa_stream_seek`: sample exact, in MP4 and Matroska alike | `stream.Position` | ✅ |
| BASS_CTYPE_STREAM_ALAC (from BASS_ChannelGetInfo) | `fa_stream_get_info`: codec `alac` | `stream.Info` | ✅ |
| ALAC packets from your own demuxer | `fa_stream_open_packets` with codec `alac` and the 24-byte ALACSpecificConfig (or the `alac` box holding it) as extradata ([FFmpeg hand-off](../features/ffmpeg.md)) | `engine.OpenPackets`, `PushPacket` | ✅ |
| BASS_PluginLoad("bassalac.dll") | Not needed: built in | — | ➖ (by design) |

### basswma (Windows Media Audio)

WMA plays through the ordinary open calls: a built-in ASF demuxer, decoded by Windows' own Media Foundation decoders. On macOS, iOS, Linux and Android WMA is not supported (`FA_ERROR_UNSUPPORTED_FORMAT`); an app that needs it there can decode it with its own FFmpeg and push the frames ([WMA](../features/streams.md#wma)). Only playback is replaced; basswma's encoder and its Windows Media server functions are not.

| basswma | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_WMA_StreamCreateFile (file or memory) | `fa_stream_open_path` / `fa_stream_open_memory` | `engine.OpenFile` / `engine.OpenMemory` | ✅ on Windows; ➖ elsewhere (unsupported) |
| BASS_WMA_StreamCreateFileUser | `fa_stream_open_io` (forward-only when `seek` is NULL) | `engine.OpenStream` | ✅ |
| BASS_StreamCreateURL / BASS_WMA_StreamCreateFile on an `http://` WMA/ASF file | `fa_stream_open_url` | `engine.OpenUrl` | ✅ |
| BASS_WMA_StreamCreateFileAuth (DRM licence) | Protected files return `FA_ERROR_PROTECTED_CONTENT` | `ErrorCode.ProtectedContent` | ➖ (only Windows' DRM components can decrypt them) |
| `mms://` URLs, Windows Media Server HTTP streaming (WMSP), BASS_CONFIG_WMA_PREBUF, BASS_CONFIG_WMA_NETSEEK | Not supported | — | ➖ (the protocol and its servers are retired) |
| `audio_track`-style stream choice in multi-stream / multiple-bitrate files | `fa_open_options.audio_track`, `fa_stream_info.audio_track_count` | `StreamOptions.AudioTrack`, `Info.AudioTrackCount` | ✅ |
| BASS_TAG_WMA_CODEC (via BASS_ChannelGetTags) | `fa_stream_info.codec` (`wmav1`, `wmav2`, `wmapro`, `wmalossless`, `wmavoice`) | `Info.Codec` | ✅ |
| BASS_TAG_WMA / BASS_WMA_GetTags (title, author, attributes) | `fa_tags_read_path` / `fa_stream_get_tags`: Title, Author, Copyright, Description, Rating and every extended attribute (`WM/AlbumTitle`, ...) on every platform, plus `WM/Picture` covers ([tags](../features/tags.md)); ReplayGain and R128 attributes also feed normalization | `Tags.Read`, `GetTags()` | ✅ |
| BASS_TAG_WMA_META, BASS_SYNC_WMA_META, BASS_SYNC_WMA_CHANGE (broadcast script commands and stream changes) | Not supported | — | ➖ (Windows Media broadcasts) |
| BASS_WMA_GetWMObject | No Windows Media Format SDK reader is used | — | ➖ (by design) |
| BASS_CONFIG_WMA_BASSFILE, BASS_CONFIG_WMA_ASYNC, BASS_CONFIG_WMA_VIDEO | Not needed: FatzAudio reads every file through its own reader, decodes on its worker threads, and never reads video | — | ➖ (by design) |
| BASS_WMA_EncodeOpen, EncodeOpenFile, EncodeWrite, EncodeClose, EncodeGetRates, EncodeSetTag, EncodeSetNotify | Not provided | — | ➖ (playback only: WMA is a legacy format nobody needs to create) |
| BASS_WMA_EncodeOpenNetwork(Multi), EncodeOpenPublish(Multi), EncodeGetPort, EncodeGetClients | Not provided | — | ➖ (Windows Media Services broadcasting is retired) |
| BASS_PluginLoad("basswma.dll") | Not needed: built in | — | ➖ (by design) |

### bass_fx tempo, pitch and rate

bass_fx needs a decode stream wrapped in a separate tempo stream; FatzAudio puts three parameters on every stream ([tempo and pitch](../features/tempo-pitch.md)).

| bass_fx | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_FX_TempoCreate(decode stream, BASS_FX_FREESOURCE) | Not needed: open the stream as usual and set the parameters on it | — | ➖ (by design) |
| BASS_FX_TempoGetSource / BASS_FX_TempoGetRateRatio | Not needed: there is one stream; the speed heard is `TEMPO × RATE` | — | ➖ (by design) |
| BASS_ATTRIB_TEMPO (percent, -95 … +5000) | `FA_PARAM_TEMPO` as a multiplier: `1 + percent / 100`, range 0.1 … 10 | `Tempo` | ✅ |
| BASS_ATTRIB_TEMPO_PITCH (semitones) | `FA_PARAM_PITCH`, semitones -36 … 36, exact to the cent | `Pitch` | ✅ |
| BASS_ATTRIB_TEMPO_FREQ (Hz) | `FA_PARAM_RATE` as a multiplier: `new_freq / original_freq` | `Rate` | ✅ |
| BASS_ChannelSlideAttribute on TEMPO / TEMPO_PITCH / TEMPO_FREQ | `fa_stream_ramp_param` with any curve, optionally at an exact time | `Ramp`, `RampAsync` | ✅ |
| BASS_ATTRIB_TEMPO_OPTION_USE_QUICKALGO | `fa_tempo_options.quality = FA_STRETCH_FAST` | `TempoOptions.Quality` | ✅ |
| BASS_ATTRIB_TEMPO_OPTION_USE_AA_FILTER / AA_FILTER_LENGTH | Not needed: the resampler is always band-limited (windowed sinc, about -90 dB) | — | ➖ (by design) |
| BASS_ATTRIB_TEMPO_OPTION_SEQUENCE_MS / SEEKWINDOW_MS / OVERLAP_MS / PREVENT_CLICK | Not needed: no splicing to tune. The stretcher is a phase vocoder; `quality` picks its block size | — | ➖ (by design) |
| Formant-preserving pitch (not in bass_fx) | `fa_tempo_options.keep_formants = 1` | `TempoOptions.KeepFormants` | ✅ |
| Keeping the buffer short so tempo changes respond quickly | `fa_tempo_options.latency_ms` (default 150) | `TempoOptions.Latency` | ✅ |
| Converting positions back with BASS_FX_TempoGetRateRatio | Not needed: positions, markers, the clock and CD+G stay in song time | — | ➖ (by design) |
| BASS_FX_ReverseCreate / BASS_ATTRIB_REVERSE_DIR | Reverse playback | | 🗓️ |
| BASS_FX_BPM_* / BASS_FX_BPM_Beat* | BPM and beat detection | | 🗓️ |

### CD+G karaoke graphics

BASS has no CD+G decoder: apps read the `.cdg` themselves, time it from `BASS_ChannelGetPosition` and draw it. basscd only gives raw subchannel data from discs (`BASS_CD_SUBCHANNEL`). FatzAudio decodes, times and draws CD+G itself ([CD+G](../features/cdg.md)).

| BASS / your app | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| Open the `.mp3` and the `.cdg` separately | `fa_stream_open_path` on the audio loads the `.cdg` with the same name | `engine.OpenFile("song.mp3")`, then `song.HasCdg` | ✅ |
| Unzip a karaoke `.zip` to a temp folder first | `fa_stream_open_path` / `fa_stream_open_memory` on the `.zip` | `engine.OpenFile("song.zip")` | ✅ |
| Read a `.cdg` from elsewhere into your decoder | `fa_stream_load_cdg_path` / `fa_stream_load_cdg_memory` | `song.LoadCdg(path)` / `song.LoadCdg(bytes)` | ✅ |
| Your pack decoder (memory/border preset, tiles, scroll, colour tables) | Built in | Built in | ✅ (CD+EG ❌) |
| Timer: `BASS_ChannelGetPosition` → `Bytes2Seconds` → pack index → decode up to it | `fa_cdg_surface_update` (follows `fa_stream_get_clock`, device latency included) | `surface.Update()` | ✅ |
| Your own sync offset | `fa_stream_set_cdg_offset` | `song.CdgOffset` | ✅ |
| Converting the 300x216 index buffer to a bitmap, scaling, borders | `fa_cdg_surface_config`: BGRA/RGBA/indexed, any size, sharp or smooth, with or without border, transparent background, premultiplied | `CdgSurfaceOptions` | ✅ |
| Redrawing the whole bitmap each tick | Upload only `frame.dirty_*`, only when `frame.changed` | `frame.Changed`, `frame.Dirty` | ✅ |
| CD+G from a disc via `BASS_CD_StreamCreate(..., BASS_CD_SUBCHANNEL)`, then de-interleaving the subchannel and decoding it yourself | `fa_stream_open_disc`: the graphics are read with the audio and drawn by the same surfaces ([disc playback](../features/disc.md#cdg-karaoke-discs)) | `engine.OpenDisc(disc, n)`, then `CreateCdgSurface()` | ✅ |

### basscd (audio CDs)

basscd is an add-on DLL; FatzAudio reads CDs and `.cue` images itself, with one API on Windows, macOS and Linux ([disc playback](../features/disc.md)). Drives are opened by id (`"D:"`, `"/dev/sr0"`, `"disk2"`) instead of a drive number, and a `fa_disc*` handle carries the drive.

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_CD_GetInfo (per drive number) | `fa_disc_drive_count` + `fa_disc_drive_get_info` (id, name, vendor, product, revision) | `Disc.Drives` | ✅ |
| Drive number argument on every call | `fa_disc_open(id, options, &disc)` once, then the `fa_disc*` | `Disc.Open("D:")` | ✅ |
| BASS_CD_SetInterface (SPTI/ASPI/WinMM) | Not needed: the OS pass-through is always used, without administrator rights | — | ➖ |
| BASS_CD_Door (open / close / lock / unlock) | `fa_disc_eject`, `fa_disc_load`, `fa_disc_set_locked` | `Eject()`, `Load()`, `Locked` | ✅ |
| BASS_CD_DoorIsOpen / DoorIsLocked / IsReady | `fa_disc_get_status`: state, locked, change counter (notices swaps between polls) | `disc.Status` | ✅ |
| BASS_CD_GetTracks / GetTrackLength | `fa_disc_get_toc`: tracks with sectors, frames, duration and flags | `disc.Toc` | ✅ |
| BASS_CD_GetTrackPregap | Not provided yet (track starts are INDEX 01) | | 🗓️ |
| BASS_CD_GetID(BASS_CDID_CDDB) | `toc.freedb_query` | `toc.FreedbQuery` | ✅ |
| BASS_CD_GetID(BASS_CDID_CDDB2) | `toc.freedb_id` | `toc.FreedbId` | ✅ |
| BASS_CD_GetID(BASS_CDID_MUSICBRAINZ) | `toc.musicbrainz_id` (Enhanced CDs handled) | `toc.MusicBrainzId` | ✅ |
| BASS_CD_GetID(BASS_CDID_TEXT) (parse the null-separated list yourself) | `fa_disc_get_text(disc, track, FA_DISC_TEXT_TITLE, ...)`: one field, UTF-8 | `disc.GetText(track, DiscTextField.Title)` | ✅ |
| BASS_CD_GetID(BASS_CDID_UPC / BASS_CDID_ISRC + track) | `fa_disc_get_text(..., FA_DISC_TEXT_CODE, ...)` (track 0 = UPC/EAN) | `DiscTextField.Code` | ✅ |
| BASS_CD_GetID(BASS_CDID_CDPLAYER), BASS_CD_GetID(BASS_CDID_TOC...) raw TOC | Not provided; the parsed TOC has everything | | ➖ |
| BASS_CD_GetID(BASS_CDID_CDDB_QUERY / CDDB_READ) online lookups | Not provided: no network access from the library; look the ids up where you like | | ➖ |
| BASS_CD_GetSpeed / SetSpeed (kB/s, 176.4 = 1x) | `fa_disc_get_speed` / `fa_disc_set_speed` in x (0 = maximum) | `disc.Speed` | ✅ |
| BASS_CD_SetOffset | `fa_disc_options.read_offset` (samples) | `DiscOptions.ReadOffset` | ✅ |
| BASS_CD_StreamCreate | `fa_stream_open_disc(engine, disc, track, options, &stream)` (track numbers as on the disc, not 0-based) | `engine.OpenDisc(disc, track)` | ✅ |
| BASS_CD_StreamCreateFile (`.cda`) | `fa_stream_open_path` (Windows) | `engine.OpenFile` | ✅ |
| BASS_CD_StreamCreateEx / BASS_CD_SUBCHANNEL, BASS_CD_SUBCHANNEL_NOHW (subchannel callback) | Not needed for CD+G: `fa_stream_open_disc` reads, de-interleaves and decodes it; `FA_DISC_NO_CDG` skips the subcode. Raw subchannel access is not provided | `DiscOptions.ReadCdg` | ✅ CD+G, ➖ raw access |
| BASS_CD_StreamGetTrack | Not needed: you opened the track, so you know its number | | ➖ |
| BASS_CD_StreamSetTrack (switch track on the same stream) | Open the next track and `fa_stream_set_next` (gapless) or crossfade | `SetNext` | ✅ |
| BASS_CD_Analog_* (drive's own DAC) | Not provided: modern drives have no analog output | | ➖ |
| BASS_CONFIG_CD_RETRY | `fa_disc_options.retries` (default 5) | `DiscOptions.Retries` | ✅ |
| BASS_CONFIG_CD_SKIPERROR | Always on: unreadable sectors play as silence and raise `FA_EVENT_DECODE_ERROR` | automatic | ✅ |
| BASS_CD_C2ERRORS / BASS_CONFIG_CD_... C2 handling | Automatic when the drive supports it; `FA_DISC_NO_C2` turns it off | `DiscOptions.UseC2` | ✅ |
| BASS_CONFIG_CD_AUTOSPEED, BASS_CONFIG_CD_FREEOLD, BASS_CONFIG_CD_CDDB_SERVER, BASS_CD_READ | Not needed (reads are buffered per stream; no server access) | | ➖ |
| De-emphasis of pre-emphasized tracks (your own filter) | Automatic for tracks with the PRE flag; `FA_DISC_NO_DEEMPHASIS` turns it off | `DiscOptions.DeEmphasis` | ✅ |

### bassmidi (MIDI files and soundfonts)

MIDI files are a built-in format: the ordinary open calls play them with a SoundFont 2 you load once ([MIDI](../features/midi.md)). There are no per-stream font arrays, font handles per preset or sample loading on demand.

| BASSMIDI | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_MIDI_StreamCreateFile / BASS_MIDI_StreamCreateFileUser | `fa_stream_open_path` / `fa_stream_open_memory` / `fa_stream_open_io` (`.mid`, `.kar`, `.rmi` detected from content) | `engine.OpenFile` / `engine.OpenMemory` / `engine.OpenStream` | ✅ |
| BASS_MIDI_StreamCreateURL | `fa_stream_open_url` | `engine.OpenUrl` | ✅ |
| BASS_PluginLoad("bassmidi.dll") | Not needed: built in | — | ➖ (by design) |
| BASS_MIDI_FontInit (file or BASS_MIDI_FONT_MEM) | `fa_soundfont_open_path` / `fa_soundfont_open_memory` | `SoundFont.FromFile` / `SoundFont.FromMemory` | ✅ SF2; SF3 and SFZ 🗓️ |
| BASS_MIDI_FontInitUser | Read the file yourself and use `fa_soundfont_open_memory` | `SoundFont.FromMemory` | ✅ |
| BASS_MIDI_FontFree | `fa_soundfont_close` (reference-counted: streams using it keep it) | `soundFont.Dispose()` | ✅ |
| BASS_MIDI_FontGetInfo | `fa_soundfont_get_info` (name, presets, sample memory) | `soundFont.Info` | ✅ |
| BASS_MIDI_FontLoad / BASS_MIDI_FontCompact / BASS_MIDI_FontUnload | Not needed: the whole bank loads at open | — | ➖ (by design) |
| BASS_MIDI_FontPack / BASS_MIDI_FontUnpack (SF2PACK) | Not provided: SF3 support is planned instead | — | ➖ |
| BASS_CONFIG_MIDI_DEFFONT, BASS_MIDI_StreamSetFonts(0, ...) | `fa_engine_set_soundfont` | `engine.DefaultSoundFont` | ✅ |
| BASS_MIDI_StreamSetFonts(stream, ...) | `fa_open_options.soundfont` when opening | `StreamOptions.SoundFont` | ✅ (one font per stream; no stacks or per-preset mapping) |
| BASS_MIDI_FontSetVolume | Stream volume (`FA_PARAM_VOLUME`) | `Volume` | ✅ |
| BASS_MIDI_StreamGetMark (LYRIC, TEXT, MARKER, CUE, COPY, TRACK, INST) | `fa_stream_get_midi_text` (cleaned-up lyrics with line and paragraph flags, UTF-8) and `fa_stream_get_midi_info` | `stream.MidiTexts`, `stream.MidiInfo` | ✅ |
| BASS_MIDI_MARK_KEYSIG / TIMESIG / TEMPO / TICK | Not provided | — | 🗓️ |
| BASS_SYNC_MIDI_LYRIC / BASS_SYNC_MIDI_TEXT / BASS_SYNC_MIDI_MARK | `FA_EVENT_MIDI_TEXT` (`value` = text index) | `stream.MidiTextReached` | ✅ |
| BASS_MIDI_StreamEvent MIDI_EVENT_TRANSPOSE | `fa_stream_set_midi_transpose` | `stream.MidiTranspose` | ✅ |
| BASS_MIDI_StreamEvent MIDI_EVENT_MUTE | `fa_stream_set_midi_channel_mute` | `stream.SetMidiChannelMuted` | ✅ |
| BASS_MIDI_StreamEvent MIDI_EVENT_MASTERVOL / BASS_ATTRIB_VOL | `FA_PARAM_VOLUME` | `Volume` | ✅ |
| BASS_MIDI_StreamEvent MIDI_EVENT_TEMPO / BASS_ATTRIB_MIDI_SPEED | `FA_PARAM_TEMPO` (lyrics and the clock follow) | `Tempo` | ✅ |
| BASS_MIDI_StreamEvent (other events: notes, programs, controllers), BASS_MIDI_StreamEvents | Not provided | — | 🗓️ |
| BASS_MIDI_StreamCreate / BASS_MIDI_StreamCreateEvents (real-time synth, event lists) | Not provided | — | 🗓️ |
| BASS_MIDI_StreamGetEvent / BASS_MIDI_StreamGetEvents | Not provided | — | 🗓️ |
| BASS_MIDI_InInit / BASS_MIDI_InStart (MIDI input devices) | Not provided | — | 🗓️ |
| BASS_ATTRIB_MIDI_VOICES | Fixed at 256 voices per stream | — | ✅ (fixed) |
| BASS_ATTRIB_MIDI_PPQN, BASS_POS_MIDI_TICK | Not provided: positions are frames and time | — | ➖ |
| BASS_ATTRIB_MIDI_TRACK_VOL, BASS_MIDI_StreamGetChannel | Not provided | — | 🗓️ |
| BASS_MIDI_DECAYEND | Always on: one second after the last event | — | ✅ |
| BASS_MIDI_DECAYSEEK (held notes after a seek) | Not provided: held notes sound again from their next note-on | — | ➖ |
| BASS_MIDI_NOFX (reverb and chorus) | No reverb or chorus yet | — | 🗓️ |
| BASS_ATTRIB_MIDI_SRC / BASS_MIDI_SINCINTER | Not configurable: the synthesizer's own interpolation | — | ➖ |

## 3. Add-on map

| Legacy DLL | Replaced by | Status |
|---|---|---|
| bass_aac.dll | `BASS_AAC_StreamCreateFile`/`URL`/`User` → the normal open calls: MP4/M4A/MOV files (built-in demuxer, any audio track), `.aac` (ADTS) files and AAC radio, and AAC packets (`fa_stream_open_packets`), decoded by the built-in AAC decoder (LC, HE-AAC, HE-AAC v2, Main, LTP) on every platform | ✅ |
| bass_ac3.dll | Built-in AC-3 and E-AC-3 decoder: raw `.ac3`/`.ec3` files, MP4/MKV tracks, web streams and HLS, and pushed packets, with dynamic range modes ([Dolby Digital](../features/dolby.md)) | ✅ (passthrough 🗓️) |
| bassalac.dll | Built-in ALAC decoder: ALAC tracks in MP4/M4A/MOV and Matroska files, over HTTP, and pushed packets, bit exact on every platform ([bassalac](#bassalac-apple-lossless)) | ✅ (CAF files 🗓️) |
| bassflac.dll | Built-in: `.flac` files, Ogg FLAC (`.oga`), FLAC tracks in MP4 and Matroska, FLAC web streams and FLAC packets through `fa_stream_open_packets`, all with the same decoder ([bassflac](#bassflac)) | ✅ |
| basshls.dll (if used) | Built-in: `fa_stream_open_url` plays `.m3u8` (MPEG-TS, fMP4, AES-128) | ✅ |
| bassopus.dll | Built-in: `.opus` files, Opus web radio, Opus in WebM/MKV/MP4, and raw packets through `fa_stream_open_packets` ([Ogg](../features/streams.md#ogg-opus-and-ogg-vorbis)) | ✅ |
| basswebm.dll | Built-in Matroska/WebM demuxer, any audio track (`audio_track`); PCM, MP3, AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC and ALAC tracks built in, others unsupported ([streams](../features/streams.md#video-files-and-audio-tracks)) | ✅ |
| OGG Vorbis in bass.dll | Built-in: `.ogg` files, chained files, Ogg radio with song titles ([Ogg](../features/streams.md#ogg-opus-and-ogg-vorbis)) | ✅ |
| basswma.dll | Built-in ASF demuxer (`.wma`, `.asf`, `.wmv` audio; files, memory, your own reader, HTTP), decoded by Media Foundation on Windows (unsupported elsewhere); ReplayGain attributes for normalization, tags and covers ([WMA](../features/streams.md#wma)) | ✅ playback; ➖ encoding (record to MP3, FLAC or WAV with `fa_encoder_*` instead), MMS, broadcasting and DRM |
| bassenc.dll, bassenc_mp3.dll, bassenc_flac.dll (if added later) | Built in: `fa_encoder_*` writes WAV and FLAC itself and MP3 through the app's LAME ([recording and encoding](../features/recording-encoding.md)) | ✅ WAV/FLAC/MP3, 🗓️ Opus/AAC/casting |
| bassmidi.dll | Built-in SoundFont 2 synthesizer for `.mid`, `.kar` and `.rmi`: engine-wide or per-stream soundfont, lyrics and text events, transpose and channel mute ([MIDI](../features/midi.md)) | ✅ (SF3, SFZ and MIDI input 🗓️) |
| bass_fx.dll | Tempo/pitch/rate params on every stream ([tempo and pitch](../features/tempo-pitch.md)); reverse and BPM planned. DAMP and COMPRESSOR2 used for level control are replaced by normalization and the output limiter | ✅ tempo/pitch/rate and level control, 🗓️ reverse and BPM |
| basscd.dll | Built-in disc source: drives on Windows, macOS and Linux, `.cue` images, CD-Text, freedb and MusicBrainz ids, CD+G graphics ([disc playback](../features/disc.md)); analog playback not provided | ✅ disc and CD+G discs |
| Your own CD+G decoder and renderer (MP3+G, ZIP) | Built in: `fa_cdg_surface_*` draws the graphics in sync with the audio ([CD+G](../features/cdg.md)) | ✅ |

## 4. Common patterns

### Play a file

BASS:
```c
BASS_Init(-1, 44100, 0, hwnd, NULL);
HSTREAM h = BASS_StreamCreateFile(FALSE, L"song.flac", 0, 0, BASS_UNICODE);
if (!h) h = BASS_FLAC_StreamCreateFile(FALSE, L"song.flac", 0, 0, BASS_UNICODE); /* or BASS_PluginLoad first */
if (!h) printf("error %d\n", BASS_ErrorGetCode());
BASS_ChannelPlay(h, FALSE);
```

FatzAudio:
```c
fa_engine* e; fa_device* d; fa_stream* s;
fa_engine_create(NULL, &e);
fa_device_open(e, NULL, &d);
fa_result r = fa_stream_open_path(e, "song.flac", NULL, &s);  /* any supported format */
if (r != FA_OK) printf("error: %s\n", fa_result_string(r));
fa_stream_attach(s, d, NULL);
fa_device_start(d);
fa_stream_play(s);
```

.NET:
```csharp
using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();
using var song = engine.OpenFile("song.flac");
song.PlayOn(output);
```

### Decode to PCM (no playback)

BASS: `BASS_StreamCreateFile(..., BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT)` then loop `BASS_ChannelGetData(h, buf, len)`.

FatzAudio: open with `FA_OPEN_DECODE_ONLY`, then loop `fa_stream_read(s, buf, frames, &got)` until it returns `FA_ERROR_END_OF_STREAM`. In .NET, `stream.Read(span)` until it returns 0.

### Seek to 30 seconds

BASS: `BASS_ChannelSetPosition(h, BASS_ChannelSeconds2Bytes(h, 30.0), BASS_POS_BYTE);`

FatzAudio: `fa_stream_seek(s, 30 * info.format.sample_rate);` or in .NET `song.Position = TimeSpan.FromSeconds(30);`

### Read from your own stream (BASS_StreamCreateFileUser)

BASS:
```c
BASS_FILEPROCS procs = { MyClose, MyLen, MyRead, MySeek };
HSTREAM h = BASS_StreamCreateFileUser(STREAMFILE_NOBUFFER, 0, &procs, user);
```

FatzAudio:
```c
fa_io_callbacks io = { my_read, my_seek, my_length, my_close };
fa_stream* s;
if (fa_stream_open_io(engine, &io, user, NULL, &s) != FA_OK)
    free_user(user);            /* on failure the caller still owns user */
/* ... later: fa_stream_close(s) calls my_close(user) exactly once */
```

When porting, check your BASS error path: in FatzAudio a failed open never calls `close`, so the error path frees `user` itself.

.NET: `engine.OpenStream(stream, leaveOpen: false)`.

### Move playback to another device (BASS_ChannelSetDevice)

BASS: `BASS_ChannelSetDevice(h, newDevice);` (both devices must be initialised, and you juggle `BASS_SetDevice` around it).

FatzAudio: `fa_stream_attach(s, other_device, NULL);` keeps the position, and play/pause state carries on. In .NET: `song.PlayOn(otherDevice);`.

### Fade out, then stop

BASS:
```c
BASS_ChannelSlideAttribute(h, BASS_ATTRIB_VOL, 0, 3000);         /* linear or log only */
BASS_ChannelSetSync(h, BASS_SYNC_SLIDE | BASS_SYNC_ONETIME, 0, SlideDone, 0);
/* SlideDone fires for any finished slide on the channel; check which attribute... */
```

FatzAudio:
```c
fa_ramp r = { FA_CURVE_EQUAL_POWER }; r.tag = 7;
fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, 3000000000LL, 0, &r);
/* FA_EVENT_RAMP_DONE with value == 7 tells you exactly which ramp finished */
```

.NET:
```csharp
await song.RampAsync(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower);
song.Stop();
```

### React to the end of a track (BASS_SYNC_END)

BASS: `BASS_ChannelSetSync(h, BASS_SYNC_END, 0, EndProc, user);`. The SYNCPROC runs on BASS's update thread, and `BASS_SYNC_MIXTIME` variants run inside the mixer, where most BASS calls aren't allowed.

FatzAudio: `fa_stream_set_event_callback(s, on_event, user);` and handle `FA_EVENT_ENDED`. The callback runs on the event thread, so it can start the next track, close the stream, or update state directly. .NET: `song.Ended += ...`.

### Internet radio with song titles (BASS_StreamCreateURL + BASS_SYNC_META)

BASS:
```c
HSTREAM h = BASS_StreamCreateURL(url, 0, BASS_STREAM_AUTOFREE, NULL, NULL);
BASS_ChannelSetSync(h, BASS_SYNC_META, 0, MetaSync, NULL);
/* in MetaSync: parse "StreamTitle='...';" out of BASS_ChannelGetTags(h, BASS_TAG_META) */
BASS_ChannelSetSync(h, BASS_SYNC_STALL, 0, StallSync, NULL);
BASS_ChannelPlay(h, FALSE);
```

FatzAudio ([web streams](../features/web-streams.md)):
```c
fa_stream* radio;
fa_stream_open_url(engine, url, NULL, &radio);
fa_stream_set_event_callback(radio, on_event, NULL);  /* FA_EVENT_METADATA, STALLED, RESUMED */
fa_stream_attach(radio, device, NULL);
fa_stream_play(radio);
/* in on_event: fa_metadata md; fa_stream_get_metadata(ev->stream, &md); md.artist, md.title */
```

.NET:
```csharp
using var radio = engine.OpenUrl(url);
radio.MetadataChanged += (_, e) => nowPlaying.Text = $"{e.Metadata.Artist} - {e.Metadata.Title}";
radio.PlayOn(output);
```

No title parsing, no Latin-1 guessing, and the title changes when the listener hears the new song. Reconnecting after a dropped connection is built in (`reconnect_ms`), where BASS apps usually restart the stream themselves.

### Normalize loudness (replaces your own analysis pass)

BASS (typical app code):
```c
/* 1. Analyse: decode the whole file once, on your own thread. Slow on long videos. */
HSTREAM dec = BASS_StreamCreateFile(FALSE, path, 0, 0, BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
while (BASS_ChannelIsActive(dec)) { BASS_ChannelGetData(dec, buf, sizeof(buf)); /* your RMS/peak math */ }
BASS_StreamFree(dec);
/* 2. Play, then set the gain you computed. */
HSTREAM h = BASS_StreamCreateFile(FALSE, path, 0, 0, 0);
BASS_ChannelSetAttribute(h, BASS_ATTRIB_VOL, gain);
BASS_ChannelPlay(h, FALSE);
```

FatzAudio:
```c
fa_engine_set_normalization(engine, FA_NORMALIZE_TRACK, -16.0);  /* once, at startup */
fa_stream_open_path(engine, path, NULL, &s);                      /* analysis starts in the background */
fa_stream_attach(s, device, NULL);
fa_stream_play(s);                                                /* no gain to set */
```

.NET:
```csharp
engine.SetNormalization(NormalizeMode.Track, -16);   // once
using var song = engine.OpenFile(path);
song.PlayOn(output);
```

When porting, delete the analysis pass and the `BASS_ATTRIB_VOL` call that applied its result; keep using volume for fades and the user's volume control. If your library database already holds loudness values, pass them with `fa_stream_set_loudness` / `song.SetLoudness(lufs)` and no file is scanned at all.

### Crossfade to the next track

BASS (typical app code):
```c
/* At a position sync near the end of the current track: */
HSTREAM next = BASS_StreamCreateFile(FALSE, path, 0, 0, 0);
BASS_ChannelSetAttribute(next, BASS_ATTRIB_VOL, 0);
BASS_ChannelPlay(next, FALSE);                                /* starts whenever the next update runs */
BASS_ChannelSlideAttribute(next, BASS_ATTRIB_VOL, 1, 5000);   /* linear or log only */
BASS_ChannelSlideAttribute(cur, BASS_ATTRIB_VOL, -1, 5000);   /* -1 = stop when the slide ends */
/* ...and the user's volume setting is lost, because the fade used BASS_ATTRIB_VOL. */
```

FatzAudio:
```c
fa_stream_open_path(engine, path, NULL, &next);
fa_crossfade(cur, next, 5000000000LL, NULL);   /* equal power; both start on the same frame */
/* cur ends by itself when its fade is done (FA_EVENT_ENDED) */
```

.NET:
```csharp
var next = engine.OpenFile(path);
await current.CrossfadeTo(next, TimeSpan.FromSeconds(5));
current.Dispose();
current = next;
```

When porting, delete the position sync and both slides. The volume the user set stays as it was, because crossfades have their own gain.

### Gapless playlist or automatic crossfade

BASS: a `BASS_SYNC_END | BASS_SYNC_MIXTIME` sync that creates and starts the next stream inside the mixer, with a small gap or overlap depending on the update period.

FatzAudio:
```c
fa_stream_set_next(cur, next, 0, NULL);               /* gapless, to the sample */
fa_stream_set_next(cur, next, 6000000000LL, NULL);    /* or: 6 s crossfade before the end */
/* FA_EVENT_STARTED on next: queue the one after it */
```

.NET:
```csharp
current.QueueNext(next);                               // gapless
current.QueueNext(next, TimeSpan.FromSeconds(6));      // or a 6 s crossfade
next.Started += (_, _) => next.QueueNext(OpenFollowing());
```

Per-track points from your database (where the intro starts, where the outro fades, where the next song should come in) go into markers: `Markers.CueIn`, `CueOut`, `FadeOutStart`, `MixPoint`. The queue uses them.

### Two decks and a crossfader

BASS: two channels and two `BASS_ChannelSetAttribute(h, BASS_ATTRIB_VOL, ...)` calls from the slider, or a BASSmix mixer per deck.

FatzAudio: one bus per deck, and the slider sets or ramps the bus volumes ([buses](../features/buses.md)). In .NET: `deckA.Volume = MathF.Cos(x * MathF.PI / 2); deckB.Volume = MathF.Sin(x * MathF.PI / 2);`.

### Audio of a video played with FFmpeg

BASS: decode every audio packet with libavcodec, convert it with swresample to the push stream's format, `BASS_StreamPutData` it, and on every video frame compute the audio time from `BASS_ChannelGetPosition`, the device latency and the first pts, then nudge the video when it drifts. After a seek you clear the push stream and recompute the offset.

FatzAudio: push the packets and show the video frame for the clock ([FFmpeg hand-off](../features/ffmpeg.md)).

```c
fa_codec_params params;
fa_codec_params_from_av(&params, st->codecpar, st->time_base);   /* fatzaudio_ffmpeg.h */
fa_stream_open_packets(engine, &params, NULL, &audio);
fa_stream_attach(audio, device, NULL);
fa_stream_play(audio);

/* demux loop */
fa_packet p;
fa_packet_from_av(&p, pkt);
fa_stream_push_packet(audio, &p);

/* render loop */
fa_time now;
fa_stream_get_clock(audio, &now);   /* the pts being heard, in ns */

/* seek */
av_seek_frame(fmt, -1, target, AVSEEK_FLAG_BACKWARD);
fa_stream_push_flush(audio);
```

.NET:
```csharp
using var audio = engine.OpenPackets(parameters);
audio.PlayOn(output);
audio.PushPacket((nint)pkt->data, pkt->size, pkt->pts, pkt->duration);   // demux loop
TimeSpan now = audio.Clock;                                               // render loop
audio.Flush();                                                            // after seeking the demuxer
```


### Karaoke (MP3+G or ZIP) with lyrics on screen

BASS: open the audio with `BASS_StreamCreateFile`, read the `.cdg` (unzipping first if needed), and on a timer convert `BASS_ChannelGetPosition` to a pack index, decode up to it and repaint a bitmap.

FatzAudio: open the audio (or the `.zip`); the graphics come with it. Create a surface once and call it from the render loop ([CD+G](../features/cdg.md)):

```csharp
using var song = engine.OpenFile(path);                   // song.mp3 (+ song.cdg) or song.zip
using var cdg = song.CreateCdgSurface(new CdgSurfaceOptions { Premultiplied = true });
song.PlayOn(output);

CompositionTarget.Rendering += (_, _) =>
{
    var frame = cdg.Update();                             // the picture for what is heard now
    if (frame.Changed) CopyToBitmap(frame);               // only frame.Dirty changed
};
```

Pausing, seeking, crossfading to the next singer and tempo changes need no extra code: delete the timer, the position maths and the pack decoder.

### Play an audio CD (BASS_CD_StreamCreate)

BASS: pick a drive number, check `BASS_CD_IsReady`, loop `BASS_CD_GetTracks`/`GetTrackLength`, split the `BASS_CDID_TEXT` list yourself, then `BASS_CD_StreamCreate(drive, track, 0)` and `BASS_ChannelPlay`.

```c
fa_disc* disc;
fa_disc_open(NULL, NULL, &disc);                  /* first drive; or "D:", "/dev/sr0", "album.cue" */
fa_disc_toc toc;
if (fa_disc_get_toc(disc, &toc) == FA_ERROR_NO_DISC) { /* ask for a disc */ }
fa_stream* s;
fa_stream_open_disc(engine, disc, toc.first_track, NULL, &s);
fa_stream_attach(s, device, NULL);
fa_stream_play(s);
fa_disc_close(disc);                               /* the stream keeps the drive open */
```

```csharp
using var disc = Disc.Open();
using var track = engine.OpenDisc(disc, disc.Toc.FirstTrack);
track.PlayOn(output);
```

Poll `fa_disc_get_status` (`disc.Status`) to notice a new disc; a stream of a removed disc ends with `FA_ERROR_NO_DISC`.

### MIDI karaoke (.kar) with lyrics (bassmidi)

BASS: `BASS_MIDI_FontInit` and `BASS_MIDI_StreamSetFonts`, `BASS_MIDI_StreamCreateFile`, then `BASS_MIDI_StreamGetMark(BASS_MIDI_MARK_LYRIC)` for the text and `BASS_SYNC_MIDI_LYRIC` to highlight it, stripping the `/` and `\` marks yourself; `MIDI_EVENT_TRANSPOSE` for the key.

FatzAudio ([MIDI](../features/midi.md)):

```csharp
using (var gm = SoundFont.FromFile("GeneralUser.sf2")) engine.DefaultSoundFont = gm;   // once
using var song = engine.OpenFile("song.kar");
var lines = song.MidiTexts.Where(t => t.Type == MidiTextType.Lyric).ToList();         // marks already turned into Flags
song.MidiTextReached += (_, e) => Highlight(e.Text.Index);
song.MidiTranspose = -2;                                                               // a whole tone down, drums untouched
song.PlayOn(output);
```

### Tempo and key change (bass_fx)

BASS:
```c
HSTREAM dec = BASS_StreamCreateFile(FALSE, path, 0, 0, BASS_STREAM_DECODE | BASS_STREAM_PRESCAN);
HSTREAM h = BASS_FX_TempoCreate(dec, BASS_FX_FREESOURCE);
BASS_ChannelSetAttribute(h, BASS_ATTRIB_TEMPO, -15);         /* percent */
BASS_ChannelSetAttribute(h, BASS_ATTRIB_TEMPO_PITCH, -2);    /* semitones */
BASS_ChannelPlay(h, FALSE);
/* positions: BASS_ChannelGetPosition(dec, ...) on the source, not h */
```

FatzAudio:
```c
fa_stream_open_path(engine, path, NULL, &s);
fa_stream_set_param(s, FA_PARAM_TEMPO, 0.85f);              /* multiplier: 1 - 15 / 100 */
fa_stream_set_param(s, FA_PARAM_PITCH, -2.0f);
fa_stream_attach(s, device, NULL);
fa_stream_play(s);
/* positions, markers and the clock are in song time already */
```

.NET:
```csharp
using var song = engine.OpenFile(path);
song.Tempo = 0.85f;
song.Pitch = -2f;
song.TempoOptions = new TempoOptions { KeepFormants = true };   // natural voices on key changes
song.PlayOn(output);
```

When porting, delete the tempo stream, the `BASS_FX_FREESOURCE` bookkeeping and any position conversion through `BASS_FX_TempoGetRateRatio`. bass_fx's tempo is in percent and its frequency in Hz; convert both to multipliers.

### Record the output to MP3 (BASSenc + BASSenc_MP3)

BASS:
```c
HENCODE enc = BASS_Encode_MP3_StartFile(mixer, "-V 2 --tt \"Live\"", BASS_ENCODE_AUTOFREE | BASS_UNICODE, L"live.mp3");
/* ... */
BASS_Encode_Stop(enc);
```

FatzAudio:
```c
fa_encoder_options o;
fa_encoder_options_init(&o);
o.format = FA_ENCODE_MP3;      /* VBR quality 2 = -V 2 */
o.title = "Live";
fa_encoder* enc;
fa_encoder_create_path("live.mp3", &o, &enc);
fa_encoder_attach_device(enc, device);   /* or fa_encoder_attach_stream(enc, deck) */
/* ... */
fa_encoder_close(enc);         /* FA_OK = the file is complete */
```

.NET:
```csharp
using var enc = AudioEncoder.Create("live.mp3", new EncoderOptions { Format = EncodeFormat.Mp3, Title = "Live" });
enc.Record(output);
// ...
enc.Close();
```

### Convert a file (decoding channel + encoder)

BASS needs a decoding channel, an encoder on it and a `BASS_ChannelGetData` loop to drive it. FatzAudio's encoder reads the stream itself on its own thread:

```c
fa_encoder_encode_stream(enc, decode_only_stream);
fa_encoder_wait(enc, -1);
fa_encoder_close(enc);
```

```csharp
AudioEncoder.ConvertFile(engine, "in.mp3", "out.flac", new EncoderOptions { Format = EncodeFormat.Flac });
```

*More patterns are added here as each feature lands.*

## 5. Measured against BASS

[tools/bass-compare](../../tools/bass-compare/README.md) decodes the same generated files through both libraries (BASS as a black box, through its public API) and scores them. The legacy `bass.dll` reports itself as 2.4.17.0 through `BASS_GetVersion`. Lossless formats (WAV, FLAC) are bit exact in both and AC-3 scores the same; FatzAudio's AAC follows the file's encoder delay where bass_aac does not, and it seeks AC-3 exactly where BASS lands about 20 ms off.

### Positions that move when you port

Stored positions (cue points, mix points, lyrics or CD+G offsets saved by your app) were measured on BASS's timeline. These formats start at a different frame, so shift stored positions by the amount below:

| Format | BASS | FatzAudio | Shift a stored BASS position by |
|---|---|---|---|
| AAC in MP4/M4A (and video files) | Plays the encoder's priming (1024 frames) and ignores the edit list, so it is 1024 frames long too | Starts on the first real sample, exact length | −1024 frames (about −23 ms at 44.1 kHz) |
| WAV, FLAC, MP3 (with or without a LAME header), AC-3, AAC in `.aac` (ADTS) files and AAC radio | Same frame | Same frame | none |

### Playback timing

With `--rec`, the harness also plays both libraries through the output device and records what comes out (tools/bass-compare README, "Playback on a real device"). Measured on Windows hardware (WASAPI loopback of the default output device, 2026-10-17):

- **Gapless:** `fa_stream_set_next` joins two files with no gap and no overlap. With plain BASS, starting the next stream from a `BASS_SYNC_END` sync leaves a gap of about 50 ms, and with `BASS_SYNC_MIXTIME` the next stream starts about 460 ms early, while the end of the first is still in BASS's playback buffer. Replace either with `fa_stream_set_next(current, next, 0, NULL)`.
- **Crossfades:** both libraries follow the curve closely and start both fades together (within 1 ms). `fa_crossfade` also offers equal power (no dip in loudness); BASS slides are linear, which dips 3 dB in the middle.
- **Latency:** in this run FatzAudio started a sound about 9 ms later than BASS (36.5 against 27.7 ms from the call to the sound) and a crossfade about 19 ms later (49.5 against 30.1 ms). The causes were its device buffer (three 10 ms periods, 32 ms, against BASS's 25 ms) and one extra period that `fa_crossfade` waited. Both are fixed since: the buffer on Windows is two periods, and a crossfade starts in the next callback, like a play. That should take about 10 ms off a start and about 20 ms off a crossfade; a new run will confirm it.

### Where BASS is still ahead

The comparison report opens with every measurement where BASS won; the ones to fix are in the [engineering notes](../engineering-notes.md) (area "bass-compare"). In short: the start and crossfade latency measured on Windows hardware (about 9 and 19 ms behind BASS) is fixed but not yet measured again; WMA has not been compared yet (basswma does not run under Wine), and 16 AC-3 5.1 streams at once take more memory each than in bass_ac3. The AC-3 decoder now runs faster than bass_ac3 (5.1 about 400× real time against 350×, stereo 900× against 820×) and opens a file in about 0.5 ms. MP3 now seeks exactly in about 0.3 ms (BASS lands off target unless it prescans), decodes as accurately as BASS, and its loudness scan runs faster than BASS decodes the file. `.aac` files and AAC radio start on the same frame as in bass_aac. Ogg Opus and Ogg Vorbis match bassopus and BASS's Vorbis decoder.

