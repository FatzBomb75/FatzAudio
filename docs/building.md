# Building FatzAudio

One source tree builds one library per platform. Nothing else ships with it: no
add-on libraries, no C++ runtime library, and FFmpeg and LAME are only ever
loaded at runtime if the app has them.

| Platform | Output | Built and tested |
|---|---|---|
| Windows x64 | `fatzaudio.dll` | CI: build and all tests |
| macOS | `libfatzaudio.dylib` | CI: build and all tests |
| Linux x64 | `libfatzaudio.so` | CI: build and all tests (including a dependency and export check), plus ASan/UBSan |
| Android arm64-v8a, x86_64, armeabi-v7a, x86 | `libfatzaudio.so` per ABI | CI: build of every ABI with a dependency and export check; all tests on x86_64 and arm64 in an Android 14 user space ([below](#run-the-tests-without-a-device)) |
| iOS device (arm64), simulator (arm64, x86_64) | `fatzaudio.xcframework` | CI: build only (iOS programs need a signed app to run) |

## Windows, macOS, Linux

Requirements: CMake 3.21+ and a C11 compiler (Visual Studio 2026, Xcode, GCC or Clang).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The library is written to `build/bin/` (`build/bin/Release/` with Visual Studio).
On Windows use the "Visual Studio 18 2026" generator and keep any old compilers
off `PATH`. Linux needs the ALSA headers for audio output (`libasound2-dev`).

The C++ runtime is linked into the library on Windows and Linux, so
`fatzaudio.dll` needs only Windows' own DLLs and `libfatzaudio.so` needs only the
C library (`libc`, `libm`), whatever GCC version built it. On Linux only the
`fa_*` functions are exported (`cmake/fatzaudio.map`), so the C++ runtime inside
cannot clash with the app's. The `deps` test checks both on Windows and Linux.
MinGW builds name the library `fatzaudio.dll`, like Visual Studio.

## Android

Requirements: the Android NDK (r28 or later recommended; Android Studio's SDK
Manager installs it as "NDK (Side by side)"), CMake 3.21+ and Ninja. Build each
ABI into its own folder:

```bash
NDK=$HOME/Android/Sdk/ndk/28.2.13676358     # where your NDK is
for abi in arm64-v8a x86_64 armeabi-v7a x86; do
  cmake -S . -B build-android-$abi -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build build-android-$abi
done
```

On Windows, run the same from a Developer prompt with `-G Ninja` (Visual Studio
includes Ninja) and the NDK under `%LOCALAPPDATA%\Android\Sdk\ndk\`.

Each `build-android-<abi>/bin/libfatzaudio.so` is the whole library, about
1.8 MB (1.5 MB once the app's packaging strips it). It depends only on the system's `libc.so`, `libm.so` and `libdl.so`:

- The C++ runtime is linked in (the NDK's default `c++_static`), so there is no
  `libc++_shared.so` to ship, and only `fa_*` functions are exported.
- Release builds drop the debug info but keep the symbol table (for readable
  crash reports), and are aligned for 16 KB pages, which Google Play requires
  for new apps and Android 15 devices with 16 KB pages need.
- Minimum Android version: 7.0 (API 24). The 64-bit ABIs also build for API 21,
  which is untested; the 32-bit ABIs need API 24 (64-bit file offsets).
- Audio output is AAudio on Android 8.1 and later, OpenSL ES before; both are
  part of the system and loaded at runtime.

Most apps only need `arm64-v8a`. Add `x86_64` for the emulator, and the 32-bit
ABIs only for old or low-end phones.

What your app needs:

- `android.permission.INTERNET` for web streams, `RECORD_AUDIO` for recording.
- `https://` goes through Android's own `HttpURLConnection` on the app's Java
  VM. Apps on Android 12 and later and .NET for Android apps need nothing more;
  Java and Kotlin apps that support Android 7 to 11 call
  `fa_android_set_java_vm` once ([web streams](features/web-streams.md#android)).
- FFmpeg or LAME, if the app wants them: their `.so` files in the app's own
  native library folder, as for any other library it loads.

### Use it from .NET for Android

Put each ABI's library under the binding project's `runtimes` folder before
packing, and the NuGet package carries them to Android apps:

```
bindings/dotnet/src/FatzAudio/runtimes/android-arm64/native/libfatzaudio.so
bindings/dotnet/src/FatzAudio/runtimes/android-x64/native/libfatzaudio.so
bindings/dotnet/src/FatzAudio/runtimes/android-arm/native/libfatzaudio.so
bindings/dotnet/src/FatzAudio/runtimes/android-x86/native/libfatzaudio.so
```

In an app that references the native library directly instead, add it as
`<AndroidNativeLibrary Include="path/to/libfatzaudio.so" Abi="arm64-v8a" />`.
The binding calls it as `fatzaudio`, which Android resolves to `libfatzaudio.so`.

### Run the tests without a device

`tools/android-test/run.sh` runs a build's whole `ctest` suite on a Linux PC or
CI runner, without an emulator. It downloads an Android 14 user space once (the
`redroid` image, pinned by digest, about 870 MB) and runs every test inside it
with `chroot`, against the real Android C library and dynamic linker. x86_64 and
x86 run natively; arm64-v8a and armeabi-v7a run under qemu-user
(`apt install qemu-user-static`; loudness and tempo then take a few minutes).

```bash
sudo tools/android-test/run.sh build-android-x86_64
sudo tools/android-test/run.sh build-android-arm64-v8a 'core|ogg'   # only some tests
```

The `net` suite also runs the web stream tests over `https://` through Android's
own `HttpURLConnection`: the test starts Android's Java runtime inside the chroot,
for which `run.sh` mounts the APEX modules and writes the linker namespaces as a
booted device does (it compiles `tools/android-test/props.c` with the build's
NDK compiler for that). The Java runtime opens IPv6 sockets, so the machine needs
IPv6 support in its kernel (CI runners have it).

It needs root (`chroot` and bind mounts). What it cannot cover is anything that
needs Android's system services: real audio output (AAudio, OpenSL ES) and the
MediaCodec decoders. Those need an emulator or a phone: copy
`build-android-<abi>/bin` to `/data/local/tmp` with `adb push` and run the test
programs there with `LD_LIBRARY_PATH=/data/local/tmp/bin`.

## iOS

Requirements: a Mac with Xcode and CMake 3.21+.

```bash
tools/build-xcframework.sh            # writes build-ios/fatzaudio.xcframework
FA_IOS_MIN=14.0 tools/build-xcframework.sh build-ios   # another minimum iOS
```

The script builds a dynamic `fatzaudio.framework` for devices (arm64) and one
for the simulator (arm64 and x86_64), and combines them into
`fatzaudio.xcframework`, which Xcode and .NET for iOS both take. The minimum is
iOS 13.0 unless `FA_IOS_MIN` says otherwise. To build one slice by hand:

```bash
cmake -S . -B build-ios-device -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_BUILD_TYPE=Release
cmake --build build-ios-device --config Release
```

- Tests and tools are off by default for iOS (`FA_BUILD_TESTS`, `FA_BUILD_TOOLS`):
  iOS programs only run inside a signed app bundle.
- The framework's bundle identifier is `com.fatzaudio.fatzaudio`; set
  `-DFA_IOS_BUNDLE_ID=...` for your own.
- It links only system frameworks: Foundation, CFNetwork, CoreFoundation,
  CoreAudio, AudioToolbox and AVFoundation. `https://` goes through URLSession,
  so App Transport Security rules apply to it; `http://` stations use the
  library's own HTTP client and need no ATS exception.
- Audio CDs are not available on iOS (no drives); disc images still open.

In Xcode, add `fatzaudio.xcframework` to the app target with "Embed & Sign" and
`#include <fatzaudio/fatzaudio.h>`. A .NET for iOS app references it as
`<NativeReference Include="fatzaudio.xcframework" Kind="Framework" />`; that
route has not been tried with the binding yet ([engineering notes](engineering-notes.md)).
