# bass.dll analysis (legacy folder)

Target: `C:\development\nuget\fatzaudio\legacy\bass.dll` (not modified; analysed read-only with REA 6.5.0 + Ghidra 12.1.4).

## 1. Identity

| Fact | Value | Source |
|---|---|---|
| Product | Un4seen **BASS** audio library | VERSIONINFO `CompanyName` / `FileDescription` |
| Version | **2.4.18** | VERSIONINFO `FileVersion` |
| Copyright | © 1999-2025 | VERSIONINFO |
| Architecture | PE32+ **x86-64** DLL, image base `0x180000000` | PE header |
| Size / SHA-256 | 166,872 bytes / `febb2cf1882d554c3a958280777da0b69f07de6e262df271de11c56e4a54afd4` | file |
| Signature | Authenticode **Valid**, signer "Un4seen Developments Ltd" (London), SSL.com CA. Cert validity 2023-01-17 to 2026-01-16 (expired now, but the signature is timestamped so it still verifies) | `Get-AuthenticodeSignature` |
| Debug info | References `bass.pdb` (RSDS GUID e4eebffe-405a-4d74-b02a-55ad8c39daa6); PDB not shipped | PE debug dir |
| Resources | Only one: RT_VERSION (language 2057, en-GB) | REA `inspect_pe_resources` |

Evidence IDs: overview `ev_045b786b…`, resources `ev_69e5f836…`, procedures `ev_b4512b36…`, strings `ev_5ddf9d3c…`.

## 2. Packing: why the internals can't be read statically

The DLL is compressed with the **PEtite** executable packer (observed: a section literally named `petite`, the entry point at `0x18005a01a` inside it, and the code section `SECTION.0` with no recoverable functions). Ghidra finds only **3 internal procedures**, all part of the unpacking stub.

Consequences:
- Implementation code (decoders, mixer, output drivers) is **not visible** to static analysis. Decompiling it would require unpacking at runtime (running the DLL and dumping memory), which I did not do because it means executing the target and goes beyond the brief.
- The import table is the packer's minimal one (one placeholder function per DLL). The real imports are resolved at runtime via `GetProcAddress`.

**Observed import DLLs** (and the single placeholder function in each):

| DLL | Placeholder | What it implies (inference) |
|---|---|---|
| kernel32 | GetModuleHandleA, GetProcAddress, VirtualAlloc/Protect/Free, ExitProcess | Unpacker plumbing; real code uses threads, files, events |
| user32 | MessageBoxA, wsprintfA | Unpacker error box; BASS_Init takes an HWND |
| winmm | mixerOpen | Legacy MME mixer / waveIn for recording input selection |
| msacm32 | acmStreamOpen | ACM codecs (BASS can decode ACM-compressed WAV via them) |
| ole32 | CoInitialize | COM: needed for DirectSound / WASAPI / Media Foundation |
| shlwapi | SHSetValueA | Registry/path helpers |
| msvcrt | log | C runtime math |

Not visible because of packing: dsound.dll, WASAPI (mmdevapi/audioses), Media Foundation, ws2_32/wininet (needed for `BASS_StreamCreateURL`). Per BASS docs these are loaded dynamically anyway.

## 3. Exports, grouped by area

110 named exports were recovered from the export name table (Ghidra string inventory over the export section; ordinals not separately parsed). Descriptions come from the public BASS 2.4 documentation (un4seen.com), not from decompiled code. Items marked *(newer)* are recent additions whose exact semantics I'd confirm against the 2.4.18 `bass.h` before relying on them.

Legend for the last column, relevance to FatzAudio (a native cross-platform library):
**Core** = an equivalent is needed in any audio library; **Useful** = worth having; **Legacy** = Windows/DirectSound-era, skip or emulate.

