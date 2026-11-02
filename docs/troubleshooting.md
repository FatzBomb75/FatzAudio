# Troubleshooting and error codes

Every C function returns an `fa_result`: `FA_OK` (0) or a negative `FA_ERROR_*` code. `fa_result_string(r)` gives a sentence for any code. In .NET a failure throws `FatzAudioException`; read its `ErrorCode`. There is no "last error" to poll. Coming from BASS? The full BASS code table is in [Appendix A of the migration guide](migration/bass.md#appendix-a-error-codes).

## Error codes

| C code | .NET `ErrorCode` | What it means | What to do |
|---|---|---|---|
| `FA_OK` (0) | `Ok` | Success | |
| `FA_ERROR_GENERIC` (-1) | `Generic` | Something failed that has no better code | Check the call's comment in the header; report a bug if it repeats |
| `FA_ERROR_INVALID_ARG` (-2) | `InvalidArgument` | A `NULL` handle or pointer, a value out of range, a `struct_size` of 0, or a call that does not apply to this stream | Fix the call. Output structs need `struct_size = sizeof(...)` before the call |
| `FA_ERROR_OUT_OF_MEMORY` (-3) | `OutOfMemory` | An allocation failed | Close what you no longer use; see [limits](limits.md) |
| `FA_ERROR_NOT_FOUND` (-4) | `NotFound` | No such file, track, device, tag or header | Check the path (UTF-8 in C), the track number or the device id |
| `FA_ERROR_UNSUPPORTED_FORMAT` (-5) | `UnsupportedFormat` | No built-in decoder recognised the data, or it is a codec FatzAudio does not play (DTS, TrueHD, WMA outside Windows), or a damaged file, or a file with no audio | See [formats](formats.md). For other codecs, hand FatzAudio frames from your own FFmpeg ([FFmpeg hand-off](features/ffmpeg.md)) |
| `FA_ERROR_DEVICE` (-6) | `Device` | The output or input device could not be opened | In use by another app in exclusive mode, unplugged, or no permission. List devices and try another ([devices](features/devices.md)) |
| `FA_ERROR_IO` (-7) | `Io` | A file could not be read or created, or a temp directory is missing | Check permissions and free space |
| `FA_ERROR_NETWORK` (-8) | `Network` | A URL could not be reached, timed out, or the server said 401 or 403 | See [web streams](features/web-streams.md); read `fa_net_status.last_error` |
| `FA_ERROR_NOT_SEEKABLE` (-9) | `NotSeekable` | The call needs a seekable source and this one is live or forward-only | Use a file or memory source, or avoid the seek |
| `FA_ERROR_BUSY` (-10) | `Busy` | Every sound voice is busy and the steal rule is `NONE`; or a queue is full (64 armed frames per stream, 256 MB of pushed packets) | Wait, or use another steal rule ([sounds](features/sounds.md)) |
| `FA_ERROR_END_OF_STREAM` (-11) | `EndOfStream` | A forward-only read ran past the end | Normal at the end of a song; `Read` returns 0 frames instead of throwing in .NET |
| `FA_ERROR_NOT_IMPLEMENTED` (-12) | `NotImplemented` | Declared in the API but not available here: 3D, EAX, generic FX, a platform without the feature (CD drives on iOS) | See the feature's page for the alternative |
| `FA_ERROR_PROVIDER_MISSING` (-13) | `ProviderMissing` | A library loaded at run time is not there: LAME for MP3 encoding, libcurl or a system TLS stack for HTTPS | Ship LAME with your app ([recording and encoding](features/recording-encoding.md)); see [web streams](features/web-streams.md) |
| `FA_ERROR_NO_DISC` (-14) | `NoDisc` | The drive is empty or its tray is open | Ask the user to insert a disc ([discs](features/disc.md)) |
| `FA_ERROR_NO_SOUNDFONT` (-15) | `NoSoundFont` | A MIDI file and no soundfont, and the system has no GM bank | Set `engine.DefaultSoundFont` or call `fa_engine_set_soundfont` ([MIDI](features/midi.md)) |
| `FA_ERROR_PROTECTED_CONTENT` (-16) | `ProtectedContent` | DRM-protected or encrypted media (WMA DRM, FairPlay, Common Encryption, `SAMPLE-AES`) | Tell the user the file is copy-protected. FatzAudio never decrypts |
| `FA_ERROR_TOO_LARGE` (-17) | `TooLarge` | A compressed ZIP entry bigger than the memory limit with no temp directory, or a sample over 1 GB of PCM | Set `zip_memory_limit_mb` or `temp_directory` ([CD+G](features/cdg.md#zip-archives)) |
| `FA_ERROR_SIZE_MISMATCH` (-18) | `SizeMismatch` | The buffer given to `fa_surface_update_into` is not the surface's size | Allocate `width * height * 4` bytes |
| `FA_ERROR_NOT_ENOUGH_CHANNELS` (-19) | `NotEnoughChannels` | The headphone cue needs output channels the device does not have | Use a device with 4 or more channels, or a second device ([DJ mixer](features/dj-mixer.md)) |
| `FA_ERROR_PERMISSION_DENIED` (-20) | `PermissionDenied` | The operating system refused the microphone | Ask for permission first (macOS, iOS, Android, Windows privacy settings) |
| `FA_ERROR_OUT_OF_RANGE` (-21) | `OutOfRange` | A value valid on its own that this deck, stream or tempo cannot do (for example a sync tempo outside the deck's range) | Pick a value inside the range the page names |
| `FA_ERROR_CANCELLED` (-22) | `Cancelled` | Your cancel flag stopped an open (`fa_open_options.cancel`; .NET `StreamOptions.CancellationToken`) | Not a failure: you asked for it |

A problem *inside* a playing stream (a bad packet, an unreadable disc sector) does not fail a call. It raises a `DecodeError` event and playback goes on ([events](features/events.md)).

## Symptoms

**No sound.** Work through the [checklist in Getting started](getting-started.md#6-no-sound-a-checklist). The two usual causes are a console program that exits before the song ends, and an output that was never started.

**MIDI is silent or fails with `NoSoundFont`.** MIDI needs instrument sounds. FatzAudio uses a soundfont you load, or the operating system's bank (Windows `gm.dls`, macOS's, an installed `.sf2` on Linux). It never bundles one. See [MIDI](features/midi.md).

**`UnsupportedFormat` on a file that plays elsewhere.** Check the codec with a different tool: DTS, TrueHD and WMA outside Windows are not supported. A file renamed to the wrong extension is fine (FatzAudio reads the content), but a damaged header or a file that is really a text or HTML error page is not. See [formats](formats.md).

**.NET: `DllNotFoundException` or `BadImageFormatException`.** The native library is missing or is for another architecture. The NuGet package carries `win-x64` and `linux-x64`; other platforms need their library staged ([platforms](platforms.md#net-package)).

**.NET: `InvalidOperationException` about a version.** The binding refuses a native library of another major.minor version. Update the package and the native library together ([versions](about.md#versions)).

**Crackles or dropouts.** Check `worker_load` and the underrun count (`fa_stream_get_stats`, [dropouts](features/streams.md#dropouts-underruns)); a busy machine, a very small device buffer or a slow network source are the usual causes ([playback](features/playback.md)).

**Event handlers run on the wrong thread.** Events are raised on an engine thread, not your UI thread. Marshal to the UI thread yourself. The callbacks may call any FatzAudio function ([events](features/events.md), [concepts](concepts.md#threads)).

**HTTPS fails on Android 7 to 11.** Java and Kotlin apps call `fa_android_set_java_vm` once ([web streams](features/web-streams.md#android)).

**Microphone fails on a phone.** The app needs the system's microphone permission before opening input (`RECORD_AUDIO` on Android, the usage description on iOS and macOS).

**Linux: no devices.** There is no sound server or ALSA device (a container, a build server). Use `NullOutput` / `FA_ENGINE_NULL_OUTPUT` for tests ([Getting started](getting-started.md#5-run-without-sound-hardware)).

## Still stuck?

Open the file with `DecodeOnly`, read a few frames and look at the levels. If there is signal, the problem is the output, not the file. Known issues are collected in the [engineering notes](engineering-notes.md).
