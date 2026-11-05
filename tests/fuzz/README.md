# Fuzzing the file readers

Fuzz targets for the readers that take untrusted bytes: AAC and HE-AAC (ADTS, MP4, CAF, Matroska), MP3, FLAC (native,
Ogg, Matroska, MP4), MP4/MOV, Matroska/WebM, CAF, AVI, FLV, ASF, Ogg (Vorbis, Opus, FLAC, chained), AC-3/E-AC-3, WAV/W64/RF64/AIFF,
trackers (libopenmpt), MIDI (SMF, RMID, karaoke, GS/XG effect SysEx), tags (ID3, APE, Vorbis comments, MP4, RIFF, AIFF, Matroska),
ZIP (central directory, ZIP64, DEFLATE), HLS playlists with the MPEG-TS extractor, `.pls` / `.m3u` radio lists, and the web client
(HTTP replies, cookies, byte-range reads for MP4 and Matroska indexes) against a server inside the process; sounds (`fa_sound_*`);
and the parsers that take no audio file: soundfonts (SF2 with 24-bit `sm24`, SF3, DLS with conditional chunks), disc images and the
simulated drive, CD+G and fonts.

| Target | Entry point | Covers |
|---|---|---|
| `stream` | `fa_stream_open_memory`, read, seek, twice (all tracks, track 1); tracker cell and row events on; a MIDI file's first SysEx also sent live (`fa_stream_send_midi`) to a real-time synth | every audio and container reader and the decoders behind them; also fails on a non-finite sample. MIDI plays through a tiny built-in soundfont (a looped sine as a melodic and a drum preset), so notes, envelopes and effects run |
| `tags` | `fa_tags_read_memory`, every item and picture, `fa_tags_read_loudness` | tag readers |
| `zip` | `fa_zip_list`, `fa_zip_entry_offset`, `fa_zip_extract`, `fa_zip_extract_file` | ZIP reader and inflate |
| `hls` | `fa_hls_parse`, `fa_playlist_first_entry`, `fa_ts_feed`, `fa_aes128_cbc_decrypt` | playlist parser, `.pls` / `.m3u` first entry, TS demuxer |
| `net` | first byte = mode. 0: URL, headers, URL as text (`fa_url_*`, `fa_http_cookies_*`, `fa_http_header`, `fa_playlist_first_entry`). 1 to 3: a server on 127.0.0.1 in the process and `fa_stream_open_url` on it: the input is its reply (1), a file it serves with byte ranges (2), or an HLS playlist, a NUL, the segment bytes (3); read, seek twice | Set-Cookie and dates, `net_http.c` replies (chunked, unframed, redirects, 206/416, ICY), MP4 sidx / tfra / mfra and Matroska Cues / last-block reads over their own range requests, HLS over the network. Modes 1 to 3 do nothing on Windows |
| `sound` | byte 0 = script length, then the script (play, stop, voice limit and steal rule, list, close or detach a voice, render, info), then the file loaded as a sound (raw 16-bit PCM when it is no format) | `sound.c` voice pool, steal rules, voices closed while listed; the voices render on a null device |
| `soundfont` | `fa_soundfont_open_memory`, then notes on every preset (up to 256) with the mod wheel, pressure and pitch wheel moved | SF2 reader (`ifil` version, `sm24` 24-bit samples), SF3 Vorbis samples, the DLS converter (conditional `cdl ` chunks), TinySoundFont's zones, modulators and voices; fails on a non-finite sample |
| `cue` | a `.cue` sheet, a NUL, the image bytes: `fa_disc_open`, TOC, CD-Text, each track's stream read and seeked; then the same bytes as the answers of a simulated MMC drive | CUE parser, BINARY/WAVE/MOTOROLA images, `.sub` subcode, `disc_mmc.c` TOC, CD-Text and sector reads |
| `cdg` | 4 bytes of surface options, then raw packs: `fa_stream_load_cdg_memory` and surface draws at several times; the same bytes as disc subcode (`fa_disc_cdg_feed`) | CD+G decoder and checkpoints, classic and scaled surfaces, smoothness and edge blend, surface effects, the disc subcode layouts and parity |
| `font` | every face (up to 4): cmap, advances, kerning, outlines drawn as the lyric surface draws them; `fa_font_probe` | TrueType `glyf`, CFF charstrings, collections, `kern`/GPOS, the rasterizer |

The targets are libFuzzer-style (`LLVMFuzzerTestOneInput`). Where libFuzzer is not installed (GCC, or a clang without its
runtimes) `fuzz_driver.c` runs them: a small coverage-guided mutator (GCC `-fsanitize-coverage=trace-pc`) that keeps
inputs reaching new code, and saves the input that crashed or hung to `OUT/<target>/crashes`.

