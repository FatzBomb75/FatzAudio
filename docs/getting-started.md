# Getting started

This page takes you from nothing to a song playing, in .NET or C, and then to the things that go wrong first. It has six steps:

1. [Install](#1-install)
2. [Play a file](#2-play-a-file)
3. [Choose the output device](#3-choose-the-output-device)
4. [Handle errors](#4-handle-errors)
5. [Run without sound hardware](#5-run-without-sound-hardware)
6. [No sound? A checklist](#6-no-sound-a-checklist)

Next: [Concepts](concepts.md), then [Common tasks in five minutes](common-tasks.md).

## 1. Install

### .NET (NuGet)

The package is `FatzAudio`, for `net8.0` and `net10.0`. CI publishes a prerelease of every `main` build to the repository owner's GitHub Packages feed; stable versions are published by hand. Setting up the feed (`nuget.config`, token, version pinning) is in [Building](building.md#nuget-package-and-versions).

```bash
dotnet add package FatzAudio --version 0.3.0-ci.*
```

The package carries the native library for **win-x64** and **linux-x64**, in `runtimes/<rid>/native/`, and .NET finds it from there. For **macOS, Android and iOS** build the native library yourself ([Building](building.md)) and put it in `runtimes/<rid>/native/` (macOS), in the app's `lib/<abi>/` folders (Android) or link the xcframework (iOS). Nothing else is needed: there are no add-on libraries and no runtime to install.

Use the same version of the package and the native library. The first call throws `InvalidOperationException` when their major.minor versions differ (`AudioEngine.BindingVersion` against `AudioEngine.NativeVersion`).

### C

Build the library ([Building](building.md) has every platform):

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The library is written to `build/bin/<Config>/` (`fatzaudio.dll`, `libfatzaudio.dylib` or `libfatzaudio.so`). To use it:

- include `include/fatzaudio/fatzaudio.h`;
- link `fatzaudio.lib` (Windows, next to the dll), `-lfatzaudio` (Linux and macOS) or, with CMake, `target_link_libraries(app PRIVATE fatzaudio)` after `add_subdirectory`;
- ship the shared library next to your program.

`fa_version()` returns `(major << 16) | (minor << 8) | patch`. Compare it with `FA_VERSION_MAJOR` and `FA_VERSION_MINOR` from the header you built against.

## 2. Play a file

### .NET

A complete console program:

```csharp
using FatzAudio;

using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();               // the system default device
using var song = engine.OpenFile(args[0]);            // the format is detected from the content

Console.WriteLine($"{song.Info.Codec}, {song.Info.Duration}");

var ended = new ManualResetEventSlim();
song.Ended += (_, _) => ended.Set();                   // raised on a native thread: keep it short
song.PlayOn(output);                                   // attaches, starts the device, plays

ended.Wait();                                          // without this the program exits before any sound
```

The last line matters: playback runs on the engine's threads, so `Main` has to stay alive until the song ends. A window app does not need it, because the window keeps the process running.

This is the [PlayFile sample](../bindings/dotnet/samples/PlayFile/Program.cs), which also shows a progress line and error handling. Run it with `dotnet run --project bindings/dotnet/samples/PlayFile -- song.mp3`.

### C

```c
#include <fatzaudio/fatzaudio.h>
#include <stdio.h>

int main(int argc, char** argv) {
    fa_engine* engine; fa_device* device; fa_stream* stream;
    fa_result r;

    if ((r = fa_engine_create(NULL, &engine)) != FA_OK) { fprintf(stderr, "%s\n", fa_result_string(r)); return 1; }
    if ((r = fa_device_open(engine, NULL, &device)) != FA_OK) { fprintf(stderr, "%s\n", fa_result_string(r)); return 1; }   /* NULL = default */
    if ((r = fa_stream_open_path(engine, argv[1], NULL, &stream)) != FA_OK) { fprintf(stderr, "%s\n", fa_result_string(r)); return 1; }

    fa_stream_attach(stream, device, NULL);   /* NULL bus = the device's master mix */
    fa_device_start(device);
    fa_stream_play(stream);

    fa_stream_state state;
    do {
        sleep_ms(200);                       /* Sleep() on Windows, usleep() elsewhere */
        fa_stream_get_state(stream, &state); /* or wait for FA_EVENT_ENDED (see common tasks) */
    } while (state == FA_STATE_PLAYING || state == FA_STATE_BUFFERING);

    fa_stream_close(stream);
    fa_device_close(device);
    fa_engine_destroy(engine);
    return 0;
}
```

Close streams before the device and the device before the engine. `sleep_ms` stands for your platform's sleep; the engine has no sleep call of its own.

## 3. Choose the output device

`OpenOutput()` and `fa_device_open(engine, NULL, ...)` use the system's default device and follow it when the user changes it. To pick one, list the devices and open it by its id:

```csharp
foreach (var d in engine.OutputDevices)
    Console.WriteLine($"{d.Name}{(d.IsDefault ? " (default)" : "")}");

using var speakers = engine.OpenOutput(engine.OutputDevices.First(d => d.Name.Contains("Speakers")));
```

```c
uint32_t n = 0;
fa_device_count(engine, FA_DEVICE_PLAYBACK, &n);
for (uint32_t i = 0; i < n; ++i) {
    fa_device_info di = { .struct_size = sizeof(di) };
    fa_device_get_info(engine, FA_DEVICE_PLAYBACK, i, &di);
    printf("%s%s\n", di.name, di.is_default ? " (default)" : "");
}
/* fa_device_open(engine, di.id, &device) opens that one */
```

More in [devices](features/devices.md): a second output for headphone preview, what happens when a device is unplugged, and the device clock.

## 4. Handle errors

Every C function returns an `fa_result`: `FA_OK` (0), or a negative `FA_ERROR_*` code. `fa_result_string(r)` gives a sentence. In .NET every failure throws `FatzAudioException` with an `ErrorCode`. There is no "last error" to poll. The BASS error codes map one to one in [the migration guide](migration/bass.md#appendix-a-error-codes).

```csharp
try { using var song = engine.OpenFile(path); /* ... */ }
catch (FatzAudioException ex) when (ex.ErrorCode == ErrorCode.NoSoundFont)
{
    // a MIDI file and no soundfont set and no system bank: ask for one (engine.DefaultSoundFont)
}
catch (FatzAudioException ex)
{
    Console.Error.WriteLine($"{ex.ErrorCode}: {ex.Message}");
}
```

The errors you will meet first when opening:

| Code | Usually means |
|---|---|
| `NotFound` | The path is wrong, or the file has no such track |
| `UnsupportedFormat` | Not a format FatzAudio plays (DTS, TrueHD and WMA outside Windows are not), or a damaged file |
| `NoSoundFont` | A MIDI file and no soundfont: set `engine.DefaultSoundFont` |
| `ProtectedContent` | DRM-protected or encrypted media |
| `Device` | The output device could not be opened (in use, unplugged, no permission) |
| `Network` | A URL could not be reached |

A problem *inside* a playing stream (a bad packet, an unreadable sector) does not fail a call; it raises a `DecodeError` event and playback goes on.

## 5. Run without sound hardware

On a build server, in a container or in a unit test there may be no sound device. Create the engine with the silent output and everything works: streams advance, end and raise events in real time, with no sound.

```csharp
using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
```

```c
fa_engine_config cfg; fa_engine_config_init(&cfg);
cfg.flags = FA_ENGINE_NULL_OUTPUT;
fa_engine_create(&cfg, &engine);
```

To read samples without playing at all, open a stream with `DecodeOnly` (`FA_OPEN_DECODE_ONLY`) and pull frames ([decoding](features/streams.md#decoding-without-playback)).

## 6. No sound? A checklist

1. **Is the device started and the stream playing?** `PlayOn` does both. In C you need `fa_stream_attach`, `fa_device_start` and `fa_stream_play`.
2. **Did the program exit?** A console program must wait for the song to end (step 2).
3. **Volume.** Check `song.Volume`, `output.Volume`, `song.Muted`, `output.Muted`, and the operating system's own volume and mute.
4. **Right device?** Print `engine.OutputDevices`. The default may be a monitor or a virtual device. `NullOutput` plays nothing by design.
5. **Is the stream at its end?** A stream that has ended is silent until you `Play()` it again. Check `song.State`.
6. **MIDI is silent or fails?** MIDI needs a soundfont. Without one FatzAudio uses the operating system's bank (Windows `gm.dls`, macOS, a Linux `.sf2`); `NoSoundFont` means there is none ([MIDI](features/midi.md)).
7. **Linux/macOS/Android/iOS: permissions and the library.** A missing native library is a `DllNotFoundException` (.NET) or a link error (C); a microphone needs the system's permission (`PermissionDenied`).
8. **Web stream silent?** Check `NetworkStatus` and the `Network` error; Android below 12 needs `fa_android_set_java_vm` for HTTPS ([web streams](features/web-streams.md#android)).
9. **Still nothing?** Open the file with `DecodeOnly`, read a few frames and look at `GetLevels`: if there is signal, the problem is the output, not the file.

## Where next

- [Concepts](concepts.md): engine, devices, streams, buses, the clock and threads.
- [Common tasks](common-tasks.md): volume, seek, end of song, URLs, tags and karaoke in a few lines each.
- [Feature guides](features/README.md): one page per feature.
- Coming from BASS? [Migrating from BASS](migration/bass.md).
