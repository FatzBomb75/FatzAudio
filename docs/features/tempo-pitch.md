# Tempo, pitch and rate

Three parameters change how a stream sounds in time and key. They work on every stream, playing or decode-only, with no wrapper stream to create, and they combine freely.

| Parameter | Range | What it does | .NET |
|---|---|---|---|
| `FA_PARAM_TEMPO` | 0.1 … 10 (1 = normal) | Speed, with the pitch kept | `stream.Tempo` |
| `FA_PARAM_PITCH` | -36 … 36 semitones (0 = normal) | Key, with the speed kept; fractions are cents (0.5 = 50 cents) | `stream.Pitch` |
| `FA_PARAM_RATE` | 0.1 … 10 (1 = normal) | Speed and pitch together, like a turntable or tape | `stream.Rate` |

The speed you hear is `TEMPO × RATE`; the key moves by `PITCH` semitones plus whatever `RATE` adds. Values out of range are clamped. Zero, negative or NaN multipliers are refused with `FA_ERROR_INVALID_ARG`.

## C

```c
fa_stream_set_param(s, FA_PARAM_TEMPO, 0.85f);   /* practice at 85 % speed, same key */
fa_stream_set_param(s, FA_PARAM_PITCH, -2.0f);   /* karaoke: two semitones down */
fa_stream_set_param(s, FA_PARAM_RATE, 1.02f);    /* DJ-style pitch fader: +2 % speed and pitch */

/* Glide the tempo back to normal over 4 s; FA_EVENT_RAMP_DONE fires as usual. */
fa_ramp ramp = { FA_CURVE_SCURVE };
fa_stream_ramp_param(s, FA_PARAM_TEMPO, 1.0f, 4000000000LL, 0, &ramp);

/* Keep voices natural when changing key, and choose how quickly changes are heard. */
fa_tempo_options o;
fa_tempo_options_init(&o);
o.keep_formants = 1;
o.latency_ms = 100;
fa_stream_set_tempo_options(s, &o);
```

## C#

```csharp
song.Tempo = 0.85f;
song.Pitch = -2f;
song.Rate = 1.02f;

await song.RampAsync(AudioParameter.Tempo, 1f, TimeSpan.FromSeconds(4), Curve.SCurve);

song.TempoOptions = new TempoOptions { KeepFormants = true, Latency = TimeSpan.FromMilliseconds(100) };
```

## Options

| `fa_tempo_options` | .NET `TempoOptions` | Default | Meaning |
|---|---|---|---|
| `quality` | `Quality` | `FA_STRETCH_BEST` | `FA_STRETCH_FAST` uses shorter analysis blocks: about a third less CPU, slightly less clean. Best is the right choice on any desktop. `FA_STRETCH_TRANSIENTS` (.NET `Transients`) is Best plus transient preservation (below): for drum-heavy material |
| `keep_formants` | `KeepFormants` | off | A pitch change keeps the character of voices, so a key change for a singer doesn't sound like a chipmunk or a giant. Affects `PITCH` only |
| `latency_ms` | `Latency` | 150 | How far ahead the stream is decoded while any of the three is in use, so how soon a change is heard. Clamped to 50 … the stream's `buffer_ms` |

`fa_stream_set_tempo_options` (`fa_stream_get_tempo_options`) takes effect at once; `NULL` (.NET `null`) restores the defaults. Changing `quality` on a playing stream restarts the processing from the position being heard, like a seek to the same place; on a stream that can't seek (live web streams, push streams) the new quality applies after the next stop, seek or attach.

## How it behaves

