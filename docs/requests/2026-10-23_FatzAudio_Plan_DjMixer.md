# FatzAudio plan: DJ deck mixer, beat-matching and microphone input

Answer to FatzBomb.MediaControl's request of 2026-10-23 ("robust but not ridiculous" DJ setup for the
karaoke app).

**Status: approved 2026-10-23 with a smaller scope** (MediaControl's reply,
`2026-10-23_MediaControl_Reply_DjMixerPlan.md`, and Keith in the project thread). Section A below is
the agreed scope and order and wins over anything later in this document. Items marked *(future)* in the
design sections are pinned in section 19 and not built now; the API leaves room for them.

Checked against main at e56853b (PR 90, karaoke step 6a). Numbers marked *measured* come from the repo's
docs; numbers marked *estimate* are mine and get replaced by measurements in the PR that builds the part.

---

## A. Agreed scope and order (2026-10-23)

The app is a karaoke app first, with a DJ section that is easy and complete, not a Traktor competitor.
Build everything karaoke needs, plus the common DJ items done well; everything else is in section 19.

**Karaoke (build now):** two mics (one 2-input interface or two devices) with gain, mute, gate, EQ (plus a
"Vocal" preset), shared reverb and echo (echo time in milliseconds), monitor on/off, raw capture where the
OS has it; host-mic ducking (singers never duck the music); latency modes NORMAL and LOW;
`fa_mixer_get_latency`; input-lost event; permission error; recording of the master and of the vocals
(both mics, wet), aligned to the music as the singer heard it.

**Common DJ (build now):** `fa_mixer` with 2 decks; deck gain and trim; isolator EQ with kills; meters;
crossfader with constant power (default) and linear curves; master gain; limiter forced on; headphone
cue on channels 3-4, split mono, or a second device; analysis with a constant BPM grid, key (name,
Camelot, `fa_key_transpose`), waveform overview and cache, exact grids from MIDI and trackers,
`fa_stream_set/get_beat_grid`; sync (tempo plus beat alignment, with lock) and `fa_deck_play_synced`;
loops by frames and by beats (1, 2, 4, 8, 16); auto-mix (timed crossfade at the mix point, beat-aligned
when both grids are good, plain crossfade otherwise, the three events, skip); CD cue point.

**Answers:** isolator EQ with kills; waveform overview yes; two threads at once is fine.

**Two requirements from MediaControl:**

1. *The singer's voice is never delayed for cue alignment.* While any mic is monitored, the master output
   is never delayed to line up a second-device cue; the cue side is delayed instead, or alignment is
   skipped when the cue is the slower output. `fa_mixer_get_latency` reports which happened.
2. *A deck stream's heard position includes any delay added to the master*, and the time map matches.
   MediaControl's video, CD+G and lyrics follow that position. Covered by a test: a deck with a known
   master delay reports `fa_stream_get_clock` and its position shifted by exactly that delay, and CD+G and
   lyric surfaces driven from it show the same frame as without the delay, that much later.

**Order:**

| # | What | Notes |
|---|---|---|
| 0 | **Mic latency spike** on Keith's PC: capture and monitoring only, NORMAL vs LOW (exclusive measured for reference only) | If LOW cannot get mic to speaker near 20 ms on Keith's interface, MediaControl is told before any mic PR, because the app would then default to monitoring off |
| 1 | Mixer core: 2 decks, gain/trim, isolator EQ, crossfader (2 curves), master gain, limiter, meters, channels 3-4 and split cue, frame loops, CD cue point | Opus high, 6-9 h |
| 2 | Mics: capture through the drift-corrected bridge, gate, EQ, reverb, echo, ducking, latency report, recording (master + vocals), and the built-in setup check (below) | Opus high, 9-13 h; the karaoke-critical part |
| 3 | Second-device cue on the bridge from step 2, with requirement 1 | Opus high, 3-5 h |
| 4 | Analysis: constant grid, key, waveform, cache, MIDI/tracker grids, set/get grid | Opus high, 6-9 h; second thread, from step 1 on |
| 5 | Sync with lock, `play_synced`, beat loops, auto-mix | Opus high, 6-9 h |

**Setup check (Keith, 2026-10-23):** `fa_mixer_measure_latency(mixer, mic, options, &result)` (.NET
`DjMixer.MeasureLatencyAsync`) turns the spike tool into a library call for the app's setup screen. It
plays a few chirps through the real speakers and listens on the real mic, then reports per available mode
(normal, low, exclusive) the measured round trip and mic-to-speaker delay, the highest monitor gain that
stays clear of feedback (a room resonates several dB above its average; Keith's PC howled at a broadband
loop gain of 0.5), and a recommendation (monitor on in a given mode, or off, with the reason). The mixer
keeps the measured delays and uses them instead of the OS's reported latency for vocal recording alignment
and cue alignment. About 8 KB, no new dependencies.

About 30-45 hours of thread time in 5 PRs plus the spike, binary growth about 70-100 KB (*estimate*).
Every PR keeps struct sizes, enum values and flags open for the section 19 items.

---

## 0. Summary (original proposal; section A supersedes its scope and order)

- **What FatzAudio can offer:** a `fa_mixer` object inside one engine: 2 or 4 decks, each holding an
  ordinary `fa_stream` (so markers, tempo/pitch, EQ, CD+G and lyric surfaces keep working on a deck),
  a deck EQ with kills, a crossfader with four curves, one sample clock for everything, a master bus with
  the existing true-peak limiter switched on, headphone cue (second device, channels 3-4 of a DJ
  interface, or split mono), per-deck meters, beat grids with tempo maps and key, sync and quantized
  loops/jumps, unattended auto-mix, one or two microphones with gate/EQ/reverb/echo, ducking, and
  multi-track recording.
