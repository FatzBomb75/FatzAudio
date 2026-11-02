# Tutorial: a console player

You will play any file from the command line, show the position, and exit when the song ends. The finished program is the repository's sample, `bindings/dotnet/samples/PlayFile`.

## 1. Create the project

```bash
dotnet new console -o PlayFile
cd PlayFile
dotnet add package FatzAudio
```

See [Getting started](../getting-started.md) for the NuGet feed.

## 2. Play

```csharp
using FatzAudio;

using var engine = AudioEngine.Create();          // one engine per app
using var output = engine.OpenOutput();           // the system default device, followed when the user changes it
using var song = engine.OpenFile(args[0]);        // the format is read from the content
song.PlayOn(output);                              // attach, start the device and play
```

`PlayOn` returns at once and the sound plays on engine threads. If the program ends here, you hear nothing: the process exits first.

## 3. Wait for the end

```csharp
var ended = new ManualResetEventSlim();
song.Ended += (_, _) => ended.Set();              // raised on an engine thread
song.PlayOn(output);
ended.Wait();
```

Subscribe before `PlayOn`, so a very short sound cannot end before you listen.

## 4. Show progress and handle errors

```csharp
var info = song.Info;
while (!ended.Wait(250))
    Console.Write($"\r{song.Position:mm\\:ss\\.f} / {info.Duration:mm\\:ss\\.f}   ");
```

Wrap the whole thing in `try { ... } catch (FatzAudioException ex) { ... }` and print `ex.ErrorCode`. A missing file is `NotFound`; a file FatzAudio cannot play is `UnsupportedFormat` ([troubleshooting](../troubleshooting.md)).

## 5. Try it without sound hardware

`AudioEngine.Create(new AudioEngineOptions { NullOutput = true })` runs in real time with no sound, which is how the tests run on build servers.

## Next

[WPF player](wpf-player.md), [playback](../features/playback.md), [events](../features/events.md).
