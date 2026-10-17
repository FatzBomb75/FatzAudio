# Loudness normalization

Normalization makes every stream play at the same perceived loudness, measured to **ITU-R BS.1770-4 / EBU R128** (the standard broadcasters and streaming services use). You turn it on once for the engine. From then on every stream gets the right gain automatically, with no analysis pass in your app and no waiting before playback.

## Quick start

C:
```c
fa_engine_set_normalization(engine, FA_NORMALIZE_TRACK, -16.0);  /* target in LUFS */

fa_stream* s;
fa_stream_open_path(engine, "concert.mkv", NULL, &s);  /* analysis starts here, in the background */
fa_stream_attach(s, device, NULL);
fa_stream_play(s);                                      /* plays at once, at the right level */
```

C#:
```csharp
engine.SetNormalization(NormalizeMode.Track, -16);

using var song = engine.OpenFile("concert.mkv");
song.PlayOn(output);
```

That's all. There is no rate or gain to set afterwards.

## Where the loudness comes from

For each stream, the first of these that is available wins:

| # | Source | Speed | `fa_loudness.source` |
|---|---|---|---|
| 1 | A value your app stored earlier, given with `fa_stream_set_loudness` | instant | `FA_LOUDNESS_APP` |
| 2 | ReplayGain 2 or R128 tags in the file (ID3v2 `TXXX`, FLAC Vorbis comments, APEv2) | instant | `FA_LOUDNESS_TAGS` |
| 3 | The scan cache (`fa_engine_config.cache_directory`), keyed by the file's content | instant | `FA_LOUDNESS_CACHE` |
| 4 | A background scan of the file, then written to the cache | see below | `FA_LOUDNESS_SCAN` |
| 5 | Until one of the above is known, and for streams that cannot be scanned: a running estimate over the last 15 s of decoded audio | continuous | `FA_LOUDNESS_ESTIMATE` |

### Why the scan is fast, even on large video files

- **Only audio is decoded.** The scanner opens the file through the same decoder registry as playback, so a video container is read through its audio track alone and video packets are never decoded. (MP4 and Matroska demuxing arrives with the FFmpeg hand-off phase; their files are then scanned this way with no change to your code.)
- **All cores at once.** The file is cut into chunks at 100 ms boundaries and each chunk is scanned by its own decoder on its own thread, starting one second early so the filters are settled. The chunks' results are joined exactly, so the answer is identical to a single pass.
- **Once per file.** With a cache directory set, the result is stored under a fingerprint of the file's content (size plus the first, middle and last 64 KB), so renamed or moved files are still recognised and nothing is scanned twice. Entries written by an older version of the measurement are ignored and the file is scanned again.
- **Nothing waits for it.** Analysis starts when the stream is opened, on scan threads separate from the playback workers (one per core, minus one left for playback).

Measured on the CI test file (3 minutes of stereo 44.1 kHz WAV, 4-core container): about 0.4 s, roughly 450x real time. MP3 files without a seek table are scanned in one pass, since seeking in them means decoding from the start.

### Playback never waits

Playback starts with the best value known at that moment. When the exact value arrives, the gain **glides** to it (at most 12 dB/s down, 6 dB/s up) so no jump is heard, and `FA_EVENT_LOUDNESS_FINAL` is raised. If the exact value is already known when the stream starts playing, which is the usual case for tagged, cached or short files, the first sample already has the exact gain.

### Live streams and app I/O

Streams that can't be read a second time (`fa_stream_open_io` and [web streams](web-streams.md)) are never scanned. They use the running estimate, which adapts as the program changes. You can still give them a value with `fa_stream_set_loudness`.

## Gain

`gain = target − loudness`, limited to **−30 … +12 dB**. Silent or unmeasurable streams get 0 dB. `fa_loudness.gain_db` tells you the gain a stream gets with the current settings.

| Target | Typical use |
|---|---|
| −14 LUFS | Matches Spotify, YouTube and similar services |
| −16 LUFS | Music players, karaoke, podcasts (a good default) |
| −23 LUFS | EBU R128 broadcast |

Changing the target or mode while streams play takes effect at once, with the same glide.

### Track and album mode

| Mode | Behaviour |
|---|---|
| `FA_NORMALIZE_OFF` | Default. No gain. Analysis still runs when you ask for a stream's loudness. |
| `FA_NORMALIZE_TRACK` | Every stream to the target. |
| `FA_NORMALIZE_ALBUM` | Album gain from tags when present, so the quiet and loud songs of an album keep their balance; otherwise per track. A value from `fa_stream_set_loudness` wins in both modes. |

