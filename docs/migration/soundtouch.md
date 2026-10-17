# Migrating from SoundTouch

SoundTouch (and wrappers around it such as NAudio's `SoundTouchWaveProvider` or SoundTouch.Net) is a processor you feed samples into and pull samples out of. FatzAudio has no separate processor: tempo, pitch and rate are parameters on the stream ([tempo and pitch](../features/tempo-pitch.md)), so the feeding loop, the buffering and the position bookkeeping go away. It also sounds cleaner: SoundTouch splices overlapping chunks of audio, which makes chords phase and moves drum hits by several milliseconds; FatzAudio uses a phase vocoder and an exact resampler (measured in the [feature guide](../features/tempo-pitch.md#how-it-works-and-how-good-it-is)).

| SoundTouch | FatzAudio (C) | FatzAudio (.NET) |
|---|---|---|
| `setTempo(t)` / `setTempoChange(percent)` | `FA_PARAM_TEMPO` = `t` (or `1 + percent / 100`) | `stream.Tempo` |
| `setPitchSemiTones(n)` / `setPitchOctaves(o)` / `setPitch(ratio)` | `FA_PARAM_PITCH` in semitones: `n`, `12 × o`, or `12 × log2(ratio)` | `stream.Pitch` |
| `setRate(r)` / `setRateChange(percent)` | `FA_PARAM_RATE` = `r` (or `1 + percent / 100`) | `stream.Rate` |
| `setSampleRate`, `setChannels` | Not needed: taken from the stream | — |
| `putSamples` / `receiveSamples` loop | Not needed for playback. For offline processing open the stream decode-only and call `fa_stream_read`: the output is already processed | `stream.Read(buffer)` |
| `flush()` at the end | Not needed: the end of the stream is played or returned in full | — |
| `clear()` after a seek | Not needed: seeking restarts the processing | — |
| `setSetting(SETTING_USE_QUICKSEEK, 1)` | `fa_tempo_options.quality = FA_STRETCH_FAST` | `TempoOptions.Quality = StretchQuality.Fast` |
| `SETTING_USE_AA_FILTER`, `SETTING_AA_FILTER_LENGTH` | Not needed: the resampler is always band-limited | — |
| `SETTING_SEQUENCE_MS`, `SETTING_SEEKWINDOW_MS`, `SETTING_OVERLAP_MS` | Not needed: nothing to tune | — |
| `numUnprocessedSamples`, `getInputOutputSampleRatio` to correct positions | Not needed: positions and the clock are in song time | `stream.Position`, `stream.Clock` |
| (not available) | Formant-preserving key changes: `fa_tempo_options.keep_formants` | `TempoOptions.KeepFormants` |
| (not available) | Ramps with curves and exact start times | `Ramp`, `RampAsync` |

## Pattern: practice player

SoundTouch through NAudio:
```csharp
var reader = new AudioFileReader(path);
var st = new SoundTouchWaveProvider(reader) { Tempo = 0.8, PitchSemiTones = -2 };
var output = new WaveOutEvent();
output.Init(st);
output.Play();
// after seeking reader.CurrentTime: st.Clear()
```

FatzAudio:
```csharp
using var song = engine.OpenFile(path);
song.Tempo = 0.8f;
song.Pitch = -2f;
song.PlayOn(output);
song.Position = TimeSpan.FromSeconds(30);   // seeking needs nothing else
```
