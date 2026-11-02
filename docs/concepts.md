# Concepts

This page explains the ideas the rest of the documentation assumes: the objects you hold, how audio flows from a file to the speaker, how time and position work, which thread does what, who owns what, and a few words used throughout. Each topic links to its full guide.

## The path of a sound

```
source ─▶ decoder ─▶ tempo/pitch ─▶ equalizer ─▶ gain ─▶ bus ─▶ device mix ─▶ limiter ─▶ device volume ─▶ speaker
```

- A **source** is whatever produces audio: a file, memory, your own reader, a URL, packets or frames you push, your own callback, a microphone, a disc track, a MIDI file or a tracker module. All of them open with one `fa_stream_open_*` call and give the same kind of `fa_stream`. The **format is detected from the content**, never from the file name, and there are no add-ons to load.
- The per-stream controls (tempo, pitch and rate, the equalizer, volume, pan, mute, ramps, loudness normalization) sit before the **bus**. A bus groups streams (a DJ deck, a cue bus) with its own volume and pan.
- The **device** mixes everything attached to it, runs the true-peak limiter, then applies its master volume.
- A stream that is not attached to a device is **decode-only**: you pull samples with `fa_stream_read`. Analysis, scanning and encoding work on streams the same way.


## Objects

| Object | What it is | Created by | Released by |
|---|---|---|---|
| `fa_engine` | The library instance: thread pool, caches, normalization settings. Usually one per app. | `fa_engine_create` | `fa_engine_destroy` |
| `fa_device` | An opened output device. `NULL` id = the system's primary device, and it follows the primary device when that changes. | `fa_device_open` | `fa_device_close` |
| `fa_stream` | Anything that produces audio: file, memory, custom I/O, URL, pushed PCM/packets, CD track. The format is detected from the content by the decoder registry ([streams](features/streams.md)). | `fa_stream_open_*` | `fa_stream_close` |
| `fa_bus` | A mix group (deck, cue bus) with its own volume and pan. Streams attach to a bus or straight to the device ([buses](features/buses.md)). | `fa_bus_create` | `fa_bus_destroy` or `fa_device_close` |

There is **no hidden state**: every call names the object it acts on. Unlike BASS there is no "current device" per thread and no "last error" to poll.

## Errors

Every function returns `fa_result`. `FA_OK` (0) means success, negative values are errors. `fa_result_string()` gives a readable message. Decoding problems inside a playing stream don't fail calls; they raise an `FA_EVENT_DECODE_ERROR` event and playback continues past the bad data.

In .NET, errors become a `FatzAudioException` with an `ErrorCode`.

## Versioned structs

A struct whose first field is `struct_size` can gain fields at its end in a later version without breaking an app built
against an older header. Set the size once and the rest follows:

```c
fa_stream_info info = { .struct_size = sizeof(info) };   /* structs the library fills */
fa_stream_get_info(stream, &info);

fa_open_options o;                                         /* structs you pass in */
fa_open_options_init(&o);                                  /* sets struct_size and the defaults */
o.flags = FA_OPEN_DECODE_ONLY;
```

- **Structs the library fills** (info, status, stats): set `struct_size` to `sizeof` before the call. The struct is
  filled up to it, and `struct_size` is set to how much was filled. A `struct_size` of 0 (a struct that was never set
  up) is `FA_ERROR_INVALID_ARG`.
- **Structs you pass in** (options, configs): their `_init` function sets `struct_size` and the defaults, which are not
  always zero. `struct_size` must cover at least the struct's first layout, else `FA_ERROR_INVALID_ARG`. Fields past it
  take their defaults, and a bigger struct from a newer header is read up to the library's own size.
- Structs **without** `struct_size` keep their layout until the next breaking version: array elements
  (`fa_midi_event`, `fa_midi_mark`, `fa_tempo_segment`, ...), small values inside other structs (`fa_audio_format`,
  `fa_rect`) and the event structs handed to callbacks.

In .NET none of this shows: the binding sets the sizes.

## Time and position

- **Frames** (`fa_frames`, int64) are sample-exact positions: one frame is one sample on every channel.
- **Time** (`fa_time`, int64 nanoseconds) is used for durations, markers and clocks.
- Other units are in the name: `_ms` milliseconds, `_seconds` seconds, `_db` decibels, `_lufs` / `_lu` loudness,
  `_kbps` kilobits per second. Rates without a unit in the name are bits per second (`fa_stream_info.bitrate`); sizes
  of data are bytes.
