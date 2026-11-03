# Platforms

One source tree builds one library per platform. Nothing ships beside it: no add-on libraries, no C++ runtime library. Build instructions are in [Building](building.md); this page says what each platform needs and where it differs.

## What you get on each platform

| Platform | Library | Audio output | Needs | Tested |
|---|---|---|---|---|
| Windows x64 | `fatzaudio.dll` | The system's audio (WASAPI through miniaudio) | Windows' own DLLs only | CI: all tests |
| macOS | `libfatzaudio.dylib` | Core Audio | System frameworks | CI: all tests with real audio output |
| Linux x64 | `libfatzaudio.so` | ALSA directly, or through PulseAudio or PipeWire | `libc` and `libm`; glibc 2.35 or newer for the NuGet build | CI: all tests plus ASan/UBSan |
| Android (arm64-v8a, x86_64, armeabi-v7a, x86) | `libfatzaudio.so` per ABI | AAudio on Android 8.1 and later, OpenSL ES before | Android 7.0 (API 24) or later; `libc`, `libm`, `libdl` | CI: all tests on x86_64 and arm64 userspace and on an Android 14 emulator |
| iOS (device arm64; simulator arm64 and x86_64) | `fatzaudio.xcframework` | Core Audio | iOS 13 or later; system frameworks only | CI: all tests in the iOS simulator; never run on a device |

"Tested" is what CI does. Checks on real hardware (a real sound card, a USB mixer, a phone) wait for an app that uses the library.

## Where features differ

| Feature | Windows | macOS | Linux | Android | iOS |
|---|---|---|---|---|---|
| Playback, decoding, MIDI, trackers, tags, tempo and pitch, analysis, DJ mixer, karaoke | ✅ | ✅ | ✅ | ✅ | ✅ |
| WMA / ASF audio | ✅ (Media Foundation) | ➖ | ➖ | ➖ | ➖ |
| xHE-AAC | System decoder if present | System decoder if present | ➖ | System decoder if present | System decoder if present |
| Audio CD drives | ✅ | ✅ | ✅ | ➖ | ➖ (disc images still open) |
| Microphone input | ✅ (privacy setting) | ✅ (permission) | ✅ | ✅ (`RECORD_AUDIO`) | ✅ (usage description) |
| Loopback capture (what the PC plays) | ✅ | ➖ | ➖ | ➖ | ➖ |
| HTTPS | System TLS | System TLS | libcurl (`libcurl.so.4`), loaded at run time | `HttpURLConnection` | URLSession |
| System GM soundfont for MIDI | `gm.dls` | System bank | An installed `.sf2` | You supply one | You supply one |
| Lyric text shaping (Arabic, Hebrew, Indic, marks) | DirectWrite (Windows 10 1703+) | Core Text | HarfBuzz and FriBidi (`libharfbuzz.so.0`, `libfribidi.so.0`), loaded at run time | `TextRunShaper` (Android 12+, in an app) | Core Text |

Details: [streams](features/streams.md) (formats), [MIDI](features/midi.md), [discs](features/disc.md), [web streams](features/web-streams.md), [capture](features/callback-capture.md). Playing a codec the platform lacks returns `FA_ERROR_UNSUPPORTED_FORMAT`; see [formats](formats.md).

## .NET package

The `FatzAudio` NuGet package targets .NET 8 and .NET 10 and carries the `win-x64` and `linux-x64` libraries under `runtimes/<rid>/native/`. For other platforms stage the library in the same layout (`dotnet pack` takes it from `bindings/dotnet/src/FatzAudio/runtimes/<rid>/native/`) or reference the iOS xcframework as a native reference ([Building](building.md#nuget-package-and-versions)).

| Target | How |
|---|---|
| Windows, Linux desktop | The package alone |
| macOS | Stage `libfatzaudio.dylib` under `runtimes/<rid>/native/` (the package does not carry it yet) |
| .NET for Android | Add the `.so` per ABI under `lib/<abi>`; see [Building](building.md#use-it-from-net-for-android) |
| .NET for iOS | `<NativeReference ... Kind="Framework" />`; not yet tried with the binding ([engineering notes](engineering-notes.md)) |

The binding checks the native library's major.minor version on first use and refuses a mismatch with `InvalidOperationException`. Update the package and the native library together.

## Platform setup for apps

- **Android.** Declare `INTERNET` for web streams and `RECORD_AUDIO` for recording. Java and Kotlin apps that support Android 7 to 11 call `fa_android_set_java_vm` once for HTTPS. The library is aligned for 16 KB pages. Most apps need only `arm64-v8a`.
- **iOS.** Add the xcframework with "Embed & Sign". Configure the app's `AVAudioSession` yourself (category and activation) the way any audio app does. `https://` falls under App Transport Security.
- **macOS and iOS microphone.** Add the usage description to the app, and request permission before opening input; otherwise you get `PermissionDenied`.
- **Linux.** Build machines need the ALSA headers (`libasound2-dev`). Containers without a sound card use `NullOutput`.
- **Windows.** Build with the "Visual Studio 18 2026" generator and keep old compilers off `PATH`. Windows N editions without the Media Feature Pack have no Media Foundation, so WMA reports unsupported.

## Library size

Sizes from CI (stripped release builds): Windows x64 4.15 MiB, Linux x64 4.15, macOS 3.01, iOS arm64 2.91, Android arm64 3.70 MiB. That is the whole library: every format, effect, the DJ mixer and the karaoke surfaces.
