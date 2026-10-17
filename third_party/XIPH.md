# libopus, libvorbis and libogg

Vendored, BSD licensed (see the COPYING file in each folder), built by
`xiph.cmake` as static libraries linked into fatzaudio. Only the files the
library needs are copied; nothing is changed except where noted.

| Library | Version | Files | Source |
|---|---|---|---|
| libopus | 1.5.2 (tag v1.5.2, ddbe4838) | `opus/celt`, `opus/silk` (float build, no SIMD or DNN code), `opus/src`, `opus/include`; `opus/sources.txt` lists the compiled files (CELT, SILK, SILK float and Opus sources of the upstream `*_sources.mk`) | https://github.com/xiph/opus |
| libvorbis | 1.3.7 plus the fixes on master up to c2aa86b0 (2025-12-05: undefined shifts, a psy bounds check) | `vorbis/lib`: libvorbis itself (no vorbisfile, no vorbisenc), `vorbis/include/vorbis/codec.h` | https://github.com/xiph/vorbis |
| libogg | 1.3.6 (tag v1.3.6, be05b13e) | `ogg/src/bitwise.c`, `framing.c`, `crctable.h`, `ogg/include/ogg` | https://github.com/xiph/ogg |

Changes: `ogg/include/ogg/config_types.h` is written by hand (upstream generates
it from `config_types.h.in`) and uses `<stdint.h>`.

FatzAudio reads Ogg pages itself (`src/demux_ogg.c`); libogg is here for the bit
reader libvorbis uses. The libopus encoder is compiled in too, for the encoding
work (design doc R2: Opus encoding built in).
