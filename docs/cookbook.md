# Cookbook

Short recipes for common jobs, in .NET. Each links to the guide with the C version and the details. The basics (play, seek, tags, karaoke, URLs, sound effects) are in [Common tasks](common-tasks.md); whole programs are in the [tutorials](tutorials/README.md).

## Fade out and stop

```csharp
await song.RampAsync(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower);
song.Stop();                    // after the fade really ended
```

Fade in: set `song.Volume = 0f`, start a ramp to 1, then `PlayOn`. [Ramps](features/ramps.md).

## Crossfade to the next song

```csharp
await current.CrossfadeTo(next, TimeSpan.FromSeconds(5));   // equal power by default
current.Dispose();
```

Start the next song exactly on a device frame with `next.PlayAt(output.Time + TimeSpan.FromSeconds(2))`. Queues, markers and gapless: [crossfade](features/crossfade.md).

## Keep volume even between songs

```csharp
engine.SetNormalization(NormalizeMode.Track, -16);
```

Stored values skip the scan: `song.SetLoudness(storedLufs)`. [Normalization](features/normalization.md).

## Level meters and a spectrum visualizer

```csharp
song.GetLevels();                                           // once: starts the meters
var levels = song.GetLevels(TimeSpan.FromMilliseconds(50)); // in a UI timer
var spectrum = output.GetSpectrum(2048);
```

[Levels and spectrum](features/levels-spectrum.md).

## Find the tempo and key of a song

```csharp
var t = await engine.AnalyzeAsync("song.mp3", AnalysisKinds.Beats | AnalysisKinds.Key);
Console.WriteLine($"{t.Beats?.Bpm:F2} BPM, {t.Key?.Name}");
```

[Analysis](features/analysis.md).

## Change speed or key

`song.Tempo = 1.1f;` speeds up without changing pitch, `song.Pitch = 2;` changes the key without changing speed. [Tempo and pitch](features/tempo-pitch.md).

## Record the output, or convert a file

```csharp
using var rec = AudioEncoder.Create("friday.flac", new EncoderOptions { Format = EncodeFormat.Flac });
rec.Record(output);
// ...
rec.Close();

AudioEncoder.ConvertFile(engine, "song.wav", "song.flac", new EncoderOptions { Format = EncodeFormat.Flac });
```

[Recording and encoding](features/recording-encoding.md).

## Preview the next song on headphones

Open a second device by its id and attach the preview stream to it. [Devices](features/devices.md#cue-output-previewing-on-a-second-device).

## Test without sound hardware

```csharp
using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
```

[Getting started](getting-started.md#5-run-without-sound-hardware).

## Play a MIDI keyboard through the synthesizer

```csharp
using var synth = engine.OpenMidi();
synth.PlayOn(output);
synth.SendMidi(MidiEvent.NoteOn(0, 60, 100));
```

[MIDI](features/midi.md#sending-events-and-the-real-time-synthesizer).

## Moving from BASS

The [migration guide](migration/bass.md) maps every BASS call, flag and error code. The patterns that change most: no `BASS_Init` (you hold the engine), no "last error" (calls return it), no add-on plugins (formats are detected from the content), one `OpenFile` for every format.
