# Concepts

## Objects

| Object | What it is | Created by | Released by |
|---|---|---|---|
| `fa_engine` | The library instance: thread pool, caches, normalization settings. Usually one per app. | `fa_engine_create` | `fa_engine_destroy` |
| `fa_device` | An opened output device. `NULL` id = the system's primary device, and it follows the primary device when that changes. | `fa_device_open` | `fa_device_close` |
| `fa_stream` | Anything that produces audio: file, memory, custom I/O, URL, pushed PCM/packets, CD track. The format is detected from the content by the decoder registry ([streams](features/streams.md)). | `fa_stream_open_*` | `fa_stream_close` |
| `fa_bus` | A mix group (deck, cue bus). Streams attach to a bus or straight to the device. | (planned) | (planned) |

There is **no hidden state**: every call names the object it acts on. Unlike BASS there is no "current device" per thread and no "last error" to poll.

## Errors

Every function returns `fa_result`. `FA_OK` (0) means success, negative values are errors. `fa_result_string()` gives a readable message. Decoding problems inside a playing stream don't fail calls; they raise an `FA_EVENT_DECODE_ERROR` event and playback continues past the bad data.

In .NET, errors become a `FatzAudioException` with an `ErrorCode`.

## Time and position

- **Frames** (`fa_frames`, int64) are sample-exact positions: one frame is one sample on every channel.
- **Time** (`fa_time`, int64 nanoseconds) is used for durations, markers and clocks.
- `fa_stream_get_clock()` returns the presentation time of the audio *currently leaving the speaker*, with device latency included. Sync video or CD+G graphics to this value and it stays in sync through pauses, seeks and tempo changes.

## Threads

- The **audio thread** only mixes buffers that are already prepared. It never decodes, waits for locks, allocates or does I/O.
- **Worker threads** decode, stretch, resample and normalize each stream ahead of time into a ring buffer (about 500 ms by default).
- Event callbacks run on the engine's **event thread**, never on the audio thread, so they may call any FatzAudio function. DSP callbacks (planned) will run on worker threads.
- All API functions are thread-safe unless their documentation says otherwise.

Details, including the buffer length option and transport timing: [playback.md](features/playback.md). Event delivery: [events.md](features/events.md).