### 3.1 Library / device lifecycle (output)

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_Init | Open an output device (device index, sample rate, flags, HWND, CLSID). Everything else hangs off this | Core |
| BASS_Free | Close the current device and free all its streams/samples/musics | Core |
| BASS_GetDeviceInfo | Enumerate output devices (name, driver id, flags: enabled/default/initialised/type) | Core |
| BASS_SetDevice / BASS_GetDevice | Select the device the calling thread's subsequent calls target (BASS context is per-thread) | Core (design note: per-thread "current device" is awkward; prefer explicit handles) |
| BASS_GetInfo | Device capabilities: latency, min buffer, speakers, frequency, etc. | Core |
| BASS_Start / BASS_Stop / BASS_Pause / BASS_IsStarted | Start/stop/pause the device's output mixer as a whole | Core |
| BASS_Update | Manually render/update playback buffers (when auto-update thread is disabled) | Useful |
| BASS_SetVolume / BASS_GetVolume | Master output volume (device or session) | Core |
| BASS_GetVersion | Returns packed version (0x02041200-style) | Core |
| BASS_ErrorGetCode | Error code of the last call on this thread (BASS_ERROR_*) | Core (prefer returned error codes over thread-local state) |
| BASS_GetCPU | CPU usage of BASS's update threads | Useful |
| BASS_SetConfig / BASS_GetConfig / BASS_SetConfigPtr / BASS_GetConfigPtr | Global options: buffer length, update period, float DSP, net timeouts/proxy, device options, etc. | Core (as a config struct, not a global key/value bag) |

### 3.2 Streams (the main playback path)

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_StreamCreate | Create a stream fed by a user callback (STREAMPROC), a push stream, or a "dummy"/device stream | Core |
| BASS_StreamCreateFile | Open a file or memory block and decode it (WAV/AIFF/MP3/MP2/MP1/OGG built in; others via plugins or OS codecs) | Core |
| BASS_StreamCreateFileUser | Same, but the file I/O is provided through user callbacks (open/close/length/read/seek), buffered or unbuffered | Core (abstract I/O layer) |
| BASS_StreamCreateURL | Stream from HTTP/HTTPS/FTP, with download callback and metadata (Shoutcast/Icecast) | Useful |
| BASS_StreamPutData | Push PCM into a push stream | Core |
| BASS_StreamPutFileData | Push raw file bytes into a "buffered push" file stream | Useful |
| BASS_StreamGetFilePosition | Download/decode progress: current, downloaded, end, buffering, connected | Useful (net streaming) |
| BASS_StreamFree | Free a stream | Core |
| BASS_StreamCancel *(newer)* | Cancels an in-progress stream creation / network wait (inferred from name) | Useful |

