# FatzAudio documentation

| Section | What's in it |
|---|---|
| [Getting started](getting-started.md) | Install, play a file in C and in .NET, choose a device, handle errors, run without sound hardware, a "no sound?" checklist |
| [Concepts](concepts.md) | Engine, devices, streams, buses, the clock, threading, errors |
| [Common tasks](common-tasks.md) | Volume, seek, end of song, URLs, tags and karaoke in a few lines each |
| [Building](building.md) | Build for Windows, macOS, Linux, Android and iOS, and run the tests |
| [Feature guides](features/README.md) | One page per feature: what it does, how to use it, options, pitfalls; for example [MIDI and karaoke files](features/midi.md) |
| [Migrating from BASS](migration/bass.md) | Every BASS function and add-on mapped to FatzAudio |
| [Migrating from SoundTouch](migration/soundtouch.md) | SoundTouch calls mapped to the tempo, pitch and rate parameters |
| [Design (historical)](../fatzaudio-design.md) | The early architecture draft; the feature guides are current |
| [Engineering notes](engineering-notes.md) | Known issues and out-of-scope follow-ups |

## Status legend

Used in every table in these docs:

- ✅ **Available**: implemented and tested.
- 🧩 **Declared**: in the API, returns `FA_ERROR_NOT_IMPLEMENTED` for now.
- 🗓️ **Possible future update**: not built and not needed for release; the page says what to do meanwhile.
- ➖ **Not planned**: deliberately left out (reason given).

## Documentation rule

A feature is not finished until it has:

1. A comment on every function and type in `include/fatzaudio/*.h` (plain `/* */` blocks; they are read as they are, no Doxygen markup).
2. XML doc comments on every public .NET member (the build fails without them).
3. A page in `docs/features/` with a C and a C# example.
4. Its rows in [migration/bass.md](migration/bass.md) updated (status and example).
