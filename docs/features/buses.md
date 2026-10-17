# Buses and the channel matrix

A **bus** is a group of streams on one device, with its own volume and pan: a DJ deck, a group of jingles, a preview channel. A stream goes either into a bus or straight into the device's master mix.

```
 deck A streams -> bus 1 -+
                          +-> device master mix -> limiter -> speakers
 deck B streams -> bus 2 -+
 other streams  ----------+
```

Bus gains multiply with each member's own gains (volume, fades, normalization). A bus has no buffer of its own, so it adds no latency and no CPU beyond one multiply per sample.

## C

```c
fa_bus *deck_a, *deck_b;
fa_bus_create(device, &deck_a);
fa_bus_create(device, &deck_b);

fa_stream_attach(song_a, device, deck_a);
fa_stream_attach(song_b, device, deck_b);

fa_bus_set_param(deck_b, FA_PARAM_VOLUME, 0.0f);

/* Crossfader: move from A to B over 4 s with an equal-power curve. */
fa_ramp shape = { FA_CURVE_EQUAL_POWER };
fa_bus_ramp_param(deck_a, FA_PARAM_VOLUME, 0.0f, 4000000000LL, 0, &shape);
fa_bus_ramp_param(deck_b, FA_PARAM_VOLUME, 1.0f, 4000000000LL, 0, &shape);

fa_stream_attach(song_b, device, NULL);   /* move a stream to the master mix, no gap */
fa_bus_destroy(deck_a);                   /* its streams continue on the master mix */
```

## C#

```csharp
using var deckA = output.CreateBus();
using var deckB = output.CreateBus();

songA.PlayOn(deckA);
songB.PlayOn(deckB);

deckB.Volume = 0f;
deckA.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(4), Curve.EqualPower);
deckB.Ramp(AudioParameter.Volume, 1f, TimeSpan.FromSeconds(4), Curve.EqualPower);

songB.PlayOn(output);   // back to the master mix
```

## Rules

- Bus volume (≥ 0) and pan (−1..1) behave like a stream's: changes glide over 5 ms, ramps take any curve and an optional start time on the device clock (`device_time` / `startAt`).
- Bus ramps run whenever the device runs, even with no stream playing. They raise no events.
- `fa_stream_attach` on the device the stream is already on only changes its bus, instantly and without a gap.
- A bus belongs to one device. Attaching a stream to a bus of another device is `FA_ERROR_INVALID_ARG`.
- `fa_bus_destroy` moves its streams to the master mix. `fa_device_close` releases all its buses, so don't destroy a bus after closing its device. In .NET, `MixBus` keeps its device alive, so the order is always right.

## Channel matrix

`fa_stream_set_matrix` routes a stream's source channels to the device's channels with gains you choose. `weights[in * out_channels + out]` is the gain from source channel `in` to device channel `out`.

```c
/* Karaoke track: vocal guide on the right channel. Play only the left (music) on both speakers. */
const float music_only[2 * 2] = {
    1.0f, 1.0f,   /* source left  -> device left, device right */
    0.0f, 0.0f,   /* source right -> muted */
};
fa_stream_set_matrix(s, music_only, 2, 2);

fa_stream_set_matrix(s, NULL, 0, 0);   /* back to the default mapping */
```

```csharp
song.SetMatrix(new float[,] {
    { 1f, 1f },   // source left  -> both speakers
    { 0f, 0f },   // source right -> muted
});
song.ResetMatrix();
```

- `in_channels` must be the source's channel count and `out_channels` the device's (2). A 5.1 source takes a 6 × 2 matrix, which is a custom downmix.
- It takes effect at once. A playing stream re-buffers from where it is, so there's no jump in position.
- A forward-only source (custom I/O without seek) can't re-buffer once attached: set its matrix before attaching it, otherwise the call returns `FA_ERROR_NOT_SEEKABLE`.
- The matrix runs on the decode workers, before the resampler, so it costs the audio thread nothing.
