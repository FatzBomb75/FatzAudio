# Track analysis

Track analysis measures a song once and gives you its **beat grid** (tempo and where the beats fall), its **musical key**, a **waveform** overview for drawing, and its **loudness**. One decode of the audio feeds all of them, so asking for several costs little more than asking for one. It is what a DJ deck needs for sync and beat loops, and what a library needs to show BPM and key. It replaces `BASS_FX_BPM_*`.

| Kind | `fa_analysis_kind` | .NET `AnalysisKinds` | You get |
|---|---|---|---|
| Beats | `FA_ANALYZE_BEATS` | `Beats` | A beat grid: tempo, the frame of the first beat, a confidence, and tempo segments |
| Key | `FA_ANALYZE_KEY` | `Key` | Tonic and mode, the name (`F#m`) and the Camelot code (`11A`), a confidence |
| Waveform | `FA_ANALYZE_WAVEFORM` | `Waveform` | Peak and low, mid and high levels every 10 ms |
| Loudness | `FA_ANALYZE_LOUDNESS` | `Loudness` | The same measurement as `fa_scan_loudness_path` ([normalization](normalization.md)), in the same pass |

The default is beats and key. MIDI files and tracker modules do not get their beat grid by listening: they carry their tempo, so the grid comes from the file and is exact (see [Exact grids](#exact-grids-from-midi-files-and-tracker-modules)).

## Quick start

C:
```c
fa_analysis_options opt;
fa_analysis_options_init(&opt);
opt.kinds = FA_ANALYZE_BEATS | FA_ANALYZE_KEY | FA_ANALYZE_WAVEFORM;

fa_analysis* a = NULL;
if (fa_analyze_path(engine, "song.mp3", &opt, &a) == FA_OK) {   /* blocks, about 1.5 s for 4 minutes */
    fa_beat_grid grid;
    if (fa_analysis_get_beat_grid(a, &grid) == FA_OK)
        printf("%.2f BPM, first beat at frame %lld, confidence %.2f\n",
               grid.bpm, (long long)grid.first_beat, grid.confidence);

    fa_key_info key;
    if (fa_analysis_get_key(a, &key) == FA_OK)
        printf("%s (%s)\n", key.name, key.camelot);              /* "F#m (11A)" */

    uint32_t n = 0;
    fa_analysis_get_waveform(a, NULL, 0, &n);                    /* ask the count */
    fa_waveform_point* wave = malloc(n * sizeof *wave);
    fa_analysis_get_waveform(a, wave, n, &n);                    /* point i covers 10 ms from i * 10 ms */

    fa_analysis_free(a);
}
```

C#:
```csharp
var t = await engine.AnalyzeAsync("song.mp3", AnalysisKinds.Beats | AnalysisKinds.Key | AnalysisKinds.Waveform);

if (t.Beats is { } grid)
    Console.WriteLine($"{grid.Bpm:F2} BPM, first beat at {grid.FirstBeatTime}, confidence {grid.Confidence:F2}");
if (t.Key is { } key)
    Console.WriteLine($"{key.Name} ({key.Camelot})");              // "F#m (11A)"
if (t.Waveform is { } wave)
    Console.WriteLine($"{wave.Length} points, one per {WaveformPoint.Interval.TotalMilliseconds} ms");
```

`options` may be `NULL` (.NET: leave it out) for the defaults. The .NET `TrackAnalysis` is a plain object with everything copied out of the native result, so there is nothing to dispose; in C, free the result with `fa_analysis_free`.

To analyse the file or memory of a stream that is already open, without touching its playback, use `fa_analyze_stream` (.NET `stream.Analyze()` and `stream.AnalyzeAsync()`). It reads with its own decoder, and MIDI files use the stream's soundfont.

## Reading the results

Each getter returns `FA_ERROR_NOT_FOUND` when its kind was not asked for, or when nothing was found: no steady beat (silence, a few seconds of speech), or no key (silence, drums only). .NET gives `null` (`Beats`, `Key`, `Waveform`, `Loudness`) in the same cases. `fa_analysis_get_info` says which kinds the result holds, the sample rate of its frames, the length analysed, and whether it came from the cache.

### Beat grid

| Field | Meaning |
|---|---|
| `bpm` | The main tempo: the one that lasts longest. A double: the grid is fitted to the beats, so on steady music it is finer than 0.05 BPM |
| `first_beat` | Source frame of the first beat heard. The grid goes on backwards from it, at the first segment's tempo |
| `sample_rate` | Of the source frames (`first_beat` and the segments) |
| `beats_per_bar`, `bar_offset` | 4 and 0, unless the grid says it knows (`FA_GRID_BARS`). `bar_offset` is the number of the beat that starts a bar, counting `first_beat` as 0 |
| `confidence` | 0 to 1; 1 for exact grids. See [Accuracy](#accuracy) |
| `flags` | `FA_GRID_CONSTANT` (one tempo), `FA_GRID_EXACT` (from the file), `FA_GRID_EDITED` (set by your app), `FA_GRID_BARS` (bars known) |
| `segment_count` | Tempo segments; 1 is a constant tempo |

The tempo map, `fa_analysis_get_tempo_map`, is a list of `fa_tempo_segment`: the source frame where a stretch starts, its BPM, and the beat number at that frame. Beats count from `first_beat`, which is beat 0, and are not whole where a tempo change falls between beats. A constant grid is one segment starting at `first_beat` with beat 0.

To turn a beat number into a frame, find the last segment whose `beat` is at or before it (the first segment stands in before that) and add the distance at that segment's tempo:

```
frame = segment.start + (beat - segment.beat) * 60 * sample_rate / segment.bpm
```

The .NET `BeatGrid` does this for you, and the other way round:

```csharp
long frame   = grid.BeatToFrame(16);              // where beat 16 falls
double beat  = grid.FrameToBeat(position);        // negative before the first beat, fractional between beats
long nextBar = grid.BeatToFrame(Math.Ceiling(grid.FrameToBeat(position) / grid.BeatsPerBar) * grid.BeatsPerBar);
```

(The last line is right while `BarOffset` is 0; add the offset otherwise.)

### Key

`fa_key_info` has `tonic` (0 = C, 1 = C#/Db, up to 11 = B), `minor` (1 or 0), a `confidence`, the usual name of the key (`C`, `F#m`, `Bb`, `Ebm`) and the Camelot code, the notation DJ software uses for harmonic mixing: C major is `8B`, each fifth up adds one, and minor keys end in `A` (A minor is `8A`). Keys that mix well have the same number or numbers one apart.

A key change, such as a karaoke singer's pitch shift, moves the key. `fa_key_transpose` gives the key after a shift of some semitones, with the name and Camelot code to match and the confidence kept. `fa_key_make` builds a key from a tonic and mode (for keys stored in your library), with confidence 1.

C:
```c
fa_stream_set_param(song, FA_PARAM_PITCH, 2.0f);
fa_key_info shifted;
fa_key_transpose(&key, 2, &shifted);          /* Am (8A) up two semitones: Bm (10A) */
```

C#:
```csharp
song.Pitch = 2;
var shifted = t.Key!.Transpose(2);            // Am (8A) up two semitones: Bm (10A)
var fromLibrary = MusicalKey.FromTonic(9, minor: true);   // Am, confidence 1
```

### Waveform

One `fa_waveform_point` per 10 ms of audio; point *i* covers the 10 ms from *i* × 10 ms. All levels are linear, 0 to 1.

| Field | Meaning |
|---|---|
| `peak` | The highest sample of any channel |
| `low` | RMS level below 250 Hz (a sine at full scale reads 1) |
| `mid` | RMS level from 250 Hz to 2.5 kHz |
| `high` | RMS level above 2.5 kHz |

`peak` gives the outline of a waveform and the three bands give its colour (bass in one colour, vocals in another, hats in a third), as DJ software draws them. A 4-minute song has 24,000 points, 384 KB. To fit a window, take the maximum `peak` over each group of points when you draw. In .NET, `WaveformPoint` has `Peak`, `Low`, `Mid` and `High`, and `WaveformPoint.Interval` is the 10 ms.

### Loudness

`fa_analysis_get_loudness` returns the same `fa_loudness` as `fa_scan_loudness_path`, so a library import can get loudness, tempo and key in one pass. `gain_db` is what normalization would apply with the engine's settings at the time of the analysis. In .NET it is `t.Loudness`.

## Using a grid on a stream

A deck needs the grid on the stream, for sync, beat loops and beats aligned across two songs. `fa_stream_set_beat_grid` gives a stream a grid: one from an analysis, one the user corrected, or one from your own library. `fa_stream_get_beat_grid` reads back the one set last.

C:
```c
fa_beat_grid grid;
fa_analysis_get_beat_grid(a, &grid);

uint32_t count = 0;
fa_analysis_get_tempo_map(a, NULL, 0, &count);
fa_tempo_segment* segs = malloc(count * sizeof *segs);
fa_analysis_get_tempo_map(a, segs, count, &count);

fa_stream_set_beat_grid(song, &grid, segs, count);
free(segs);
```

C#:
```csharp
song.BeatGrid = t.Beats;                      // null removes it
```

With `count` 0 the grid is constant: one segment at `first_beat`, beat 0. Otherwise `segments` are the tempo map, with rising starts and tempos from 0.1 to 100,000 (a module at speed 1 can run past 1000 BPM), and `grid->segment_count` is ignored. Frames are in the grid's `sample_rate`. The flags are kept as given, except `FA_GRID_CONSTANT`, which is set when there is one segment and cleared otherwise.

`fa_stream_get_beat_grid` returns the grid set last; else, for a MIDI file or tracker module, the exact one from the file; else `FA_ERROR_NOT_FOUND` (.NET: `null`). Setting `NULL` (.NET `BeatGrid = null`) removes the stream's grid, and on a MIDI or tracker stream that brings back the file's own.

### Correcting a grid

An analysis can be wrong, and every DJ tool lets the user fix it. The usual mistake is half or double tempo. Set a new grid with the same first beat and the tempo times 2 or divided by 2.

C:
```c
grid.bpm *= 2.0;                              /* the tempo was read at half speed */
grid.flags = FA_GRID_CONSTANT | FA_GRID_EDITED;
fa_stream_set_beat_grid(song, &grid, NULL, 0);
```

C#:
```csharp
var g = t.Beats!;
song.BeatGrid = BeatGrid.Constant(g.Bpm * 2, g.FirstBeat, g.SampleRate);   // or g.Bpm / 2
```

When halving, the first beat may now sit between two beats: add one beat of the old tempo (`60 * sample_rate / old_bpm` frames) to `first_beat`. A user who drags the grid to line it up with the audio changes `first_beat` the same way. A corrected grid is yours to keep: store the tempo and first beat in your library and set them again next time.

## Exact grids from MIDI files and tracker modules

A MIDI file holds its tempo changes and time signature, and a tracker module holds its speed, tempo and rows per beat. For both, FatzAudio reads the grid from the file instead of listening for it. It is flagged `FA_GRID_EXACT`, has confidence 1, and is never folded into the preferred tempo range.

- **Tempo changes are segments.** A song that changes tempo gets one segment per tempo; `bpm` is the one that lasts longest, and beat numbers carry on across the changes.
- **Bars** are known for MIDI files with one time signature (`FA_GRID_BARS`, with `beats_per_bar` from the signature and `bar_offset` 0). Modules have no time signature, so their grids say 4 and 0 without the flag.
- **MIDI** counts 120 BPM until the file's first tempo event, as the standard says, and `first_beat` is frame 0.
- For a module, the first `fa_stream_get_beat_grid` renders the song once without sound, about 0.25 s for a 4-minute song. On a DJ deck that render runs in the background (see [DJ mixer sync](dj-mixer.md#sync-and-beat-loops)); `fa_stream_get_beat_grid` waits for it.

C:
```c
fa_stream_open_path(engine, "song.mid", NULL, &song);
fa_beat_grid grid;
uint32_t count = 0;
fa_stream_get_beat_grid(song, &grid, NULL, 0, &count);           /* the exact grid, no analysis */
printf("%.2f BPM, %u segment(s), exact: %d\n", grid.bpm, count, (grid.flags & FA_GRID_EXACT) != 0);
```

C#:
```csharp
using var song = engine.OpenFile("song.mid");
if (song.BeatGrid is { } grid)                                    // exact, from the file
    Console.WriteLine($"{grid.Bpm:F2} BPM, {grid.Segments.Count} segment(s), exact: {grid.Flags.HasFlag(BeatGridFlags.Exact)}");
```

`fa_analyze_path` on a MIDI file or module gives the same exact grid in its result. If you also ask for the key or the waveform of a MIDI file, the file is rendered with the engine's soundfont (else the OS's instruments), as playback does, and that audio is analysed.

## Tempo range

Tempo is ambiguous by a factor of two: a song at 90 BPM can be felt at 180, and a half-time groove at 70. The analysis settles it with a preferred range, `min_bpm` to `max_bpm`, default **70 to 180**. A measured tempo outside it is doubled or halved into it ("folded"). A BPM tag in the file, when it is within 4% of the half, the same or the double of the measured tempo, picks between them.

```c
opt.min_bpm = 80.0f;
opt.max_bpm = 160.0f;
```

```csharp
var t = engine.Analyze("song.mp3", AnalysisKinds.Beats, new AnalysisOptions { MinBpm = 80, MaxBpm = 160 });
```

`max_bpm` must be at least 1.9 times `min_bpm`, so that every tempo has a place in the range. `min_bpm` must also be 30 or more and `max_bpm` 400 or less. Anything else returns `FA_ERROR_INVALID_ARG`. Pick a range for your music: a drum-and-bass library wants something like 120 to 220, a ballad library 55 to 110.

## The cache

With `fa_engine_config.cache_directory` set (.NET `AudioEngineOptions.CacheDirectory`), results are stored in that directory next to the loudness scans, one small file per song and audio track. The key is the file's content, as for [loudness](normalization.md#why-the-scan-is-fast-even-on-large-video-files), so a renamed or moved file is still found. For a MIDI file the soundfont and rate are part of the key, and for a module the rate.

- The file holds the **union of the kinds measured so far**. Asking for beats, then the key, then the waveform measures only what is new and keeps what was there, so asking one at a time does not repeat work.
- A result read from the cache is flagged `FA_ANALYSIS_FROM_CACHE` (.NET `FromCache`).
- A **different BPM range** measures a non-exact grid again, because the folded tempo may change. Exact grids are not folded and stay.
- `FA_ANALYZE_NO_CACHE` (.NET `AnalysisOptions.NoCache`) neither reads nor writes the cache.
- When a later version changes a measurement, older entries are ignored and the song is measured again.
- Loudness measured by an analysis is also written to the loudness cache, so normalization finds it.

Without a cache directory every call measures again. For a library, set one: a rescan then costs a file read.

## Errors

| Error | When |
|---|---|
| `FA_ERROR_NOT_FOUND` | `fa_analyze_path`: the file is missing. A getter: the kind was not asked for, or nothing was found (see above) |
| `FA_ERROR_UNSUPPORTED_FORMAT` | No decoder reads the file |
| `FA_ERROR_NOT_SEEKABLE` | `fa_analyze_stream` on a web, push or app-I/O stream, which cannot be read twice. Analyse the file instead |
| `FA_ERROR_INVALID_ARG` | Unknown kinds or flags, or a tempo range that is not allowed |

## Threads

- `fa_analyze_path` and `fa_analyze_stream` **block** the calling thread until the analysis is done, about 1.5 s for a 4-minute song. Call them from a worker thread. Several can run at once, so a library import can analyse a file per core.
- .NET: `AnalyzeAsync` runs `Analyze` on a pool thread. Disposing the engine or the stream while it runs is safe: the analysis keeps the native object alive until it is done. A `CancellationToken` cancels the **wait**; the native work cannot be interrupted and runs to its end.
- An analysis never touches playback, and nothing waits for one. It does not use the audio or decode threads.
- `fa_stream_set_beat_grid` and `fa_stream_get_beat_grid` can be called from any thread.

## How it works

All of it is written for FatzAudio from published methods; no analysis library is built in.

**Tempo and beats** (`src/beat.c`):

1. Onset strength: the change of the log-magnitude spectrum from one FFT frame to the next (1024 points every 256 samples at 44.1 and 48 kHz). Two envelopes are kept, one over everything up to 11 kHz and one below 200 Hz where kicks are, and summed.
2. The envelope minus its local mean (half a second), rectified.
3. Tempo candidates: the autocorrelation of the envelope at one, two, three and four beat periods, weighted 1, 1/2, 1/3 and 1/4 (a comb), times a preference for tempos around 120 BPM, searched in 0.05 BPM steps inside the preferred range. The three best peaks are tracked and the one whose grid lands on the strongest onsets wins.
4. Half or double: when a tempo and its double are both in range, the faster is tracked, and the slower is kept when every other beat is clearly weaker. A BPM tag helps decide.
5. Beats: dynamic-programming beat tracking (Ellis 2007), each beat refined to a fraction of a frame on its onset peak. Weak beats at the ends are dropped.
6. The constant grid: a robust straight-line fit of beat time over beat number, with outliers dropped and the line fitted again. This gives the tempo far more finely than the envelope's frames. The confidence is the share of beats on the grid, squared, times how much the grid's onsets stand out.

**Key** (`src/key.c`): every half window (an FFT of about 0.17 s) the spectral peaks from 55 Hz to 5 kHz are placed on a pitch circle of 120 steps (10 cents each), weighted by magnitude, with each frame counting equally so loud passages do not outvote quiet ones. The circle gives the tuning (songs are often a little sharp or flat) and is folded into the 12 pitch classes. The key is the one of the 24 whose Krumhansl-Kessler profile correlates best with that chroma. The confidence grows with the correlation and with its lead over the runner-up.

**Waveform**: peak and RMS per 10 ms, with the three bands split by filters at 250 Hz and 2.5 kHz. **Loudness** is the BS.1770 meter that normalization uses.

## Accuracy

Accuracy was measured on **48 General MIDI songs** made by the test suite. Each has a chord progression (piano, bass and strings) over drums, is about 100 s long, is rendered to audio with the TimGM6mb SoundFont and then analysed as audio, so the right answer is known from the MIDI file. Tempos are random from 70 to 180 BPM and the songs go through all 24 keys. There are four drum styles, 12 songs each:

- **four on the floor**: a kick on every beat.
- **rock**: kick on beats 1 and 3, snare on 2 and 4.
- **half-time**: a kick on beat 1 and a snare on beat 3 only.
- **humanized, drifting**: rock with each kick and snare up to 12 ms early or late, and the tempo rising by 1% over the song.

All have eighth-note hi-hats.

| Style | Songs | Tempo within 0.05 BPM | Of those, half or double | Worst tempo error (BPM) | Worst beat error (ms) | Mean confidence |
|---|---|---|---|---|---|---|
| four on the floor | 12 | 12 | 0 | 0.000 | 1.7 | 1.00 |
| rock | 12 | 12 | 0 | 0.000 | 1.4 | 1.00 |
| half-time | 12 | 12 | 4 | 0.000 | 2.0 | 1.00 |
| humanized, drifting | 12 | 12 | 3 | 0.038 | 359.1 | 0.38 |

**Key:** 48 of 48 exactly right (0 relative, 0 a fifth away, 0 parallel, 0 other).

**Speed:** 4791 s of audio in 27.8 s, which is **172 times real time on one core** for beats, key and waveform together. A 4-minute song takes about 1.4 s.

What this says, and what it does not:

- **Steady, quantized music is read very well.** Tempo within 0.05 BPM and beats within 2 ms, with confidence 1. That is what most pop, dance and karaoke backing tracks are like.
- **Half and double are counted apart.** In 4 of the half-time songs and 3 of the humanized ones the analysis found half or double the tempo of the file. That is a reasonable reading of the same music (a half-time groove can be counted either way), but it is not the file's tempo. The table measures those songs against the tempo found, and the per-song table below shows the tempo each was compared with. This is what the preferred range and the user's correction ([above](#correcting-a-grid)) are for.
- **Drifting songs are not followed.** The grid is one constant tempo, and a drummer who wanders or a song that speeds up leaves it behind: beat errors up to 359 ms in this set, with a low confidence (0.24 to 0.67) that says so. The average tempo is still right (within 0.04 BPM here). Following a drifting tempo needs a tempo map measured from audio, which is not done yet (see [Limits](#limits)).
- **These songs are synthetic.** Clean drums, one tempo, one key, one set of General MIDI instruments. Real recordings are harder: dense mixes, swing, tempo that breathes, modulations, unclear keys. Expect them to do worse, **especially the key**. Published key finders of this kind are typically right roughly 6 or 7 times in 10 on real pop, with most misses the relative major or minor, or a fifth away. The 48 of 48 here says the method works, not that your library will do as well. Show the Camelot code, let the user correct it, and treat a key as a good suggestion for mixing rather than a fact.
- Use `confidence` to decide when to trust a grid, for example before a long locked overlap.

To run the report yourself, build the library and run the test with a General MIDI SoundFont:

```sh
FA_ANALYSIS_REPORT_SF2=/path/to/a/GM.sf2 build/bin/fa_test_analysis
FA_ANALYSIS_REPORT_SF2=/path/to/a/GM.sf2 FA_REPORT_ONLY=12 build/bin/fa_test_analysis   # one song
```

The songs come from a fixed random sequence, so they are the same on every run. The tables are printed as Markdown.

## Limits

- **Constant grids only from audio.** There is no tempo map measured from audio, so a song that changes tempo, or a band that drifts, gets one average tempo and a lower confidence. MIDI and tracker grids do have tempo maps, and `fa_stream_set_beat_grid` takes one from your app.
- **No bar detection from audio.** `beats_per_bar` is 4 and `bar_offset` 0 without `FA_GRID_BARS`: beat 0, the first beat heard, is taken as a bar start, and it may not be one. Use bars only if the user has confirmed them.
- **One key per song.** A song that modulates gets its main key.
- Audio that is not music (speech, silence) has no grid or key, and the getters say so.
- Tempo maps from audio, bar detection and bar sync, beat jump and quantized seeks, and automatic key matching in an auto-mix are on the list of later work (section 19 of the [DJ mixer plan](../requests/2026-10-23_FatzAudio_Plan_DjMixer.md)).

## Per-song results

<details>
<summary>All 48 songs of the accuracy report</summary>

"BPM (file)" is the tempo the result was compared with: the file's tempo, or half or double of it where the analysis found that (the 7 songs counted as half or double above). "Key match" says how the found key relates to the file's key.

| # | Style | BPM (file) | Found | Error (BPM) | Beat error (ms) | Confidence | Key | Found | Key match |
|---|---|---|---|---|---|---|---|---|---|
| 1 | four on the floor | 72.20 | 72.20 | -0.000 | 1.4 | 1.00 | C | C | exact |
| 2 | rock | 71.80 | 71.80 | 0.000 | 1.2 | 1.00 | Db | Db | exact |
| 3 | half-time | 129.70 | 129.70 | -0.000 | 2.0 | 1.00 | D | D | exact |
| 4 | humanized, drifting | 70.22 | 70.22 | -0.000 | 359.1 | 0.63 | Eb | Eb | exact |
| 5 | four on the floor | 170.10 | 170.10 | -0.000 | 1.5 | 1.00 | E | E | exact |
| 6 | rock | 82.40 | 82.40 | 0.000 | 1.2 | 1.00 | F | F | exact |
| 7 | half-time | 124.50 | 124.50 | -0.000 | 1.8 | 1.00 | F# | F# | exact |
| 8 | humanized, drifting | 130.90 | 130.90 | 0.001 | 83.1 | 0.33 | G | G | exact |
| 9 | four on the floor | 135.60 | 135.60 | -0.000 | 1.7 | 1.00 | Ab | Ab | exact |
| 10 | rock | 156.20 | 156.20 | -0.000 | 1.4 | 1.00 | A | A | exact |
| 11 | half-time | 74.40 | 74.40 | -0.000 | 1.7 | 1.00 | Bb | Bb | exact |
| 12 | humanized, drifting | 76.25 | 76.24 | -0.003 | 331.1 | 0.67 | B | B | exact |
| 13 | four on the floor | 176.20 | 176.20 | 0.000 | 1.5 | 1.00 | Cm | Cm | exact |
| 14 | rock | 78.10 | 78.10 | -0.000 | 1.2 | 1.00 | C#m | C#m | exact |
| 15 | half-time | 73.50 | 73.50 | 0.000 | 1.0 | 1.00 | Dm | Dm | exact |
| 16 | humanized, drifting | 121.15 | 121.16 | 0.013 | 96.1 | 0.34 | Ebm | Ebm | exact |
| 17 | four on the floor | 113.60 | 113.60 | 0.000 | 1.6 | 1.00 | Em | Em | exact |
| 18 | rock | 175.50 | 175.50 | 0.000 | 1.3 | 1.00 | Fm | Fm | exact |
| 19 | half-time | 114.70 | 114.70 | -0.000 | 1.7 | 1.00 | F#m | F#m | exact |
| 20 | humanized, drifting | 87.80 | 87.80 | -0.000 | 287.1 | 0.54 | Gm | Gm | exact |
| 21 | four on the floor | 179.30 | 179.30 | 0.000 | 1.7 | 1.00 | G#m | G#m | exact |
| 22 | rock | 143.00 | 143.00 | 0.000 | 1.4 | 1.00 | Am | Am | exact |
| 23 | half-time | 116.50 | 116.50 | 0.000 | 1.7 | 1.00 | Bbm | Bbm | exact |
| 24 | humanized, drifting | 125.57 | 125.56 | -0.009 | 84.6 | 0.26 | Bm | Bm | exact |
| 25 | four on the floor | 101.60 | 101.60 | 0.000 | 1.4 | 1.00 | C | C | exact |
| 26 | rock | 116.60 | 116.60 | -0.000 | 1.3 | 1.00 | Db | Db | exact |
| 27 | half-time | 72.15 | 72.15 | 0.000 | 1.1 | 1.00 | D | D | exact |
| 28 | humanized, drifting | 87.50 | 87.49 | -0.004 | 124.9 | 0.24 | Eb | Eb | exact |
| 29 | four on the floor | 79.80 | 79.80 | -0.000 | 1.4 | 1.00 | E | E | exact |
| 30 | rock | 163.80 | 163.80 | 0.000 | 1.3 | 1.00 | F | F | exact |
| 31 | half-time | 104.30 | 104.30 | -0.000 | 1.8 | 1.00 | F# | F# | exact |
| 32 | humanized, drifting | 71.12 | 71.13 | 0.003 | 158.4 | 0.28 | G | G | exact |
| 33 | four on the floor | 120.90 | 120.90 | -0.000 | 1.7 | 1.00 | Ab | Ab | exact |
| 34 | rock | 102.10 | 102.10 | 0.000 | 1.3 | 1.00 | A | A | exact |
| 35 | half-time | 133.00 | 133.00 | 0.000 | 1.7 | 1.00 | Bb | Bb | exact |
| 36 | humanized, drifting | 176.70 | 176.74 | 0.038 | 68.5 | 0.37 | B | B | exact |
| 37 | four on the floor | 71.70 | 71.70 | 0.000 | 1.4 | 1.00 | Cm | Cm | exact |
| 38 | rock | 155.80 | 155.80 | -0.000 | 1.3 | 1.00 | C#m | C#m | exact |
| 39 | half-time | 103.20 | 103.20 | 0.000 | 2.0 | 1.00 | Dm | Dm | exact |
| 40 | humanized, drifting | 105.78 | 105.78 | -0.006 | 102.0 | 0.26 | Ebm | Ebm | exact |
| 41 | four on the floor | 175.00 | 175.00 | -0.000 | 1.5 | 1.00 | Em | Em | exact |
| 42 | rock | 130.20 | 130.20 | 0.000 | 1.2 | 1.00 | Fm | Fm | exact |
| 43 | half-time | 71.70 | 71.70 | 0.000 | 1.1 | 1.00 | F#m | F#m | exact |
| 44 | humanized, drifting | 142.95 | 142.97 | 0.017 | 80.5 | 0.31 | Gm | Gm | exact |
| 45 | four on the floor | 138.60 | 138.60 | 0.000 | 1.5 | 1.00 | G#m | G#m | exact |
| 46 | rock | 87.70 | 87.70 | -0.000 | 1.3 | 1.00 | Am | Am | exact |
| 47 | half-time | 86.10 | 86.10 | -0.000 | 1.4 | 1.00 | Bbm | Bbm | exact |
| 48 | humanized, drifting | 155.11 | 155.12 | 0.013 | 72.6 | 0.33 | Bm | Bm | exact |

</details>
