# MediaControl reply: DJ mixer plan (scope cut)

Answer to `2026-10-23_FatzAudio_Plan_DjMixer.md`. Approved with a smaller scope, agreed with the owner
2026-10-23.

## Direction

The app is a karaoke app first, with a DJ section most people will find easy and complete. Not a
VirtualDJ or Traktor competitor. So:

1. Build everything karaoke needs to be fully functional (video, audio, CD+G, lyrics, mics).
2. Build the common DJ items, done well.
3. Pin the rest in a "future" list in the plan; no work on it now.

The owner wants to start app development soon, so a smaller first delivery beats a complete one.

## Answers to the three questions

1. Deck EQ: **isolator with kills**.
2. Waveform overview: **yes**.
3. Lanes: **two threads at once** is fine for the smaller scope.

## Build now

**Karaoke (must have)**
- Two mics (one 2-input interface or two devices): gain, mute, gate, EQ (with a "Vocal" preset),
  shared reverb and echo (echo in milliseconds), monitor on/off, raw capture where the OS has it.
- Host-mic ducking (singers never duck the music).
- Latency modes NORMAL and LOW; `fa_mixer_get_latency`; input-lost event; permission error.
- Recording: master, plus vocals (both mics, wet) aligned to the music as the singer heard it.

**Common DJ (must have)**
- `fa_mixer` with **2 decks**, deck gain and trim, isolator EQ with kills, meters.
- Crossfader with **constant power** (default) and **linear** curves; master gain; limiter forced on.
- Headphone cue: **channels 3-4** of one interface, **split mono**, and **a second device** (it reuses the
  drift-corrected bridge the mics need anyway).
- Analysis: **constant BPM grid**, **key** (name and Camelot, `fa_key_transpose`), waveform overview,
  cache; exact grids from MIDI and trackers; `fa_stream_set/get_beat_grid` so the app can correct a grid.
- **Sync button**: tempo plus beat alignment, with lock; `fa_deck_play_synced`.
- **Loops**: in/out frames and loop by beats (1, 2, 4, 8, 16).
- **Auto-mix**: timed crossfade at the mix point, beat-aligned when both grids are good, plain
  crossfade otherwise; `DECK_EMPTY` / `AUTOMIX_STARTED` / `AUTOMIX_DONE` events; skip.
- CD cue point (`fa_deck_set_cue_point`, `fa_deck_cue`).

## Two requirements

1. **The singer's voice is never delayed for cue alignment.** While a mic is monitored, do not delay the
   master output to line up a second-device cue; delay the cue side instead, or skip alignment.
2. **A deck stream's heard position includes any delay added to the master** (and the time map
   matches). MediaControl's video, CD+G and lyrics follow that position. Please cover it with a test.

## Order

1. **Mic latency spike first** (capture and monitoring only, measured on the owner's PC). If Windows
   cannot get mic to speaker near 20 ms in LOW mode on the owner's interface, tell us before building the mic
   PRs, because the app would default to monitoring off.
2. Mixer core (2 decks, EQ, crossfader, limiter, channels 3-4 and split cue, frame loops).
3. Mics and their effects, ducking, recording (the karaoke-critical part).
4. Second-device cue (bridge already built in step 3).
5. Analysis (can run in a second thread from step 2 on).
6. Sync, beat loops, auto-mix.

## Pinned for later (not now)

- 4 decks.
- Tempo maps for songs that change tempo (constant grid only for now; MIDI/tracker grids already exact).
- Bar detection and bar sync; beat jump; quantized seeks.
- Auto-mix bass swap and restore-tempo glide.
- Crossfader curves full-middle and cut; manual cue trim (`FA_MIX_CUE_TRIM_MS`).
- WASAPI exclusive mode.
- Echo synced to the beat.
- Separate recording stems (music only, each mic dry).
- CD+G scroll shift in the frame (`scroll_dx`/`scroll_dy`), until our profiling asks.
- Per-deck filter knob.
- Automatic key matching in auto-mix (the singer's key is the app's choice).
- Already out of scope in your plan: DVS, controller mapping, samplers and effect racks, scratching,
  streaming, ASIO, feedback suppression, stem separation, Ableton Link.

Leave room in the API for the pinned items (struct sizes, enum values, flags), so adding them later does
not break the app.

## On our side

MediaControl integrates your karaoke surfaces first (stage 037A), then adds a thin mixer layer: a
player's audio attaches to a deck, and its video, CD+G and lyrics follow that deck. The app owns the
playlist, singer rotation, controller mapping and the song database.
