# FatzAudio documentation

| Section | What's in it |
|---|---|
| [Concepts](concepts.md) | Engine, devices, streams, buses, the clock, threading, errors |
| [Getting started](getting-started.md) | Play a file in C and in .NET |
| [Feature guides](features/README.md) | One page per feature: what it does, how to use it, options, pitfalls; for example [MIDI and karaoke files](features/midi.md) |
| [Migrating from BASS](migration/bass.md) | Every BASS function and add-on mapped to FatzAudio |
| [Migrating from NAudio](migration/naudio.md) | Planned |
| [Migrating from SoundTouch](migration/soundtouch.md) | SoundTouch calls mapped to the tempo, pitch and rate parameters |
| [Design](../fatzaudio-design.md) | Architecture and decisions |
| [Engineering notes](engineering-notes.md) | Known issues and out-of-scope follow-ups |

## Status legend

Used in every table in these docs:

- ✅ **Available**: implemented and tested.
- 🧩 **Declared**: in the API, returns `FA_ERROR_NOT_IMPLEMENTED` for now.
- 🗓️ **Planned**: designed, not yet in the API.
- ➖ **Not planned**: deliberately left out (reason given).

## Documentation rule

A feature is not finished until it has:

1. Doxygen-style comments on every function and type in `include/fatzaudio/*.h`.
2. XML doc comments on every public .NET member (the build fails without them).
3. A page in `docs/features/` with a C and a C# example.
4. Its rows in [migration/bass.md](migration/bass.md) updated (status and example).