## Running

```sh
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFA_FUZZ=ON -DFA_BUILD_TOOLS=OFF \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
cmake --build build-fuzz -j --target fuzz_stream fuzz_tags fuzz_zip fuzz_hls fuzz_net fuzz_sound fuzz_soundfont fuzz_cue fuzz_cdg fuzz_font
tests/fuzz/run.sh build-fuzz fuzz-out 1000000 stream_aac     # one run per format; no names = all
```

`run.sh` seeds each format from `tests/data` (and `seeds/` for ZIP, HLS, WAV, MIDI, trackers, soundfonts, disc images and
CD+G, made by `make_seeds.py`; the WAV and SF3 seeds need ffmpeg once and are committed), caps inputs at
128 KiB (`FUZZ_MAXLEN`) and takes a seed (`FUZZ_SEED`). Speed under the sanitizers is roughly 70 to 250 execs a second
for the decoders (about 20 for trackers, which render whole seconds, and about 9 for MIDI with the effect seeds, which run every GS/XG effect), about 4 for `net` (a real connection per input), 200 for `sound`, 600 to 1500 for tags, ZIP and HLS, about
200 for fonts, disc images and soundfonts, and 2 for CD+G (scaled surfaces with effects are drawn in full each time;
the engine's workers help, so give it the whole machine). `cue` writes its files into the working directory. UBSan aborts
(`abort_on_error`) so the driver can save the input. The FA_FUZZ tree builds only the `fuzz_*` targets.

## Regression inputs

Every input that crashed or hung a reader goes in `regress/<target>/` with a name saying what it hit. A normal build
(`FA_FUZZ` off) builds `fuzz_regress_<target>`, which runs each saved input once (and the target's `seeds/<target>`, where
there is one), and ctest runs it with the other tests.

## Runs so far

GCC ASan+UBSan in a 4-core cloud container, `FUZZ_SEED=1`, 128 KiB inputs.

| Round | Formats | Execs each | Found (all fixed, inputs in `regress/`) |
|---|---|---|---|
| 1 (PR 141) | AAC/HE-AAC, MP3, FLAC, MKV, MP4, CAF, tags, ZIP, HLS | 1M | dr_flac null reads after a bad frame and a seek (2), HLS BYTERANGE and MEDIA-SEQUENCE overflows |
| 2a (extras step 2) | `stream_ogg`, `stream_asf`, `stream_dolby`, `stream_wav`, `stream_midi` 1M (MIDI as 941k + 100k from its corpus); `stream_tracker` 200k | see left | NaN/infinity passed through from float PCM; AC-3 codec `memmove` from a null buffer; a 2 GHz PCM sample rate asked for gigabytes of gap silence; WAV frame count times frame size overflowed |
| Tags (extras step 14) | `tags` with zlib ID3 frames, SYLT, Lyrics3, Matroska targeted tags, FLAC cue sheets, MIDI without a soundfont | 1M (+150k after review fixes) | nothing |
| 2b (extras step 3) | `soundfont` 1M + 300k (seed 2, from its corpus) + 500k (seed 3, fixed seeds); `font` 1M + 300k (seed 2); `cue` 1M; `cdg` 17k + 5.5k (seed 2, 45 minutes; about 2 a second) | see left | nothing |
| One walker each (extras step 23) | `tags`, `stream_mkv`, `stream_mp4`, `stream_ogg`, `stream_aac`, `stream_caf` after the shared EBML/MP4 walkers and the single ADTS, AudioSpecificConfig and Ogg comment parsers | 200k | nothing |
| Audit step 9 | `hls` (with `.pls` / `.m3u` first entries) 1M; `sound` 60k; `soundfont` 300k (with `sm24`, `cdl `, stack seeds); `tags` 300k and `zip` 200k (with ZIP songs); `stream_zip` 30k; `stream_midi` 30k (GS EFX, XG insertion and variation, drum and scale-tuning seeds; sent SysEx; about 9 a second); `stream_tracker` 20k (cell and row events on); `net` 3k (about 4 a second, a connection each) | see left | nothing |

The `net` target's first run found only a harness limit: a live HLS playlist whose segments are empty keeps `fa_stream_open_url` waiting
by design (the app's cancel flag ends it), so mode 3 closes the playlist with an ENDLIST and the target raises the cancel flag after 3 s.
The fuzz build shortens `FA_MIDI_TAIL_MAX_S` to 2 s, since a measured MIDI tail is rendered in full on every open.
