# Events

Each stream has one event callback. The audio thread queues events without locking. The engine's **event thread** delivers them in order, a few milliseconds later, so your callback never runs on the audio thread and can call any FatzAudio function, including closing the stream.

## Event fields

| Type | `frame` | `value` | Delivered |
|---|---|---|---|
| `FA_EVENT_ENDED` | position at the end | 0 | ✅ |
| `FA_EVENT_RAMP_DONE` | position | the ramp's `tag` | ✅ |
| `FA_EVENT_POSITION` | the requested frame | 0 | 🧩 |
| `FA_EVENT_MARKER` | the marker's frame | the `fa_marker` | 🗓️ crossfade phase |
| `FA_EVENT_METADATA` | position | 0 | 🗓️ web streams |
| `FA_EVENT_STALLED` / `RESUMED` | position | 0 | 🗓️ web streams |
| `FA_EVENT_DECODE_ERROR` | position | the `fa_result` | 🗓️ |
| `FA_EVENT_DEVICE_CHANGED` | position | 0 | 🗓️ R14 |
| `FA_EVENT_LOUDNESS_FINAL` | position | 0 | 🗓️ normalization |

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
```

## Lifetime guarantees

- When `fa_stream_set_event_callback` (replacing a callback) or `fa_stream_close` returns, the previous callback isn't running and won't be called again, so you can free its `user` data. The one exception is calling them from inside that callback itself.
- Events queued for a stream that has since been closed are dropped, not delivered.
- Events for a stream arrive in the order they happened, whether the audio thread raised them (a ramp finishing) or an API call did (a ramp replaced before it started).
- `ENDED` and `RAMP_DONE` aren't lost, so a `RampAsync` always completes. A slow callback only delays later events: the decode workers keep moving events off the audio thread's queue every few milliseconds while your callback runs. If the system is so overloaded that the event storage fills up anyway, the audio thread waits to finish a ramp or end a stream until there is room for its event. The sound is unchanged: a finished ramp holds its target and an ended stream is silent. Only a new volume or pan command waits for that callback, so nothing is dropped.
- Closing a device delivers what it had queued. A callback may also call `fa_engine_destroy` after closing the engine's streams; the teardown finishes once the callback returns.
