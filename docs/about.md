# About: versions and licenses

## Versions

FatzAudio follows semantic versioning for its C ABI and the .NET package, which share one version number (`FA_VERSION_*` in the header, `fa_version()` at run time, `AudioEngine.NativeVersion` in .NET).

- Before 1.0 a **minor** bump (0.1 to 0.2) may break the API or ABI. Every break is listed in the [changelog](../CHANGELOG.md) under "Breaking". A **patch** bump never breaks.
- From 1.0 on only a major bump breaks.
- The .NET binding refuses a native library of another major.minor version, so a mismatched library is caught at once. On Linux the SONAME carries the minor version while the major is 0, and only the major from 1.0 on (`libfatzaudio.so.1`).
- New functions, new enum values at the end and new fields at the end of a struct that has a `struct_size` are not breaking.

Versioned output structs start with a `struct_size` that you set to `sizeof` before the call, so a newer library never writes past an older caller's struct ([concepts](concepts.md)).

## Licenses

FatzAudio's own code is original and proprietary: copyright (c) 2026 Keith (FatzBomb75), all rights reserved, no redistribution without written permission. See [LICENSE](../LICENSE). The third-party components below keep their own licenses.

The third-party libraries that are built into the library (miniaudio, libopus, libvorbis, libogg, TinySoundFont, Signalsmith Stretch, libopenmpt) are all permissively licensed and linked statically, so nothing else has to ship for them. Their names, versions, licenses and the notices your app should carry are in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

Loaded at run time and never bundled: LAME (LGPL, MP3 encoding only) and FFmpeg (never used by FatzAudio itself).

## Clean-room

FatzAudio was written from public specifications and the documented behaviour of the BASS API. BASS's own binaries were not decompiled.
