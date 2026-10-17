# Volume, pan and ramps

Volume and pan are applied in the mixer, per sample, with no zipper noise. A plain change glides over 5 ms when the stream is audible. A **ramp** moves a parameter to a target over a set time with a chosen curve, and tells you when it's done.

| Parameter | Range | Notes |
|---|---|---|
| `FA_PARAM_VOLUME` | ≥ 0 (1 = unity) | Linear gain |
| `FA_PARAM_PAN` | -1 (left) … 1 (right) | Stereo balance; clamped |
| `FA_PARAM_RATE`, `TEMPO`, `PITCH` | — | `FA_ERROR_NOT_IMPLEMENTED` until the tempo/pitch phase |

## Curves

| Curve | Shape | Good for |
|---|---|---|
| `FA_CURVE_LINEAR` | straight line | short UI fades |
| `FA_CURVE_EQUAL_POWER` | sin rising, cos falling | crossfades: keeps loudness constant (out² + in² = 1) |
| `FA_CURVE_LOG` | fast start, slow finish | fade-ins that should be heard at once |
| `FA_CURVE_EXP` | slow start, fast finish | natural-sounding fade-outs |
| `FA_CURVE_SCURVE` | smooth start and end | musical transitions |
| `FA_CURVE_BEZIER` | CSS `cubic-bezier(x1, y1, x2, y2)` | anything custom (x1, x2 in 0..1) |
| `FA_CURVE_POINTS` | your own values at evenly spaced times, joined by straight lines | shapes drawn in an editor (2 to `FA_CURVE_MAX_POINTS` = 64 values, normally 0 first and 1 last) |

A point curve is set with `ramp.points` and `ramp.point_count`; the array is copied, so it only has to live for the call. In .NET: `Curve.Points(0f, 0.05f, 0.1f, 1f)`.

## C

```c
fa_stream_set_param(s, FA_PARAM_VOLUME, 0.8f);
fa_stream_set_param(s, FA_PARAM_PAN, -0.5f);

/* Fade out over 3 s with an equal-power curve; tag it to recognise its event. */
fa_ramp ramp = { FA_CURVE_EQUAL_POWER };
ramp.tag = 1001;
fa_stream_ramp_param(s, FA_PARAM_VOLUME, 0.0f, 3000000000LL, 0, &ramp);

/* In your event callback: */
static void FA_CALL on_event(const fa_event* ev, void* user)
{
    if (ev->type == FA_EVENT_RAMP_DONE && ev->value == 1001)
        fa_stream_stop(ev->stream);   /* safe: callbacks run on the event thread */
}
```

## C#

```csharp
song.Volume = 0.8f;
song.Pan = -0.5f;

song.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower);   // fire and forget

await song.RampAsync(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower);
song.Stop();                                                                        // after the real end

song.Ramp(AudioParameter.Pan, 1f, TimeSpan.FromSeconds(1), Curve.Bezier(0.25f, 0.1f, 0.25f, 1f));
```

## Rules

- **Ramps advance with playback.** They run while the stream plays on a running device, and wait while it's paused, stopped or not attached yet. So this is a fade-in:
  ```csharp
  song.Volume = 0f;
  song.Ramp(AudioParameter.Volume, 1f, TimeSpan.FromSeconds(2));
  song.PlayOn(output);
  ```
- **Exactly one `FA_EVENT_RAMP_DONE` per ramp**, with `value` = the ramp's `tag`. It fires when the ramp reaches its target, or when a newer ramp or `set_param` on the same parameter replaces it (the value then continues from where the old ramp was). `RampAsync` completes on that event, and disposing the stream cancels it.
- `fa_stream_get_param` returns the value being applied now, mid-ramp values included. A value you just set reads back at once.
- A ramp starts from the current value. If you set a value and then ramp before any audio was mixed, the ramp starts from the value you set.
- **Scheduled starts.** `device_time` (.NET `startAt`) starts the ramp exactly on that frame of the device clock (`fa_device_get_time` / `OutputDevice.Time`); 0 or null means now. Until then the value holds where it is. See [crossfade](crossfade.md#ramps-at-an-exact-time).
  ```csharp
  song.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower, startAt: output.Time + TimeSpan.FromSeconds(10));
  ```
- Volume ramps are separate from crossfades and marker fades, which have gains of their own ([crossfade](crossfade.md#how-the-gains-combine)). Buses take the same ramps ([buses](buses.md)).
