# FatzAudio documentation

| Section | What's in it |
|---|---|
| [Getting started](getting-started.md) | Install, play a file in C and in .NET, choose a device, handle errors, run without sound hardware, a "no sound?" checklist |
| [Concepts](concepts.md) | Engine, devices, streams, buses, the clock, threading, errors |
| [Tutorials](tutorials/README.md) | Eight walk-throughs: console and WPF players, karaoke, DJ, radio recorder, library scanner, FFmpeg A/V sync, mobile setup |
| [Cookbook](cookbook.md) | Short recipes: fades, crossfades, meters, tempo and key, recording |
| [Common tasks](common-tasks.md) | Volume, seek, end of song, URLs, tags and karaoke in a few lines each |
| [Platforms](platforms.md) | What each OS needs, where features differ, the .NET package layout, sizes |
| [Formats and codecs](formats.md) | What plays, what is written, per platform |
| [Building](building.md) | Build for Windows, macOS, Linux, Android and iOS, and run the tests |
| [Feature guides](features/README.md) | One page per feature: what it does, how to use it, options, pitfalls; for example [MIDI and karaoke files](features/midi.md) |
| [Migrating from BASS](migration/bass.md) | Every BASS function and add-on mapped to FatzAudio |
| [Migrating from SoundTouch](migration/soundtouch.md) | SoundTouch calls mapped to the tempo, pitch and rate parameters |
| [C API reference](reference/c-api.md) | Every function and type, generated from the header |
| [Troubleshooting and error codes](troubleshooting.md) | Every `FA_ERROR_*` code and what to do; symptoms |
| [Limits and performance](limits.md) | The documented limits in one table |
| [FAQ](faq.md) | Short answers |
| [About: versions and licenses](about.md) | Versioning rules, third-party licenses |
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

## Reading and publishing these docs as a website

The Markdown here is the single source; GitHub shows it as it is. To build a searchable HTML site (with the .NET API reference made from the XML comments, and the C reference made from the header):

```bash
dotnet tool install -g docfx          # once; the .NET 10 SDK is needed
python3 tools/build-docs.py --zip     # build/docs-site/ and build/FatzAudio-docs.zip
```

Upload the contents of `build/docs-site/` to any static web host. To browse offline, unzip `FatzAudio-docs.zip` and run `open-docs.ps1` (Windows) or `python3 serve.py`: the menu and search need a local web server, so opening the pages straight from disk shows them without the menu. `python3 tools/check-doc-links.py` checks every link and anchor; `python3 tools/gen-c-reference.py --check` says whether the C reference matches the headers.
