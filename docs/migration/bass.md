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
| Fades via `BASS_ChannelSlideAttribute` (linear or log only) | `fa_stream_ramp_param` with linear, equal-power, log, exp, S-curve or bezier | |
| Tempo/pitch need bass_fx and a separate "tempo stream" wrapping the source | `FA_PARAM_TEMPO` and `FA_PARAM_PITCH` on any stream | No wrapper streams to create and free |
| Sync with external video means pushing PCM and correcting drift by hand | `fa_stream_get_clock` gives the presentation time leaving the speaker | Video follows the audio clock; no manual resync |

## 2. Function map: bass.dll

### Library and devices

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_GetVersion | `fa_version` | `AudioEngine.NativeVersion` | ✅ |
| BASS_ErrorGetCode | Return value of every call; `fa_result_string` | `FatzAudioException.ErrorCode` | ✅ |
| BASS_Init | `fa_engine_create` + `fa_device_open` | `AudioEngine.Create()` + `engine.OpenOutput()` | ✅ |
| BASS_Free | `fa_device_close` + `fa_engine_destroy` | `Dispose()` | ✅ |
| BASS_GetDeviceInfo | `fa_device_count` + `fa_device_get_info` | `engine.OutputDevices` | ✅ |
| BASS_SetDevice / BASS_GetDevice | Not needed: pass the `fa_device*` | — | ➖ (by design) |
| BASS_GetInfo | `fa_device_get_info`, `fa_device_get_latency` | `device.Latency` | ✅ (latency, rate, channels) |
| BASS_Start / BASS_Stop | `fa_device_start` / `fa_device_stop` | `device.Start()` / `Stop()` | ✅ |
| BASS_Pause | `fa_device_stop` (streams keep their position) | `device.Stop()` | ✅ |
| BASS_IsStarted | Planned `fa_device_is_started` | | 🗓️ |
| BASS_SetVolume / BASS_GetVolume | `fa_device_set_volume` / `get_volume` | `device.Volume` | ✅ |
| BASS_SetConfig / GetConfig (+Ptr) | `fa_engine_config` fields at creation; per-stream options | `AudioEngineOptions` | ✅ partially (rate, period, threads, cache dir) |
| BASS_Update / BASS_ChannelUpdate | Not needed: worker threads keep buffers full | — | ➖ (by design) |
| BASS_GetCPU | Planned `fa_engine_get_stats` | | 🗓️ |
| BASS_GetDSoundObject | Not provided (Windows-only escape hatch) | — | ➖ |

### Streams

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_StreamCreateFile (path) | `fa_stream_open_path` | `engine.OpenFile(path)` | ✅ WAV/MP3/FLAC so far |
| BASS_StreamCreateFile (memory) | `fa_stream_open_memory` | `engine.OpenMemory(bytes)` | ✅ |
| BASS_StreamCreateFileUser | `fa_stream_open_io` | `engine.OpenStream(System.IO.Stream)` | 🧩 |
| BASS_StreamCreateURL | `fa_stream_open_url` | `engine.OpenUrl(url)` | 🧩 |
| BASS_StreamCreate (STREAMPROC_PUSH) + BASS_StreamPutData | `fa_stream_open_push` + `fa_stream_push_pcm` | `engine.CreatePushStream(format)` | 🧩 |
| BASS_StreamCreate (STREAMPROC callback) | Planned pull-callback source | | 🗓️ |
| BASS_StreamPutFileData | Use `fa_stream_open_io` with your own buffer | | 🧩 |
| BASS_StreamGetFilePosition | Planned buffering/download progress query | | 🗓️ |
| BASS_StreamCancel | Async open with cancellation | | 🗓️ |
| BASS_StreamFree / BASS_ChannelFree | `fa_stream_close` | `Dispose()` | ✅ |
| `BASS_STREAM_DECODE` flag + BASS_ChannelGetData (PCM) | `FA_OPEN_DECODE_ONLY` + `fa_stream_read` | `Read(Span<float>)` | ✅ |

### Channel control

