# Tutorial: a radio recorder

Listen to an internet radio station, show the current song, and record what you hear.

## 1. Open the station

```csharp
using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();

using var radio = engine.OpenUrl("https://stream.example.com/live.mp3");   // may block: call off the UI thread
radio.MetadataChanged += (_, e) => Console.WriteLine($"{e.Metadata.Artist} - {e.Metadata.Title}");
radio.PlayOn(output);
```

HTTP, HTTPS and HLS work with the same call. Reconnects, buffering and the connection state (`NetworkStatus`) are in [web streams](../features/web-streams.md). `https://` needs a system TLS stack; on Linux that is libcurl, on Android 7 to 11 the Java VM must be handed over ([platforms](../platforms.md)).

## 2. Record it

```csharp
using var rec = AudioEncoder.Create("radio.flac", new EncoderOptions { Format = EncodeFormat.Flac });
rec.Record(radio);                       // the stream as heard: only what it plays
// ... later
rec.Close();                             // completes the file; throws FatzAudioException on errors
```

`EncodeFormat.Mp3` works if your app ships LAME (`ProviderMissing` otherwise). To record the whole output mix instead, pass the output: `rec.Record(output)`. A progress display reads `rec.Status` (`Duration`, `Bytes`, `DroppedFrames`).

## Next

[Recording and encoding](../features/recording-encoding.md), [tags](../features/tags.md) for writing a title into the file's options (`EncoderOptions.Title`, `Artist`).
