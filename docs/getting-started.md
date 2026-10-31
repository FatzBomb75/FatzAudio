# Getting started

## Build the native library

Requirements: CMake 3.21+ and a C11 compiler (Visual Studio 2026, Xcode, GCC or Clang).

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The library is written to `build/bin/<Config>/` (`fatzaudio.dll`, `libfatzaudio.dylib` or `libfatzaudio.so`).
For Android and iOS, see [Building](building.md).

`fa_version()` returns the version of the loaded library as `(major << 16) | (minor << 8) | patch`; compare it with `FA_VERSION_MAJOR` and `FA_VERSION_MINOR` from the header you built against. In .NET, `AudioEngine.NativeVersion` is the same value as a `Version`, and the binding refuses to run against a library with another major.minor.

## Play a file (C)

```c
#include <fatzaudio/fatzaudio.h>

int main(void) {
    fa_engine* engine;
    fa_device* device;
    fa_stream* stream;

    if (fa_engine_create(NULL, &engine) != FA_OK) return 1;
    if (fa_device_open(engine, NULL, &device) != FA_OK) return 1;   /* NULL = primary device */
    if (fa_stream_open_path(engine, "song.flac", NULL, &stream) != FA_OK) return 1;

    fa_stream_attach(stream, device, NULL);
    fa_device_start(device);
    fa_stream_play(stream);

    /* ... wait for FA_EVENT_ENDED or poll fa_stream_get_state ... */

    fa_stream_close(stream);
    fa_device_close(device);
    fa_engine_destroy(engine);
    return 0;
}
```

## Play a file (.NET)

The .NET package wraps the same library with ordinary classes, properties and events. See `bindings/dotnet/` for its README and API reference.

```csharp
using FatzAudio;

using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();              // primary device
using var song = engine.OpenFile("song.flac");

song.Ended += (_, _) => Console.WriteLine("done");
song.PlayOn(output);
```

Next: [Common tasks in five minutes](common-tasks.md) (volume, seek, end of song, URLs, tags, karaoke).
