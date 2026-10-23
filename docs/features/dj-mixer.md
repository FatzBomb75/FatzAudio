# DJ mixer

A **DJ mixer** puts two decks on one output device, the way a two-channel DJ mixer does: each deck has a channel fader, a trim, a three-band isolator EQ with kills and a meter; a crossfader sits between them; then a master gain and the output limiter, which is always on. A headphone **cue** lets you hear a deck before the crowd does.

```
 deck: your stream (decode, tempo/pitch, its own EQ, volume, fades)
   -> trim -> isolator (low / mid / high) -+-> cue send (pre-fader)
                                           '-> fader -> meter -> crossfader side
 decks -> master gain -> limiter -> master output (channels 1-2)
 cue = (1 - cue mix) x cue sends + cue mix x master, x cue gain -> headphones
```

Decks hold ordinary streams. Play, pause, seek, tempo, pitch, markers, CD+G and lyrics work on them as on any stream, and both decks share the master device's clock, so `fa_stream_play_at` on two decks starts them on the same frame. The mixer opens and owns its output device: use `fa_mixer_get_device` for the clock, levels, spectrum and recording of the master mix.

## C

```c
fa_mixer_config cfg;
fa_mixer_config_init(&cfg);           /* 2 decks, default device, low latency, no cue */
cfg.cue_mode = FA_CUE_CHANNELS;       /* headphones on outputs 3-4 of a 4-output interface */

fa_mixer* mixer;
if (fa_mixer_create(engine, &cfg, &mixer) == FA_ERROR_NOT_ENOUGH_CHANNELS)
    cfg.cue_mode = FA_CUE_SPLIT, fa_mixer_create(engine, &cfg, &mixer);   /* Y-cable */

fa_deck *a, *b;
fa_mixer_get_deck(mixer, 0, &a);      /* side A of the crossfader */
fa_mixer_get_deck(mixer, 1, &b);      /* side B */

fa_deck_load(a, song_a, 0);           /* the deck doesn't own the stream */
fa_deck_load(b, song_b, 0);
fa_stream_play(song_a);

/* Pre-listen deck B in the headphones, bass cut, then bring it in. */
fa_deck_set_param(b, FA_DECK_CUE, 1);
fa_deck_set_param(b, FA_DECK_EQ_LOW_DB, FA_DECK_EQ_KILL_DB);
fa_stream_play(song_b);
fa_ramp shape = { FA_CURVE_LINEAR };
fa_mixer_ramp_param(mixer, FA_MIX_CROSSFADER, 1.0f, 8000000000LL, 0, &shape);   /* 8 s to B */
fa_deck_ramp_param(b, FA_DECK_EQ_LOW_DB, 0.0f, 8000000000LL, 0, &shape);         /* bass back in */

/* A four-beat loop at 128 BPM from the current position. */
fa_frames pos; fa_stream_get_position(song_b, &pos);
fa_frames beat = 44100 * 60 / 128;    /* the stream's sample rate (fa_stream_get_info) */
fa_deck_set_loop(b, pos, pos + 4 * beat);
fa_deck_set_loop(b, 0, 0);            /* off: plays on from where it is */

fa_mixer_destroy(mixer);              /* unloads the decks; close the streams yourself */
```

## C#

```csharp
using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Channels34 });
DjDeck a = mixer.Decks[0], b = mixer.Decks[1];

a.Load(songA);
b.Load(songB);
songA.Play();

b.Cue = true;                         // pre-listen B
b.Eq.Low = DeckEq.Kill;
songB.Play();
mixer.Crossfader.Ramp(1f, TimeSpan.FromSeconds(8));
b.Ramp(DeckParameter.EqLowDb, 0f, TimeSpan.FromSeconds(8));

b.CuePoint = songB.PositionFrames;    // CDJ cue: ReturnToCuePoint() pauses and goes back
long beat = songB.Info.Format.SampleRate * 60 / 128;
b.Loop = new DeckLoop(songB.PositionFrames, songB.PositionFrames + 4 * beat);
b.Loop = null;                        // off

var levels = a.GetLevels();           // the channel meter
```

`DjMixer.Dispose` unloads the decks (even playing ones) and closes the output; the streams stay open for you to dispose. A stream that is disposed, or played on another device or bus, leaves its deck by itself.

## Cue (headphones)

| Mode | C | .NET | What you get |
|---|---|---|---|
| None | `FA_CUE_NONE` | `CueOutput.None` | Master only |
| Channels 3-4 | `FA_CUE_CHANNELS` | `CueOutput.Channels34`, `CueOutput.Channels(n)` | Master on outputs 1-2, cue on two more outputs of the same device (DJ controllers, 4-output USB interfaces). Same clock: no drift, no delay. `cue_channel` picks the first cue output (counted from 0, default 2). `FA_ERROR_NOT_ENOUGH_CHANNELS` when the device has too few |
| Split | `FA_CUE_SPLIT` | `CueOutput.Split` | One stereo output and a Y-cable: master in mono on the left, cue in mono on the right |
| Second device | `FA_CUE_DEVICE` | — | Planned (`FA_ERROR_NOT_IMPLEMENTED`) |