- **Cost:** 6 PRs, about 40-60 hours of thread time in total (*estimate*), all Opus at high effort
  except docs passes. Binary grows by about 90-130 KB (*estimate*, roughly 2-3% of today's build). CPU
  for 4 stretched decks plus 2 mics with reverb stays under about 15% of one desktop core (*estimate*
  built from the *measured* stretch cost).
- **Order:** mixer core, then headphone cue, then sync and auto-mix; beat analysis runs as a second lane
  in parallel because it touches different files; microphones come last because they carry the most
  per-OS risk. Details in section 15.
- **Interleaving with karaoke:** not needed. The karaoke plan's last step (6b) is in review now, so this
  starts when it merges. One small optional karaoke follow-up is in section 1.
- **Scope reversal:** "No mic input" was an earlier scope decision. This request reverses it, for
  karaoke mics only (section 11).

---

## 1. The three karaoke surface plan changes (Keith's 10-20 approval)

All three are in the plan and in merged code.

| Change | Plan text | Code | Status |
|---|---|---|---|
| `update_into` takes any of the last 8 revisions (accumulated dirty rectangles for rotating buffers) | plan line 565, section 2.3 | `FA_SURFACE_HISTORY 8` (fatzaudio.h:1476), 8-slot history `fa_history_push/since` (src/surface.c:91-107), used by CD+G, render-ahead and lyric surfaces; older revisions get a full draw. Tested with three rotating buffers compared byte for byte (tests/test_cdg.c:2876-2916) | Done |
| Palette and transparency in `fa_surface_frame` for classic INDEXED8 | plan line 567 | `palette[16][4]`, `transparency[16]`, `palette_revision`, background/border index (fatzaudio.h:1541-1545), filled on every path; .NET `Surface.Palette/Transparency` | Done |
| CD+G scroll at 2160p drawn as a shift plus the exposed band | plan line 569, section 3.0 | `master_shift` memmoves the rendered bitmap and marks only the exposed band plus the effects' reach (src/cdg.c:926-956, 1018-1077); smooth-scroll offsets only move the copied window | Done for rendering |

One gap on the third: after a scroll step the dirty area handed to the app is the whole picture
(src/cdg.c:1079-1083), because every pixel of the app's buffer moved. The *drawing* is band-only, but a
GPU uploader still uploads the full 3840x2160 frame (about 33 MB) on each scroll step. If MediaControl
wants that cheaper too, the frame could also report the shift (`scroll_dx`, `scroll_dy`) so the app
shifts its texture on the GPU and uploads only the band. That is a few hours in the karaoke thread and
independent of everything below. I would only do it if MediaControl's profiling shows the upload.

Also seen: the plan's header (lines 3-4) still says "Step 0 is under way" and "Nothing in sections 2 to 9
is built yet". The karaoke thread can fix that in its last PR.

---

## 2. What exists and what is missing

| Need | Today | Missing |
|---|---|---|
| Several songs on one device, one clock | Streams on one device share its 64-bit frame clock; `fa_stream_play_at`, ramps with a start time and queues land on the exact frame | A deck object; scheduled jumps (loops at any points, beat jumps) |
| Gain, EQ | Per-stream volume (5 ms glide), 10-band parametric EQ with presets on the mix side | A DJ 3-band EQ with kills, per deck |
| Crossfader | `fa_crossfade`, buses with volume/pan ramps; header even sketches "a DJ crossfader is two buses" | A crossfader position with curves; bus buffers (buses only scale gains today, no processing or meters) |
| Tempo/pitch | Signalsmith stretch, TEMPO/RATE/PITCH params, ramps, key lock, `FA_STRETCH_TRANSIENTS`; ~70x real time per stereo stream (*measured*) | Sync and phase control |
| Master limiter | `src/limiter.c`: 4x true peak, 1.5 ms look-ahead, 150 ms release, -1 dBTP | Default is AUTO (only on with normalization), so two loud songs can hard-clip today |
| Headphone cue | Several devices per engine, each its own clock | A stream can be on one device only; no splitter; devices are stereo only; no drift handling between devices |
| Meters | `fa_stream_get_levels`, `fa_device_get_levels`, spectra | Deck (bus) meters |
| Recording | WAV, FLAC, MP3 (LAME at run time) of a device or a stream | Taps for music/mic stems, started on the same frame |
| BPM, beats, key | Nothing (tag `FA_TAG_BPM` read only; MIDI tempo marks exist) | All of it |
| Mic input | Capture devices are listed, never opened | All of it |

---

## 3. The mixer graph

Fixed topology, rendered inside the master device's audio callback. No general routing graph.

```
deck n:  fa_stream (decode side: decoder, tempo/pitch, your DSP)
           -> mix side: your EQ, normalization, volume, fades
           -> deck channel: trim -> DJ EQ (low/mid/high, kills) -> channel fader -> deck meter
                 |-> cue send (if CUE on) ------------------------------+
                 '-> crossfader gain (side A / B / thru) -> music sum   |
                                                                        |
music sum -> ducker (sidechain: mics marked "ducks music") -+           |
mic 1/2: capture -> bridge -> gain -> gate -> EQ -> pan ----+-> mic bus |
           '-> reverb send / echo send -> shared reverb + echo -> mic bus
music + mic bus -> master gain -> limiter -> master output             |
                                     '-> taps: master, music, mic1, mic2 (recording)
cue out = blend(cue sum, master) -> cue gain -> [bridge] -> headphones -+
```

Buses get real buffers for this: today a bus only multiplies gains into the device sum, which cannot
carry an EQ, a meter or a send. Ordinary buses outside a mixer keep their current fast path.

---

## 4. Deck mixer API

### 4.1 C

```c
typedef struct fa_mixer fa_mixer;   /* one per DJ setup, owned by an engine */
typedef struct fa_deck  fa_deck;    /* owned by the mixer */
typedef struct fa_mic   fa_mic;     /* section 11 */

typedef enum fa_cue_mode {
    FA_CUE_NONE = 0,
    FA_CUE_CHANNELS,   /* channels 3-4 of the master device (DJ interfaces): same clock, best */
    FA_CUE_DEVICE,     /* a second output device (cue_device_id): drift-corrected bridge */
    FA_CUE_SPLIT       /* one stereo output: master in mono on the left, cue in mono on the right */
} fa_cue_mode;

typedef enum fa_latency_mode {
    FA_LATENCY_NORMAL = 0,   /* the engine's usual period */
    FA_LATENCY_LOW,          /* smallest shared-mode period the OS gives (WASAPI IAudioClient3, CoreAudio
                                small buffers, AAudio low latency, PulseAudio/PipeWire latency hint) */
    FA_LATENCY_EXCLUSIVE     /* (future, value reserved) Windows WASAPI exclusive, Linux ALSA hw */
} fa_latency_mode;

typedef struct fa_mixer_config {
    uint32_t        struct_size;
    uint32_t        deck_count;      /* 2; 4 is (future) */
    const char*     device_id;       /* master output, NULL = default (follows the default device) */
    fa_cue_mode     cue_mode;        /* default FA_CUE_NONE */
    const char*     cue_device_id;   /* FA_CUE_DEVICE only */
    fa_latency_mode latency;         /* default FA_LATENCY_LOW */
    uint32_t        flags;           /* FA_MIXER_CUE_LOWEST_LATENCY: do not delay the faster output (5.2) */
} fa_mixer_config;

FA_API void      FA_CALL fa_mixer_config_init(fa_mixer_config* config);
FA_API fa_result FA_CALL fa_mixer_create(fa_engine* engine, const fa_mixer_config* config, fa_mixer** out_mixer);
FA_API void      FA_CALL fa_mixer_destroy(fa_mixer* mixer);       /* unloads decks; your streams stay open */
/* The master output as an ordinary device: its clock (fa_device_get_time), levels, spectrum, encoders. */
FA_API fa_result FA_CALL fa_mixer_get_device(fa_mixer* mixer, fa_device** out_device);
FA_API fa_result FA_CALL fa_mixer_get_deck(fa_mixer* mixer, uint32_t index, fa_deck** out_deck);
FA_API fa_result FA_CALL fa_mixer_set_event_callback(fa_mixer* mixer, fa_event_callback cb, void* user);

typedef enum fa_mix_param {
    FA_MIX_CROSSFADER = 0,   /* 0 = side A only, 0.5 = middle, 1 = side B only */
    FA_MIX_MASTER_GAIN,      /* linear, default 1, before the limiter */
    FA_MIX_CUE_MIX,          /* 0 = cue only .. 1 = master only, default 0 */
    FA_MIX_CUE_GAIN,         /* linear, default 1 */
    FA_MIX_CUE_TRIM_MS       /* (future) manual correction of the cue/master alignment, -50..50 */
} fa_mix_param;

/* Same smoothing, curves and device-time starts as fa_stream_ramp_param. */
FA_API fa_result FA_CALL fa_mixer_set_param(fa_mixer* mixer, fa_mix_param param, float value);
FA_API fa_result FA_CALL fa_mixer_get_param(fa_mixer* mixer, fa_mix_param param, float* out_value);
FA_API fa_result FA_CALL fa_mixer_ramp_param(fa_mixer* mixer, fa_mix_param param, float target,
                                             fa_time duration, fa_time device_time, const fa_ramp* ramp);

typedef enum fa_xfader_curve {
    FA_XFADER_CONSTANT_POWER = 0, /* default: steady loudness across the travel (-3 dB each in the middle) */
    FA_XFADER_LINEAR,             /* -6 dB each in the middle: a slight dip */
    FA_XFADER_FULL_MIDDLE,        /* (future) both at full level in the middle (club mixers' "dipless" setting) */
    FA_XFADER_CUT                 /* (future) the incoming side is at full level within 5% of the travel */
} fa_xfader_curve;
FA_API fa_result FA_CALL fa_mixer_set_crossfader_curve(fa_mixer* mixer, fa_xfader_curve curve);

/* ---- Decks ---- */

/* Puts your stream on the deck (it is attached to the deck's channel). The deck does not own it: close it
 * after unloading. NULL unloads. FA_ERROR_BUSY while the current stream is playing and the deck is
 * audible (fader up and crossfader on its side); pass FA_DECK_LOAD_FORCE to replace it anyway.
 * The deck shortens the stream's tempo latency to 60 ms so pitch moves are heard quickly. */
FA_API fa_result FA_CALL fa_deck_load(fa_deck* deck, fa_stream* stream, uint32_t flags);
FA_API fa_result FA_CALL fa_deck_get_stream(fa_deck* deck, fa_stream** out_stream);

typedef enum fa_deck_param {
    FA_DECK_GAIN = 0,        /* channel fader, linear 0..1, default 1 */
    FA_DECK_TRIM_DB,         /* -12..+12, on top of loudness normalization */
    FA_DECK_EQ_LOW_DB,       /* -60..+6 (-60 or less = kill), default 0 */
    FA_DECK_EQ_MID_DB,
    FA_DECK_EQ_HIGH_DB,
    FA_DECK_XFADER_SIDE,     /* 0 = thru (not on the crossfader), 1 = A, 2 = B. Default: deck 1/3 A, 2/4 B */
    FA_DECK_CUE,             /* 1 = sent to the headphones */
    FA_DECK_TEMPO_RANGE      /* how far sync may move the tempo: 0.08 (default), 0.16, 0.5 */
} fa_deck_param;
FA_API fa_result FA_CALL fa_deck_set_param(fa_deck* deck, fa_deck_param param, float value);
FA_API fa_result FA_CALL fa_deck_get_param(fa_deck* deck, fa_deck_param param, float* out_value);
FA_API fa_result FA_CALL fa_deck_ramp_param(fa_deck* deck, fa_deck_param param, float target,
                                            fa_time duration, fa_time device_time, const fa_ramp* ramp);

/* Transport stays on the stream: fa_stream_play / play_at / pause / seek / seek_async / set_param(TEMPO,
 * PITCH). The deck adds what a deck means: */
FA_API fa_result FA_CALL fa_deck_set_cue_point(fa_deck* deck, fa_frames source_frame); /* default CUE_IN */
FA_API fa_result FA_CALL fa_deck_cue(fa_deck* deck);  /* CDJ cue: pause and go back to the cue point */
/* Seamless loop between two source frames (decode side, so it follows tempo); out <= in turns it off. */
FA_API fa_result FA_CALL fa_deck_set_loop(fa_deck* deck, fa_frames loop_in, fa_frames loop_out);
FA_API fa_result FA_CALL fa_deck_get_levels(fa_deck* deck, fa_time window, float* out_peak, float* out_rms,
                                            uint32_t channels);  /* after EQ and fader, before crossfader */
```

Beat-aware deck calls (loop by beats, beat jump, sync) are in section 9.

The DJ EQ is an isolator: two Linkwitz-Riley crossovers at 250 Hz and 2.5 kHz split the deck into
three bands, each with its own gain, and the bands sum back flat at 0 dB. That is what gives real kills
(a band at -60 dB is gone), which shelving EQs cannot do. It sits after the song's own EQ and presets,
so a song EQ the app set stays as it is. It runs on the mix side, so a twist is heard within one period.

### 4.2 .NET

```csharp
using var mixer = engine.CreateDjMixer(new DjMixerOptions
{
    Decks = 2,
    Output = engine.OutputDevices[0],
    Cue = CueOutput.Device(engine.OutputDevices[1]),   // or CueOutput.Channels34, CueOutput.Split
    Latency = LatencyMode.Low,
});

DjDeck a = mixer.Decks[0], b = mixer.Decks[1];
a.Load(engine.OpenFile(@"C:\karaoke\song1.mp3"));
a.Stream.Play();
a.Gain = 0.9f;
a.Eq.Low = -6;            // dB
a.Eq.High = DeckEq.Kill;
a.Cue = true;             // in the headphones

mixer.Crossfader.Curve = CrossfaderCurve.ConstantPower;
mixer.Crossfader.Ramp(1.0f, TimeSpan.FromSeconds(8));   // to side B over 8 s
mixer.CueMix = 0.5f;

var (peak, rms) = a.GetLevels(TimeSpan.FromMilliseconds(50));
mixer.AutoMixStarted += (s, e) => Log($"mixing into deck {e.Incoming.Index}");
```

`DjDeck.Stream` is the ordinary `AudioStream`, so `Markers`, `Tempo`, `Pitch`, `Equalizer`, CD+G and
lyric surfaces are used exactly as today.

---

## 5. One clock, and the headphone cue

### 5.1 Sample accuracy

All decks are streams on the master device, so they already share its frame counter. Everything the
mixer schedules (starts, crossfader ramps, EQ swaps, loop and beat jumps, auto-mix) is expressed as a
device time or a source frame and executed on the exact frame. What is *not* sample-accurate is music:
a beat grid is only as good as its analysis (section 8), typically within a few milliseconds.

### 5.2 Headphone cue: three modes

1. **Channels 3-4 of one interface (`FA_CUE_CHANNELS`, recommended).** Most DJ controllers and many
   USB interfaces present a 4-channel output. Same device, same clock: perfectly in sync, no drift, no
   extra latency. Needs the device to open with 4 channels (today devices are stereo only: a small change).
2. **A second device (`FA_CUE_DEVICE`).** Two devices run on two crystals that drift apart, typically
   20-100 ppm: up to about 0.36 s per hour if left alone. The master callback renders the cue mix into a
   lock-free ring; the cue device reads it through an adaptive resampler (the existing sinc resampler with
   a ratio that moves by at most 0.05%, about 1 cent, steered by the ring's fill level). It also converts
   between different rates (48 kHz master, 44.1 kHz headphones).
   **Latency compensation:** the faster output is delayed by the difference of the two reported
   latencies, so the master heard through the cue blend lines up with the room. **Exception (agreed
   requirement 1): while any mic is monitored the master is never delayed**; if the master is the faster
   output, alignment is skipped and the latency report says so. Any delay added to the master is part of
   every deck's heard position (requirement 2). The OS's latency reports
   are not perfect, so `FA_MIX_CUE_TRIM_MS` lets the app correct by ear. `FA_MIXER_CUE_LOWEST_LATENCY`
   skips the alignment. If the delayed output is the master, mic monitoring gets that delay too; the
   mixer reports it (`fa_mixer_get_latency`, section 11.3).
   If the cue device is unplugged, the cue goes silent and an event is raised; the master is untouched.
3. **Split cue (`FA_CUE_SPLIT`).** One stereo output with a Y-cable: master in mono on the left, cue on
   the right. Laptops and phones with one output.

Per OS: all three work on Windows, macOS and Linux. On iOS only split cue works (two simultaneous outputs
need the MultiRoute session category, which our audio layer does not drive). On Android a second device
may work with a USB interface plus the headphone jack, but Android often routes both to one place, so it
is not promised.

### 5.3 Master bus

The mixer turns the existing limiter on (ceiling -1 dBTP, 1.5 ms look-ahead, 150 ms release) for its
master, whatever the engine-wide setting. Two loudness-normalized songs fully overlapped with a
constant-power crossfader in the middle peak about 2-3 dB over a single one (*estimate*); the limiter
takes that cleanly. Two songs both at full level (`FA_XFADER_FULL_MIDDLE`) can reach 6 dB over, where
some limiting is audible; the docs will recommend normalization on and that curve only for short mixes.
Mics enter before the limiter, so a shouted announcement cannot clip either.

---

## 6. Meters and events

- **Meters:** `fa_deck_get_levels` (after EQ and fader, before the crossfader, like a club mixer's
  channel meters), `fa_mic_get_levels`, and the master through `fa_device_get_levels` on
  `fa_mixer_get_device`. Spectra work the same way through the existing spectrum calls.
- **Mixer events** (through `fa_mixer_set_event_callback`, .NET events): `FA_EVENT_AUTOMIX_STARTED`,
  `FA_EVENT_AUTOMIX_DONE`, `FA_EVENT_DECK_EMPTY`, `FA_EVENT_CUE_DEVICE_LOST`, `FA_EVENT_INPUT_LOST`,
  `FA_EVENT_SYNC_LOST` (a locked deck's grid ended or its tempo went out of range). Stream events
  (started, ended, markers, loops) still come from each deck's stream as today.

Sections 7-10 cover item 2 of the request, sections 11-13 item 3.

---

## 7. What beat-matching needs from the engine

- Positions in the song while stretching are already exact: `fa_tmap` maps every played frame to its
  source frame, so a deck's heard beat phase is known at any moment.
- Tempo changes are applied on the decode side and heard after the tempo latency (150 ms default, 60 ms
  on decks). Sync does not suffer from this, because it schedules in source frames and corrects what it
  measures. Hand-nudging feels slightly delayed at 60 ms; that is the price of high-quality stretching
  and it is the same in most software DJ tools.
- Decode-side scheduled jumps: "when source frame X is decoded, continue at Y" with a 3 ms crossfade.
  Loops are the repeating case; beat jumps and quantized seeks are one-off cases. If X is already decoded
  ahead, the deck re-renders from the heard position (the mechanism the first tempo change already uses).

---

## 8. BPM, beat grid, tempo map and key

### 8.1 What it produces

- **Beat grid:** tempo in BPM, the source frame of the first beat, beats per bar (4, or 3 when clearly
  so), which beat starts a bar, and a confidence from 0 to 1.
- **Tempo map** *(future: constant grids only for now; MIDI and tracker grids are exact already)* for songs that change tempo: segments of (start frame, BPM, beat number). A constant-tempo
  song has one segment.
- **Key:** tonic and mode, the name ("F#m") and the Camelot code ("11A"), with a confidence.
- **Optional waveform overview:** peak and low/mid/high energy per 10 ms, which every DJ screen draws.
  It costs nothing extra in the same pass (about 3 KB of code), so I would include it unless you say no.

### 8.2 How (clean room, from published methods)

1. Decode once (shared with the loudness scan when both are asked for, so one pass gives both).
2. Onset strength: spectral flux on a 1024-point FFT with a 256-sample hop (5.8 ms at 44.1 kHz), using
   the FFT that levels/spectrum already has, weighted toward the low end where kicks live.
3. Tempo: autocorrelation of the onset envelope through a comb filter bank, 60-200 BPM, folded into a
   preferred range (default 70-180) to settle half/double-time; a `FA_TAG_BPM` tag, when present, breaks ties.
4. Beats: dynamic-programming beat tracking (Ellis 2007) over the onset envelope, which handles drift.
   If the tracked beats fit one tempo within 0.1%, the result is a constant grid; otherwise the beat
   times become a smoothed tempo map.
5. Bars *(future)*: the beat phase (of 4, or 3) whose beats carry the most low-frequency accent and harmonic change.
6. Key: a 12-bin chroma from the same FFT, averaged over the song, matched against major/minor key
   profiles (Krumhansl-Schmuckler).

**Exact grids for free:** MIDI files carry their tempo map (`fa_stream_get_midi_marks` already reads it)
and trackers carry speed/tempo per row, so their grids are exact, flagged `FA_GRID_EXACT`, with no audio
analysis. That covers a large part of a karaoke library.

### 8.3 Honest accuracy (*estimate*, to be measured)

- Steady, quantized music (most pop, dance and karaoke backing tracks): tempo within 0.05 BPM, beats
  within about 5 ms, bar start right most of the time.
- Live drummers and rubato ballads: a tempo map with lower confidence; usable for auto-mix alignment,
  not for long locked overlaps.
- Key: about 70-75% exactly right on pop, with most misses being the relative major/minor or a fifth
  away. Good enough to show and to suggest compatible songs; not something to trust blindly.
- Errors the app can fix: the grid can be edited (`fa_stream_set_beat_grid`), as every DJ tool allows.

**Ground truth without a test corpus:** render MIDI files with known tempo maps and keys through our own
synthesizer and check the analysis against the file. Plus a report on a sample of Keith's songs.

### 8.4 Speed and cache

Beat and key analysis together should run at roughly 200-400x real time on one core (*estimate*;
loudness scanning is *measured* at about 450x on 4 cores): about 1 s for a 4-minute song. Results are
cached in the existing scan cache directory, keyed by content like the loudness results.

### 8.5 API

```c
typedef enum fa_analysis_kind {
    FA_ANALYZE_LOUDNESS = 1 << 0,
    FA_ANALYZE_BEATS    = 1 << 1,
    FA_ANALYZE_KEY      = 1 << 2,
    FA_ANALYZE_WAVEFORM = 1 << 3
} fa_analysis_kind;

typedef struct fa_analysis_options {
    uint32_t struct_size;
    uint32_t kinds;          /* fa_analysis_kind bits, default BEATS | KEY */
    float    min_bpm;        /* preferred range, default 70 */
    float    max_bpm;        /* default 180 */
    uint32_t flags;          /* FA_ANALYZE_NO_CACHE */
} fa_analysis_options;

typedef struct fa_analysis fa_analysis;
FA_API void      FA_CALL fa_analysis_options_init(fa_analysis_options* options);
/* Blocking, like fa_scan_loudness_path; uses the decode workers. */
FA_API fa_result FA_CALL fa_analyze_path(fa_engine* engine, const char* path, const fa_analysis_options* options,
                                         fa_analysis** out_analysis);
FA_API fa_result FA_CALL fa_analyze_stream(fa_stream* stream, const fa_analysis_options* options,
                                           fa_analysis** out_analysis);  /* own decoder copy; MIDI exact */
FA_API void      FA_CALL fa_analysis_free(fa_analysis* analysis);

typedef struct fa_beat_grid {
    uint32_t  struct_size;
    double    bpm;             /* main tempo (first segment) */
    fa_frames first_beat;      /* source frame */
    uint32_t  sample_rate;     /* of the source frames */
    uint32_t  beats_per_bar;   /* 4 or 3 */
    uint32_t  bar_offset;      /* beat number (0..beats_per_bar-1) that starts a bar */
    float     confidence;      /* 0..1 */
    uint32_t  flags;           /* FA_GRID_CONSTANT, FA_GRID_EXACT (MIDI/tracker), FA_GRID_EDITED */
    uint32_t  segment_count;   /* 1 = constant tempo */
} fa_beat_grid;

typedef struct fa_tempo_segment {
    fa_frames start;           /* source frame */
    double    bpm;
    double    beat;            /* beat number at start */
} fa_tempo_segment;

typedef struct fa_waveform_point {   /* one per 10 ms */
    float peak;                /* 0..1, both channels */
    float low, mid, high;      /* band energy, 0..1, for coloured waveforms */
} fa_waveform_point;

typedef struct fa_key_info {
    uint32_t struct_size;
    int32_t  tonic;            /* 0 = C .. 11 = B */
    int32_t  minor;            /* 1 = minor */
    float    confidence;
    char     name[8];          /* "F#m" */
    char     camelot[4];       /* "11A" */
} fa_key_info;

FA_API fa_result FA_CALL fa_analysis_get_beat_grid(const fa_analysis* a, fa_beat_grid* out_grid);
FA_API fa_result FA_CALL fa_analysis_get_tempo_map(const fa_analysis* a, fa_tempo_segment* out_segments,
                                                   uint32_t capacity, uint32_t* out_count);
FA_API fa_result FA_CALL fa_analysis_get_key(const fa_analysis* a, fa_key_info* out_key);
FA_API fa_result FA_CALL fa_analysis_get_waveform(const fa_analysis* a, fa_waveform_point* out_points,
                                                  uint32_t capacity, uint32_t* out_count);
/* Your key after a pitch shift of `semitones` (karaoke key changes). */
FA_API void      FA_CALL fa_key_transpose(const fa_key_info* key, int32_t semitones, fa_key_info* out_key);

/* A stream's grid: from an analysis, edited by the user, or your own. MIDI and tracker streams have one
 * from the file. Decks need it for sync, beat loops and aligned auto-mix. */
FA_API fa_result FA_CALL fa_stream_set_beat_grid(fa_stream* stream, const fa_beat_grid* grid,
                                                 const fa_tempo_segment* segments, uint32_t count);
FA_API fa_result FA_CALL fa_stream_get_beat_grid(fa_stream* stream, fa_beat_grid* out_grid,
                                                 fa_tempo_segment* out_segments, uint32_t capacity,
                                                 uint32_t* out_count);
```

```csharp
TrackAnalysis t = await engine.AnalyzeAsync(path, AnalysisKinds.Beats | AnalysisKinds.Key | AnalysisKinds.Waveform);
Console.WriteLine($"{t.Beats.Bpm:F2} BPM, key {t.Key.Name} ({t.Key.Camelot}), confidence {t.Beats.Confidence:P0}");
deck.Stream.BeatGrid = t.Beats;                 // includes the tempo map
var shifted = t.Key.Transpose(-2);              // after a karaoke key change of -2
```

---

## 9. Sync, quantized loops and beat jumps

```c
enum {
    FA_SYNC_TEMPO = 1 << 0,   /* match the leader's heard BPM (half/double time tried when out of range) */
    FA_SYNC_BEAT  = 1 << 1,   /* align beats */
    FA_SYNC_BAR   = 1 << 2,   /* (future) align bars (implies BEAT) */
    FA_SYNC_LOCK  = 1 << 3    /* keep it aligned (follows tempo maps and the leader's tempo moves) */
};
FA_API fa_result FA_CALL fa_deck_sync(fa_deck* deck, fa_deck* leader, uint32_t flags);
/* Starts a stopped deck from its cue point so its next beat (or bar) lands exactly on the leader's. */
FA_API fa_result FA_CALL fa_deck_play_synced(fa_deck* deck, fa_deck* leader, uint32_t flags);
FA_API fa_result FA_CALL fa_deck_set_loop_beats(fa_deck* deck, double beats);   /* 1, 2, 4, 8, 16; 0 = off */
/* (future) FA_API fa_result FA_CALL fa_deck_beat_jump(fa_deck* deck, double beats); */

typedef struct fa_deck_beat_info {
    uint32_t struct_size;
    double   bpm;              /* heard: grid tempo x TEMPO x RATE */
    double   beat;             /* heard position in beats (fractional) */
    double   bar;              /* heard position in bars */
    double   phase_error_ms;   /* against the leader, when synced */
    int32_t  locked;
} fa_deck_beat_info;
FA_API fa_result FA_CALL fa_deck_get_beat_info(fa_deck* deck, fa_deck_beat_info* out_info);
```

How it works:

- **Tempo:** the deck's TEMPO becomes leader BPM x leader tempo / deck BPM, clamped to the deck's tempo
  range; outside it, half or double time is tried; still outside: `FA_ERROR_OUT_OF_RANGE` (new) and
  nothing changes. Key lock is the stretcher, so pitch stays.
- **Phase on a stopped deck:** `fa_deck_play_synced` picks the leader's next beat or bar, computes the
  device time it is heard and starts the deck there from the matching source frame. Exact by construction.
- **Phase on a playing deck:** a small error (under 30 ms) is pulled in by bending the tempo over two
  beats (under 1% tempo, not audible). A larger one is fixed with a quantized jump on the next beat
  boundary (seamless, 3 ms crossfade), because bending half a beat away would be a 25% tempo swing.
- **Lock:** a slow control loop on the event thread (about 20 Hz) measures the phase error from the two
  decks' heard positions and makes tiny tempo corrections, so tempo-mapped songs stay together.
  Target residual under 1 ms (*estimate*).
- **Quantize:** loops by beats and beat jumps start on the grid; `fa_deck_set_loop` with frames is free.

```csharp
b.SyncTo(a, SyncMode.Bar | SyncMode.Lock);
b.PlaySynced(a, SyncMode.Bar);
a.LoopBeats(4);
a.BeatJump(-16);
var info = b.BeatInfo;   // Bpm, Beat, Bar, PhaseErrorMs, Locked
```

---

## 10. Auto-mix

```c
enum {
    FA_AUTOMIX_MATCH_TEMPO   = 1 << 0,  /* bring the incoming deck to the outgoing BPM (within max_tempo_change) */
    FA_AUTOMIX_ALIGN_BEATS   = 1 << 1,  /* start on a bar, length in whole bars */
    FA_AUTOMIX_BASS_SWAP     = 1 << 2,  /* (future) incoming low EQ killed until the middle, then swapped over one beat */
    FA_AUTOMIX_RESTORE_TEMPO = 1 << 3   /* (future) afterwards the incoming deck glides back to its own tempo */
};
typedef struct fa_automix_options {
    uint32_t struct_size;
    fa_time  length;             /* default 8 s */
    float    length_beats;       /* used instead of length when > 0 and both grids are known (e.g. 32) */
    uint32_t flags;              /* default MATCH_TEMPO | ALIGN_BEATS */
    float    max_tempo_change;   /* default 0.08 */
    float    min_confidence;     /* grids below this are treated as unknown, default 0.5 */
    fa_time  restore_time;       /* (future) */
} fa_automix_options;
FA_API void      FA_CALL fa_automix_options_init(fa_automix_options* options);
FA_API fa_result FA_CALL fa_mixer_set_automix(fa_mixer* mixer, const fa_automix_options* options); /* NULL = off */
FA_API fa_result FA_CALL fa_mixer_automix_now(fa_mixer* mixer);   /* start the next transition now (skip) */
```

Behaviour:

- Decks 1 and 2 alternate (with 4 decks, auto-mix uses 1 and 2; 3 and 4 stay manual).
- The transition starts at the outgoing song's mix point, using the same rules as `fa_stream_set_next`
  today: its `MIX_POINT` marker, else `CUE_OUT` (or the end) minus the length; fade markers win. The
  incoming deck starts at its `CUE_IN`, snapped to its first downbeat at or after it when beats are aligned.
- The crossfader is ramped with its curve over the transition; all of it (start, ramp, EQ swap) is
  scheduled on device time a few seconds ahead, so it is sample-accurate and survives a busy UI thread.
- **It never fails unattended:** no grid, low confidence, or tempos too far apart gives a plain timed
  crossfade, which is today's behaviour.
- Events: `FA_EVENT_AUTOMIX_STARTED` and `FA_EVENT_AUTOMIX_DONE` (value = incoming deck),
  `FA_EVENT_DECK_EMPTY` when the idle deck needs its next song (raised when a transition ends, and again
  30 s before the planned mix if it is still empty). The app owns the playlist and calls `fa_deck_load`.
  If nothing is loaded, the outgoing song plays to its end.

```csharp
mixer.AutoMix.Start(new AutoMixOptions { LengthBeats = 32, MatchTempo = true, AlignBeats = true, BassSwap = true });
mixer.AutoMix.DeckEmpty += (s, e) => e.Deck.Load(engine.OpenFile(queue.Next()));
mixer.AutoMix.Skip();
```

---

## 11. Microphone input

### 11.1 Scope reversal

The earlier decision was "No mic input". This request reverses it. The scope stays karaoke-shaped: up to
two mics (one stereo interface with two inputs, or two separate devices), mixed into the master, with the
effects below. Not a general recording or multi-input routing feature.

### 11.2 How

- Each input device is opened as a capture device in the same audio layer we already use (miniaudio
  0.11.25 has capture on every backend we ship, so it adds almost no code to the binary).
- Capture runs on its own clock, so it goes through the same drift-corrected bridge as the cue device
  (section 5.2). One mechanism, built once, used twice. Extra buffering is about one capture period plus
  1-2 ms.
- Two mics on one 2-input interface share one capture device: `channel` 0 and 1.
- A mic can be monitored (heard through the master) or not. With a hardware mixer, turn monitoring off:
  the mic still records and still ducks the music.
- OS voice processing (automatic gain, noise suppression, echo cancelling) ruins singing and adds latency,
  so mics ask for raw capture where the OS has it: Android `UNPROCESSED` (and `VOICE_PERFORMANCE`, made
  for karaoke, which needs a one-line patch to the vendored miniaudio), Windows RAW mode (a small patch
  too: miniaudio 0.11 does not set it), iOS measurement mode.
- Permissions are the app's job (manifest entries, the OS prompt). Without them the open fails with a
  new `FA_ERROR_PERMISSION_DENIED`, documented per OS.
- If a mic is unplugged: it goes silent, `FA_EVENT_INPUT_LOST`, and the mix carries on.

### 11.3 Monitoring latency per OS (honest)

Mic to speaker, round trip. These are *estimates* from how each OS behaves; the Windows rows get measured
on Keith's PC (loopback cable or mic next to the speaker, the repo's `tools/bass-compare/loop_rec.c` is a
start) before the PR merges, and the others are labelled unmeasured in the docs until someone measures them.

| OS and mode | Expected round trip | Notes |
|---|---|---|
| Windows, WASAPI shared (normal) | 30-45 ms | 10 ms periods each way plus buffering. Audible as a slap-back |
| Windows, WASAPI shared low latency (IAudioClient3) | 10-20 ms | Needs the device's own sample rate and a driver that offers small periods (the inbox HD Audio and USB Audio 2 drivers do; many vendor drivers fall back to 10 ms) |
| Windows, WASAPI exclusive | 8-15 ms | Other apps lose the device while the mixer runs |
| macOS, CoreAudio (128-frame buffers) | 8-15 ms | Built-in and USB interfaces alike |
| iOS (5 ms IO buffer) | 10-20 ms | Wired or USB only |
| Android, AAudio low latency | 15-30 ms on good devices | Many devices are 40-100 ms or more; the app should read the reported latency and offer "monitor off" |
| Linux, PipeWire (through its PulseAudio layer) | 45-60 ms default, 12-20 ms with a low-latency request | Our audio layer has no native PipeWire backend; it goes through pipewire-pulse |
| Linux, PulseAudio | 30-60 ms | |
| Linux, ALSA hw direct / JACK | 5-12 ms | Exclusive / needs JACK running |
| Any OS, Bluetooth output or headset | 150-300 ms | Not usable for monitoring; Bluetooth headset mics are also narrowband |

For karaoke through speakers in a room, under about 20-25 ms feels natural (the sound already takes
3-10 ms to cross the room); 30-45 ms is a noticeable echo; singers on headphones notice anything over
about 10 ms. So on Windows, `FA_LATENCY_LOW` is the default for the mixer, with a clear fallback report.

```c
typedef struct fa_mixer_latency {
    uint32_t struct_size;
    fa_time  master;            /* output latency the OS reports */
    fa_time  cue;
    fa_time  cue_alignment;     /* delay added to the faster output */
    fa_time  mic_monitor[2];    /* input to speaker, all included */
} fa_mixer_latency;
FA_API fa_result FA_CALL fa_mixer_get_latency(fa_mixer* mixer, fa_mixer_latency* out_latency);
```

### 11.4 API

```c
typedef struct fa_mic_config {
    uint32_t    struct_size;
    const char* device_id;   /* capture device, NULL = default input */
    int32_t     channel;     /* input channel, 0-based; -1 = sum of all (default) */
    int32_t     monitor;     /* 1 = heard through the master (default) */
    uint32_t    flags;       /* FA_MIC_DUCKS_MUSIC (host mic; off for singers), FA_MIC_OS_PROCESSING */
} fa_mic_config;
FA_API void      FA_CALL fa_mic_config_init(fa_mic_config* config);
FA_API fa_result FA_CALL fa_mixer_add_mic(fa_mixer* mixer, const fa_mic_config* config, fa_mic** out_mic); /* max 2 */
FA_API void      FA_CALL fa_mic_remove(fa_mic* mic);

typedef enum fa_mic_param {
    FA_MIC_GAIN_DB = 0,       /* -20..+40, default 0 */
    FA_MIC_MUTE,
    FA_MIC_PAN,               /* -1..1 */
    FA_MIC_GATE_DB,           /* gate threshold, default -50; -100 = off */
    FA_MIC_REVERB_SEND,       /* 0..1, default 0.2 */
    FA_MIC_ECHO_SEND          /* 0..1, default 0 */
} fa_mic_param;
FA_API fa_result FA_CALL fa_mic_set_param(fa_mic* mic, fa_mic_param param, float value);
FA_API fa_result FA_CALL fa_mic_get_param(fa_mic* mic, fa_mic_param param, float* out_value);
FA_API fa_result FA_CALL fa_mic_set_eq(fa_mic* mic, const fa_eq_band* bands, uint32_t count); /* same EQ as streams */
FA_API fa_result FA_CALL fa_mic_get_levels(fa_mic* mic, fa_time window, float* out_peak, float* out_rms);

/* One shared reverb and one echo for both mics, like the effects unit of a karaoke mixer. */
typedef struct fa_mic_effects {
    uint32_t struct_size;
    float    reverb_size;      /* 0..1 (room .. hall), default 0.5 */
    float    reverb_damping;   /* 0..1, default 0.5 */
    float    echo_time_ms;     /* 20..1000, default 250 (0 = synced to the beat is future) */
    float    echo_beats;       /* (future) when synced: 0.25, 0.5, 0.75, 1 */
    float    echo_feedback;    /* 0..0.9, default 0.35 */
    float    echo_tone;        /* low-pass in the feedback, 0..1 (dark .. bright), default 0.6 */
} fa_mic_effects;
FA_API fa_result FA_CALL fa_mixer_set_mic_effects(fa_mixer* mixer, const fa_mic_effects* effects);

typedef struct fa_ducking {
    uint32_t struct_size;
    float    depth_db;         /* default -12 */
    fa_time  attack;           /* default 50 ms */
    fa_time  hold;             /* default 300 ms */
    fa_time  release;          /* default 600 ms */
    float    threshold_db;     /* voice level that ducks, default -40 */
} fa_ducking;
FA_API fa_result FA_CALL fa_mixer_set_ducking(fa_mixer* mixer, const fa_ducking* ducking); /* NULL = off */
```

```csharp
var host = mixer.AddMic(new MicOptions { Device = engine.InputDevices[0], Channel = 0, DucksMusic = true });
var singer = mixer.AddMic(new MicOptions { Device = engine.InputDevices[0], Channel = 1 });
singer.Gain = 12; singer.Gate = -50; singer.ReverbSend = 0.3f;
singer.Equalizer.Set(EqPreset.Vocal);
mixer.MicEffects = new MicEffects { ReverbSize = 0.6f, EchoBeats = 0.5f, EchoFeedback = 0.3f };
mixer.Ducking = new Ducking { DepthDb = -12, Release = TimeSpan.FromMilliseconds(600) };
Console.WriteLine(mixer.Latency.MicMonitor[0]);
```

---

## 12. Mic effects and ducking: how

- **Gain and gate:** gain with the 5 ms de-zipper; gate with hysteresis (opens at the threshold, closes
  6 dB below), 1 ms attack, 100 ms hold, 150 ms release, so breaths and room noise stay out.
- **EQ:** the existing `src/eq.c`, so mics get the same bands and presets as songs (a "Vocal" preset can
  join the 13 there).
- **Reverb:** the Freeverb-style reverb in `src/midi_fx.c`, with its MIDI-tuned constants made parameters
  (size, damping, input gain, wet level). No new algorithm.
- **Echo:** a new, small delay line with feedback and a tone filter, optionally synced to the playing
  deck's beat.
- **Ducking:** the gated level of mics flagged `FA_MIC_DUCKS_MUSIC` drives a gain on the music sum only,
  with attack, hold and release. Singers are not flagged by default, so the music never dips under a
  singer; a host announcement pulls it down.
- All of this runs in the master callback (live input cannot be decoded ahead), so it is kept to simple
  per-sample work.

---

## 13. Recording

```c
typedef enum fa_mixer_tap {
    FA_TAP_MASTER = 0,  /* what the speakers play, after the limiter */
    FA_TAP_MUSIC,       /* (future) decks after the crossfader, before ducking */
    FA_TAP_MIC1,        /* (future) mic after gain, gate and EQ, dry */
    FA_TAP_MIC2,        /* (future) */
    FA_TAP_VOCALS       /* both mics with reverb and echo */
} fa_mixer_tap;
FA_API fa_result FA_CALL fa_encoder_attach_mixer(fa_encoder* encoder, fa_mixer* mixer, fa_mixer_tap tap);
/* Starts every listed encoder on the same device frame (they wait paused until then). */
FA_API fa_result FA_CALL fa_mixer_start_recording(fa_mixer* mixer, fa_encoder* const* encoders, uint32_t count,
                                                  fa_time device_time);
```

- Formats are today's: WAV, FLAC, MP3 through LAME loaded at run time.
- Stems start on the same frame, so they line up in any editor.
- Mic taps are shifted earlier by the input latency plus the output latency, so the voice lines up with
  the music as the singer heard it (`FA_MIXER_REC_RAW_TIMING` turns that off).

```csharp
await using var rec = mixer.Record((MixerTap.Master, "night.flac"), (MixerTap.Music, "music.flac"),
                                   (MixerTap.Mic1, "host.wav"), (MixerTap.Mic2, "singer.wav"));
```

---

## 14. Threads and real-time safety

- Master callback: decks' mix-side work (today's), deck channels, crossfader, mic chain from the bridges,
  ducking, limiter, taps, cue ring write. No locks, no allocation; parameters arrive through the existing
  lock-free parameter path.
- Cue and capture callbacks: only ring reads/writes and the adaptive resampler.
- Decode workers: decoding and stretching (today's), scheduled jumps.
- Event thread: sync lock loop (20 Hz), auto-mix planning, device/input loss. Analysis runs on the
  caller's thread with the decode workers, like the loudness scan.

---

## 15. Costs and build order

### 15.1 Pull requests

Each PR carries its docs page, migration-guide rows, .NET binding and tests, and merges when clean.
Thread time is an *estimate* from the karaoke pace (12 PRs in about 2.5 days).

| PR | What | Model / effort | Thread time | Depends on |
|---|---|---|---|---|
| D1 | Mixer core: bus buffers, `fa_mixer`, 2/4 decks, deck gain/trim, isolator EQ, crossfader + curves, master gain + limiter on, deck meters, frame loops (decode-side scheduled jumps), 4-channel devices, split cue, `FA_CUE_CHANNELS` | Opus high | 6-10 h | karaoke 6b merged |
| D2 | Drift-corrected bridge, `FA_CUE_DEVICE` with latency alignment and trim, cue device loss | Opus high | 5-8 h | D1 |
| D3 | Analysis: beat grid, tempo map, key, waveform overview, cache, MIDI/tracker exact grids, `fa_stream_set/get_beat_grid`, accuracy report | Opus high | 8-12 h | none (parallel lane) |
| D4 | Sync (tempo, beat, bar, lock), `play_synced`, beat loops and jumps, auto-mix with fallbacks and events | Opus high | 8-12 h | D1, D3 |
| D5 | Mic capture through the bridge, 1-2 mics, latency modes per OS, raw capture patches, permissions error, latency report, Windows latency measured on Keith's PC | Opus high | 6-10 h + one session on Keith's PC | D2 |
| D6 | Mic gain/gate/EQ, parameterized reverb, echo, ducking, recording taps and aligned start | Opus high (DSP) | 5-8 h | D5 |

Total: 6 PRs, about 40-60 hours of thread time, 6-8 CI runs (each ~15-25 min). With D3 in a second
thread alongside D1/D2, about 3-4 days of calendar time.

### 15.2 Binary size (*estimate*)

| Part | Size |
|---|---|
| Mixer, decks, crossfader, isolator EQ, bus buffers, 4-channel devices | 30-40 KB |
| Bridge (reuses the sinc resampler) | 6-8 KB |
| Analysis (beats, tempo map, key, waveform; reuses the FFT) | 25-35 KB |
| Sync and auto-mix | 12-18 KB |
| Capture (the audio layer already has the capture code compiled in) | 8-12 KB |
| Mic effects, ducking, taps (reverb reused) | 8-12 KB |
| **Total** | **about 90-130 KB**, roughly 2-3% of today's binary; the .NET assembly grows by about 60 KB |

No new libraries, no new DLLs, nothing bundled.

### 15.3 CPU (*estimate*, desktop x64, 48 kHz stereo)

| Part | One core |
|---|---|
| A deck with tempo changed (stretch *measured* at about 70x real time) | about 1.4% |
| A deck with pitch changed (*measured* about 30x) | about 3.3% |
| Deck channel (isolator EQ, meter, crossfader) | under 0.1% |
| Limiter (existing) | under 1% |
| Bridge per device | 0.3-0.8% |
| Mic chain (gate, EQ) | under 0.2% |
| Shared reverb + echo | 0.5-1% |
| **4 decks stretching, 2 mics, cue device** | **about 8-15%**, decks spread over the decode workers |
| Analysis | about 1 s per 4-minute song on one core, cached |

### 15.4 Order and interleaving

1. After karaoke 6b merges: **D1** in a DJ thread and **D3** in a second thread (different files, no
   conflicts beyond `fatzaudio.h` and the .NET project, which the second PR updates from main).
2. **D2**, then **D4** once D3 is in.
3. **D5**, then **D6**.

Nothing needs to interleave with the karaoke work: it is on its last step. The one karaoke follow-up worth
considering (scroll shift in the frame, section 1) is independent and small.

### 15.5 Testing

CI has no sound cards, so all tests use simulated devices: a white-box clock test drives two simulated
devices at +/-200 ppm for hours of simulated time to prove the bridge holds its fill; a fake capture
source feeds known signals for gate, ducking, effects and tap alignment; MIDI renders give the analysis
ground truth. Real-device latency is measured on Keith's PC (a Remote Control session, approved on
desktop) and reported in the D5 PR. Mobile stays CI-built and untested on devices, as today.

---

## 16. Left out, and why

From the request (not wanted): DVS and timecode vinyl, MIDI controller mapping, sampler and effects racks,
streaming and broadcast. Also left out:

| Item | Why |
|---|---|
| ASIO | Steinberg's licence, not part of the OS; WASAPI exclusive gets close on Windows |
| Scratching and jog-wheel vinyl feel | Tempo runs on the decode side with 60 ms latency; scratching needs a different, mix-side playback path. Same family as DVS |
| Automatic feedback (howl) suppression | Hard to do well without real-room testing; hardware or a manual EQ notch does it better |
| OS echo cancelling on mics | Adds latency and colours the voice; mics ask for raw capture |
| Vocal removal, auto-tune, harmony | Not asked; vocal removal from mixed audio is poor without stem separation, which is huge |
| Stem separation | Machine-learning models of tens of MB; against "very complete, not everything" |
| Per-deck effects, filter knob | "No effects racks". A one-knob filter per deck is a DJ staple and about half a day if you want it |
| Ableton Link, MIDI clock out | Link's licence (GPL or commercial); not needed for one machine |
| More than 4 decks, hot cue banks, slip mode | The app can do hot cues with markers and seeks; slip mode is niche |
| Line-in as a deck (record player through an input) | Possible after D5, but it is DVS-adjacent and not asked |
| Automatic key matching in auto-mix | Easy after D3 + pitch (a flag), but it changes the singer's key, so it should be the app's choice. Ask if wanted |
| Recording to Opus or AAC | No encoders for them today (noted in engineering notes) |
| iOS second output, Android guarantees | Section 5.2 |

---

## 17. Migration-guide rows (to add as each PR lands)

| BASS | FatzAudio |
|---|---|
| BASSmix mixer + one channel per deck | `fa_mixer_create`, `fa_deck_load` |
| `BASS_Split_StreamCreate` (feed a second device) | `FA_CUE_DEVICE` / `FA_CUE_CHANNELS` |
| `BASS_FX_BPM_DecodeGet` | `fa_analyze_path` with `FA_ANALYZE_BEATS`, `fa_analysis_get_beat_grid` |
| `BASS_FX_BPM_BeatDecodeGet` | beat times from `fa_analysis_get_tempo_map` / the grid |
| `BASS_RecordInit` / `BASS_RecordStart` | `fa_mixer_add_mic` |
| `BASS_FX_BFX_FREEVERB`, `BASS_FX_BFX_ECHO4` on a recording channel | `fa_mixer_set_mic_effects`, `FA_MIC_REVERB_SEND`, `FA_MIC_ECHO_SEND` |
| Ducking by hand with `BASS_ChannelSlideAttribute` | `fa_mixer_set_ducking` |
| `BASS_Encode_Start` on the mixer | `fa_encoder_attach_mixer` + `fa_mixer_start_recording` |

Doc fixes found while reading, to fold into D1: `docs/concepts.md` still calls DSP callbacks "planned";
the `fa_crossfade` header comment says the start is one period ahead (it is the next callback now);
`docs/features/levels-spectrum.md` says device levels include the device volume (they do not); the
features README lists default-device following as planned (it is built).

---

## 18. Questions for Keith (defaults in bold; work can start on the defaults)

1. Deck EQ: **isolator with full kills** (DJ style), or shelving tone controls like a home stereo?
2. Waveform overview data in the analysis pass: **yes** (cheap), or leave it to the app?
3. Lanes: **D1 and D3 in two threads at once**, or one thread doing all six in order (about a day slower)?

---

## 19. Future (pinned 2026-10-23, no work now)

From MediaControl's reply. The API keeps struct sizes, enum values and flags open for each.

- 4 decks.
- Tempo maps for songs that change tempo (constant grid only for now; MIDI and tracker grids are exact).
- Bar detection and bar sync; beat jump; quantized seeks.
- Auto-mix bass swap and the restore-tempo glide.
- Crossfader curves full-middle and cut; manual cue trim (`FA_MIX_CUE_TRIM_MS`).
- WASAPI exclusive mode (measured in the spike for reference only).
- Echo synced to the beat.
- Separate recording stems (music only, each mic dry).
- CD+G scroll shift in the frame (`scroll_dx`/`scroll_dy`), until MediaControl's profiling asks.
- Per-deck filter knob.
- Automatic key matching in auto-mix (the singer's key is the app's choice).
- Out of scope as before: DVS, controller mapping, samplers and effect racks, scratching, streaming, ASIO,
  feedback suppression, stem separation, Ableton Link.

