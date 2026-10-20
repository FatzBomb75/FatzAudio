# FatzBomb.MediaControl: review of FatzAudio's MIDI and CD+G

To: FatzAudio. From: FatzBomb.MediaControl, 2026-10-20.
Reviewed against FatzAudio `main` @ c6e4446 (PR 78), side by side with our own CD+G engine and
our MIDI lyric, metadata and DLS code.

## Short version

- **CD+G:** your decoder matched ours pixel for pixel and palette for palette on 9,500 fuzzed
  packs, sequentially and with random seeks across checkpoints. That covers every instruction
  except the two places where ours is wrong (smooth scroll and memory-preset repeats). We found
  no instruction bug in yours. Your seeks are about 15x cheaper than ours.
- **MIDI/DLS:** your `dls.c` is far more complete than our DLS-to-SF2 converter (ptbl, loops via
  sampleModes, articulation, key groups, 8-bit and stereo waves, gain). We have the edge on
  karaoke lyric handling. Suggestions below.
- Everything marked "Verified by test" was run. Everything else is from reading and says so.

## CD+G

### How we tested

`src/cdg.c` was compiled standalone with stubs for the mutex, source and engine symbols, then
fed the same pack files as our C# decoder (`CdgStateMachine`, plus `CdgEngine` for seeks). Each
pack's 300x216 index frame and RGB palette were hashed and compared. Cases: all 900 tiles,
border, XOR, out-of-range rows and columns, every scroll command 0..3 with copy and fill, tiles
between scrolls, memory-preset repeats, the transparent-colour pack, both colour-table halves at
the maximum 6-bit values, non-graphics and unknown instructions, and 9,500 random packs.

With smooth-scroll offsets zeroed and memory-preset repeat 0 there were 0 mismatches. The
remaining differences are our bugs (we are fixing them).

### Suggestions

1. **Memory-preset repeats after drawing re-clear the screen.** `cdg.c:254` skips a repeat
   (repeat != 0) only while `clean` is set, and any border or tile clears `clean`. So
   preset(c, 0), border(x), preset(c, 1) wipes the screen again. Real files rarely interleave,
   but a repeat group is meant to be redundant copies of one clear: consider skipping a repeat
   when the previous pack was a preset of the same colour, whatever was drawn before it.
   (Verified by reading; the tile-between-presets case verified by test.)
2. **Disc packs are written without a parity check.** `disc_cdg.c` `emit` (about line 150)
   applies every decoded pack; parity is only used to choose the layout. A scratched disc can
   write garbage tiles. Consider dropping packs that fail Q parity (and correcting what P parity
   can). (Verified by reading; we have no disc data.)
3. **Possible use-after-free on `f->stream`.** `surface_update` (`cdg.c:1042`) and
   `fa_cdg_surface_destroy` (`cdg.c:955`) read `f->stream` before taking `cdg_lock`, while
   `fa_stream_cdg_close` nulls it under the lock. An update or destroy racing a stream close can
   touch freed memory. (Likely; single-threaded use is safe.)
4. **Things of ours you might take:**
   - Precise dirty rects: we add a rect only when a tile or border actually changed a pixel;
     yours marks every tile and border preset.
   - Immutable frames for other threads: our snapshots are never mutated after creation and carry
     a `DirtyBaseRevision` (the revision their dirty rects are relative to), so a GPU thread can
     upload only the changed regions safely. Your pixel buffer is updated in place; a reader on
     another thread would tear. Worth documenting even if you keep it.
5. **Instruction 28 (transparent colour).** You read it as 16 per-colour 6-bit levels; we treat
   it as unused. Neither of us can point to a spec reference that settles it. If you have one,
   we would like it.

## MIDI and DLS

### Suggestions

1. **Text encoding.** `dec_midi.c:340-358`: valid UTF-8 or else Windows-1252. Shift-JIS (common
   in Japanese karaoke `.kar` files), GBK, Big5 and EUC-KR come out garbled, and a UTF-8 BOM is
   not handled. A cheap win: score the bytes as Shift-JIS before falling back to 1252, or let the
   app pass a code page. (Our side currently gets the system ANSI code page from BASS, which is
   no better; neither of us does this right yet.)
2. **Karaoke detection is gated too tightly.** `dec_midi.c:1010-1015, 1061` turn text events
   (meta 1) into lyrics only with `@K`/`@T` or a track named "Words", and any stray lyric event
   (meta 5) disables the text fallback (`:1008`). Plenty of `.kar` files have neither header nor
   a "Words" track. Ours falls back further (and is too loose in the other direction: it can turn
   instrument names into lyrics). A middle rule: the track with the most text events that look
   like syllables (short, `/` or `\` breaks, timed with notes).
3. **Chord lines.** Tune1000 `%...` and Solton `<...` chord markers pass through as syllables
   (`:961-980`). We filter them.
4. **Line markers mid-fragment.** `/` and `\` are recognised only at the start of a syllable
   (`:969-970`). Some files put them at the end (`word/`). We handle both.
5. **`@I` metadata.** Writer, lyricist and year in `@I` lines are not parsed; only the first
   copyright event and two `@T` lines are kept.
6. **Program change fallback.** `dec_midi.c:531` ignores the result of
   `tsf_channel_set_presetnumber`; when neither the bank nor bank 0 has the program, the channel
   keeps its previous instrument. GM practice is usually to fall back to program 0 of bank 0
   (or a drum kit 0 on drum channels).
7. **Velocity curve.** `tsf.h:1694` uses 20·log10(velocity). The SF2 2.04 default
   velocity-to-attenuation modulator is a concave curve reaching 96 dB (about 40·log10), so
   dynamics may be shallower than other SF2 players. (Medium-low confidence: please compare
   against FluidSynth on one file.)
8. **Smaller ones:**
   - `dls.c:449`: a region's `lart` replaces the instrument's rather than merging. Harmless on
     Windows `gm.dls` (no instrument has both), uncertain for other banks.
   - `dls.c:547` silently caps an instrument at 128 regions; `:576` clamps gain so boosts are lost.
   - `sf3.c:167` decodes only the first channel of a Vorbis sample.

### What we learned from yours

Your `dls.c` is the reference for us: ptbl cue offsets, wave-level wsmp as the fallback,
sampleModes (our converter never writes it, so nothing loops), key groups, art1/art2 connections
(envelopes, LFO, pan, gain), and fuzz tests for malformed banks. We will likely retire our
converter in favour of your DLS loading when MIDI moves to FatzAudio (our stage 037A).

For reference, Windows `gm.dls`: DLS level 1, 235 instruments, 1,498 regions (all `rgn `), 495
mono 16-bit waves at 22,050 or 24,000 Hz, ptbl in wave order, 439 looping waves (region and wave
loops always agree), only `art1` articulation, 120 regions with key groups, and one region with
the 0x80000000 "silent" gain sentinel.
