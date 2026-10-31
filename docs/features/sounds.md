# Sounds: one short sound, many voices

Load a short sound once and play as many overlapping copies of it as you like, each with its own volume and pan: jingles, applause, air horns, sound-effect pads in a karaoke host. This is BASS's sample API (`BASS_SampleLoad`, `BASS_SampleGetChannel`), with one difference that matters: each copy (a **voice**) is an ordinary stream, so everything a stream can do works on it.

| What | C | .NET | BASS |
|---|---|---|---|
| Load a file (any format a stream opens, ZIP included) | `fa_sound_load_path`, `fa_sound_load_memory` | `engine.LoadSound(path)`, `engine.LoadSound(bytes)` | `BASS_SampleLoad` |
| Your own PCM | `fa_sound_load_pcm` | `engine.CreateSound(samples, rate, channels)` | `BASS_SampleCreate` + `BASS_SampleSetData` |
| Play a copy | `fa_sound_play(sound, device, bus, volume, pan, &voice)` | `sound.Play(output, volume, pan)` | `BASS_SampleGetChannel` + `BASS_ChannelPlay` |
| Stop every copy | `fa_sound_stop` | `sound.Stop()` | `BASS_SampleStop` |
| The copies playing | `fa_sound_get_voices` | `sound.GetPlayingVoices()` | `BASS_SampleGetChannels` |
| Voice limit and steal rule | `fa_sound_options.max_voices`, `.steal`; `fa_sound_set_options` | `SoundOptions`, `sound.MaxVoices`, `sound.Steal` | `max` and `BASS_SAMPLE_OVER_*` |
| Format, length, memory | `fa_sound_get_info` | `sound.Info`, `VoiceCount`, `PlayingCount` | `BASS_SampleGetInfo` |
| Free it | `fa_sound_close` | `sound.Dispose()` | `BASS_SampleFree` |

## How it works

- **One copy of the audio.** The whole file is decoded once into memory as 32-bit float at its own rate and channel count. Every voice reads that buffer, so ten voices of a 2 MB sound still use 2 MB (plus a 200 ms decode-ahead buffer per voice). Each voice converts to its device's rate, as any stream does.
- **Voices are streams.** `fa_sound_play` attaches a voice to the device (and bus) you name, sets its volume and pan at once (no glide from a previous copy) and starts it from the beginning. The first audio is buffered before the call returns, so the copy is heard on the next audio callback. The voice is returned: change its volume or pan while it plays, ramp it, put an effect or EQ on it, loop it, change its rate or pitch, listen for `FA_EVENT_ENDED`.
- **Voices are reused.** A voice that ended or was stopped is free. A later play takes a free voice (one already on the same device first), else makes a new one while under the limit. So a voice pointer stays valid until `fa_sound_close`, but may by then be playing a newer copy. A reused voice starts at the volume and pan of its play with looping off; anything else you set on it (rate, effects, an event callback) stays.
- **The limit.** At most `max_voices` (default 8, up to 1024) play at once. Paused and scheduled voices count as busy. A play with every voice busy takes one over by the steal rule, and that voice restarts at once, without a fade, as the new copy:

| Steal rule | Takes over | BASS |
|---|---|---|
| `FA_SOUND_STEAL_OLDEST` (default) | The voice that started longest ago | `BASS_SAMPLE_OVER_POS` |
| `FA_SOUND_STEAL_QUIETEST` | The voice with the lowest volume now (ramps included); the older one on a tie | `BASS_SAMPLE_OVER_VOL` |
| `FA_SOUND_STEAL_NONE` | None: the play fails with `FA_ERROR_BUSY` | no `OVER` flag (BASS's default) |

The default differs from BASS on purpose: a pad that is hit again should always sound.

## C

```c
#include <fatzaudio/fatzaudio.h>

fa_sound* horn = NULL;
fa_sound_options so;
fa_sound_options_init(&so);
so.max_voices = 4;                       /* four horns at once at most */
if (fa_sound_load_path(engine, "horn.wav", &so, &horn) != FA_OK) { /* ... */ }

fa_sound_play(horn, device, NULL, 1.0f, 0.0f, NULL);      /* fire and forget */

fa_stream* left = NULL;
fa_sound_play(horn, device, NULL, 0.5f, -0.8f, &left);    /* a second copy over it */
fa_stream_ramp_param(left, FA_PARAM_PAN, 0.8f, 2000000000LL, 0, NULL);   /* sweeps to the right */

/* ... */
fa_sound_close(horn);                    /* stops and closes its voices */
```

Your own PCM, for example a generated beep:

```c
float beep[4800];
for (int i = 0; i < 4800; i++) beep[i] = 0.3f * sinf(i * 2.0f * 3.14159265f * 1000.0f / 48000.0f);
fa_audio_format f = { FA_FORMAT_F32, 48000, 1, 0 };
fa_sound* sound = NULL;
fa_sound_load_pcm(engine, &f, beep, 4800, NULL, &sound);   /* copied */
```

## C#

```csharp
using var horn = engine.LoadSound("horn.wav", new SoundOptions { MaxVoices = 4 });
horn.Play(output);                                // fire and forget
var left = horn.Play(output, volume: 0.5f, pan: -0.8f);
left.Ramp(AudioParameter.Pan, 0.8f, TimeSpan.FromSeconds(2));

// A pad bank: one sound per pad, played through an effects bus.
var pads = files.Select(f => engine.LoadSound(f)).ToArray();
void Hit(int pad) => pads[pad].Play(fxBus);
```

`Play` starts the output device if it is not running. The returned voice is an `AudioStream` the sound owns: there is no need to dispose it (disposing it is allowed; the sound makes a new voice when it needs one). Disposing the sound disposes its voices.

## Options

| Field | Default | Meaning |
|---|---|---|
| `max_voices` / `MaxVoices` | 8 | Copies playing at once, 1 to 1024 |
| `steal` / `Steal` | Oldest | What a play does when every voice is busy (above) |
| `open` / `Open` | NULL | How the file is opened: audio track, soundfont (a MIDI file renders through it), MIME hint. Its flags are ignored |

`fa_sound_set_options` (or setting `MaxVoices` and `Steal`) changes the limit and the rule at any time. Lowering the limit retires the voices over it, free ones first, then the oldest busy ones: they stop and the sound no longer uses them. Their pointers stay valid until `fa_sound_close`.

## Threads

Every call is thread-safe. A play takes the sound's lock while it picks a voice, so plays of one sound from several threads are served one after another; plays of different sounds do not wait for each other. A play may be called from an event callback (to fire a sound when a song ends, for example). The audio thread does nothing new: it mixes voices as it mixes any stream.

## Pitfalls

- **For short sounds.** The whole file is decoded into memory: a 3-minute stereo song at 44.1 kHz is about 64 MB. Sounds over 1 GB of PCM fail with `FA_ERROR_TOO_LARGE`. Songs belong in streams.
- **Don't close voices behind the sound's back expecting the sound to stay the same.** `fa_stream_close` on a voice is allowed: the sound forgets that voice. But a voice pointer you kept from an earlier play may now be playing someone else's copy: stop a copy right after its play, or keep its `FA_EVENT_ENDED`, rather than holding the pointer for minutes.
- **Stealing cuts.** A stolen voice restarts at once. With short sounds and a sensible limit nobody hears it; for long sounds, use `FA_SOUND_STEAL_NONE` and fade copies out yourself.
- **Normalization.** Voices are never normalized (`FA_OPEN_NO_NORMALIZE`): set the level with the play's volume.
- **Close every sound before `fa_engine_destroy`**, as with streams.
