# TinySoundFont and TinyMidiLoader

SoundFont 2 synthesizer (`tsf.h`, MIT, see `LICENSE`) and Standard MIDI File parser
(`tml.h`, zlib, license text at the top of the file), by Bernhard Schelling.

| Library | Version | Source |
|---|---|---|
| TinySoundFont | v0.9 (header) | https://github.com/schellingb/TinySoundFont |
| TinyMidiLoader | v0.7 (header) | same repository |

Upstream commit `853a0a171759f1ddba0de1442133a75912bbeffa` (2026-07-19). Line endings
were converted from CRLF to LF; nothing else is reformatted.

The implementation is compiled once, in `src/tinysoundfont_impl.c` (with `TSF_NO_STDIO`
and `TML_NO_STDIO`: files are read through FatzAudio's own I/O), and used by
`src/dec_midi.c`. No symbol is exported from the library.

## Altered source

Both files are altered (the zlib license asks for it to be marked: each file says so at
the top, and every change carries a `FatzAudio` comment). The changes harden the parsers
against untrusted files and add what the decoder needs:

`tml.h`
- `tml_load` rewritten: 32-bit header length (6..256), SMPTE time division, files with no
  tracks or a zero division rejected, unknown chunks skipped, truncated tracks parsed up
  to the cut. Tracks are merged with a min-heap on (64-bit tick, track) instead of a
  running 32-bit sum, and each message gets an exact `seconds` from the tempo map.
  Format 2 tracks (independent patterns) play one after another instead of together.
- Limits: `TML_MAX_TRACK_BYTES` (64 MB) and `TML_MAX_MESSAGES` (4M).
- Negative deltas rejected, running status only from channel messages, sysex and meta
  lengths bounds-checked, system common/real-time bytes skipped, no leak on a failed
  `realloc`, `tml_load_memory` rejects NULL or a negative size.
- `tml_message` gains `seconds`, `text`/`text_length`/`text_offset` (meta text 1..7 kept
  in a pool after the messages) and `track`; `enum TMLSystemType` is public.

`tsf.h`
- RIFF sizes clamped to the buffer (`tsf_load_memory` passes the real size); chunk size
  checks that cannot overflow; a LIST shorter than 4 bytes rejected; a duplicate hydra
  chunk fails the load.
- `initialAttenuation` is applied at the SF2 spec's 0.1 dB per centibel, up to 144 dB
  (upstream used 0.01 dB, at most 14.4 dB, which flattened the level differences between
  zones and instruments).
- `tsf_hydra_valid`: minimum record counts, and every bag/generator/instrument/sample
  index checked monotonic and in range; out-of-range sample IDs and instrument indices
  skipped; sample offsets and loop points clamped inside the sample.
- Sample data followed by zeroed padding floats; a short read is an error. Hydra arrays
  zeroed after allocation. `TSF_MAX_REGIONS` caps the region count.
- No negative left shift (`GEN_UINT_ADD15`), keynum-scaled hold/decay clamped,
  `offsetof` in `_TSFREGIONOFFSET`, presets sorted with `qsort` (was O(n^2)).