### 3.3 Channel control (applies to streams, samples' channels, MOD music, recordings)

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_ChannelPlay / BASS_ChannelStart / BASS_ChannelPause / BASS_ChannelStop | Transport control. `Start` is the newer no-restart variant of Play | Core |
| BASS_ChannelIsActive | State: stopped, playing, paused, stalled, paused-by-device | Core |
| BASS_ChannelGetInfo | Format: freq, channels, flags, ctype (codec), origres, plugin, sample, filename | Core |
| BASS_ChannelFlags | Change runtime flags (loop, auto-free, speaker assignment, etc.) | Core |
| BASS_ChannelGetLength / BASS_ChannelGetPosition / BASS_ChannelSetPosition | Length and seeking, in bytes, MOD orders/rows, or decode-relative modes | Core |
| BASS_ChannelBytes2Seconds / BASS_ChannelSeconds2Bytes | Unit conversion for the channel's format | Core |
| BASS_ChannelGetData | Pull decoded PCM (for decode-only channels) or FFT/level data (for playing channels) | Core (decode-to-buffer API and an analysis API) |
| BASS_ChannelGetLevel / BASS_ChannelGetLevelEx | Peak/RMS level meters over a time window | Useful |
| BASS_ChannelSetAttribute / GetAttribute (+Ex) | Volume, pan, freq (rate), buffer, net resume, tempo-like attributes, etc.; Ex for binary attributes | Core |
| BASS_ChannelSlideAttribute / BASS_ChannelIsSliding | Fade/ramp an attribute over time | Core (fades are expected) |
| BASS_ChannelSetSync / BASS_ChannelRemoveSync | Callbacks at events: end, position, metadata, stall, free, device fail, etc. | Core (event system) |
| BASS_ChannelSetDSP / BASS_ChannelSetDSPEx *(newer)* / BASS_ChannelRemoveDSP | Install a user callback in the channel's processing chain (with priority) | Core |
| BASS_ChannelSetFX / BASS_ChannelRemoveFX | Add a built-in effect (DX8-style: chorus, compressor, distortion, echo, flanger, gargle, I3DL2 reverb, param-EQ, reverb, plus BASS's own volume FX) | Useful |
| BASS_ChannelSetLink / BASS_ChannelRemoveLink | Link channels so they start/stop/pause together sample-synchronously | Useful |
| BASS_ChannelLock | Lock a channel to make several changes atomic w.r.t. the mixer thread | Useful |
| BASS_ChannelUpdate | Pre-fill the channel's playback buffer | Useful |
| BASS_ChannelSetDevice / BASS_ChannelGetDevice | Move a channel to another output device (device switching without recreating) | Core |
| BASS_ChannelGetTags | Metadata: ID3v1/v2, OGG comments, APE, MP4, HTTP headers, Shoutcast/Icecast meta, RIFF INFO, MOD names, etc. | Useful (metadata is often a separate concern) |
| BASS_ChannelFree | Free any channel type | Core |
| BASS_ChannelRef *(newer)* | Handle reference counting (inferred: keeps a handle alive while in use from another thread/callback) | Design note: use RAII/ref-counted handles natively |

### 3.4 Effects parameters

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_FXSetParameters / BASS_FXGetParameters | Set/get an effect's parameter struct | Useful |
| BASS_FXReset | Clear an effect's internal state (e.g. reverb tail) | Useful |
| BASS_FXSetPriority | Reorder an effect in the chain | Useful |
| BASS_FXSetBypass *(newer)* | Bypass an effect without removing it | Useful |
| BASS_FXFree | (Name suggests freeing an FX handle independently of its channel; confirm) | Useful |

Much richer DSP (tempo/pitch, reverse, BPM, filters) lives in the separate **bass_fx.dll** add-on in the same folder.

### 3.5 Samples (in-memory sounds, many simultaneous voices)

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_SampleLoad | Load a file/memory into a sample (decoded fully in RAM) | Useful (game-style SFX) |
| BASS_SampleCreate / BASS_SampleSetData / BASS_SampleGetData | Build a sample from raw PCM, or read it back | Useful |
| BASS_SampleGetInfo / BASS_SampleSetInfo | Default freq/volume/pan, max simultaneous playbacks, 3D settings | Useful |
| BASS_SampleGetChannel | Get a playable channel (voice) for the sample, with override/steal rules | Useful |
| BASS_SampleGetChannels | List the sample's active voices | Useful |
| BASS_SampleStop / BASS_SampleFree | Stop all voices / free sample | Useful |

### 3.6 Tracker music (MOD/S3M/XM/IT/MTM/UMX/MO3)

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_MusicLoad | Load a tracker module; play it as a channel | Optional (niche; a library like libopenmpt covers it cross-platform) |
| BASS_MusicFree | Free it | Optional |

### 3.7 Recording (input)

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_RecordGetDeviceInfo | Enumerate input devices | Core |
| BASS_RecordInit / BASS_RecordFree | Open/close an input device | Core |
| BASS_RecordSetDevice / BASS_RecordGetDevice | Per-thread current recording device | Core (same handle-design note) |
| BASS_RecordGetInfo | Input device caps (formats, channels, freq) | Core |
| BASS_RecordGetInput / BASS_RecordSetInput / BASS_RecordGetInputName | Select source line (mic, line-in…) and its level; uses the old WinMM mixer | Legacy (OS mixers differ; most modern APIs expose one endpoint per input) |
| BASS_RecordStart | Start capturing into a channel with a RECORDPROC callback | Core |

### 3.8 3D audio and EAX

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_Set3DFactors / BASS_Get3DFactors | Distance, rolloff and Doppler factors | Useful if 3D is in scope |
| BASS_Set3DPosition / BASS_Get3DPosition | Listener position, velocity, orientation | Useful if 3D is in scope |
| BASS_ChannelSet3DAttributes / Get3DAttributes | Per-source mode, min/max distance, cone | Useful if 3D is in scope |
| BASS_ChannelSet3DPosition / Get3DPosition | Per-source position/orientation/velocity | Useful if 3D is in scope |
| BASS_Apply3D | Commit pending 3D changes | Useful if 3D is in scope |
| BASS_SetEAXParameters / BASS_GetEAXParameters | Creative EAX environmental reverb | Legacy (hardware EAX is gone) |
| BASS_GetDSoundObject | Return the underlying IDirectSound/IDirectSound3DListener COM objects | Legacy (Windows-only escape hatch) |

### 3.9 Plugins

| Function | What it does | FatzAudio |
|---|---|---|
| BASS_PluginLoad / BASS_PluginFree | Load/unload an add-on DLL (e.g. bassflac.dll) that adds formats to StreamCreateFile/URL/User | Core concept (codec registry), though a native library may link codecs statically |
| BASS_PluginGetInfo | Plugin version and the formats it handles (ctype, name, extensions) | Core concept |
| BASS_PluginEnable *(newer)* | Enable/disable a loaded plugin without unloading | Useful |

## 4. What this means for FatzAudio

**The minimum surface to replicate** (what most BASS-using apps actually call): Init/Free + device enumeration, StreamCreateFile / StreamCreateFileUser / StreamCreate (callback + push), ChannelPlay/Pause/Stop/IsActive, Get/SetPosition + Bytes2Seconds, Get/SetAttribute (volume, pan, rate) + SlideAttribute, SetSync (end/position), SetDSP, ChannelGetData (decode + FFT), ChannelGetLevel, ErrorGetCode, Set/GetConfig, and the Record* trio (enumerate, init, start).

**Architecture notes taken from BASS's API shape:**
- One "channel" abstraction covers streams, sample voices, music and recordings; FX, DSP, syncs and attributes all attach to it. That unification is worth keeping.
- Decoding is pluggable (built-in MP3/OGG/WAV/AIFF + plugins). The add-ons present here show which formats the legacy app needed: **AAC/MP4, AC-3, FLAC, MIDI (with the TimGM6mb.sf2 soundfont), Opus, WebM/Matroska, WMA, CD audio**, plus **bass_fx** for tempo/pitch/filters. Each needs a cross-platform replacement (e.g. FLAC: libFLAC/dr_flac; Opus: libopus/opusfile; AAC: platform codecs or FDK/FAAD; MIDI: FluidSynth/TinySoundFont; WMA: only via platform codecs or FFmpeg).
- BASS keeps per-thread state (current device, last error). For a modern library, explicit device/context handles and returned error codes are cleaner and easier to bind from other languages.
- Output backend: on Windows BASS 2.4.17+ defaults to WASAPI with DirectSound as fallback (per release notes; not observable here due to packing). Cross-platform equivalents are WASAPI, CoreAudio, AAudio/OpenSL, ALSA/PulseAudio/PipeWire. A backend layer such as miniaudio or a custom one would sit under the "device" functions in §3.1.
- Windows-only items you can drop or emulate: EAX, GetDSoundObject, WinMM-mixer input selection (RecordSet/GetInput*), ACM decoding.

## 5. Observed vs inferred vs unknown

- **Observed:** version, signer, x64, PEtite packing, the 110 export names, the packer's placeholder imports, single version resource.
- **Inferred:** function semantics (from BASS public docs, not from code); the meaning of the newer exports marked *(newer)*; which OS APIs are loaded at runtime.
- **Unknown:** the actual implementation, real import list, and any undocumented behaviour, because the code is packed. Getting at those would need a runtime unpack (run the DLL in a sandbox and dump), which wasn't done.
- **Not analysed:** the 9 add-on DLLs in the same folder (only listed). They can be done the same way if needed.

## 6. Overlap between bass.dll and the add-ons (for a single, non-redundant library)

Exports below are **observed** (parsed from each DLL's export table); "overlaps" notes come from BASS's public docs and are **inferred**.

| Add-on (version) | Exports | Overlaps with core / others | Unique, must keep |
|---|---|---|---|
| bass_aac (2.4.7.2) | AAC/MP4 `StreamCreateFile/FileUser/URL` | The 3 create calls duplicate core `BASS_StreamCreate*`. Core BASS already plays AAC/MP4 through OS codecs (Media Foundation, CoreAudio) on Windows/macOS/iOS | Only the decoder itself, on platforms without an OS codec |
| bass_ac3 (2.4.0.9) | AC-3 `StreamCreate*` | Same 3 duplicated create calls; Windows MF can decode AC-3 | AC-3 decoder (patents expired; open decoders exist) |
| bassflac (2.4.6) | FLAC `StreamCreate*` | Same 3 duplicated create calls | FLAC decoder |
| bassopus (2.4.3) | Opus `StreamCreate*` + `StreamCreate`/`PutData` (raw packets) | Same duplicated create calls | Opus decoder; raw-packet push |
| basswebm (2.4.2) | WebM `StreamCreate*` | Same duplicated create calls; relies on bassopus/core Vorbis for the audio inside | Matroska/WebM demuxer |
| basswma (2.4.5) | WMA `StreamCreate*`, `GetTags`, 13 `Encode*` | Decode: duplicated create calls, MF on Windows. Tags: duplicates `BASS_ChannelGetTags` | WMA encoding/broadcast (Windows-only; suggest dropping) |
| bass_fx (2.4) | Tempo, Reverse, BPM/beat (17) | Its BFX_* effects (used via `BASS_ChannelSetFX`) overlap core's built-in DX8 effects and volume FX | Tempo/pitch/rate, reverse playback, BPM and beat detection |
| bassmidi (2.4.16) | Soundfonts, MIDI streams/events, MIDI input (41) | `StreamCreateFile/FileUser/URL` duplicated | Synth, soundfonts, event API, MIDI input |
| basscd (2.4.8) | CD drive, TOC, CD streams (29) | `StreamCreateFile` duplicated | Drive/TOC/CDDB ID; analog CD playback is legacy |

**Pattern:** every add-on re-exports the same open-from-file / user-I/O / URL trio and a `BASSplugin` entry, so the plugin system mainly exists to route `BASS_StreamCreate*` to the right decoder. In one library all of that collapses into a **single `open` call (file, memory, callback, URL or push) that auto-detects the format**. Format-specific functions stay only where a format really has its own features: MIDI (fonts, events, input), CD (drive, TOC), raw Opus packets, tempo/reverse/BPM.

**Rule of one:** each capability has exactly one implementation: one open path, one tag reader, one effects chain (built-in effects + user DSP, no second "BFX" set), one decoder per format chosen per platform at build time (OS codec or built-in, never both exposed).

## 7. Licensing reminder

BASS is proprietary (free for non-commercial use; commercial use needs a licence from Un4seen). Reimplementing the *API shape* is fine, but FatzAudio shouldn't copy or redistribute BASS code, and reverse-engineering the implementation may conflict with its licence terms.