The cue send is taken before the channel fader, so a deck with its fader down can still be heard in the headphones. `FA_MIX_CUE_MIX` blends from cue only (0, the default) to master only (1), and `FA_MIX_CUE_GAIN` sets the headphone level. The master part of the blend is taken after the limiter, as the crowd hears it.

## Parameters

| Parameter | Range | Default |
|---|---|---|
| `FA_MIX_CROSSFADER` / `Crossfader.Position` | 0 (A) .. 1 (B) | 0.5 |
| `FA_MIX_MASTER_GAIN` / `MasterGain` | 0..4, linear, before the limiter | 1 |
| `FA_MIX_CUE_MIX` / `CueMix` | 0 (cue) .. 1 (master) | 0 |
| `FA_MIX_CUE_GAIN` / `CueGain` | 0..4, linear | 1 |
| `fa_mixer_set_crossfader_curve` / `Crossfader.Curve` | constant power (each side −3 dB in the middle, steady loudness) or linear (−6 dB, a slight dip) | constant power |
| `FA_DECK_GAIN` / `Gain` | 0..1, the channel fader | 1 |
| `FA_DECK_TRIM_DB` / `TrimDb` | −12..+12 dB, on top of loudness normalization | 0 |
| `FA_DECK_EQ_LOW_DB`, `_MID_DB`, `_HIGH_DB` / `Eq.Low`, `Eq.Mid`, `Eq.High` | `FA_DECK_EQ_KILL_DB` (−60, silent) .. +6 dB | 0 |
| `FA_DECK_XFADER_SIDE` / `Side` | thru (0), A (1), B (2) | deck 1 A, deck 2 B |
| `FA_DECK_CUE` / `Cue` | off / on | off |

## Rules

- Sets glide over 5 ms; ramps take any curve and an optional start time on the device clock, like stream ramps ([ramps](ramps.md)). Out-of-range values are clamped. Side and cue switch at once and can't be ramped. Mixer and deck ramps raise no events.
- The isolator splits the deck at 250 Hz and 2.5 kHz with fourth-order Linkwitz-Riley crossovers, so with every band at 0 dB the bands add back to the original exactly in level (the phase turns gently around the crossovers, as on hardware isolators). It runs always, so moving a knob never clicks.
- `fa_deck_load` attaches the stream to the mixer's device through the deck, from wherever it was, keeping its position. Loading over a stream that is playing and heard (fader up and the crossfader not closed on its side) is `FA_ERROR_BUSY` unless `FA_DECK_LOAD_FORCE` (`force: true`). Loading a stream that isn't playing also shortens its decode-ahead to 60 ms, so tempo and pitch moves are heard quickly.
- A deck holds one stream. `fa_crossfade` or `fa_stream_set_next` from a deck's stream to a stream that isn't attached yet is `FA_ERROR_INVALID_ARG`: load the next song on the other deck and use the crossfader. A crossfade between the streams of two decks works.
- Close (dispose) a deck's stream on the thread that drives the deck, or unload it first: closing it while another thread is inside a call on that deck isn't safe.
- The device mute (`fa_device_set_mute`, `Output.Muted`) silences the headphones too.
- The cue point (`fa_deck_set_cue_point`, `CuePoint`) defaults to the stream's CUE_IN marker, else 0. `fa_deck_cue` (`ReturnToCuePoint`) pauses the deck and goes back to it.
- The meter (`fa_deck_get_levels`) shows the deck after trim, EQ and fader, before the crossfader, as heard now.
- `latency` defaults to `FA_LATENCY_LOW`: on Windows the WASAPI low-latency shared mode at the device's own rate (needs a driver that offers small periods, most do), about 5 ms periods elsewhere. `FA_LATENCY_NORMAL` uses the engine's period. `FA_LATENCY_EXCLUSIVE` comes with the mic input.
- Closing the mixer's device, or the engine, destroys the mixer. In .NET, `DjMixer` keeps its `OutputDevice`, so dispose the mixer, not its `Output`.

## Deck loops

`fa_deck_set_loop(deck, in, out)` (`Loop`) loops between two source frames, at any time, also while the deck plays. The loop is made where the deck is decoded, so it is seamless (no gap, no fade), follows tempo and pitch, raises `FA_EVENT_LOOPED` (`Looped`) at each turn, and the position and clock report it as heard.

- Setting an out point the deck has only just passed (up to 0.5 s ago, as a "loop out" button does) loops back at once and keeps the loop's phase. An out point further behind jumps to the in point.
- `out <= in` turns the loop off; playback carries on from where it is. A seek past the out point also leaves the loop; a seek inside it keeps it.
- While on, the deck loop replaces the CUE_IN..CUE_OUT loop of `fa_stream_set_loop`. Unloading the deck clears it.
- Only seekable streams loop (`FA_ERROR_NOT_SEEKABLE`): files, memory and discs, not web radio.

## Planned

More than two decks, a second-device cue, exclusive mode with the mic input, beat sync and beat loops (BPM analysis), and auto-mix are the next steps of the [DJ mixer plan](../requests/2026-10-23_FatzAudio_Plan_DjMixer.md).