- `fa_device_get_time()` is the **device clock**: the audio the device has mixed since it opened. Scheduled starts, ramps, crossfades and queues are timed on it, exact to the frame ([crossfade](features/crossfade.md)).
- `fa_stream_get_clock()` returns the presentation time of the audio *currently leaving the speaker*, with device latency included. Sync video to this value and it stays in sync through pauses, seeks and tempo changes. CD+G surfaces follow it by themselves ([CD+G](features/cdg.md)).

## Threads

- The **audio thread** only mixes buffers that are already prepared. It never decodes, waits for locks, allocates or does I/O.
- **Worker threads** decode, stretch and resample each stream ahead of time into a ring buffer (about 500 ms by default), and keep a running loudness estimate of what they decode.
- **Scan threads** (one per core, minus one) measure files' loudness in the background, in parallel chunks, for [normalization](features/normalization.md). They start on first use.
- Event callbacks run on the engine's **event thread**, never on the audio thread, so they may call any FatzAudio function. DSP callbacks (`fa_stream_add_dsp`) run where the stream is decoded, normally an engine worker, never on the audio thread.
- All API functions are thread-safe unless their documentation says otherwise.

Details, including the buffer length option and transport timing: [playback.md](features/playback.md). Event delivery: [events.md](features/events.md). Loudness scans: [normalization.md](features/normalization.md).

## Stream states

A stream is always in one state (`fa_stream_get_state`, .NET `State`):

| State | Meaning |
|---|---|
| `STOPPED` | Opened or stopped; the position is at the start (or the cue-in marker) |
| `PLAYING` | Attached to a started device and being heard |
| `PAUSED` | Holding its position; `play` continues |
| `BUFFERING` | A push, packet or web stream ran dry; it resumes by itself when data arrives |
| `ENDED` | Reached its end (or cue-out); `play` starts it again |
| `SCHEDULED` | Waiting for a start time or a queue (`play_at`, a crossfade, `set_next`) |

## Ownership and lifetime

- You close what you open: `fa_stream_close` for every stream, `fa_device_close` for every device, `fa_engine_destroy` for the engine, in that order. In .NET, `Dispose()` (or `using`) does the same, and disposal order never crashes: a stream keeps its engine alive until it is disposed.
- Closing a device detaches the streams still on it; they are not closed. Closing a stream while it plays is fine.
- Handles are valid until you close them. There is no handle recycling to guard against.
- A callback may close the stream it was called for. When `fa_stream_close` (or replacing the callback) returns, the old callback is not running and will not run again, so its `user` data can be freed.
- The library copies strings and buffers you pass in, unless a function says it keeps a reference (a soundfont, for example, is reference counted and streams hold it).

## Time, rate and speed

Positions, markers, the clock, CD+G and lyrics are always in **song time**: they do not change when you change tempo or rate. The speed you hear is `TEMPO x RATE`; `PITCH` changes the key without changing speed. See [tempo and pitch](features/tempo-pitch.md).

## Glossary

| Term | Meaning |
|---|---|
| Frame | One sample on every channel at the same instant. 44,100 frames per second at 44.1 kHz, whatever the channel count |
| Decode-ahead | The stream is decoded on worker threads into a buffer (500 ms by default) before it is heard |
| Device clock | Frames a device has mixed since it opened; used to schedule exact starts and ramps |
| Presentation clock | The time of the audio leaving the speaker, with latency; what a video should follow |
| Marker | A named point on a stream: cue-in, cue-out, mix point, fades |
| Ramp | A change of volume, pan, tempo, pitch or rate over time on one of seven curves |
| Gapless / queue | The next stream starts on the exact frame the current one ends, or at its mix point |
| Bus | A group of streams with its own volume and pan, between the streams and the device |
| Normalization | Loudness matching to a target in LUFS (EBU R128), per track or per album |
| Limiter | The device's last stage: stops peaks above the ceiling clipping, with look-ahead |
| Push / packet stream | A stream you feed with PCM, frames or compressed packets (from FFmpeg) |
| Surface | A picture the engine draws for you in sync with the audio: CD+G graphics or MIDI lyrics |
| Soundfont | The bank of instrument samples a MIDI file plays with (SF2, SF3 or DLS) |
| Deck | One of the DJ mixer's two players |
| Struct size | The first field of structs that can grow; see [Versioned structs](#versioned-structs) |