- **Nothing changes until you use it.** A stream whose parameters are all neutral passes its audio through untouched, bit for bit, with no extra CPU or latency. The processing starts the first time a parameter leaves its neutral value, and from then on stays in the chain until the stream is seeked, stopped or attached elsewhere, so a slider moving through 1.0 never clicks. When it starts mid-song it fades in from the plain audio over about 5 ms.
- **When a change is heard.** The parameters are applied where the stream is decoded, on a worker thread, so a change is heard after the decode-ahead buffer. While any of them is in use that buffer is kept to `latency_ms` (150 ms by default). The first change on a playing file re-renders from the position being heard, so even that first change arrives within the latency, not after the whole normal buffer.
- **Ramps** work like volume ramps ([ramps](ramps.md)): any curve, scheduled starts, exactly one `FA_EVENT_RAMP_DONE`, and the duration is playback time. The value is updated every 512 frames (about 11 ms), which sounds continuous. Like a plain change, a ramp is heard the latency after it is applied. On a stream playing on a device, its `FA_EVENT_RAMP_DONE` comes when its end is heard (within one device period), like a volume ramp's; the event's `frame` is the source position heard then. A detach raises the RAMP_DONE of a ramp whose end was decoded but not heard yet at once; a seek keeps it for the same playing time on the new position. On a decode-only stream it comes when the end is read.
- **Positions stay in the song.** `fa_stream_get_position`, markers, the cue-in/cue-out loop, the mix point of a queue, `fa_stream_get_clock` and CD+G graphics all count source frames, and follow the tempo by themselves. A track at tempo 2 still reports 1:00 when the music at 1:00 is heard, a crossfade still starts at the mix point, and the lyrics stay on the words.
- **Lengths.** The output of a decode-only stream (`fa_stream_read`) is exactly `source / (tempo × rate)` frames long, and starts with the first sample of the source: nothing is cut from either end.
- **Push and packet streams** (`fa_stream_open_push`, `fa_stream_open_packets`, your app's FFmpeg audio) take `TEMPO`, `RATE` and `PITCH` like any stream. `fa_stream_get_clock` still reports your timestamps, and runs at the speed heard: at tempo 1.25 it advances 1.25 s of your timeline per second, so a video player following it plays the video at 1.25× and stays in sync. Jumps in your timestamps (after `fa_stream_push_flush`, or a gap) still show up when their audio is heard. Push the audio as fast as it is used: at tempo 2 that is twice as fast. `fa_stream_get_buffered` counts the queue in your timeline's time and the decoded part in playback time.
- **Buses** have no tempo, pitch or rate (`FA_ERROR_INVALID_ARG`); set them on the streams.
- **Threads.** All processing runs on the decode workers (or the thread calling `fa_stream_read`), never on the audio callback. A 48 kHz stereo stream processes at about 70× real time with tempo or rate changed and about 30× with pitch changed (one core, Linux x64), so dozens of streams can be processed at once.

## How it works and how good it is

Tempo uses a phase vocoder with phase locking and transient handling ([Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch), MIT, built into the library). Pitch is done by stretching the time and then resampling with FatzAudio's own windowed-sinc resampler (Kaiser window, 96 taps, about -90 dB aliasing), so the key lands exactly where you ask: 0.00 cents of error at any setting. Rate is the same resampler on its own.

Measured with `tools/tempo-compare` against bass_fx 2.4 (the legacy app's DLL, through its public API), SoundTouch 2.3.2 (`soundstretch`) and Rubber Band 3.3 on the same 48 kHz test signals. Higher dB is cleaner (signal against everything that should not be there); the full table is in the tool's report.

| Test | FatzAudio | bass_fx | SoundTouch | Rubber Band |
|---|---|---|---|---|
| Sine, tempo 0.8 … 1.25 or pitch ±3 … -12 | 59 … 66 dB | 35 … 48 dB | 38 … 58 dB | 64 … 68 dB |
| Chord (C major), tempo or pitch | 49 … 57 dB | 21 … 32 dB | 21 … 29 dB | 36 … 62 dB |
| Harmonic tone (held vowel), tempo or pitch | 57 … 67 dB | 34 … 38 dB | 36 … 42 dB | 55 … 61 dB |
| Pitch error | 0.00 cents | up to 0.45 cents | up to 0.27 cents | 0.00 cents |
| Drum hits moved from their time | 0.1 … 0.6 ms | 2.4 … 24 ms | 3.8 … 12 ms | 4 … 6 ms |
| Pre-echo before drum hits | -24 … -28 dB | none | none | -9 … -13 dB |

In practice: sustained notes and chords stay steady instead of warbling or phasing, and beats stay on time. The trade-off of any phase vocoder is a faint smear just before sharp hits (the pre-echo row); bass_fx and SoundTouch avoid it by splicing whole chunks of audio, which is why their hits drift and their chords phase.

### Transient mode

`FA_STRETCH_TRANSIENTS` removes that smear. It finds onsets in the audio (a jump in short-term energy: drum hits, plucks, hard consonants) and, from 24 ms before each onset to 12 ms after it, plays the original audio at its own speed instead of the stretched sound, lined up so that the onset lands exactly where it belongs, with 4 ms crossfades on each side. That includes a hit in the last moments of a track. Measured on the same drum hits:

| Test (tempo 0.8, 1.25, pitch -3) | Best | Transients |
|---|---|---|
| Pre-echo before drum hits | -24 … -28 dB | -48 … -52 dB (none at pitch -3) |
| Drum hits moved from their time | 0.1 … 0.6 ms | 0.1 … 0.5 ms |
| Sine, chord, held vowel | as above | identical (no onsets, nothing changed) |

The cost is that those 40 ms around an onset are not stretched. When a note starts under a held chord, the chord can show a faint seam there, because the two crossfaded versions of it don't line up exactly. That is why it is an option and not the default. It adds no latency and next to no CPU. With `keep_formants` and a pitch change, onsets are left to the stretcher (the original audio would not have the corrected formants). A new `quality` applies from the next restart of the processing, like the other qualities.

## Comparing by ear against bass_fx

On Windows, from a build with the tools on (`FA_BUILD_TOOLS`, the default):

```bat
cmake -S . -B build
cmake --build build --config Release
pip install numpy
python tools\tempo-compare\compare.py --fa build\bin\Release\fa_tempo_render.exe ^
    --bassfx build\bin\Release\bassfx_render.exe --bass-dir C:\path\to\legacy --out tempo-results
```

`--bass-dir` is the folder holding your 64-bit `bass.dll` and `bass_fx.dll`. The script writes `tempo-results\report.md` and every render as a WAV in `tempo-results\renders`, named by signal, tempo, pitch and engine, so you can play them side by side. To hear your own music, render the same file through both:

```bat
build\bin\Release\fa_tempo_render.exe song.mp3 fa.wav --tempo 0.8 --pitch -2
build\bin\Release\bassfx_render.exe C:\path\to\legacy song.mp3 bassfx.wav --tempo 0.8 --pitch -2
```

Both take `--tempo` (multiplier), `--pitch` (semitones) and `--rate` (multiplier); `fa_tempo_render` also takes `--formants` and `--fast`. Piano, held vocals and acoustic guitar show the difference most. See [tools/tempo-compare](../../tools/tempo-compare/README.md).

## Reverse playback

`fa_stream_set_reverse` / `fa_stream_get_reverse` (`AudioStream.Reverse` in .NET) plays a stream backwards, as bass_fx's `BASS_FX_ReverseCreate` with `BASS_ATTRIB_REVERSE_DIR` does, but on the stream itself: there is no second stream to make. It can be switched at any time, also while playing, and the turn is instant: playing or paused, the stream turns at the frame being mixed, and the last 200 ms it played go back out the other way straight from its buffer while the decoder starts on the new direction behind them, so there is no gap and no re-buffer (the DJ "instant reverse"). A stopped or ended stream starts over at the position, the way a seek does.

```c
fa_stream_set_reverse(deck, 1);                  /* backwards from here */
fa_stream_set_reverse(deck, 0);                  /* forwards again from where it got to */
```

```csharp
deck.Reverse = true;
deck.Reverse = false;
```

Reversed:

- The position and the clock count down, so a progress bar, lyrics display or CD+G screen that follows them goes backwards too.
- The stream ends (`FA_EVENT_ENDED`) at the cue-in marker, or the start. Playing it from stopped or ended starts at the cue-out marker, or the end, so a stream reversed before its first play plays the whole song backwards.
- A loop, `fa_stream_set_loop` or a deck loop, wraps from its start back to its end.
- Tempo, pitch, rate, volume, the equalizer, DSPs, normalization and the fade markers work as they do forward. The output is exactly the forward audio, last frame first.
- [Events](events.md) are raised as the position counts down past their frames: markers, loop turns (`FA_EVENT_LOOPED`, from the loop's start to its end), position events, MIDI texts and tracker rows, so they come last first. The cue-out marker doesn't end the stream and a mix point doesn't start the queued stream; a queued stream starts when this one ends, without a crossfade.
- On a DJ deck, sync, deck loops and beat jumps work as forward, mirrored: sync pulls the deck onto the leader's beat with its beats counting down (a jump on its next beat down when the error is large), a deck loop turns at its in point back up to its out point, and loop changes and beat jumps join without a gap. A beat loop set while reversed ends at the beat heard and lies below it. With [slip mode](dj-mixer.md#slip-mode) on, turning back forward returns to where the deck would have been.

The turn itself mirrors the audio: the frame heard last plays again and the samples go back the way they came, so a turn adds no step of its own. Where the 200 ms run out the decoder takes over at the frame they reached; at TEMPO, PITCH and RATE 1 that join is exact, while through the stretcher it is a cut like a loop's join (the stretcher starts over there). The history stops early where the audio heard came from elsewhere: right after a loop turned, a jump or a seek, the turn plays back only what was heard since, and then the loop (reversed: from its in point up to its out point) or the decoder carries on.

The source is decoded forward in half-second blocks, each after a seek, and handed out last frame first, so reverse needs a seekable source and costs one seek per half second. Each block is decoded 10 ms past its end and cross-faded there with the block above (linearly, from the block above to the one below), because the audio right after a seek is not always what playing through gives. Decoders that seek to the exact frame join the blocks exactly (tested with PCM, FLAC in four containers, MP3 and tracker modules): the fade then mixes identical audio and leaves it as it is. Where a seek is not exact the fade hides the join: AAC's noise-filled bands and SBR noise floor (a different stretch of noise after a seek, [AAC](streams.md#aac)), AAC Main and LTP prediction, and MIDI notes held across a block start, which a seek strikes again (with effects on, a second before it is heard). The blocks themselves keep that difference: a MIDI chord held across several blocks is struck again in each one, so it swells every half second. Push, packet and forward-only streams return `FA_ERROR_NOT_SEEKABLE`, and web streams `FA_ERROR_NOT_IMPLEMENTED`, since each block would be a new range request.

## Pitfalls

- `RATE` changes the key. For a speed change that keeps the key use `TEMPO`; for a key change that keeps the speed use `PITCH`.
- `fa_stream_get_param` returns the value being applied now. A value you just set reads back at once, but is heard after the latency above.
- A forward-only stream (an HTTP download without range requests) can't re-render from the position being heard, so the first change on it is heard after the whole decode-ahead buffer.
- Extreme settings (tempo below 0.25 or above 4, pitch beyond an octave) work but sound processed with any stretcher; keep_formants helps voices most within about ±5 semitones.
- BPM and beat detection is [track analysis](analysis.md).