### Opting a stream out

Open it with `FA_OPEN_NO_NORMALIZE` (.NET: `new StreamOptions { NoNormalize = true }`) for jingles, sound effects or mixes that are already mastered. Decode-only streams (`FA_OPEN_DECODE_ONLY`) are never normalized: `fa_stream_read` returns the source's own samples.

## True-peak limiter

A boost can push peaks over full scale, and so can two loud streams crossfading. Every output device therefore has a **true-peak limiter**: it detects peaks between samples too (4x oversampling), looks 1.5 ms ahead so it is already at the right gain when a peak arrives, releases smoothly over about 150 ms, and never touches audio below the ceiling.

```c
fa_engine_set_limiter(engine, FA_LIMITER_AUTO, -1.0);  /* the default: on while normalization is on */
```

| Mode | Behaviour |
|---|---|
| `FA_LIMITER_AUTO` | On while normalization is on (default) |
| `FA_LIMITER_ON` | Always on |
| `FA_LIMITER_OFF` | Always off |

The look-ahead (about 1.9 ms in total) is always in the signal path, also while the limiter is off, and the limiter keeps measuring while off. Switching it on or off fades over 20 ms and a new ceiling applies to audio not yet in the look-ahead, so neither ever clicks or clips. `fa_device_get_latency` and `fa_stream_get_clock` include it, so video and CD+G stay in sync.

## Reading and storing loudness

C:
```c
fa_loudness l;
fa_stream_get_loudness(s, &l);        /* never blocks; starts the analysis if needed */
if (l.is_final)
    save_to_library(path, l.integrated_lufs, l.range_lu, l.true_peak_dbtp);

/* Next time, skip all analysis: */
fa_loudness known = { stored_lufs, NAN, NAN };
fa_stream_set_loudness(s, &known);

/* Or measure files without playing them (e.g. a library import), blocking: */
fa_scan_loudness_path(engine, "song.flac", &l);
```

C#:
```csharp
song.LoudnessFinal += (_, _) =>
{
    var l = song.GetLoudness();
    library.Save(path, l.IntegratedLufs, l.RangeLu, l.TruePeakDbtp);
};

song.SetLoudness(storedLufs);                                  // next time

var scanned = await Task.Run(() => engine.ScanLoudness("song.flac"));
Console.WriteLine($"{scanned.IntegratedLufs:F1} LUFS, LRA {scanned.RangeLu:F1} LU, {scanned.TruePeakDbtp:F1} dBTP");
```

| Field | Meaning |
|---|---|
| `integrated_lufs` | Integrated loudness (gated, BS.1770-4). `-INFINITY` for digital silence. |
| `range_lu` | Loudness range, EBU Tech 3342. |
| `true_peak_dbtp` | True peak. From tags this is the tagged sample peak. |
| `is_final` | 1 when the source is tags, cache, scan or app; 0 for an estimate or nothing yet. |
| `source` | `fa_loudness_source`, see the table above. |
| `gain_db` | Gain normalization applies to this stream right now. |

Values that aren't known are `NAN` (.NET: `double.NaN`), for example the range and peak of an estimate.

## Threads

- Scans run on their own scan threads, never on the decode workers or the audio thread. `fa_scan_loudness_path` also uses the calling thread.
- The running estimate is computed by the decode worker that fills the stream, on audio it decodes anyway.
- The audio thread only reads the gain (atomics) and moves it a little per block.
- `FA_EVENT_LOUDNESS_FINAL` is delivered on the event thread like every other event.
- Closing a stream stops its scan at once; it never waits for a scan to finish.

## Accuracy

The meter is tested against the EBU compliance cases (Tech 3341 integrated loudness with gating at 44.1 and 48 kHz, Tech 3342 loudness range) to ±0.1 LU, true peak to the BS.1770 tolerance (flat to 20 kHz at 48 kHz, never reading high, sample peaks and the last samples of a file included), channel weights for quad, 5.1 and 6.1 (surrounds +1.5 dB, LFE excluded), and a chunked scan against a single pass to 0.01 LU. See `tests/test_loudness.c`.

## Pitfalls

- **Don't set the volume to normalize.** `FA_PARAM_VOLUME` stays yours for fades and user volume; normalization is applied on top of it.
- **ReplayGain 1 tags** (89 dB reference) are read as ReplayGain 2 (−18 LUFS reference); most taggers have written RG2 for years. If your library has old tags, set `fa_stream_set_loudness` from your own data instead.
- The cache only knows what is in its directory. Point every engine of your app at the same directory to share results.
