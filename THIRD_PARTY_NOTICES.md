# Third-party notices

FatzAudio's own code is original. The libraries below are built into the shared library (`fatzaudio.dll`, `libfatzaudio.so`, `libfatzaudio.dylib`, the xcframework), each in the folder named, with its license text in that folder. They are statically linked: nothing else is loaded or needs to be shipped for them.

The license for FatzAudio's own code is not set yet. It is chosen by the owner before a release; until then this file only covers what FatzAudio includes.

| Component | Version | License | Folder | Used for |
|---|---|---|---|---|
| [miniaudio](https://miniaud.io) (with its embedded dr_wav, dr_flac and dr_mp3) | vendored, patches listed in `PATCHES.md` | Public domain (Unlicense) or MIT-0, your choice | `third_party/miniaudio` | Device input and output on every platform; WAV, FLAC and MP3 decoding |
| [libopus](https://opus-codec.org) | 1.5.2 | BSD-3-Clause | `third_party/opus` | Opus decoding (and the encoder, compiled in) |
| [libvorbis](https://xiph.org/vorbis/) | 1.3.7 plus later fixes from master | BSD-3-Clause | `third_party/vorbis` | Vorbis decoding. `lib/mdct.c` carries a marked FatzAudio patch |
| [libogg](https://xiph.org/ogg/) | 1.3.6 | BSD-3-Clause | `third_party/ogg` | Bit reader for libvorbis |
| [TinySoundFont](https://github.com/schellingb/TinySoundFont) (`tsf.h`) | commit 853a0a1 | MIT | `third_party/tinysoundfont` | SoundFont 2 synthesizer. Altered, as marked in the file |
| TinyMidiLoader (`tml.h`) | same commit | zlib | `third_party/tinysoundfont` | Standard MIDI File parser. Altered, as marked in the file |
| [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | 1.4.0 | MIT | `third_party/signalsmith/signalsmith-stretch` | Time stretching and pitch shifting |
| [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear) | 0.6.4 | MIT | `third_party/signalsmith/signalsmith-linear` | FFT used by the stretcher |
| [libopenmpt](https://lib.openmpt.org) | 0.8.9 | BSD-3-Clause (some files Boost Software License 1.0) | `third_party/libopenmpt` | Tracker modules (MOD, XM, S3M, IT and more) |

## Loaded at run time, never bundled

- **LAME** (libmp3lame, LGPL): MP3 encoding only, and only if your app ships it. FatzAudio loads it with the system loader; without it, creating an MP3 encoder returns `FA_ERROR_PROVIDER_MISSING`. See [recording and encoding](docs/features/recording-encoding.md).
- **FFmpeg**: FatzAudio never uses it. An app may decode with its own FFmpeg and hand FatzAudio packets or frames ([FFmpeg hand-off](docs/features/ffmpeg.md)).
- **Operating-system codecs and services**: Windows Media Foundation (WMA, xHE-AAC), Apple AudioToolbox, Android MediaCodec and each system's HTTPS stack are called through the system and are not part of this package.

## What this means for an app that ships FatzAudio

The licenses above are permissive and static linking is allowed. Their notices must travel with your app: include this file, or the license text of each component you ship, in your app's about box or third-party notices.
