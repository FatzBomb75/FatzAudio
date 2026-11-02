# FatzAudio for .NET

Idiomatic .NET binding for the [FatzAudio](https://github.com/FatzBomb75/FatzAudio) native audio engine.
Targets `net8.0` and `net10.0`. The package ships the native library under `runtimes/<rid>/native/`.

```csharp
using FatzAudio;

using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();            // null / omitted = system default device
using var song   = engine.OpenFile("song.mp3");

Console.WriteLine($"{song.Info.Codec}, {song.Info.Duration}");
song.Ended += (_, _) => Console.WriteLine("done");  // raised on a native thread: keep handlers quick
song.PlayOn(output);

song.Volume   = 0.8f;
song.Position = TimeSpan.FromSeconds(30);           // seek
song.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower);
```

## Concepts

| Type | Purpose |
| --- | --- |
| `AudioEngine` | Entry point. `OutputDevices`, `InputDevices`, `OpenOutput`, `OpenFile`, `OpenMemory`, `OpenStream`, `OpenUrl`, `CreatePushStream`, `OpenPackets`, `OpenMidi` (a real-time MIDI synthesizer), `OpenMidiEvents`, `DefaultSoundFont`, `ScanLoudness`, `Analyze` / `AnalyzeAsync` (beat grid, key, waveform and loudness of a file in one pass), `SetAndroidJavaVM` (Android HTTPS, rarely needed). |
| `OutputDevice` | Opened playback device: `Volume`, `Latency`, `Time` (the device clock), `CreateBus()`, `Start()`, `Stop()`. |
| `MixBus` | A group of streams with its own `Volume`, `Pan` and `Ramp`: decks, a DJ crossfader. |
| `AudioStream` | Any playable / decodable source: `PlayOn`, `Pause`, `Stop`, `Position`, `Volume`, `Pan`, `Rate`, `Tempo`, `Pitch`, `Ramp`, `PlayAt`, `Markers`, `CrossfadeTo`, `QueueNext`, `SetMatrix`, `Read`, `GetLevels`, `Clock`, `BeatGrid`, `Analyze` / `AnalyzeAsync`, events. Web streams add `Metadata`, `NetworkStatus` and `GetHttpHeader`. Karaoke: `HasCdg`, `LoadCdg`, `CdgOffset`, `CreateCdgSurface`. MIDI: `MidiInfo`, `MidiTexts`, `MidiTextReached`, `MidiTranspose`, `SetMidiChannelMuted`, `SendMidi` / `SendMidiBytes` (events, sample-exact), `GetMidiEvents`, `GetMidiValue`, `SetMidiTrackVolume`. Tracker modules (MOD, XM, S3M, IT, ...): `TrackerInfo`, `GetTrackerTime`, `SeekTracker`, `TrackerInterpolation`, `TrackerStereoSeparation`, `TrackerVolumeRamping`, `TrackerAmigaResampler`, `TrackerRowEvents`, `TrackerPosition`, `TrackerRowReached`, `SetTrackerChannelMuted`; `StreamOptions.AudioTrack` picks a subsong (`TrackerInfo.Subsong`). |
| `SoundFont` | SoundFont 2 instruments for MIDI files: `FromFile`, `FromMemory`, `FromSystem`, `Stack` (several soundfonts as one, with preset mapping and volume), `Info`, `Presets`. Set one as `AudioEngine.DefaultSoundFont` or per stream in `StreamOptions.SoundFont`. |
| `CdgSurface` | Draws a stream's CD+G graphics in sync with the audio: `Update()` returns the pixels (BGRA, RGBA or indexed, any size) and the rectangle that changed, ready for a WPF bitmap or a texture. |
| `PushAudioStream` | Stream you feed with PCM via `Push(...)`, or with decoded frames in any format via `PushFrame(...)`. `End`, `Flush`, `Buffered`. |
| `PacketAudioStream` | Stream you feed with compressed packets from your demuxer (FFmpeg): `PushPacket(...)`. Decoded on worker threads; `Clock` follows your timestamps. |
| `FFmpeg` | `ToSampleFormat(...)`: maps an FFmpeg `AVSampleFormat` to a `SampleFormat` for `PushFrame`. FatzAudio never loads or bundles FFmpeg: your app decodes or demuxes with its own and hands over packets or frames ([FFmpeg hand-off](../../docs/features/ffmpeg.md)). |
| `DjMixer`, `DjDeck`, `Mic` | Two-deck DJ mixer: crossfader, isolator EQ, headphone cue, beat sync, loops, auto-mix (`mixer.AutoMix`), karaoke microphones with effects and ducking, latency check, aligned stem recording ([DJ mixer](../../docs/features/dj-mixer.md)). |
| `Equalizer` | Parametric and graphic EQ presets for `SetEqualizer` ([equalizer](../../docs/features/equalizer.md)). |
| `Disc` | Audio CD and `.cue` image drives: table of contents, CD-Text, ids, `engine.OpenDisc` ([disc playback](../../docs/features/disc.md)). |
| `Tags` | Tags, cover art and file properties for any file without a stream: `Tags.Read(path)` ([tags](../../docs/features/tags.md)). |
| `Sound` | A short sound loaded once and played as many overlapping voices: `engine.LoadSound`, `Play` ([sounds](../../docs/features/sounds.md)). |
| `LyricSurface`, `CdgSurface` | Draw MIDI/.kar lyrics and CD+G graphics in sync with the audio ([lyrics](../../docs/features/lyrics.md), [CD+G](../../docs/features/cdg.md)). |
| `TrackAnalysis`, `SilenceDetector` | Beat grid and BPM, key, waveform, loudness and silence of a file or of PCM you feed ([analysis](../../docs/features/analysis.md)). |
| `AudioEncoder` | Writes WAV, FLAC or MP3 to a file or any `Stream` on its own thread: `Record(output)`, `Record(stream)`, `Encode(stream)`, `Write(...)`, `ConvertFile(...)`, `IsPaused`, `Status`, `Wait`, `Close`. |
| `Lame` | `Load(folder)`, `Info`: the libmp3lame your app ships, loaded at runtime for MP3 (never bundled). |
| `Curve` | `Linear`, `EqualPower`, `Logarithmic`, `Exponential`, `SCurve`, `Bezier(x1, y1, x2, y2)`, `Points(...)`. |
| `FatzAudioException` | Thrown for every failed native call; `ErrorCode` mirrors the C `FA_ERROR_*` values. No "last error" polling. |

Decode-only use (no device):

```csharp
using var s = engine.OpenFile("song.flac", new StreamOptions { DecodeOnly = true });
var buf = new float[4096 * s.Info.Format.Channels];
int frames;
while ((frames = s.Read(buf)) > 0) { /* process buf */ }
```

## Lifetime

Native handles are `SafeHandle`s. A stream or device keeps the native engine (and an attached device)
alive until it is disposed itself, so disposal order never causes a crash.

## Native library

Resolved through the normal `runtimes/<rid>/native` layout, with a fallback to the application directory. The NuGet package ships **win-x64** and **linux-x64** only. For macOS, Android and iOS build the native library and place it in the app yourself ([Building](../../docs/building.md)).

When packing, the `win-x64` library is taken from `$(FatzAudioNativeDir)\fatzaudio.dll`
(default: `build\bin\Release` of the repository; override with `-p:FatzAudioNativeDir=...`).
Binaries for other platforms placed in `src/FatzAudio/runtimes/<rid>/native/` are packed too.
CI packs `win-x64` and `linux-x64` and publishes the package ([building](../../docs/building.md#nuget-package-and-versions)).

The binding and the native library share one version. The first call into the library throws
`InvalidOperationException` when the native library is another major.minor version
(`AudioEngine.NativeVersion` vs `AudioEngine.BindingVersion`), since before 1.0 a minor
version may change the ABI ([CHANGELOG.md](../../CHANGELOG.md)).

## Status

The public API is complete. A call a platform cannot do throws `FatzAudioException` with `ErrorCode.NotImplemented` (for example microphone capture on an engine with `NullOutput`, or Windows-only features elsewhere); the feature guides say where. See the [feature guides](../../docs/features/README.md) for what is available, and [Getting started](../../docs/getting-started.md) to install the package.

## Tests

The tests run without audio hardware (they use `AudioEngineOptions.NullOutput`). Build the native
library first, then point the tests at it:

```
dotnet test bindings/dotnet/tests/FatzAudio.Tests -p:FatzAudioNativeDir=<repo>/build/bin/Release
```

On Linux and macOS (and Windows with the Ninja generator, as in CI) the CMake output folder is `build/bin` (no `Release` subfolder).