| BASS | FatzAudio (C) | FatzAudio (.NET) | Status |
|---|---|---|---|
| BASS_ChannelPlay / BASS_ChannelStart | `fa_stream_attach` + `fa_stream_play` | `stream.PlayOn(device)` | ✅ |
| BASS_ChannelPlay at a precise time | `fa_stream_play_at` | | 🧩 |
| BASS_ChannelPause / BASS_ChannelStop | `fa_stream_pause` / `fa_stream_stop` | `Pause()` / `Stop()` | ✅ |
| BASS_ChannelIsActive | `fa_stream_get_state` | `stream.State` | ✅ |
| BASS_ChannelGetInfo | `fa_stream_get_info` | `stream.Info` | ✅ |
| BASS_ChannelGetLength | `fa_stream_info.length_frames` | `stream.Info.Duration` | ✅ |
| BASS_ChannelGetPosition / SetPosition | `fa_stream_get_position` / `fa_stream_seek` (frames) | `stream.Position` (TimeSpan) | ✅ |
| BASS_ChannelBytes2Seconds / Seconds2Bytes | Not needed: frames ÷ sample rate, or use TimeSpan in .NET | — | ➖ (by design) |
| BASS_ChannelSetAttribute VOL / PAN | `fa_stream_set_param` VOLUME / PAN | `Volume` / `Pan` | ✅ |
| BASS_ChannelSetAttribute FREQ | `FA_PARAM_RATE` | `Rate` | 🧩 |
| BASS_ChannelSetAttributeEx / GetAttributeEx | Typed functions per feature instead | | ➖ (by design) |
| BASS_ChannelSlideAttribute / IsSliding | `fa_stream_ramp_param` | `Ramp(...)` | 🧩 |
| BASS_ChannelSetSync / RemoveSync | `fa_stream_set_event_callback`, `fa_stream_add_position_event` | `Ended`, `MetadataChanged`, … events | 🧩 |
| BASS_ChannelSetDSP (+Ex) / RemoveDSP | `fa_stream_add_dsp` / `fa_stream_remove_dsp` | | 🧩 |
| BASS_ChannelGetLevel (+Ex) | `fa_stream_get_levels` | `GetLevels()` | 🧩 |
| BASS_ChannelGetData (FFT) | Planned `fa_stream_get_spectrum` | | 🗓️ |
| BASS_ChannelGetTags | Planned `fa_stream_get_tags` (one reader for all formats) | | 🗓️ |
| BASS_ChannelFlags (loop) | `FA_OPEN_LOOP`; runtime toggle planned | | ✅ open-time only |
| BASS_ChannelSetLink / RemoveLink | Buses and `fa_stream_play_at` (sample-accurate group start) | | 🗓️ |
| BASS_ChannelLock | Not needed: parameter changes are applied atomically per buffer | — | ➖ (by design) |
| BASS_ChannelSetDevice / GetDevice | Re-attach with `fa_stream_attach` | | ✅ (position currently resets, fix planned) |
| BASS_ChannelRef | Not needed: handles stay valid until you close them | — | ➖ (by design) |

### Effects

| BASS | FatzAudio | Status |
|---|---|---|
| BASS_ChannelSetFX / RemoveFX, BASS_FXSetParameters / GetParameters / Reset / SetPriority / SetBypass / Free | One effects chain per stream (EQ, compressor, reverb, echo, chorus, flanger, distortion) | 🗓️ |

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
| BASS_PluginLoad / Free / Enable / GetInfo | Not needed: all formats built in. Planned `fa_formats_list` to query support | ➖ (by design) |

## 3. Add-on map

| Legacy DLL | Replaced by | Status |
|---|---|---|
| bass_aac.dll | Built-in `fa_stream_open_*` (OS AAC codec) | 🗓️ |
| bass_ac3.dll | Built-in (OS AC-3/E-AC-3 codec, passthrough, FFmpeg bridge) | 🗓️ |
| bassflac.dll | Built-in | ✅ |
| bassopus.dll | Built-in (incl. raw packet push) | 🗓️ |
| basswebm.dll | Built-in Matroska/WebM demuxer | 🗓️ |
| basswma.dll | Decode via Media Foundation on Windows / FFmpeg bridge; encoding not provided | 🗓️ / ➖ |
| bassmidi.dll | Built-in MIDI synth (SF2, later SFZ/SF3); soundfont via stream options | 🗓️ |
| bass_fx.dll | Tempo/pitch/rate params on every stream; reverse and BPM planned | 🗓️ |
| basscd.dll | Disc source + CD+G; analog playback not provided | 🗓️ |

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

*More patterns (fades, crossfades, syncs/events, tempo, FFmpeg hand-off, normalization, CD+G) are added here as each feature lands.*
