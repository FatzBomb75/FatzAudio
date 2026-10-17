# Migrating from BASS

This guide maps every BASS 2.4.18 function, and every add-on in the legacy app (bass_aac, bass_ac3, bass_fx, basscd, bassflac, bassmidi, bassopus, basswebm, basswma), to FatzAudio. It's updated as each feature lands. Status symbols are explained in the [docs index](../README.md#status-legend).

**Goal:** after migration the app ships one native library per platform (`fatzaudio.dll`/`.dylib`/`.so`) and none of the BASS DLLs.

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
| BASS_CONFIG_NET_PROXY | `fa_net_options.proxy`: `NULL` = system settings and environment, `""` = direct, or `"user:pass@host:port"` | `NetworkOptions.Proxy` | ✅ |
| BASS_CONFIG_NET_PLAYLIST | Always on: `.pls` and `.m3u` are followed to their first entry, `.m3u8` plays as HLS | automatic | ✅ |
| BASS_CONFIG_NET_META | Always asks for ICY titles; `FA_NET_NO_ICY` turns it off | `NetworkOptions.SongTitles = false` | ✅ |
| BASS_CONFIG_NET_RESTRATE | Not needed: the download only runs 2 MB ahead of playback, so it's paced by playback anyway | — | ➖ (by design) |
| BASS_CONFIG_NET_PASSIVE (FTP) | FTP isn't supported | — | ➖ |
| BASS_CONFIG_UPDATETHREADS / UPDATEPERIOD | `fa_engine_config.worker_threads`; workers refill the emptiest buffer first, nothing to tune | `AudioEngineOptions.WorkerThreads` | ✅ |
| BASS_Update / BASS_ChannelUpdate | Not needed: worker threads keep buffers full | — | ➖ (by design) |
| BASS_GetCPU | Planned `fa_engine_get_stats` | | 🗓️ |
| BASS_GetDSoundObject | Not provided (Windows-only escape hatch) | — | ➖ |

