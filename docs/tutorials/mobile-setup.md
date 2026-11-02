# Tutorial: Android and iOS setup

Checklists to get sound out of an app on a phone. The library is tested in an Android 14 emulator and the iOS simulator; it has not been run on a physical phone yet, so test on devices.

## Android (native or .NET)

1. Build the library for your ABIs (most apps need only `arm64-v8a`; add `x86_64` for the emulator) or take them from CI: [Building](../building.md#android).
2. Put each `libfatzaudio.so` where your app packages native libraries. .NET for Android: see [Use it from .NET for Android](../building.md#use-it-from-net-for-android).
3. Manifest: `INTERNET` for web streams, `RECORD_AUDIO` for recording, and request the microphone permission at run time before opening input.
4. Android 7 to 11 and `https://`: Java and Kotlin apps call `fa_android_set_java_vm` once ([web streams](../features/web-streams.md#android)).
5. MIDI: ship a soundfont (the system has none) and set it on the engine ([MIDI](../features/midi.md)).
6. Play a file from the app's own storage with the ordinary open calls; use a content URI by opening it in your app and handing FatzAudio read callbacks (`fa_stream_open_io`).

Minimum Android is 7.0 (API 24).

## iOS

1. Build `fatzaudio.xcframework` with `tools/build-xcframework.sh` or take it from CI: [Building](../building.md#ios).
2. Xcode: add it to the target with "Embed & Sign". .NET for iOS: `<NativeReference Include="fatzaudio.xcframework" Kind="Framework" />` (not yet tried with the binding).
3. Configure your `AVAudioSession` (category such as playback, then activate it) the way any iOS audio app does, before creating the output.
4. Info.plist: a microphone usage description if you record; App Transport Security rules apply to `https://` streams.
5. MIDI: ship a soundfont.
6. No audio CD drives on iOS; disc images still open.

## Check it works

Run a short test app that plays a bundled WAV and prints `engine.GetStats()`; then try your real formats, an unplugged-headphones event and a call interruption ([devices](../features/devices.md)).
