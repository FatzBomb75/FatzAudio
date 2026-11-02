# Limits and performance

The limits that are written down in the guides, in one place. Where the table names an error, that is what you get; otherwise the guide says.

## Limits

| What | Limit | When exceeded | Guide |
|---|---|---|---|
| Parametric equalizer bands per stream (`FA_EQ_MAX_BANDS`) | 16 | See the guide | [equalizer](features/equalizer.md) |
| Custom curve points (`FA_CURVE_MAX_POINTS`) | 64 | See the guide | [ramps](features/ramps.md) |
| Armed frame markers per stream | 64 | `FA_ERROR_BUSY` | [events](features/events.md) |
| Packets queued by an FFmpeg hand-off | 256 MB | `FA_ERROR_BUSY` (the push does not block) | [FFmpeg](features/ffmpeg.md) |
| Sound (sample) loaded into memory | 1 GB of PCM | `FA_ERROR_TOO_LARGE` | [sounds](features/sounds.md) |
| A compressed ZIP entry unpacked in memory | 256 MB, or `fa_engine_config.zip_memory_limit_mb` | `FA_ERROR_TOO_LARGE`, or unpacked to `temp_directory` if you set one | [CD+G](features/cdg.md#zip-archives) |
| MIDI file size | 16 MB; events later than 6 hours are dropped (refused with `FA_ERROR_INVALID_ARG` in an event list) | See the guide | [MIDI](features/midi.md) |
| Soundfont size | 1 GB | See the guide | [MIDI](features/midi.md) |
| MIDI song length | Measured to where the sound dies away, at most 10 s after the last event | A never-ending note is cut | [MIDI](features/midi.md) |
| CD+G surface size | Up to 8192 pixels each way | See the guide | [CD+G](features/cdg.md) |
| CD+G dirty rectangles per frame | 8 | Separate rectangles stay separate; the box around them is also given | [CD+G](features/cdg.md) |

## Performance notes

- **Decode ahead.** Streams decode on worker threads, so the audio callback only mixes. `fa_engine_get_stats` reports the workers' load and the dropout count for a diagnostics panel ([playback](features/playback.md#how-busy-it-is)).
- **Seeking.** `fa_stream_seek_async` returns in about 25 µs; a blocking seek waits at most one device period for the audio callback in progress ([playback](features/playback.md)).
- **Memory.** The level and spectrum meters keep about 4 s of audio (about 2 MB at 48 kHz) once you ask for them ([levels](features/levels-spectrum.md)). The system's `gm.dls` is held as about 7 MB of samples once used ([MIDI](features/midi.md)).
- **Size on disk.** The library is 3 to 4 MiB per platform ([platforms](platforms.md#library-size)).
- **Latency.** The device buffer is the main factor; the microphone and DJ-mixer latency measurements and what to expect are in [DJ mixer](features/dj-mixer.md).
- **Karaoke rendering.** Render-ahead on a CD+G or lyric surface moves frame drawing to worker threads; turn it off if the audio runs short ([CD+G](features/cdg.md)).

Hostile input is handled: the file readers, the ZIP reader and the HLS and tag parsers are fuzz-tested and every size above is checked before memory is allocated.