### Streams

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_StreamCreateFile (path) | `fa_stream_open_path`; format detected from content ([streams](../features/streams.md)) | `engine.OpenFile(path)` | ✅ WAV/AIFF/MP3/FLAC so far |
| BASS_StreamCreateFile (memory) | `fa_stream_open_memory` | `engine.OpenMemory(bytes)` | ✅ |
| BASS_StreamCreateFileUser | `fa_stream_open_io`; `close` is called once by `fa_stream_close`, never on a failed open ([custom I/O](../features/streams.md#custom-io-your-own-reader)) | `engine.OpenStream(System.IO.Stream)` | ✅ |
| BASS_StreamCreateURL | `fa_stream_open_url`: HTTP/HTTPS files with range seeking, Icecast/Shoutcast radio, HLS, `.pls`/`.m3u`; reconnects by itself ([web streams](../features/web-streams.md)) | `engine.OpenUrl(url)` | ✅ (AAC through the app's FFmpeg) |
| BASS_StreamCreateURL `DOWNLOADPROC` (raw bytes to save a station) | Planned | | 🗓️ |
| BASS_StreamCreateURL `BASS_STREAM_BLOCK` | Not needed: memory per web stream is bounded (4 MB) and seeking still works | — | ➖ (by design) |
| BASS_StreamCreateURL `BASS_STREAM_STATUS` (+ `BASS_TAG_HTTP` during connect) | `fa_stream_get_http_header` and `fa_stream_get_net_status` after the open | `GetHttpHeader`, `NetworkStatus` | ✅ |
| BASS_StreamCreate (STREAMPROC_PUSH) + BASS_StreamPutData | `fa_stream_open_push` + `fa_stream_push_pcm`, with an optional pts per push ([FFmpeg hand-off](../features/ffmpeg.md)) | `engine.CreatePushStream(format)`, `Push(samples, pts)` | ✅ |
| BASS_StreamPutData of PCM converted from another format (swresample first) | `fa_stream_push_frame`: any sample format, planar or interleaved, any rate; converted on a worker thread | `PushFrame(format, (nint)frame->extended_data, ...)` | ✅ |
| BASS_StreamPutData(h, NULL, 0) to read the queued amount | `fa_stream_get_buffered` (queued + decoded ahead + device buffer, as a duration) | `Buffered` | ✅ |
| BASS_StreamPutData with BASS_STREAMPROC_END | `fa_stream_push_end` | `End()` | ✅ |
| BASS_ChannelSetPosition(h, 0, ...) to clear a push stream after the app seeks | `fa_stream_push_flush`: drops everything queued at once; the clock holds until the new audio plays | `Flush()` | ✅ |
| Decode compressed audio yourself (FFmpeg) and push PCM | `fa_stream_open_packets` + `fa_stream_push_packet`: packets straight from `av_read_frame`; PCM and MP3 built in, other codecs through the app's own FFmpeg, loaded at runtime | `engine.OpenPackets(params)`, `PushPacket(...)` | ✅ |
| BASS_StreamCreate (STREAMPROC callback) | Planned pull-callback source | | 🗓️ |
| BASS_StreamPutFileData | `fa_stream_open_io` with `seek = NULL` (forward-only source fed from your own buffer) | `engine.OpenStream(stream)` with a non-seekable stream | ✅ |
| BASS_StreamGetFilePosition (web streams) | `fa_stream_get_net_status`: `BASS_FILEPOS_DOWNLOAD` → `downloaded`, `BUFFER` → `buffered_bytes` (and `buffered` as time), `END`/`SIZE` → `content_length`, `CONNECTED` → `connected`, `BUFFERING` → `FA_STATE_BUFFERING` ([web streams](../features/web-streams.md#status-and-headers)) | `NetworkStatus` | ✅ |
| BASS_StreamGetFilePosition (files) | Not needed: positions are in frames; use `fa_stream_get_position` | — | ➖ (by design) |
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
| BASS_ChannelGetInfo | `fa_stream_get_info` (the source's own format; unchanged by attaching) | `stream.Info` | ✅ |
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
| BASS_ChannelGetTags | Planned `fa_stream_get_tags` (one reader for all formats) | | 🗓️ |
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
| Reading ReplayGain tags yourself (`BASS_ChannelGetTags` + parsing) | Read automatically: ID3v2 `TXXX`, FLAC Vorbis comments, APEv2; RG2 and R128 | automatic | ✅ |
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
| CD+G from a disc via `BASS_CD_StreamCreate(..., BASS_CD_SUBCHANNEL)` | Disc playback | | 🗓️ |

## 3. Add-on map

| Legacy DLL | Replaced by | Status |
|---|---|---|
| bass_aac.dll | `BASS_AAC_StreamCreateFile`/`URL`/`User` → the normal open calls: MP4/M4A/MOV files (built-in demuxer, any audio track), `.aac` (ADTS) files and AAC radio, and AAC packets (`fa_stream_open_packets`), decoded through the app's FFmpeg; a built-in or OS AAC decoder | ✅ MP4, ADTS and packets via FFmpeg, 🗓️ built-in decoder |
| bass_ac3.dll | AC-3 and E-AC-3 packets through the app's FFmpeg (`fa_stream_open_packets`); files through the OS codec, and passthrough | ✅ packets, 🗓️ files and passthrough |
| bassflac.dll | Built-in | ✅ |
| basshls.dll (if used) | Built-in: `fa_stream_open_url` plays `.m3u8` (MPEG-TS, fMP4, AES-128) | ✅ (AAC segments through the app's FFmpeg) |
| bassopus.dll | Built-in (incl. raw packet push) | 🗓️ |
| basswebm.dll | Built-in Matroska/WebM demuxer, any audio track (`audio_track`); PCM and MP3 tracks built in, Opus, Vorbis, AAC and others through the app's FFmpeg ([streams](../features/streams.md#video-files-and-audio-tracks)) | ✅ (built-in Opus/Vorbis decoders 🗓️) |
| basswma.dll | Decode via Media Foundation on Windows / FFmpeg bridge; encoding not provided | 🗓️ / ➖ |
| bassmidi.dll | Built-in MIDI synth (SF2, later SFZ/SF3); soundfont via stream options | 🗓️ |
| bass_fx.dll | Tempo/pitch/rate params on every stream ([tempo and pitch](../features/tempo-pitch.md)); reverse and BPM planned. DAMP and COMPRESSOR2 used for level control are replaced by normalization and the output limiter | ✅ tempo/pitch/rate and level control, 🗓️ reverse and BPM |
| basscd.dll | Disc source + CD+G; analog playback not provided | 🗓️ disc; ✅ CD+G from files |
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

*More patterns are added here as each feature lands.*
