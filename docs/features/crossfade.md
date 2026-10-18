# Crossfades, markers and queues

Everything here is timed on the **device clock** and is exact to the frame: a crossfade starts both streams on the same frame, a queued track starts on the frame after the previous one ends, and a scheduled start lands on the frame you asked for.

| You want | Use |
|---|---|
| Crossfade from the playing track to another one, now | `fa_crossfade` / `CrossfadeTo` |
| The next track to start by itself (gapless or with a crossfade) | `fa_stream_set_next` / `QueueNext` |
| Per-track start, end, fade and mix-in points | markers: `fa_stream_set_marker` / `stream.Markers` |
| A start or a ramp at an exact time | `fa_stream_play_at` / `PlayAt`, the `device_time` of a ramp / `startAt` |
| Any shape of fade | 7 curves, including your own points ([ramps](ramps.md#curves)) |
| Groups of streams, a DJ crossfader, a channel matrix | [buses](buses.md) |

## The device clock

`fa_device_get_time` (.NET `OutputDevice.Time`) counts the audio the device has mixed, in nanoseconds, starting at 0 when the device opens. It moves only while the device runs and never jumps, so it's the clock to schedule against.

```c
fa_time now;
fa_device_get_time(device, &now);
fa_stream_play_at(b, now + 2000000000LL);   /* B starts exactly 2 s from now */
```

```csharp
b.PlayAt(output.Time + TimeSpan.FromSeconds(2));
```

`fa_stream_play_at` needs the stream to be attached first. Until it starts, its state is `FA_STATE_SCHEDULED`; `FA_EVENT_STARTED` (.NET `Started`) is raised when it starts. A time that has already passed starts it at once. Pause and stop cancel a scheduled start.

Two streams scheduled at the same time start on the same frame, which replaces `BASS_ChannelSetLink`.

## Crossfade now

```c
fa_ramp shape = { FA_CURVE_EQUAL_POWER };
shape.tag = 42;                                     /* both fades raise RAMP_DONE with 42 */
fa_crossfade(current, next, 5000000000LL, &shape);  /* 5 s */
```

```csharp
await current.CrossfadeTo(next, TimeSpan.FromSeconds(5));   // equal power by default
current.Dispose();
```

- `next` starts from its cue-in (or where it is, if paused) and fades in while `current` fades out. Both fades start on the same frame: the first frame of the next audio callback, within one device period (usually 10 ms) of the call, or rarely the callback after it.
- The outgoing side plays the curve backwards, so the pair is symmetric. With equal power, out² + in² = 1 at every frame: no dip and no bump in loudness.
- When its fade is done, `current` ends (`FA_STATE_ENDED`, `FA_EVENT_ENDED`). `fa_stream_play` would start it again from its cue-in.
- `next` joins `current`'s device and bus if it isn't attached yet.
- The fades are gains of their own. `FA_PARAM_VOLUME` stays where the user put it, so the next track doesn't come in at volume 0.

## Queue the next track

```c
fa_stream_attach(song1, device, NULL);
fa_stream_play(song1);
fa_stream_set_next(song1, song2, 6000000000LL, NULL);  /* 6 s equal-power crossfade */
fa_stream_set_next(song2, song3, 0, NULL);             /* then gapless into song3 */
```

```csharp
song1.PlayOn(output);
song1.QueueNext(song2, TimeSpan.FromSeconds(6));
song2.QueueNext(song3);                                // gapless
song2.Started += (_, _) => UpdateNowPlaying(song2);
```

The next stream starts at the **mix point** of the one before:

1. its `MIX_POINT` marker, if set;
2. otherwise `crossfade` before its end (cue-out, or the length), when the crossfade is longer than 0;
3. otherwise right after its last frame. That's gapless: no silence and no overlap, to the sample.

With a crossfade, the outgoing stream fades out from the mix point and ends when the fade is done, and the next one fades in. A stream's own `FADE_OUT_START` or `FADE_IN_END` marker wins over the queue's fade, so tracks that carry their own fades keep them.

- A queued stream reports `FA_STATE_SCHEDULED` and raises `FA_EVENT_STARTED` when it starts. That's the moment to queue the one after it, or queue a whole playlist as a chain up front.
- Calling `set_next` again replaces the queued stream. `next` NULL (.NET `ClearNext()`) removes it; it goes back to stopped.
- `next` must not be playing already (`FA_ERROR_BUSY`) or on another device (`FA_ERROR_INVALID_ARG`).
- If the current stream has already ended, `next` starts at once.

## Markers

Markers are positions in the source, in frames (.NET: `TimeSpan`). Each one is optional.

| Marker | Meaning |
|---|---|
| `FA_MARKER_CUE_IN` | Where playback starts. Play from stopped or ended, stop, queues and crossfades all start here. Setting it on a stopped stream moves the stream there |
| `FA_MARKER_CUE_OUT` | Where playback ends, exactly: `FA_EVENT_ENDED` fires there. Changing it while playing takes effect at once |
| `FA_MARKER_FADE_IN_END` | Fade in from the cue-in (or 0) up to here |
| `FA_MARKER_FADE_OUT_START` | Fade out from here down to the cue-out (or the end) |
| `FA_MARKER_MIX_POINT` | Where the queued next stream starts |

With `FA_OPEN_LOOP`, the stream loops from the cue-out back to the cue-in without a gap.

```c
uint32_t rate = info.format.sample_rate;
fa_stream_set_marker(s, FA_MARKER_CUE_IN, 2 * rate);           /* skip 2 s of silence */
fa_stream_set_marker(s, FA_MARKER_FADE_OUT_START, 180 * rate);
fa_stream_set_marker(s, FA_MARKER_CUE_OUT, 186 * rate);
fa_stream_set_marker(s, FA_MARKER_MIX_POINT, 182 * rate);      /* next one starts here */

fa_ramp out = { FA_CURVE_SCURVE };
fa_stream_set_fade_curve(s, FA_FADE_OUT, &out);

fa_stream_set_marker(s, FA_MARKER_MIX_POINT, FA_FRAMES_UNSET); /* clear one */
```

```csharp
song.Markers.CueIn = TimeSpan.FromSeconds(2);
song.Markers.FadeOutStart = TimeSpan.FromSeconds(180);
song.Markers.CueOut = TimeSpan.FromSeconds(186);
song.Markers.MixPoint = TimeSpan.FromSeconds(182);
song.Markers.FadeOutCurve = Curve.SCurve;
song.Markers.MixPoint = null;                         // clear it

song.MarkerReached += (_, e) => Console.WriteLine($"{e.Marker} at {e.Position}");
```

- **Marker fades depend on the position, not on time.** Seeking into the middle of a fade plays it at the right level.
- The default fade shape is equal power for both, so two tracks whose marker fades overlap keep constant loudness. The fade-out plays the shape backwards.
- `FA_EVENT_MARKER` (`value` = the marker) is raised when playback passes a marker.
- Decode-only reads (`fa_stream_read`) ignore markers.

## Curves

Every fade here takes an `fa_ramp`: one of the 6 built-in curves, a CSS-style bezier, or `FA_CURVE_POINTS` with up to 64 values of your own. See [ramps](ramps.md#curves). NULL means equal power for crossfades and queues.

## Ramps at an exact time

Every ramp takes a start time on the device clock. 0 means now.

```c
fa_time now; fa_device_get_time(device, &now);
fa_ramp r = { FA_CURVE_LINEAR };
fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.3f, 500000000LL, now + 4000000000LL, &r); /* duck in 4 s */
```

```csharp
song.Ramp(AudioParameter.Volume, 0.3f, TimeSpan.FromMilliseconds(500), Curve.Linear, startAt: output.Time + TimeSpan.FromSeconds(4));
```

## How the gains combine

For each frame:

```
stream volume × crossfade fade × marker fade × normalization gain × pan
  → × bus volume and pan → device mix → output limiter → speakers
```

All of these multiply, so they never fight:

- **Normalization** keeps working through a crossfade: each track keeps its own normalization gain, and the fades are applied on top, so an equal-power crossfade between two normalized tracks holds a steady loudness.
- **The limiter** sees the sum of both tracks. Two loud tracks overlapping can't clip, because the limiter catches the peaks after the mix ([normalization](normalization.md)).
- **Volume** is the user's: crossfades and marker fades never change it.

## Accuracy and limits

- Starts, ends, crossfades and mix points are exact to the frame. Gains are exact at every 16th frame and smoothly interpolated in between, so a fade's shape is exact to well below anything audible.
- Starts and marker events need the decode-ahead buffer to have the audio. If a slow network source runs dry, the start happens when the audio arrives.
- A looping stream with a queued next stream hands over only at its `MIX_POINT`, since it has no end.

## Pitfalls

- `fa_crossfade` and `fa_stream_set_next` attach the next stream for you, but only to the same device. Attach it to another bus first if it should play through one.
- Don't stop the outgoing stream yourself when a crossfade starts. It ends by itself when its fade is done (`await CrossfadeTo(...)` in .NET).
- In C, `fa_device_close` releases the device's buses. Don't call `fa_bus_destroy` after it.
