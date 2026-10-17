# FatzAudio for .NET

Idiomatic .NET binding for the [FatzAudio](https://github.com/) native audio engine.
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
| `AudioEngine` | Entry point. `OutputDevices`, `InputDevices`, `OpenOutput`, `OpenFile`, `OpenMemory`, `OpenStream`, `OpenUrl`, `CreatePushStream`. |
| `OutputDevice` | Opened playback device: `Volume`, `Latency`, `Start()`, `Stop()`. |
| `AudioStream` | Any playable / decodable source: `PlayOn`, `Pause`, `Stop`, `Position`, `Volume`, `Pan`, `Rate`, `Tempo`, `Pitch`, `Ramp`, `Markers`, `CrossfadeTo`, `Read`, `GetLevels`, `Clock`, events. |
| `PushAudioStream` | Stream you feed with PCM via `Push(...)`. |
| `Curve` | `Linear`, `EqualPower`, `Logarithmic`, `Exponential`, `SCurve`, `Bezier(x1, y1, x2, y2)`. |
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

Resolved through the normal `runtimes/<rid>/native` layout, with a fallback to the application directory.

When packing, the `win-x64` library is taken from `$(FatzAudioNativeDir)\fatzaudio.dll`
(default: `build\bin\Release` of the repository; override with `-p:FatzAudioNativeDir=...`).
Binaries for other platforms placed in `src/FatzAudio/runtimes/<rid>/native/` are packed too.

## Status

The native library is an early scaffold. Functions it does not implement yet throw
`FatzAudioException` with `ErrorCode.NotImplemented`.
