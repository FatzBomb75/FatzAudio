# Events

Each stream has one event callback. The audio thread queues events without locking. The engine's **event thread** delivers them in order, a few milliseconds later, so your callback never runs on the audio thread and can call any FatzAudio function, including closing the stream.

## Event fields

| Type | `frame` | `value` | Delivered |
|---|---|---|---|
| `FA_EVENT_ENDED` | position at the end | 0 | ✅ |
| `FA_EVENT_RAMP_DONE` | position | the ramp's `tag` | ✅ |
| `FA_EVENT_POSITION` | the requested frame | 0 | 🧩 |
| `FA_EVENT_MARKER` | the marker's frame | the `fa_marker` | ✅ ([markers](crossfade.md#markers)) |
| `FA_EVENT_STARTED` | position at the start | 0 | ✅ after `play_at`, a crossfade or a queue ([crossfade](crossfade.md)) |
| `FA_EVENT_METADATA` | position where the new title starts | `fa_metadata.seq` of the title | ✅ radio song titles ([web streams](web-streams.md#song-titles-icy-metadata)) |
| `FA_EVENT_STALLED` / `RESUMED` | position | 0 | ✅ push and packet streams ([FFmpeg hand-off](ffmpeg.md#buffering-the-end-and-seeking)), web streams ([buffering](web-streams.md#buffering-stalls-and-reconnects)) |
| `FA_EVENT_DECODE_ERROR` | position | the `fa_result` | ✅ packet streams (the packet is skipped) |
| `FA_EVENT_DEVICE_CHANGED` | position | 0 | 🗓️ R14 |
| `FA_EVENT_LOUDNESS_FINAL` | position | 0 | ✅ ([normalization](normalization.md)) |

## C

```c
static void FA_CALL on_event(const fa_event* ev, void* user)
{
    switch (ev->type) {
    case FA_EVENT_ENDED:
        printf("ended at frame %lld\n", (long long)ev->frame);
        break;
    case FA_EVENT_RAMP_DONE:
        printf("ramp %lld done\n", (long long)ev->value);
        break;
    default: break;
    }
}

fa_stream_set_event_callback(s, on_event, my_state);
```

## C#

```csharp
song.Ended += (_, _) => Console.WriteLine("done");
song.RampCompleted += (_, e) => Console.WriteLine($"ramp {e.Value} done at {e.Position}");
song.Started += (_, _) => Console.WriteLine("started");
song.MarkerReached += (_, e) => Console.WriteLine($"{e.Marker} reached");
```

## Lifetime guarantees

- When `fa_stream_set_event_callback` (replacing a callback) or `fa_stream_close` returns, the previous callback isn't running and won't be called again, so you can free its `user` data. The one exception is calling them from inside that callback itself.
- Events queued for a stream that has since been closed are dropped, not delivered.
- Events for a stream arrive in the order they happened, whether the audio thread raised them (a ramp finishing) or an API call did (a ramp replaced before it started).
- `ENDED`, `RAMP_DONE` and `STARTED` aren't lost, so a `RampAsync` or `CrossfadeTo` always completes. `MARKER` is best effort: it can be dropped in the rare case that the device's event queue (1024 events) fills before the engine empties it. A slow callback only delays later events: the decode workers keep moving events off the audio thread's queue every few milliseconds while your callback runs.
- Closing a device delivers what it had queued. A callback may also call `fa_engine_destroy` after closing the engine's streams; the teardown finishes once the callback returns.
