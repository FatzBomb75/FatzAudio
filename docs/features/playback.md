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
| Audio callback | Mix ring buffers with volume/pan ramps, count position, queue events | Wait on a lock, allocate, do I/O, decode, or call your code |
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
| `fa_stream_play` | Starts mixing. A stopped or ended stream starts at its cue-in marker (0 when unset); a paused one continues |
| `fa_stream_play_at` | Like play, but the first frame is heard exactly at a time on the device clock. `FA_STATE_SCHEDULED` until then, then `FA_EVENT_STARTED` ([crossfade](crossfade.md#the-device-clock)) |
| `fa_stream_pause` | Takes effect before it returns: no more audio of the stream is mixed, so the position read right after is final. The buffer stays full, so resuming is instant. Cancels a scheduled or queued start |
| `fa_stream_stop` | Like pause, then rewinds to the cue-in (or 0) |
| `fa_stream_seek` | Drops the buffer and decodes the first 100 ms at the new position before returning. Seeking a stopped stream leaves it paused there. Costs below |
| `fa_stream_seek_async` | Returns at once (.NET `RequestSeek`): a decode worker seeks and refills while the stream is silent, and the position and clock report the target straight away. A newer call replaces a target not reached yet, so a dragged seek bar costs one seek per worker pass. Use it from a UI thread, and for web streams |
| `fa_stream_detach` / moving to another device | The position continues from what was actually heard, not from what had been decoded ahead |
| `FA_OPEN_LOOP` / `fa_stream_set_loop` | The worker jumps back to the cue-in (or the start) at the cue-out (or the end) without a gap; the position wraps and `FA_EVENT_LOOPED` is raised for each jump (as it is mixed, one device latency before it is heard, like markers). `fa_stream_set_loop` turns it on or off at any time, also while playing; turned off, the stream plays on to the cue-out and ends. Seekable streams only |
| `fa_stream_set_mute` / `fa_device_set_mute` | Silence without touching the volume: glides over 5 ms, fades and ramps keep running, and unmuting brings the volume back. The device mute silences everything it plays |
| Cue-out marker | The stream ends exactly there ([markers](crossfade.md#markers)) |

## What a seek costs

`fa_stream_seek` returns once the decoder has seeked and the first 100 ms at the new position are decoded. These are measured on 10-minute 44.1 kHz stereo files, 40 random seeks each, on one core of a Linux x64 machine:

| Source | Median | Worst |
|---|---|---|
| WAV | 0.13 ms | 2.0 ms |
| FLAC | 0.20 ms | 5.0 ms |
| MP3 (192 kbit/s) | 0.21 ms | 61 ms, on the first jump far into a file without a seek table, which is scanned once |
| AAC in MP4 | 0.27 ms | 3.4 ms |
| Ogg Vorbis | 0.33 ms | 10 ms |
| Opus | 1.1 ms (it decodes 300 ms ahead of the target, for exact audio) | 7.3 ms |
| AC-3 | 0.22 ms | 1.1 ms |
| Web stream | One HTTP range request: a network round trip plus the first bytes, often 50 to 500 ms | Whatever the server takes |
| Push and packet streams | Not seekable: seek your demuxer and call `fa_stream_push_flush`, which returns at once | |

The call also waits for the audio callback in progress to finish, at most one device period. `fa_stream_seek_async` returns in about 25 µs. Only when a worker is already carrying out an earlier target and holds the stream's lock does it wait for that seek; while a target is pending, a newer one replaces it without any lock. A volume ramp is never affected: ramps run on the mix side.

## Position and clock

- `fa_stream_get_position`: source frames handed to the device. Frames decoded ahead into the buffer don't count.
- `fa_stream_get_clock`: the presentation clock, the time of the audio leaving the speaker now, device latency included. It moves smoothly between audio callbacks, holds while paused, buffering or ended, and jumps on a seek. Sync video, CD+G and lyrics to it. Push and packet streams report your own timestamps ([FFmpeg hand-off](ffmpeg.md#following-the-clock-with-the-video)). It is lock-free for streams on a device and for file streams: it never waits for a seek, a decode worker or the audio thread, so a render loop can call it every vsync, even at 240 Hz. Only a push or packet stream that isn't attached takes the stream's lock, briefly.
- `fa_device_get_time` (.NET `OutputDevice.Time`): the device clock, the audio the device has mixed since it opened. Scheduled starts and ramps use it ([crossfade](crossfade.md#the-device-clock)).

```c
fa_frames pos; fa_time clock;
fa_stream_get_position(s, &pos);
fa_stream_get_clock(s, &clock);
```

```csharp
TimeSpan pos = song.Position;
TimeSpan clock = song.Clock;  // for video/lyrics sync
```

## Loop and mute while playing

```c
fa_stream_set_loop(s, 1);       /* the Loop button: on at any time, also while playing */
fa_stream_set_mute(s, 1);       /* silent; FA_PARAM_VOLUME keeps its value */
fa_device_set_mute(dev, 1);     /* everything on this device */
/* FA_EVENT_LOOPED: frame = the loop end it left, value = the loop start it went to */
```

```csharp
song.Loop = true;
song.Looped += (_, e) => Console.WriteLine($"looped from {e.Position}");
song.Muted = true;
output.Muted = true;
```

## Pitfalls

- Resampling to the device rate currently uses miniaudio's linear resampler with a low-pass filter. The windowed-sinc resampler (R12) now exists for rate and pitch ([tempo and pitch](tempo-pitch.md)); moving the device-rate conversion onto it is planned (see the engineering notes).
- Decode-only streams (`FA_OPEN_DECODE_ONLY`) don't use workers or buffers: `fa_stream_read` decodes on your thread.
