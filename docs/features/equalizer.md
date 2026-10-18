# Equalizer, presets and user DSP

Every stream has a parametric equalizer: up to 16 bands, each a peak (bell), a low shelf or a high shelf. Ten peaks on the octave frequencies make the usual 10-band graphic EQ, and 13 built-in presets fill those ten bands for you. For anything else, `fa_stream_add_dsp` runs your own processing on the stream's audio.

## What it does

- The equalizer runs in the mixer, before the stream's volume, fades and normalization gain. A change is heard at once, not after the decode-ahead buffer.
- Changes glide (about 10 ms), so dragging a slider while playing never clicks or zips.
- A band at 0 dB is skipped. A flat EQ, a bypassed one, or a stream without bands costs nothing and leaves the audio bit for bit as it was.
- It works on every stream played on a device: files, web streams, discs, MIDI, push streams and packet streams (video soundtracks).
- Decode-only streams are not equalized: `fa_stream_read` returns the audio as decoded. Use a DSP callback there.
- Values are clamped: frequency 10 Hz to just under half the device rate, q 0.1 to 20, gain -24 to +24 dB. Boosts can push the mix over full scale; the engine's limiter catches that ([normalization](normalization.md)).

## C

```c
/* A 10-band graphic EQ from a preset, then one slider moved. */
fa_eq_band bands[FA_EQ_PRESET_BANDS];
uint32_t n;
fa_eq_get_preset(FA_EQ_PRESET_ROCK, bands, &n);
fa_stream_set_eq(song, bands, n);
fa_stream_set_eq_gain(song, 0, 6.0f);         /* band 0 = 31 Hz, +6 dB */

/* Your own bands: a low shelf and a narrow cut. */
fa_eq_band custom[2] = {
    { FA_EQ_LOW_SHELF, 120.0f, 0.71f, 4.0f },
    { FA_EQ_PEAK,     3500.0f, 4.0f, -6.0f },
};
fa_stream_set_eq(song, custom, 2);

fa_stream_set_eq_enabled(song, 0);            /* the EQ "off" button: glides to flat, keeps the bands */
fa_stream_set_eq(song, NULL, 0);              /* no bands at all */

for (int p = 0; p < FA_EQ_PRESET_COUNT; p++)  /* fill a preset menu */
    printf("%s\n", fa_eq_preset_name((fa_eq_preset)p));
```

## C#

```csharp
song.SetEqualizer(EqualizerPreset.Rock);
song.SetEqualizerGain(0, 6f);                  // the 31 Hz slider

song.SetEqualizer(new[] {
    new EqualizerBand(EqualizerBandType.LowShelf, 120f, 0.71f, 4f),
    EqualizerBand.Peak(3500f, -6f, q: 4f),
});

song.EqualizerEnabled = false;                 // bypass, bands kept
foreach (var p in Equalizer.Presets) menu.Add(Equalizer.GetPresetName(p));
```

## Presets

All presets are ten peak bands at 31, 62, 125, 250, 500 Hz and 1, 2, 4, 8, 16 kHz, q 1.41 (one octave each), so switching presets moves the same sliders. Gains in dB:

| Preset | 31 | 62 | 125 | 250 | 500 | 1k | 2k | 4k | 8k | 16k |
|---|---|---|---|---|---|---|---|---|---|---|
| Flat | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| Rock | 5 | 4 | 3 | 1 | -1 | -1 | 1 | 3 | 4 | 5 |
| Pop | -1 | 1 | 3 | 4 | 4 | 2 | 0 | -1 | -1 | -1 |
| Jazz | 3 | 2 | 1 | 2 | -1 | -1 | 0 | 1 | 2 | 3 |
| Classical | 0 | 0 | 0 | 0 | 0 | 0 | -2 | -3 | -3 | -4 |
| Dance | 6 | 5 | 2 | 0 | 0 | -2 | -2 | -1 | 1 | 2 |
| Hip-hop | 5 | 5 | 3 | 1 | -1 | -1 | 1 | 0 | 1 | 2 |
| Acoustic | 3 | 3 | 2 | 1 | 1 | 1 | 2 | 2 | 2 | 1 |
| Vocal | -2 | -2 | -1 | 1 | 3 | 4 | 3 | 1 | 0 | -1 |
| Spoken word | -4 | -3 | -2 | 0 | 2 | 3 | 3 | 2 | 0 | -2 |
| Bass boost | 7 | 6 | 4 | 2 | 0 | 0 | 0 | 0 | 0 | 0 |
| Treble boost | 0 | 0 | 0 | 0 | 0 | 1 | 2 | 4 | 5 | 6 |
| Loudness | 5 | 4 | 2 | 0 | -1 | -1 | 0 | 2 | 4 | 5 |

The app owns user presets: store the bands it read with `fa_stream_get_eq` and set them again.

## User DSP

```c
static void FA_CALL widen(float* f, fa_frames n, uint32_t channels, uint32_t rate, void* user)
{
    for (fa_frames i = 0; i < n; i++) {           /* channels is 2 on a device */
        float m = (f[i * 2] + f[i * 2 + 1]) * 0.5f, s = (f[i * 2] - f[i * 2 + 1]) * 0.5f;
        f[i * 2] = m + 1.5f * s;
        f[i * 2 + 1] = m - 1.5f * s;
    }
}

uint32_t id;
fa_stream_add_dsp(song, widen, NULL, 0, &id);
fa_stream_remove_dsp(song, id);
```

```csharp
using var dsp = song.AddDsp((samples, channels, rate) => { /* change samples in place */ });
```

- Streams on a device: 2 channels at the device rate, after rate, tempo and pitch, before the equalizer, volume, fades and normalization.
- Decode-only streams: the format `fa_stream_read` returns, inside `fa_stream_read`.
- It runs where the stream is decoded, never on the audio thread, so it may take some time. That is normally an engine worker; the first 100 ms after `fa_stream_seek` or `fa_stream_attach` are decoded on the thread making the call, and decode-only streams decode inside `fa_stream_read`. A change in what it does is heard after the decode-ahead buffer (500 ms by default, `buffer_ms`).
- Higher priorities run first; equal ones in the order added. Add and remove while playing; after `fa_stream_remove_dsp` returns, the callback is never called again.

## Pitfalls

- Don't call any `fa_stream_*` function of the same stream from inside its DSP callback (not even `fa_stream_get_position`): the callback runs with the stream's locks held, so that deadlocks.
- Changing a band's type (peak to shelf) while it is not flat restarts that band, which can click. Presets never change types.
- The equalizer sits before normalization, so normalization measures the file, not the EQ: a big boost makes a track louder, and the limiter keeps it from clipping.
