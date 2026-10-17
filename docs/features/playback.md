# Playback, buffering and threads

When a stream is attached to a device, FatzAudio decodes it **ahead of time on worker threads** into a lock-free ring buffer. The device's audio callback only mixes what is already decoded. That's what keeps playback smooth: a slow disk, a big FLAC frame or a busy CPU never makes the audio thread wait.

```
 decode workers (N threads)                       audio thread (real-time)
 ┌───────────────────────────────┐   lock-free    ┌────────────────────────────┐
 │ decoder → convert rate and    │ ─ ring buffer ─▶│ volume/pan ramps → mix →   │ → device
 │ channels to the device format │   (500 ms)     │ events into a lock-free    │
 └───────────────────────────────┘                │ queue (no user code here)  │
                                                  └────────────┬───────────────┘
                              event thread ◀───────────────────┘  calls your callbacks
```

## What runs where

| Thread | Does | Never does |
|---|---|---|
| Your threads (API calls) | Open, attach, seek, play/pause, set params | — |
| Decode workers (`worker_threads`, default = CPU cores, max 8) | Decode, resample, channel-convert into each stream's ring buffer; the emptiest buffer is filled first, and a stream another worker is busy with (say, a slow network source) is skipped | Touch the device |
| Audio callback | Mix ring buffers with volume/pan ramps, count position, queue events | Lock, allocate, do I/O, decode, or call your code |
| Event thread (one per engine) | Call your event callbacks, free closed streams | Run on the audio thread |

On Windows the audio thread runs in the MMCSS "Pro Audio" class.

## Buffer length

The default decode-ahead buffer is 500 ms per stream. Change it per stream:

```c
fa_open_options o;
fa_open_options_init(&o);
o.buffer_ms = 2000;  /* 50..10000; larger rides out slow network shares */
fa_stream_open_path(engine, "track.flac", &o, &s);
```

```csharp
using var s = engine.OpenFile("track.flac", new StreamOptions { BufferLength = TimeSpan.FromSeconds(2) });
```

## Transport behaviour

| Call | What happens |
|---|---|
| `fa_stream_attach` | Builds the conversion chain for the device and decodes the first 100 ms before returning, so `play` is instant |
| `fa_stream_play` | Starts mixing. After the end (`FA_STATE_ENDED`) it restarts from the beginning |
| `fa_stream_pause` | Takes effect before it returns: no more audio of the stream is mixed, so the position read right after is final. The buffer stays full, so resuming is instant |
| `fa_stream_stop` | Like pause, then rewinds to the start |
| `fa_stream_seek` | Drops the buffer and decodes the first 100 ms at the new position before returning |
| `fa_stream_detach` / moving to another device | The position continues from what was actually heard, not from what had been decoded ahead |
| `FA_OPEN_LOOP` | The worker jumps back to the start without a gap; the position wraps |

## Position and clock

- `fa_stream_get_position`: source frames handed to the device. Frames decoded ahead into the buffer don't count.
- `fa_stream_get_clock`: the position in nanoseconds minus the device latency, which is what's leaving the speaker now. Sync video and lyrics to this.

```c
fa_frames pos; fa_time clock;
fa_stream_get_position(s, &pos);
fa_stream_get_clock(s, &clock);
```

```csharp
TimeSpan pos = song.Position;
TimeSpan clock = song.Clock;  // for video/lyrics sync
```

## Pitfalls

- Resampling to the device rate currently uses miniaudio's linear resampler with a low-pass filter. The windowed-sinc resampler from the design (R12) replaces it in the tempo/pitch phase, in the same place in the chain.
- Decode-only streams (`FA_OPEN_DECODE_ONLY`) don't use workers or buffers: `fa_stream_read` decodes on your thread.
