# Tutorial: a DJ app

Two decks, a crossfader, a headphone cue and a microphone. Everything runs inside one engine with the mixer sitting between your songs and the output.

## 1. Mixer and decks

```csharp
using var engine = AudioEngine.Create();
using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Channels34 });   // headphones on outputs 3-4
DjDeck a = mixer.Decks[0], b = mixer.Decks[1];
```

`CueOutput.Device(headphones)` uses a second sound card instead, and `Channels34` needs an interface with four outputs. See [headphone cue](../features/dj-mixer.md) for the other layouts.

## 2. Load, pre-listen, mix

```csharp
using var songA = engine.OpenFile("a.mp3");
using var songB = engine.OpenFile("b.mp3");
a.Load(songA);
b.Load(songB);
songA.Play();

b.Cue = true;                         // pre-listen B in the headphones
b.Eq.Low = DeckEq.Kill;               // isolator EQ with kills
songB.Play();
mixer.Crossfader.Ramp(1f, TimeSpan.FromSeconds(8));
b.Ramp(DeckParameter.EqLowDb, 0f, TimeSpan.FromSeconds(8));   // bring the bass in as the fade ends
```

## 3. Cue points, loops, meters

```csharp
b.CuePoint = songB.PositionFrames;                              // CDJ cue
long beat = songB.Info.Format.SampleRate * 60 / 128;            // one beat at 128 BPM, in frames
b.Loop = new DeckLoop(songB.PositionFrames, songB.PositionFrames + 4 * beat);
b.Loop = null;                                                  // off

Span<float> peak = stackalloc float[2], rms = stackalloc float[2];
a.GetLevels(peak, rms);                                         // for a channel meter, no allocation
```

## 4. Know the tempo and key before the song plays

```csharp
var t = await engine.AnalyzeAsync("b.mp3", AnalysisKinds.Beats | AnalysisKinds.Key);
Console.WriteLine($"{t.Beats?.Bpm:F1} BPM, {t.Key?.Name} ({t.Key?.Camelot})");
```

The beat grid lets the mixer match tempos and phase (`FA_SYNC_BEAT`, `FA_SYNC_BAR`): [DJ mixer sync](../features/dj-mixer.md) and [analysis](../features/analysis.md).

## 5. A singer

```csharp
var input = engine.InputDevices.First(d => d.Name.Contains("Scarlett"));
using var mic = mixer.AddMic(new MicOptions { Device = input, Channel = 0 });
mic.GainDb = 6;
mic.SetEqualizer(EqualizerPreset.LiveVocal);
mixer.InputLost += (_, e) => e.Mic.Dispose();    // unplugged: add it again when it is back
```

The mic is monitored through the master with low latency; reverb and echo sends, the gate and the latency check are in [DJ mixer microphones](../features/dj-mixer.md). Record the set with [recording](../features/recording-encoding.md).

Real-device note: latency figures depend on the audio interface and driver; test with your hardware.
