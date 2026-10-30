# Fuzzing the file readers

Fuzz targets for the readers that take untrusted bytes: AAC and HE-AAC (ADTS, MP4, CAF, Matroska), MP3, FLAC (native,
Ogg, Matroska, MP4), MP4/MOV, Matroska/WebM, CAF, ASF, Ogg (Vorbis, Opus, FLAC, chained), AC-3/E-AC-3, WAV/W64/RF64/AIFF,
trackers (libopenmpt), MIDI (SMF, RMID, karaoke), tags (ID3, APE, Vorbis comments, MP4, RIFF, AIFF, Matroska),
ZIP (central directory, ZIP64, DEFLATE) and HLS playlists with the MPEG-TS extractor.

| Target | Entry point | Covers |
|---|---|---|
| `stream` | `fa_stream_open_memory`, read, seek, twice (all tracks, track 1) | every audio and container reader and the decoders behind them; also fails on a non-finite sample. MIDI plays through a tiny built-in soundfont (a looped sine as a melodic and a drum preset), so notes, envelopes and effects run |
| `tags` | `fa_tags_read_memory`, every item and picture, `fa_tags_read_loudness` | tag readers |
| `zip` | `fa_zip_list`, `fa_zip_entry_offset`, `fa_zip_extract`, `fa_zip_extract_file` | ZIP reader and inflate |
| `hls` | `fa_hls_parse`, `fa_ts_feed`, `fa_aes128_cbc_decrypt` | playlist parser, TS demuxer |

The targets are libFuzzer-style (`LLVMFuzzerTestOneInput`). Where libFuzzer is not installed (GCC, or a clang without its
runtimes) `fuzz_driver.c` runs them: a small coverage-guided mutator (GCC `-fsanitize-coverage=trace-pc`) that keeps
inputs reaching new code, and saves the input that crashed or hung to `OUT/<target>/crashes`.

## Running

```sh
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFA_FUZZ=ON -DFA_BUILD_TOOLS=OFF \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
cmake --build build-fuzz -j --target fuzz_stream fuzz_tags fuzz_zip fuzz_hls
tests/fuzz/run.sh build-fuzz fuzz-out 1000000 stream_aac     # one run per format; no names = all
```

`run.sh` seeds each format from `tests/data` (and `seeds/` for ZIP, HLS, WAV, MIDI and trackers, made by `make_seeds.py`;
the WAV seeds need ffmpeg once and are committed), caps inputs at
128 KiB (`FUZZ_MAXLEN`) and takes a seed (`FUZZ_SEED`). Speed under the sanitizers is roughly 70 to 250 execs a second
for the decoders (about 20 for trackers, which render whole seconds), 600 to 1500 for tags, ZIP and HLS. UBSan aborts
(`abort_on_error`) so the driver can save the input. The FA_FUZZ tree builds only the `fuzz_*` targets.

## Regression inputs

Every input that crashed or hung a reader goes in `regress/<target>/` with a name saying what it hit. A normal build
(`FA_FUZZ` off) builds `fuzz_regress_<target>`, which runs each saved input once, and ctest runs it with the other tests.

## Runs so far

GCC ASan+UBSan in a 4-core cloud container, `FUZZ_SEED=1`, 128 KiB inputs.

| Round | Formats | Execs each | Found (all fixed, inputs in `regress/`) |
|---|---|---|---|
| 1 (PR 141) | AAC/HE-AAC, MP3, FLAC, MKV, MP4, CAF, tags, ZIP, HLS | 1M | dr_flac null reads after a bad frame and a seek (2), HLS BYTERANGE and MEDIA-SEQUENCE overflows |
| 2a | `stream_ogg`, `stream_asf`, `stream_dolby`, `stream_wav`, `stream_midi` 1M; `stream_tracker` 200k | see left | NaN/infinity passed through from float PCM; AC-3 codec `memmove` from a null buffer; a 2 GHz PCM sample rate asked for gigabytes of gap silence; WAV frame count times frame size overflowed |
