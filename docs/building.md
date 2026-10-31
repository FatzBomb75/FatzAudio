# Building FatzAudio

One source tree builds one library per platform. Nothing else ships with it: no
add-on libraries, no C++ runtime library, and FFmpeg and LAME are only ever
loaded at runtime if the app has them.

| Platform | Output | Built and tested |
|---|---|---|
| Windows x64 | `fatzaudio.dll` | CI: build and all tests |
| macOS | `libfatzaudio.dylib` | CI: build and all tests, with real audio output ([below](#tests-on-real-systems)) |
| Linux x64 | `libfatzaudio.so` | CI: build and all tests (including a dependency and export check), plus ASan/UBSan |
| Android arm64-v8a, x86_64, armeabi-v7a, x86 | `libfatzaudio.so` per ABI | CI: build of every ABI with a dependency and export check; all tests on x86_64 and arm64 in an Android 14 user space ([below](#run-the-tests-without-a-device)), and on an Android 14 emulator ([below](#tests-on-real-systems)) |
| iOS device (arm64), simulator (arm64, x86_64) | `fatzaudio.xcframework` | CI: build, and all tests in the iOS simulator ([below](#tests-on-real-systems)); never run on a device |

## Windows, macOS, Linux

Requirements: CMake 3.21+ and a C11 and C++17 compiler (Visual Studio 2026, Xcode, GCC 8+ or Clang 7+; libopenmpt, which plays tracker modules, is C++17).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The library is written to `build/bin/` (`build/bin/Release/` with Visual Studio; CI uses Ninja on Windows, so `build/bin/` there).
On Windows use the "Visual Studio 18 2026" generator and keep any old compilers
off `PATH`. Linux needs the ALSA headers for audio output (`libasound2-dev`).

The C++ runtime is linked into the library on Windows and Linux, so
`fatzaudio.dll` needs only Windows' own DLLs and `libfatzaudio.so` needs only the
C library (`libc`, `libm`), whatever GCC version built it. On Linux only the
`fa_*` functions are exported (`cmake/fatzaudio.map`), so the C++ runtime inside
cannot clash with the app's. The `deps` test checks both on Windows and Linux.
MinGW builds name the library `fatzaudio.dll`, like Visual Studio.

Release and MinSizeRel builds use link-time optimization across FatzAudio and the
codecs it carries (`FA_LTO`, on by default; `-DFA_LTO=OFF` turns it off, for example
to profile), and MSVC also compiles with `/Gw` so unused tables are dropped like
unused functions. Debug builds, and the sanitizer and fuzz builds, are unaffected.

## NuGet package and versions

CI builds the NuGet package `FatzAudio` on every run, from the `win-x64` and
`linux-x64` libraries its own build and test jobs just tested, and keeps it as
the run's `nuget` artifact. The Linux library is built on Ubuntu 22.04, so it
runs on any distribution with glibc 2.35 or newer (Debian 12, Ubuntu 22.04,
Fedora 36 and later).

Each run's package is a prerelease, such as `0.2.0-ci.123`, and pushes to
`main` publish it to the repository owner's GitHub Packages feed, so the
newest `main` is always there. The stable `0.2.0` is published by running the
`build` workflow by hand on `main` (Actions, build, Run workflow) with
"release" ticked.

The version is set in one place, `FA_VERSION_*` in
`include/fatzaudio/fatzaudio.h`; CMake and the .NET project read it from there.
[CHANGELOG.md](../CHANGELOG.md) lists every version and every breaking change.
Before 1.0 a minor bump may break the API, so bump the minor version with any
breaking change and the patch version otherwise. A stable version can be
published only once (the run fails otherwise), so bump the version before each
release. On Linux the minor version is part of the SONAME while the major is 0
(`libfatzaudio.so.0.2`), so a C program linked against 0.1 does not load 0.2.

To use the feed, add it to the app's `nuget.config`. GitHub Packages needs a
personal access token with `read:packages`, even for your own packages:

```xml
<configuration>
  <packageSources>
    <add key="fatzaudio" value="https://nuget.pkg.github.com/FatzBomb75/index.json" />
  </packageSources>
  <packageSourceCredentials>
    <fatzaudio>
      <add key="Username" value="FatzBomb75" />
      <add key="ClearTextPassword" value="%FATZAUDIO_PACKAGES_TOKEN%" />
    </fatzaudio>
  </packageSourceCredentials>
</configuration>
```

Or download the `nuget` artifact of a CI run and point a local folder feed at it.
Pin the version in `Directory.Packages.props` as usual:
`<PackageVersion Include="FatzAudio" Version="0.2.0" />`, or
`Version="0.2.0-ci.*"` to follow `main`.

To pack locally, put the libraries under
`bindings/dotnet/src/FatzAudio/runtimes/<rid>/native/` and run
`dotnet pack bindings/dotnet/src/FatzAudio -c Release`.

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
MediaCodec decoders. Those need an emulator or a phone: `tools/android-test/run-adb.py`
runs the suite there ([below](#tests-on-real-systems)).

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
  on a device, programs only run inside a signed app bundle. The simulator runs the
  tests ([below](#tests-on-real-systems)).
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

## Tests on real systems

Every test plays into a silent "null" output, so on their own they never touch a
system's sound devices, and they skip what a build machine cannot offer. The
`devices` CI workflow (`.github/workflows/devices.yml`) and the macOS build job
run them where those services are real, with `FA_TEST_REAL_SYSTEM=1`, which makes
the tests that need them fail instead of skipping:

- `device` (`tests/test_device.c`): the system's audio output is a real backend
  (not the null one) and pulls a stream in real time; a mic opens and runs
  (not on iOS, where a program needs an app's microphone usage string). Without
  the variable it skips.
- `aac`: the system's AAC decoder (Media Foundation, AudioToolbox, MediaCodec, the
  ones used for xHE-AAC) decodes HE-AAC like the built-in decoder.
- `net`: HTTPS through URLSession (iOS simulator) against a local server, and the
  Android HTTPS tests, which need the device's Java runtime.

| Where | What runs | When |
|---|---|---|
| Android 14 emulator (x86_64, KVM on a Linux runner) | every test, over adb: AAudio, MediaCodec, HttpURLConnection | `devices` workflow |
| iOS simulator (arm64) | every test: Core Audio output, AudioToolbox, URLSession | `devices` workflow |
| macOS runner | the build job's tests, with a virtual sound device (BlackHole); HTTPS is left to the simulator (a hosted Mac will not trust a test root without a person confirming it) | every build |

The `devices` workflow runs on every push to `main`, on demand, and on pull
requests that touch the code these systems run differently (the list is in the
workflow). Real CD drives and real phones stay untested.

### Android emulator or phone

`tools/android-test/run-adb.py` runs a build's suite over adb. The tests find their
data by absolute path and a device can only write under `/data/local/tmp`, so the
source and build folders must be there on the build machine too; they are copied
to the same paths on the device:

```bash
sudo mkdir -p /data/local/tmp/fa && sudo chown "$USER" /data/local/tmp/fa
rsync -a --exclude .git ./ /data/local/tmp/fa/src/
cmake -S /data/local/tmp/fa/src -B /data/local/tmp/fa/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release -DFA_BUILD_TOOLS=OFF
cmake --build /data/local/tmp/fa/build
FA_TEST_REAL_SYSTEM=1 tools/android-test/run-adb.py /data/local/tmp/fa/build
```

### iOS simulator

Configure the simulator slice with tests, and `tools/ios-sim/run.sh` as the
program that starts each one (`xcrun simctl spawn` in the booted simulator, which
shares the Mac's files):

```bash
cmake -S . -B build-iossim -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release -DFA_BUILD_TESTS=ON -DFA_BUILD_TOOLS=OFF \
  -DCMAKE_CROSSCOMPILING_EMULATOR="$PWD/tools/ios-sim/run.sh"
cmake --build build-iossim
xcrun simctl boot "iPhone 16"          # any iPhone simulator
ctest --test-dir build-iossim
```

### HTTPS through the system's TLS stack

`tools/test-https/serve.py DIR OUT` makes a test certificate authority and server
certificate in `OUT` and serves `DIR` on `https://127.0.0.1:8443/`. Trust `OUT/ca.crt`
(`security add-trusted-cert` on macOS, `xcrun simctl keychain booted add-root-cert`
for the simulator) and set `FA_NET_HTTPS_URL` to a file it serves.
