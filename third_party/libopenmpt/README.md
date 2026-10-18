# libopenmpt

Tracker module playback library (MOD, S3M, XM, IT, MPTM and the other formats OpenMPT reads),
BSD-3-Clause (see `LICENSE`).

| Library | Version | Source |
|---|---|---|
| libopenmpt | 0.8.9 | https://lib.openmpt.org (tag `libopenmpt-0.8.9` of the git mirror) |

## What was kept

Only the parts the library needs are here; the rest of upstream (tests, command-line players,
other build systems, documentation) was left out.

- `common/`
- `soundlib/`
- `sounddsp/`
- `src/mpt` and `src/openmpt`: only the subsets `soundlib` and `libopenmpt` include
- `libopenmpt/` (the public headers and the C and C++ API sources)
- `build/svn_version/svn_version.h`

`sources.txt` lists the `.cpp` files that are compiled, relative to this folder.
`third_party/libopenmpt.cmake` builds them as the static library `fa_openmpt` (C++17, hidden
visibility, exceptions and RTTI on) and links it into `fatzaudio`. It is built without zlib,
mpg123, libvorbis or any other external library, so MO3 files with MP3 or Ogg Vorbis compressed
samples are not supported (MO3 with plain samples play).

It is used by `src/dec_tracker.c` (through the C API, `libopenmpt.h` and `libopenmpt_ext.h`).
No libopenmpt symbol is exported from `fatzaudio`.

## Altered source

One local patch, in `libopenmpt/libopenmpt_config.h`: when `FA_OPENMPT_STATIC` is defined (the
cmake file defines it for everything that links `fa_openmpt`), `LIBOPENMPT_API` and the C++ API
macros become the header's *local* visibility helpers, so the API symbols are hidden instead of
being exported or imported. Each change carries a `FatzAudio` comment. Nothing else is modified.
