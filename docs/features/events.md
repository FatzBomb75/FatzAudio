# Events

Each stream has one event callback. The audio thread queues events without locking. The engine's **event thread** delivers them in order, a few milliseconds later, so your callback never runs on the audio thread and can call any FatzAudio function, including closing the stream.

## Event fields

| Type | `frame` | `value` | Delivered |
|---|---|---|---|
| `FA_EVENT_ENDED` | position at the end | 0 | ✅ |
| `FA_EVENT_RAMP_DONE` | position | the ramp's `tag` | ✅ |
| `FA_EVENT_POSITION` | the requested frame | 0 | ✅ streams on a device, once per `fa_stream_add_position_event` ([position events](#position-events)) |
| `FA_EVENT_MARKER` | the marker's frame | the `fa_marker` | ✅ ([markers](crossfade.md#markers)) |
| `FA_EVENT_STARTED` | position at the start | 0 | ✅ after `play_at`, a crossfade or a queue ([crossfade](crossfade.md)) |
| `FA_EVENT_METADATA` | position where the new title starts | `fa_metadata.seq` of the title | ✅ radio song titles ([web streams](web-streams.md#song-titles-icy-metadata)) |
| `FA_EVENT_STALLED` / `RESUMED` | position | 0 | ✅ push and packet streams ([FFmpeg hand-off](ffmpeg.md#buffering-the-end-and-seeking)), web streams ([buffering](web-streams.md#buffering-stalls-and-reconnects)) |
| `FA_EVENT_DECODE_ERROR` | position | the `fa_result` | ✅ packet streams (the packet is skipped); disc streams (`FA_ERROR_IO` for a sector that stays unreadable, which plays as silence) |
| `FA_EVENT_DEVICE_CHANGED` | position | 0: the system moved the output to the new default; 1: the device went away and the output was reopened on the default | ✅ every stream on the device ([devices](devices.md#when-the-device-changes-or-goes-away)) |
| `FA_EVENT_LOUDNESS_FINAL` | position | 0 | ✅ ([normalization](normalization.md)) |
| `FA_EVENT_MIDI_TEXT` | the text's frame | its index for `fa_stream_get_midi_text` | ✅ MIDI lyrics and text, streams on a device ([MIDI](midi.md#lyrics-and-text-events)) |
| `FA_EVENT_LOOPED` | the loop end it left (cue-out or the length); reversed, the loop start it left | the frame it went to (cue-in or 0); reversed, the loop end | ✅ looping streams on a device, once per jump; raised as the audio is mixed, like markers ([playback](playback.md#loop-and-mute-while-playing)) |
| `FA_EVENT_TRACKER_ROW` | the row's first frame | `order << 16 \| row` | ✅ tracker modules with `FA_TRACKER_ROW_EVENTS` on; raised as the audio is mixed, like markers ([trackers](trackers.md#row-events)) |
| `FA_EVENT_SEEK_FAILED` | the target of `fa_stream_seek_async` that was not reached | the `fa_result` | ✅ a worker could not seek there (a web server refusing the range); playback goes on where it was ([seek cost](playback.md#what-a-seek-costs)) |

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
song.MidiTextReached += (_, e) => Console.WriteLine($"lyric: {e.Text.Text}");
```

In [reverse playback](tempo-pitch.md#reverse-playback) the events tied to a frame (markers, loop turns, position events, MIDI texts and tracker rows) are raised as the position counts down past their frames, so they come last first: a reversed song raises its last lyric first, and a tracker row's event comes when playback reaches the row's first frame, which is where the row ends when heard backwards (`fa_stream_get_tracker_position` gives the row heard now either way). The cue-out marker is passed where reversed playback starts at it, and each time a reversed loop wraps to it.

## Position events

`fa_stream_add_position_event(stream, frame)` asks for one `FA_EVENT_POSITION` the first time playback passes a source frame. It is the counterpart of a marker for any frame you like:

- **Once.** The request is used up when it fires. To hear about every pass (each turn of a loop, or after seeking back), add it again from the callback.
- **Frame-exact.** It is raised in the mix callback that plays that frame, about one device latency before it is heard, and follows tempo, rate, loops and deck loops. A loop that wraps over the frame raises it on the wrap.
- **Seeks.** Seeking over the frame doesn't raise it; it stays armed until playback passes it.
- **Reverse.** Like markers, it is raised in [reverse playback](tempo-pitch.md#reverse-playback) too, when the position counts down past the frame (and where a reversed loop wraps from its start to its end).
- **Limits.** Up to 64 frames can be armed per stream (`FA_ERROR_BUSY` after that); adding one that is already armed does nothing. A frame past a known length is `FA_ERROR_INVALID_ARG`; the length itself is reached as the stream ends (reversed, as playback starts there). `FA_FRAMES_UNSET` removes every armed frame. Only streams played on a device raise it; decode-only streams don't.
- **Best effort**, like markers (see below).

```c
static void FA_CALL on_event(const fa_event* ev, void* user)
{
    if (ev->type == FA_EVENT_POSITION) {
        show_chorus_banner();
        fa_stream_add_position_event(ev->stream, ev->frame);   /* again on the next turn of the loop */
    }
}

fa_stream_set_event_callback(s, on_event, NULL);
fa_stream_add_position_event(s, 30 * 44100);                 /* 0:30 of a 44.1 kHz song */
```

```csharp
song.PositionReached += (_, e) => Console.WriteLine($"reached {e.Position}");
song.AddPositionEvent(TimeSpan.FromSeconds(30));
song.ClearPositionEvents();   // drops the ones not reached yet
```

## Lifetime guarantees

- When `fa_stream_set_event_callback` (replacing a callback) or `fa_stream_close` returns, the previous callback isn't running and won't be called again, so you can free its `user` data. The one exception is calling them from inside that callback itself.
- Events queued for a stream that has since been closed are dropped, not delivered.
- Events for a stream arrive in the order they happened, whether the audio thread raised them (a ramp finishing) or an API call did (a ramp replaced before it started).
- `ENDED`, `RAMP_DONE` and `STARTED` aren't lost, so a `RampAsync` or `CrossfadeTo` always completes. `MARKER`, `POSITION`, `MIDI_TEXT` and `TRACKER_ROW` are best effort: they can be dropped in the rare case that the device's event queue (1024 events) fills before the engine empties it. A slow callback only delays later events: the decode workers keep moving events off the audio thread's queue every few milliseconds while your callback runs.
- Closing a device delivers what it had queued. A callback may also call `fa_engine_destroy` after closing the engine's streams; the teardown finishes once the callback returns.
