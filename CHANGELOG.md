# Changelog

FatzAudio follows [semantic versioning](https://semver.org) for its C ABI and the .NET package, which share one
version number (`FA_VERSION_*` in `include/fatzaudio/fatzaudio.h`; `fa_version()` at run time;
`AudioEngine.NativeVersion` in .NET).

- Before 1.0, a **minor** bump (0.2 to 0.3) may break the API or ABI; every break is listed below under
  "Breaking". A **patch** bump never does.
- From 1.0 on, only a major bump breaks.
- The .NET binding refuses to start against a native library of another major.minor version
  (the first call into the library throws `InvalidOperationException`; `AudioEngine.NativeVersion` still
  works), so a mismatched `fatzaudio.dll` is caught at once instead of crashing later. On Linux the SONAME
  carries the minor version too (`libfatzaudio.so.0.2`).
- Additions (new functions, new enum values at the end, new fields at the end of a struct that has a
  `struct_size`) are not breaking.

## Unreleased

### Breaking

- `fa_cdg_info` gained `disc_corrected` and `disc_dropped` at its end (it has no `struct_size`): rebuild against the new
  header. .NET `CdgInfo` gained the two matching positional parameters.

### Added

- Tracker parity (extras step 18): the module's own live state, as `BASS_ATTRIB_MUSIC_*` had it. `fa_stream_set_tracker_tempo` /
  `_get_` and `fa_stream_set_tracker_speed` / `_get_` (the song's tempo and speed, not a time-stretch),
  `fa_stream_set_tracker_global_volume`, `fa_stream_set_tracker_channel_volume` (kept through seeks),
  `fa_stream_set_tracker_instrument_mute`, `FA_TRACKER_AMPLIFY` (millibel), `fa_stream_get_tracker_orders` (the order list),
  and `FA_TRACKER_CELL_EVENTS` with the new events `FA_EVENT_TRACKER_INSTRUMENT` (a note with an instrument) and
  `FA_EVENT_TRACKER_SYNC` (the `Zxx` command). A module is now read by libopenmpt through the stream's source, so the
  file is no longer held in memory twice while it opens. .NET: `TrackerTempo`, `TrackerSpeed`, `TrackerGlobalVolume`,
  `SetTrackerChannelVolume` / `GetTrackerChannelVolume`, `SetTrackerInstrumentMuted` / `IsTrackerInstrumentMuted`,
  `TrackerAmplificationDb`, `GetTrackerOrders`, `TrackerCellEvents`, `TrackerInstrumentPlayed`, `TrackerSyncReached`.
  See [docs/features/trackers.md](docs/features/trackers.md#module-tempo-volumes-and-the-order-list).
- DJ instant reverse and slip mode (extras step 9): `fa_stream_set_reverse` while playing or paused now turns at once,
  playing the last 200 ms heard back the other way out of the stream's buffer while the decoder catches up (no gap,
  no re-buffer). Slip mode: `fa_deck_set_slip` / `fa_deck_get_slip`, `fa_deck_slip_return`, `fa_deck_get_slip_position`
  (.NET `DjDeck.Slip`, `SlipReturn`, `SlipPosition`): reverses, deck loops and beat jumps count on underneath and come
  back to where the deck would be, to the frame. See [docs/features/dj-mixer.md](docs/features/dj-mixer.md#slip-mode).

- Disc and CD+G (extras step 19): `fa_disc_get_pregap` (.NET `Disc.GetPregap`, basscd's `BASS_CD_GetTrackPregap`)
  gives a track's pregap in sectors, from the cue sheet (`INDEX 00`, `PREGAP`) or from a drive's Q subcode. CD+G packs
  read from a disc with two damaged symbols are now repaired as well as those with one (three or more are still
  dropped), and `fa_cdg_info.disc_corrected` / `disc_dropped` count them. A seek before a disc track's graphics were
  found keeps the subcode read so far, and a part read again with different packs redoes the seek checkpoints after it
  at once instead of 10 s per write. See [docs/features/disc.md](docs/features/disc.md#pregaps).

- MIDI scale tuning and drum setup: GS (`40 1x 40`-`4B`) and XG (`08 nn 41`-`4C`) scale tuning, GS drum map edits
  (`41 mp rr`), XG drum setups (`3n rr pp`, and the drum setup reset) and the GS/XG drum NRPNs (MSB 14-1E) set each drum
  key's pitch, level, pan, effect sends, exclusive group, note-off and note-on reception, filter and envelope. GS drum
  map 2 and XG drum setups 1-4 are kept apart. XG SFX kits (bank 126) come from the soundfont's bank 126, else the GS
  SFX kit, else the standard kit. Files without these messages render bit for bit as before.
  See [docs/features/midi.md](docs/features/midi.md#scale-tuning-and-drum-setup).

- Dolby downmix (extras step 20): `fa_open_options.dolby_downmix` and `fa_stream_set_dolby_downmix` /
  `fa_stream_get_dolby_downmix` (.NET `StreamOptions.DolbyDownmix`, `AudioStream.DolbyDownmix`) choose Lo/Ro, Lt/Rt
  (Pro Logic matrix surround) or the stream's own preference; `fa_engine_set_dialnorm` (.NET `AudioEngine.SetDialnorm`)
  lets a Dolby file's dialogue level stand in for a loudness scan (`FA_LOUDNESS_DIALNORM`, .NET
  `LoudnessSource.Dialnorm`). See [docs/features/dolby.md](docs/features/dolby.md#stereo-downmix).

- Web streams: MP4 and Matroska files over the network read only what they need. A fragmented MP4 with a `sidx` (or an
  `mfra` at the end) reads its fragments as playback or a seek reaches them instead of every fragment at open, and
  Matroska reads Cues stored after the clusters, with the last cluster for the exact length, in one range request of
  its own while the download stays at the start. The download is told where the next audio packets are, and decode
  workers wait for 0.3 s of the whole file's bytes, so a worker never waits in a read for video between the audio.
  Tested over HTTP: fragmented MP4 with `sidx`, with `mfra` and with neither, Matroska with Cues at the end, video
  files with several audio tracks, fMP4 HLS. See
  [docs/features/web-streams.md](docs/features/web-streams.md#mp4-and-matroska-files).
- Engine and stream parity (extras step 17): `fa_engine_get_stats` (.NET `AudioEngine.GetStats`, BASS's `BASS_GetCPU`)
  reports the output callbacks' load (average and peak) and the decode workers' load. Callback streams
  (`fa_stream_open_callback`, .NET `AudioEngine.OpenCallback`, BASS's `STREAMPROC`) ask the app's function for audio on a
  decode worker. Capture streams (`fa_stream_open_capture` with `fa_capture_options`, .NET `AudioEngine.OpenCapture` with
  `CaptureOptions`) open an input as an ordinary stream, to record it with `fa_encoder_encode_stream`, read or play it.
  Momentary and short-term loudness meters: `fa_stream_get_loudness_meter`, `fa_device_get_loudness_meter` (.NET
  `GetLoudnessMeter`, BASSloud). `fa_open_options.cancel` now also stops `fa_stream_open_url` (name lookup, connection,
  first bytes) within about 100 ms; .NET adds `AudioEngine.OpenUrlAsync` and `OpenFileAsync` with a
  `CancellationToken`. See [docs/features/callback-capture.md](docs/features/callback-capture.md).

- Web streams: a raw-bytes tap for saving a station as it plays (BASS's `DOWNLOADPROC`): `fa_net_options.download_proc`
  from the first byte, `fa_stream_set_download_callback` to start or stop it while playing (.NET
  `NetworkOptions.Downloaded`, `AudioStream.SetDownloadHandler`); radio comes without its ICY title blocks, HLS as the
  segments' audio. Each stream keeps the cookies its servers set (a playlist's or a login redirect's session cookie)
  and sends them back on its later requests, on every platform. HLS plays the audio rendition of the chosen variant's
  `AUDIO=` group, so `hls_max_bandwidth` also picks the audio tier. A live stream without length or chunked framing that
  the server closes cleanly now reconnects instead of ending. `fa_stream_close` returns at once while a host name lookup
  hangs. See [docs/features/web-streams.md](docs/features/web-streams.md#saving-a-station-as-it-plays).

- Tags (extras step 14): ID3v2 frames stored with the zlib flag are read (encrypted ones are still skipped); `SYLT`
  synced lyrics come as timed `[mm:ss.mmm]text` lines; Lyrics3 v1.00 and v2.00 are read (new `FA_TAG_FORMAT_LYRICS3`,
  .NET `TagFormat.Lyrics3`) and left out of `audio_bytes` and the bitrate; Matroska tags aimed at a track, chapter, edition or attachment
  come as `TRACK:<uid>/NAME` and so on; FLAC cue sheets as `CUESHEET/...` items; the Vorbis vendor string as `VENDOR`;
  `fa_tags_read_path` / `fa_tags_read_memory` read a MIDI file's title, artist, copyright and length without a
  soundfont (`fa_tags_get_info` no longer returns `FA_ERROR_NOT_FOUND` for it).
  See [docs/features/tags.md](docs/features/tags.md#timed-lyrics-cue-sheets-and-the-rest).

- ZIP leftovers: other entries of a karaoke archive (a bundled video or PDF) are probed from a 64 KB prefix instead of
  being unpacked first, in `fa_stream_open_*` and in tag reads; `fa_open_options.cancel` (new last field; .NET
  `StreamOptions.CancellationToken`) stops a long unpack with the new `FA_ERROR_CANCELLED` (.NET `ErrorCode.Cancelled`);
  a song unpacked into a temp file now gets its loudness scan, `fa_stream_get_tags` and `fa_analyze_stream` like any
  file; a stored entry played in place is CRC checked by the worker while it plays, and a mismatch raises one
  `FA_EVENT_DECODE_ERROR` (.NET `DecodeError`) without stopping playback.
  See [docs/features/cdg.md](docs/features/cdg.md#zip-archives).

- MIDI controllers and SoundFont modulators: the mod wheel and channel pressure add vibrato (RPN 5 sets its depth),
  CC74/CC71 move the filter cutoff and resonance, CC73/CC75/CC72 the attack, decay and release, CC76-78 the vibrato rate,
  depth and delay, with the GS/XG part NRPNs and part SysEx that set the same values; portamento (CC65, CC5, CC84),
  sostenuto (CC66) and the soft pedal (CC67); a soundfont's own modulators (`pmod`/`imod`) are applied. Seeks replay
  them. Files that use none of these, with soundfonts that have no modulators of their own, render bit for bit as before.
  See [docs/features/midi.md](docs/features/midi.md#controllers-and-modulators).

- Position events: `fa_stream_add_position_event` (.NET `AddPositionEvent`, `PositionReached`, new `ClearPositionEvents`)
  now works instead of returning `FA_ERROR_NOT_IMPLEMENTED`. BASS_SYNC_POS: one `FA_EVENT_POSITION` the first time
  playback passes a frame, frame-exact through tempo, loops and seeks, and in reverse playback too; up to 64 armed per
  stream. See [docs/features/events.md](docs/features/events.md#position-events).
- Reverse playback: `fa_stream_set_reverse` / `fa_stream_get_reverse` (.NET `AudioStream.Reverse`), bass_fx's
  `BASS_FX_ReverseCreate` and `BASS_ATTRIB_REVERSE_DIR` on the stream itself, switchable while playing; works with tempo,
  pitch, rate, loops and deck loops. See [docs/features/tempo-pitch.md](docs/features/tempo-pitch.md#reverse-playback).
- MIDI: GS and XG reverb and chorus SysEx are followed: reverb type (rooms, halls, plate, GS delay and panning delay,
  XG stage, tunnel and others), level and time; chorus type (chorus, celeste, flanger, short delay), level and the GS
  chorus parameters; GS and XG part volume, pan, reverb send and chorus send. A seek with effects on renders the second
  before the target, so reverb and chorus tails ring on across it. See
  [docs/features/midi.md](docs/features/midi.md#reverb-and-chorus).
- Track analysis: songs whose tempo changes or drifts get a tempo map measured from the audio (segments joined end to end;
  on the drifting test songs every beat within 10 ms, where a constant grid was up to 359 ms off), and bars are found
  from chord changes and kicks (`FA_GRID_BARS` with `beats_per_bar` 4 or 3 and `bar_offset`) when they are clear. Steady
  songs keep their constant grid. Cached analyses are measured again once (the cache format version changed). See
  [docs/features/analysis.md](docs/features/analysis.md#songs-that-change-tempo).
- Core Audio Format: `.caf` files open with the ordinary calls. ALAC (to the bit), AAC (LC, HE-AAC, HE-AAC v2), linear
  PCM and MP3 play with exact length; other codecs in CAF (AAC-LD/ELD, µ-law, ...) return
  `FA_ERROR_UNSUPPORTED_FORMAT`.
- DJ mixer: beat jumps (`fa_deck_beat_jump`, .NET `DjDeck.BeatJump`) by whole grid beats, seamless, keeping a synced
  deck on its leader's beat; a deck loop that is on moves along. Auto-mix bass swap (`FA_AUTOMIX_BASS_SWAP`,
  `AutoMixOptions.BassSwap`): the incoming low is killed until the middle, then the lows swap over one beat. Auto-mix
  tempo glide back (`FA_AUTOMIX_RESTORE_TEMPO`, `restore_time`, `AutoMixOptions.RestoreTempo` and `RestoreTime`):
  the incoming deck returns to its own tempo after the transition. See
  [docs/features/dj-mixer.md](docs/features/dj-mixer.md#auto-mix).
- DJ mixer: `fa_mixer_create` opens two decks on one output with channel faders, trim, a three-band isolator EQ with kills
  (Linkwitz-Riley crossovers at 250 Hz and 2.5 kHz), a crossfader with constant-power and linear curves, master gain,
  the limiter always on, and per-deck meters (`fa_deck_get_levels`). Headphone cue on two more outputs of the same
  device (`FA_CUE_CHANNELS`) or a Y-cable split (`FA_CUE_SPLIT`), with cue mix and gain. `fa_deck_set_loop` makes
  seamless deck loops that follow tempo and pitch, including "loop out" while playing; `fa_deck_set_cue_point` and
  `fa_deck_cue` give a CDJ-style cue. Opens in the low-latency shared mode by default (`FA_LATENCY_LOW`). New error
  `FA_ERROR_NOT_ENOUGH_CHANNELS`. .NET: `AudioEngine.CreateDjMixer`, `DjMixer`, `DjDeck`, `DjCrossfader`, `DeckEq`.
  See [docs/features/dj-mixer.md](docs/features/dj-mixer.md).
- DJ mixer setup check: `fa_mixer_measure_latency` plays test chirps and pink noise through the speakers, listens on
  a mic, and measures each latency mode (`FA_CHECK_ALL_MODES`): mic-to-speaker time, its spread, the reported figure,
  and the most mic gain that stays 6 dB clear of feedback. It advises monitoring, in which mode, or why not
  (`fa_check_advice`); the mixer then uses the measured latency for that mic. Opt-in Windows WASAPI exclusive mode
  (`FA_LATENCY_EXCLUSIVE`, 3 ms periods) and `fa_mixer_set_latency_mode` / `fa_mixer_get_latency_mode` to switch a
  running mixer. Mics capture in WASAPI raw mode on Windows (no voice processing). .NET: `DjMixer.LatencyMode`,
  `DjMixer.MeasureLatencyAsync`, `LatencyCheckOptions`, `LatencyCheck`, `CheckAdvice`. See
  [docs/features/dj-mixer.md](docs/features/dj-mixer.md#setup-check).
- DJ mixer mic effects, ducking and recording: one shared reverb (the MIDI reverb, now tunable) and echo for the
  mics, with per-mic sends (`FA_MIC_REVERB_SEND`, default 0.2, and `FA_MIC_ECHO_SEND`) and `fa_mixer_set_mic_effects`
  (size, damping, echo time, feedback, tone). Host mics (`FA_MIC_DUCKS_MUSIC`) duck the music, set with
  `fa_mixer_set_ducking` (on by default at -12 dB). `fa_encoder_attach_mixer` records the master or the vocals tap,
  and `fa_mixer_start_recording` starts the stems on one frame, the vocals lined up as the singer heard the music
  (`FA_MIXER_REC_RAW_TIMING` for the raw timing). `fa_mixer_latency.master` now includes the limiter's look-ahead.
  .NET: `Mic.ReverbSend`, `Mic.EchoSend`, `DjMixer.MicEffects`, `DjMixer.Ducking`, `MixerTap`,
  `AudioEncoder.Record(DjMixer, MixerTap)`, `DjMixer.StartRecording`. See
  [docs/features/dj-mixer.md](docs/features/dj-mixer.md#reverb-and-echo).
- DJ mixer mics: `fa_mixer_add_mic` adds up to two karaoke mics, one channel (or all channels) of a capture device
  each, brought into the master through a drift-corrected bridge, with an 80 Hz low cut, gain, a gate, EQ (new preset
  `FA_EQ_PRESET_LIVE_VOCAL`), mute, pan, monitor on or off and a meter (`fa_mic_get_levels`). `fa_mixer_get_latency`
  reports the master, cue and voice-to-speaker latencies; `FA_EVENT_INPUT_LOST` on the new
  `fa_mixer_set_event_callback` reports an input that went away. Android captures unprocessed. New error
  `FA_ERROR_PERMISSION_DENIED`. .NET: `DjMixer.AddMic`, `Mic`, `MicOptions`, `DjMixer.Latency`, `DjMixer.InputLost`,
  `EqualizerPreset.LiveVocal`. See [docs/features/dj-mixer.md](docs/features/dj-mixer.md#microphones).
- DJ mixer sync, beat loops and auto-mix: `fa_deck_sync` matches a deck's tempo to the other deck's heard BPM
  (half and double time, within `FA_DECK_SYNC_RANGE`, else the new error `FA_ERROR_OUT_OF_RANGE`), aligns beats or bars
  (a bend of up to 2% for small errors, a seamless jump on the next beat for large ones) and with `FA_SYNC_LOCK` keeps
  it so from the event thread. `fa_deck_play_synced` starts a deck so its next beat or downbeat lands on the leader's,
  to the frame. `fa_deck_set_loop_beats` loops 1/16..32 beats on the grid; `fa_deck_get_beat_info` reports the heard
  BPM, beat, bar and phase error. `fa_mixer_set_automix` alternates the decks with timed or beat-aligned transitions
  scheduled on the device clock, raising `FA_EVENT_AUTOMIX_STARTED`, `FA_EVENT_AUTOMIX_DONE` and `FA_EVENT_DECK_EMPTY`;
  `fa_mixer_automix_now` skips. Closing a deck's stream is now safe from any thread. .NET: `DjDeck.SyncTo`,
  `PlaySynced`, `Unsync`, `LoopBeats`, `BeatInfo`, `SyncRange`, `DjMixer.AutoMix` (`DjAutoMix`, `AutoMixOptions`),
  `SyncMode`, `ErrorCode.OutOfRange`. See [docs/features/dj-mixer.md](docs/features/dj-mixer.md#sync-and-beat-loops).
- DJ mixer second-device cue: `FA_CUE_DEVICE` with `cue_device_id` puts the headphones on another output device, its
  drift (and a different rate) absorbed by a resampler that moves by at most 0.05%. The faster output is delayed so
  the master in the headphones lands with the speakers, except that the master is never delayed while a mic is
  monitored; a delay on the master is part of every heard position (`fa_stream_get_clock`, CD+G and lyric surfaces,
  `fa_device_get_latency`). New `fa_mixer_latency.cue_align` (`fa_cue_align`), `FA_MIX_CUE_TRIM_MS`,
  `FA_MIXER_CUE_LOWEST_LATENCY`, `fa_mixer_set_cue_device` and `FA_EVENT_CUE_LOST`. The mic bridge and the cue share
  one implementation. .NET: `CueOutput.Device`, `DjMixer.CueTrim`, `DjMixer.SetCueDevice`, `DjMixer.CueLost`,
  `MixerLatency.CueAligned` (a new optional record parameter), `CueAlign`. See
  [docs/features/dj-mixer.md](docs/features/dj-mixer.md#second-device-cue).
- Track analysis: `fa_analyze_path` and `fa_analyze_stream` measure a beat grid (tempo, first beat, confidence), the
  musical key (name and Camelot code), a waveform overview (peak and low/mid/high levels every 10 ms) and the loudness in
  one decode, about 170x real time on one core, cached by content in `cache_directory`. MIDI files and tracker modules get
  exact grids, with tempo changes, from the file. `fa_stream_set_beat_grid` and `fa_stream_get_beat_grid` put a grid on a
  stream (for decks), `fa_key_transpose` and `fa_key_make` handle keys. Replaces `BASS_FX_BPM_*`. Constant grids only from
  audio; tempo maps and bar detection are later. See [docs/features/analysis.md](docs/features/analysis.md).
- .NET: `AudioEngine.Analyze` / `AnalyzeAsync` and `AudioStream.Analyze` / `AnalyzeAsync` return a `TrackAnalysis`
  (`Beats`, `Key`, `Waveform`, `Loudness`); `BeatGrid` (with `BeatToFrame` / `FrameToBeat`), `MusicalKey` (with
  `Transpose`), `AnalysisKinds`, `AnalysisOptions`; `AudioStream.BeatGrid` gets and sets a stream's grid.
- CD+G smoothness 5-10: each colour region's outline is traced along the pixel edges, smoothed by local rules (diagonal
  steps at 5, shallow slopes at 6, rounded corners at 7-8, curves after the published potrace method at 9-10) and drawn
  with exact-area anti-aliasing, the colours meeting along shared outlines so no seams appear. Only the area around a
  change is drawn again, and scrolls still move the picture. Edge blend has no effect at these levels. See
  [docs/features/cdg.md](docs/features/cdg.md#presentation-options-fa_cdg_surface_set_presentation).
- Surface effects at small CD+G presentation sizes (1x to 3x) are worked out at twice the size and box-filtered down,
  so thin outlines and bevels keep their shape.
- MIDI lyric surface: `fa_lyric_surface_create`, `fa_lyric_surface_set_style`, `fa_lyric_surface_get_style` and
  `fa_lyric_style_init` draw a MIDI or `.kar` song's lyrics as a karaoke screen: the active line with lines around it,
  syllables coloured as they are sung (cross-fade or sweep), scrolling lines, page fades, a countdown after long gaps,
  an optional panel, and the surface effects, with FatzAudio's own font reader. Five presets (`FA_LYRIC_CLASSIC`,
  `MODERN`, `NEON`, `MINIMAL`, `SUBTITLE`). Drawn with `fa_surface_update_into` like a CD+G surface. .NET: `LyricSurface`,
  `LyricStyle`, `LyricPreset`, `LyricWipe`, `LyricCountdown`, `AudioStream.CreateLyricSurface`; `IKaraokeSurface` is now
  implemented by `LyricSurface` too. See
  [docs/features/lyrics.md](docs/features/lyrics.md).
- Lyric surface render-ahead: `fa_surface_set_render_ahead` (.NET `RenderAhead`) now works for lyric surfaces too
  (before, the setting was accepted but frames were drawn on the caller's thread). The engine's worker threads render
  the next frame ahead, and `fa_surface_update_into` copies the finished frame that fits; a frame the workers could not
  finish in time is shown late with `fa_surface_frame.behind = 1`. Off by default; off behaves as before. The cost
  table for 1080p and 2160p is in [docs/features/lyrics.md](docs/features/lyrics.md#cost).
- Lyric surface intro and ending pages: the song's credits (`fa_stream_get_midi_credits`) on smoked glass, on by
  default and off with `subtitle_mode`. The intro fades in at 1 s: bands at the top and bottom (title, artist, year;
  length, key, tempo, timing, copyright) when the lyrics start before 8 s, else a centred card that is gone 3 s before
  the lyrics. An ending card follows the lyrics' end fade when at least 3 s of song remain. A song with no title and no
  artist gets an intro only with `fallback_title`, and no ending. New `fa_lyric_style` fields after `effects`:
  `intro_page`, `ending_page`, `fallback_title`, `labels`, `intro_start_ms`, `intro_fade_ms`, `intro_clear_ms`,
  `intro_band_ms`, `ending_min_ms`, `ending_fade_ms`, `card_color`, `card_accent`, `title_color`, `info_color`,
  `copyright_color`. .NET: the matching `LyricStyle` properties (`IntroPage`, `EndingPage`, `FallbackTitle`, `Labels`,
  `IntroStart`, `IntroFade`, `IntroClear`, `IntroBand`, `EndingMin`, `EndingFade`, `CardColor`, `CardAccent`,
  `TitleColor`, `InfoColor`, `CopyrightColor`). When either page can show, the surface's bitmap is the whole
  presentation area (`bitmap_x` and `bitmap_y` are 0) instead of the lyric block. See
  [docs/features/lyrics.md](docs/features/lyrics.md#intro-and-ending-pages).
- CD+G presentation options: `fa_cdg_surface_set_presentation` crops a surface to a safety zone (show, remove, auto
  from a pre-pass over the song, or a tight box around everything drawn), adds transparent padding, and with
  presentation scaling draws the picture in premultiplied full colour at the whole-number multiple nearest the
  presentation size, on a transparent background with a one-pixel transparent margin, at smoothness 0-10 (5-10
  traced outlines and curves), with optional edge anti-aliasing (MLAA) and a transparent border. Scrolls move the surface's
  picture instead of redrawing it, and smooth-scroll offsets only move what is copied out. Also
  `fa_cdg_surface_get_presentation`, `fa_cdg_presentation_init`, `FA_CDG_TRANSPARENT_BORDER` and
  `FA_CDG_INDEX_TRANSPARENT` (index 16 in INDEXED8 padding). .NET: `CdgSurface.Presentation`, `CdgPresentation`,
  `CdgSurface.TransparentIndex`. Surfaces without presentation options draw exactly as before. See
  [docs/features/cdg.md](docs/features/cdg.md#presentation-options-fa_cdg_surface_set_presentation).
- Surface effects: `fa_surface_set_effects` draws an outline, a shadow (offset, spread and soft) and a bevel around a
  CD+G picture with presentation scaling, from one exact distance field of the picture's shape, so outlines are round
  and even at any width; sizes are in CD+G pixels. Also `fa_surface_get_effects` and `fa_surface_effects_init`. .NET:
  `IKaraokeSurface.Effects`, `SurfaceEffects`, `OutlineEffect`, `ShadowEffect`, `BevelEffect`. Off by default. See
  [docs/features/cdg.md](docs/features/cdg.md#presentation-options-fa_cdg_surface_set_presentation).
- Render-ahead: `fa_surface_set_render_ahead` (.NET `RenderAhead`) now works. With CD+G presentation scaling the
  engine's worker threads render the next frame ahead and `fa_surface_update_into` copies the newest finished one
  without waiting; `fa_surface_frame.behind` says when that frame is late. Workers render only when no stream needs
  decoding.
- Surfaces: `fa_surface_update_into` draws a CD+G surface into a buffer of yours, only the area that changed since the
  frame that buffer holds (any of the last 8 revisions), with up to 8 dirty rectangles and timings, the same pixels
  `fa_cdg_surface_update` gives. Also `fa_surface_get_layout`, `fa_surface_set_presentation_size`,
  `fa_surface_set_render_ahead` (kept for the presentation options), `fa_surface_destroy` and
  `FA_ERROR_SIZE_MISMATCH`. `fa_surface` is a typedef of `fa_cdg_surface` (existing code is unchanged). `fa_cdg_frame.revision` can now
  go up by more than one between two `fa_cdg_surface_update` calls when `update_into` drew in between. .NET:
  `IKaraokeSurface` (implemented by `CdgSurface`), `SurfaceLayout`, `SurfaceUpdate`, `PixelRect`, `PixelSize`,
  `ErrorCode.SizeMismatch`. See [docs/features/cdg.md](docs/features/cdg.md#drawing-into-your-own-buffers-fa_surface_update_into).
- MIDI song credits: `fa_stream_get_midi_credits` (.NET `AudioStream.MidiCredits`) gives title, artist, writer,
  lyricist, year, copyright, key, tempo, time signature and other header lines, gathered from `.kar` `@T`/`@I` lines
  and labelled text events. See [docs/features/midi.md](docs/features/midi.md#song-credits).
- MIDI text code page: Shift-JIS (Japanese) text is detected and decoded; `fa_open_options.text_code_page`
  (.NET `StreamOptions.TextCodePage`) forces UTF-8, Windows-1252 or Shift-JIS.
- .NET: `AudioStream.GetLevels(Span<float> peak, Span<float> rms)` and `OutputDevice.GetLevels(peak, rms)`, which
  write into your own buffers and allocate nothing (a VU meter at 60 Hz made about 180 small objects a second).
- Transient mode for tempo and pitch: `FA_STRETCH_TRANSIENTS` (.NET `StretchQuality.Transients`) plays the original
  audio around each onset, so drum hits have no pre-echo (-50 dB instead of -25 dB) and stay on time. See
  [docs/features/tempo-pitch.md](docs/features/tempo-pitch.md#transient-mode).
- Tracker modules: MOD, XM, S3M, IT, MPTM and the other formats libopenmpt reads (MTM, UMX, 669, AMF, DBM, MED,
  PSM and more) open with the ordinary `fa_stream_open_*` calls and play once (loop with the stream's loop
  toggle). libopenmpt 0.8.9 is built in (`third_party/libopenmpt`, BSD-3-Clause), so a C++17 compiler is
  now required to build and the Linux `.so` grows by about 2 MiB. See [docs/features/trackers.md](docs/features/trackers.md).
- Module information and names (`fa_stream_get_tracker_info`, `fa_stream_get_tracker_text`; .NET `TrackerInfo`),
  the time of an order and row and seeking to it (`fa_stream_get_tracker_time`, `fa_stream_seek_tracker`; .NET
  `GetTrackerTime`, `SeekTracker`), live interpolation, stereo separation, volume ramping and Amiga resampler
  settings (`fa_stream_set_tracker_param`; .NET `TrackerInterpolation`, `TrackerStereoSeparation`,
  `TrackerVolumeRamping`, `TrackerAmigaResampler`) and channel mute (`fa_stream_set_tracker_channel_mute`;
  .NET `SetTrackerChannelMuted`).
- Tracker subsongs: a module's subsongs are the stream's audio tracks. `fa_open_options.audio_track` (.NET
  `StreamOptions.AudioTrack`) plays subsong n, `fa_stream_info.audio_track_count` is the count, a subsong past the
  last gives `FA_ERROR_NOT_FOUND`; `fa_tracker_info.subsong` (at the end, older shorter structs are still filled up
  to `duration`; .NET `TrackerInfo.Subsong`). The restart position and duration are the playing subsong's, and
  `fa_stream_get_tracker_time` / `fa_stream_seek_tracker` return `FA_ERROR_NOT_FOUND` for rows of another subsong.
  This needed a second local libopenmpt patch (`third_party/libopenmpt/README.md`).
- `fa_stream_get_tracker_position`: the order and row heard now, within about 2 ms of the row start, lock-free
  (.NET `AudioStream.TrackerPosition`).
- Tracker row events: `FA_TRACKER_ROW_EVENTS` (0 off, 1 every row, 2 first row of each order) raises
  `FA_EVENT_TRACKER_ROW` (value = order << 16 | row), best effort like markers (.NET `TrackerRowEvents`,
  `TrackerRowReached`).
- Tags: a module's title, artist and song message (`FA_TAG_FORMAT_TRACKER`; .NET `TagFormat.Tracker`).

### Changed

- CD+G presentation scaling with smooth-scroll offsets: the picture is smoothed, edge-blended and given its effects
  as shown, with the picture area moved, so the seam where it meets the border is drawn against its real neighbours and
  a transparent border stays transparent under the effects. The classic picture maps tiles drawn while the offsets are
  set to the area they show in instead of redrawing the whole surface. Songs without the offsets draw the same pixels
  as before.
- Containers (extras step 23, no change for well-formed files): Matroska's EBML elements and MP4's boxes are read by one walker each (`src/walk.c`), shared by the demuxers and the tag reader; the ADTS header and the AudioSpecificConfig each have one parser (`fa_adts_parse`, `fa_aac_parse_asc` in aac.c), and an Ogg link's comments are parsed once. The system AAC decoders (Media Foundation, AudioToolbox, MediaCodec) lose their ADTS handling, which nothing reached: ADTS always plays through the built-in decoder. Malformed files can come out differently; see docs/engineering-notes.md.

- Analysis: the key's chroma FFT carries two windows at once (as the onset side does), and the bars' chroma shares a bin wider than a semitone among the pitch classes it covers (low chord changes show). The accuracy report is unchanged (all 44 bar songs right, 48 of 48 keys).
- Lyric surface: a line growing in during a page fade-in is drawn again at its size instead of upscaled, and a line or page that failed for lack of memory is tried again after about 90 frames.
- Reverse playback: the half-second blocks the source is decoded in are each decoded 10 ms past their end and
  cross-faded with the block above, so where a seek is not exact (AAC noise and prediction, MIDI notes struck again) the
  joins no longer click; exact decoders give the same audio as before. Markers, `FA_EVENT_LOOPED` (reversed: from the
  loop start to its end), MIDI texts and tracker rows are now raised while reversed, last first. Deck loops, loop changes
  and beat jumps join without a gap while reversed instead of restarting like a seek, deck sync pulls a reversed deck
  into phase (its beats counting down), `fa_deck_play_synced` starts a reversed deck on the beat, and a beat loop set
  while reversed lies below the beat heard. Forward playback is unchanged. See
  [docs/features/tempo-pitch.md](docs/features/tempo-pitch.md#reverse-playback).
- AC-3 and E-AC-3 with more than two channels are mixed down to stereo with the stream's own centre, surround (and
  E-AC-3 LFE) levels and its Lo/Ro or Lt/Rt preference, as ETSI TS 102 366 describes, scaled so the mix cannot clip.
  Without normalization Dolby 5.1 therefore plays about 7.7 dB quieter than with the engine's generic downmix
  before; with normalization on, the gain gives that scaling back (the limiter catches the peaks).
  `FA_DOLBY_DOWNMIX_ENGINE` (.NET `DolbyDownmix.Engine`) keeps the old downmix. Decode-only reads still hand out every
  channel. Only channels that carry audio count, so stereo plus LFE still plays at full level.
- Chained Ogg files (several songs one after another) find where each song ends by bisection on the serial numbers,
  as vorbisfile does, instead of reading every page: an hour of 20 songs opens in about 6 ms instead of 41 ms. Links,
  lengths, decoded audio and seeks are the same as before. A file whose songs reuse a serial number (against the Ogg
  spec) is read through as before ([chained files](docs/features/streams.md#ogg-opus-and-ogg-vorbis)).
- Lyric surfaces: a sweeping wipe recolours and redraws only the columns its edge crossed since the last frame instead of
  the whole syllable, and a cross-fade only the syllable's own columns (no longer padded by the effects' reach). Frames
  are identical; a wipe-only frame costs about a tenth of before (test song: 0.15 to 0.016 ms at 1080p, 0.67 to
  0.07 ms at 2160p).
- FLAC encoder: a file written to a seekable output now has a SEEKTABLE, a point every 10 s as `flac` makes (fewer,
  further apart, past about 42 minutes), so players that rely on one seek at once. The audio frames are the same bytes as
  before; the file is 4,612 bytes larger (the table's room, what is left of it joins the padding). Forward-only outputs
  are unchanged. See [docs/features/recording-encoding.md](docs/features/recording-encoding.md#how-it-behaves).
- DJ mixer, with output identical to the bit: each deck converts its trim and EQ settings to gains (`powf`), and the
  crossfader its position to the two sides' gains (sin/cos, once for both decks), only when they change, instead of
  at both ends of every 16-frame piece of every deck: a two-deck mixer block runs about 7% fewer instructions with
  steady settings (6% while the crossfader and EQ move). A mixer made with `FA_MIXER_CUE_LOWEST_LATENCY` no
  longer allocates the master's alignment delay line (256 KB at 48 kHz), which it never uses, and its meters keep no
  room for that delay. A beat jump with a deck loop on moves the loop and the position in one join instead of three
  (loop off, jump, loop on), each of which waited for the audio thread and restarted the stretcher.
- Lyrics: surfaces that use the same font file (a main screen and an operator preview) share one copy of it in memory
  instead of reading it once each; a CJK font is 20-40 MB. The copy is freed when the last surface using it closes, and a
  file changed on disk is read again. Surfaces opening the same file at the same moment on different threads read it
  once too: the others wait for that read.
- CD+G effects are faster, with the same pixels: a frame whose colours change but not its shapes (a colour fade, a
  lyric being coloured in) blends the new colours with what the effects kept instead of working the effects out again;
  the distance field, the stroke widths and the blend use SSE2; and a large redraw is shared with the engine's idle
  worker threads (up to three). A dense 2160p song with every effect at maximum draws in about a third of the time,
  its worst frame 530 to 120 ms (four cores); a colour change at 2160p costs the picture's redraw plus a blend instead
  of 115-280 ms. What is kept costs memory with effects on: 5 bytes per output pixel with outline and shadow, 9 with
  the bevel (about 32 or 58 MB at 2160p). See [CD+G presentation options](docs/features/cdg.md#presentation-options-fa_cdg_surface_set_presentation).
- CD+G presentation scaling: a change of colours (a new colour table, a fade over several frames) redraws only what is
  drawn in the colours that changed, as far as smoothing and edge blend reach (the background is transparent whatever
  its colour, so a fade leaves it alone), and a large redraw of the picture is shared with the engine's idle workers in
  bands of rows. The same pixels. A fade frame of six lines of lyrics at 2160p with smoothness 4 and edge blend: 11.7 to
  5.4 ms, with every effect 21.9 to 9.7 ms; a full 2160p redraw at smoothness 4 with edge blend 10.4 to 5.8 ms. On
  64-bit ARM the effects' distance field, stroke widths and blend use NEON, with the same pixels as SSE2 on x86 and the
  plain C build; `surface_fx.c`, `cdg_scale.c` and `cdg_contour.c` are built without fused multiply-adds
  (`-ffp-contract=off`), so ARM and x86 with FMA give the same pixels too. See
  [CD+G presentation options](docs/features/cdg.md#presentation-options-fa_cdg_surface_set_presentation).
- Web streams: the requests of one stream share their connections (curl, WinHTTP, URLSession; Android's
  `HttpURLConnection` pools by itself), so an `https://` HLS start makes one TLS handshake instead of one per playlist,
  key and segment: with a 100 ms round trip the first audio comes about 0.5 s sooner. HLS byte-range segments ask for
  their exact range (`Range: bytes=N-M`) instead of everything from N on. See
  [web streams](docs/features/web-streams.md#connections).
- Web streams: FatzAudio follows redirects itself on every transport. WinHTTP and URLSession now follow an `https://`
  to `http://` redirect too, as curl and Android did, and like them keep no cookie store: a cookie a server sets
  during a redirect is not sent to the next hop any more (the app's own `Cookie` line still is).
- Web streams: one request-header walk for every transport (plain HTTP, curl, WinHTTP, URLSession,
  HttpURLConnection), so all of them send the same headers: `Accept: */*` and `Accept-Encoding: identity` everywhere
  now, an app header replaces the built-in one of the same name instead of being sent twice, and invalid app lines
  (no colon, a bad name, a value that isn't printable ASCII, over 8192 bytes, or `Host`/`Connection`/`Content-Length`/
  `Transfer-Encoding`) are dropped on every platform. Lines up to 8192 bytes now reach the server through curl too
  (the limit was 1024). See [docs/features/web-streams.md](docs/features/web-streams.md#request-headers).
- Faster ZIP and HLS, with the same output (2026-10-26): DEFLATE in ZIP karaoke archives decodes through lookup tables
  with a 64-bit bit buffer and word-wide match copies, about 3.3 times as fast (roughly 400 MB/s on one core, on a
  par with zlib); the CRC-32 check works on 8 bytes at a time, about 4.5 times as fast; AES-128 for encrypted HLS
  segments uses 32-bit decryption tables, about 7 times as fast (roughly 320 MB/s). Checked old against new on
  114,000 streams (about 100,000 of them damaged, which must fail the same way) and against zlib in a new test
  (`fa_test_inflate`, built where CMake finds zlib; zlib is never linked into the library).

- HE-AAC v2 streams share the parametric-stereo constant tables (14 KB) built once per process instead of one copy per stream. Output is unchanged.

- Faster and smaller, with the same output (deep audit, small items, 2026-10-26): the DJ deck isolator runs both
  channels and four filter sections per SSE step (a mixer callback takes about 38% fewer instructions, output identical
  to the bit); the running loudness estimate (about 7% of decoding) runs only with normalization on or once the app asks
  `fa_stream_get_loudness`; the ALAC bit reader keeps a 64-bit window (about 8% faster); a QuickTime PCM `.mov` opens in
  well under 1 ms instead of about 40 ms for 5 minutes; the MP4/MOV/CAF packet list takes about 6.5 bytes a packet
  instead of 24 (a 10-minute M4A stream 682 to 226 KB); a mono HE-AAC stream makes its second output channel only when
  parametric stereo shows up (about 20 KB); a DJ mixer without a cue skips its 24 KB cue buffers; the MP3 reader's window
  is 4 KB; the AAC output buffer is one frame exactly. Decoded audio and seeks were compared old against new on 39 files
  (MP4, MOV, CAF, fragmented, with video, ALAC, HE-AAC, MP3): identical.

- Faster, with output identical to the bit (deep audit, 2026-10-26): the output limiter skips its gain tracking
  while nothing comes near the ceiling (about half the cost of a quiet callback); the device resampler's stereo path
  (a 44.1 kHz file on a 48 kHz device) runs both channels in one SSE register, about 2x; raw PCM in MKV, MOV, CAF and
  push streams converts with one loop per sample format, 3-8x; HE-AAC clears only the bands it reads (4-10% less
  work); the lyric surface draws with SSE on x86, about 2x (a 4K partial frame 8.4 to 4.1 ms).
- Less memory per stream, same output (deep audit, 2026-10-26): MP4/M4A/MOV frees its movie box after open and makes
  its packet table at its size (a 10-minute M4A 987 to 723 KB, a 30-second one 173 to 149 KB); HE-AAC elements share
  one SBR scratch (5.1 738 to 618 KB); 16-bit ALAC no longer makes the low-byte buffers only 24 and 32-bit files use
  (157 to 107 KB); Ogg page buffers grow in 4 KB steps instead of doubling (Opus 155 to 140 KB); decoders' output
  buffers start at the first packet's size.
- FLAC encoding is faster (level 0: 1.2 times, level 5: 2.2 times, level 8: 3.4 times; about 400× real time on one
  x64 core at level 5, 170× at level 8) and level 5 to 8 files are smaller (against `flac` 1.4.3 on 8 music files:
  level 5 went from 6% larger to 0.3%, level 8 from 13% to 1.8%, mostly on 24-bit files; pure tones and sweeps
  are up to 10% smaller too). Level 8 is now faster than `flac -8`. The encoder
  sizes predictors with SSE2/NEON kernels without storing their residual, picks each Rice parameter in closed form,
  and from level 3 on costs only the most promising LPC candidates in full. The output still decodes bit-exact and
  passes `flac -t`.
- Internal clean-up from the 2026-10-26 deep audit, no change in behaviour: six internal functions nothing called,
  nine struct fields that were written but never read, two unused macros and the never-built AC-3 statistics
  counters are gone; the byte-order readers, string copies, pi and the peak-to-dB conversion that a dozen source files
  each wrote for themselves are shared (`src/fa_util.h`); MIDI and tracker files share one read-to-end helper.
  .NET: one internal UTF-8 helper replaces 28 hand-written conversions. The 24 white-box tests link one shared build
  of the library sources instead of compiling their own copy each.
- Linux: the library is about 460 KB smaller (4.49 to 4.05 MiB on Linux x64): FatzAudio's own code, miniaudio and
  the Xiph codecs are now built with a section per function, so the linker drops what is never called. Android
  builds were already made this way by the NDK, so their size did not change.
- CI prints the shipped size of every library it builds (Windows, Linux, macOS, each Android ABI, iOS) in the job
  summary and as a notice (`tools/lib-size.sh`).
- CI runs every test suite on an Android 14 emulator and in the iOS simulator (`devices.yml`): on pull requests that
  touch platform code, on every push to main, and by hand. See [docs/building.md](docs/building.md).
- Render-ahead (CD+G and lyric surfaces) is not used on an engine with one worker thread: a 2160p frame with effects
  there could hold the only worker longer than a tempo stream's buffer lasts. `update_into` draws on your thread.
- Lyric surfaces: the animation lengths follow `FA_PARAM_TEMPO` in steps of 0.25%, so render-ahead keeps working
  during a tempo ramp or a DJ sync (every frame used to be drawn on the caller's thread then), and they read the tempo
  without waiting for a seek in progress. `fa_lyric_surface_set_style` keeps the open font when the font fields are
  unchanged, and a `font_data` font is held once instead of twice; about 10 MB less line cache at 1080p (14 lines
  instead of 24).
- The idle decode worker that watches playing streams wakes as often as the emptiest buffer needs (a quarter of what
  it holds, 5 to 50 ms) instead of every 5 ms, so a playing engine wakes about 20 times a second instead of 200.
- Radio with a stated bitrate (`icy-br`) reads ahead twice `prebuffer_ms` plus 5 s, at least 256 KB, instead of 2 MB:
  about 1.8 MB less per 128 kbps station. Streams without `icy-br`, and HLS, keep 2 MB.
- The surface bevel reaches at most half as far in as a stroke's middle, so a thin stroke is never filled in by it
  (CD+G and lyric surfaces). Wide strokes look as before.
- CD+G: a surface allocates its own pixels on the first `fa_cdg_surface_update` instead of at create, so out of
  memory is reported there (`FA_ERROR_OUT_OF_MEMORY`), and its dirty rectangle is the box around the changed tiles' own areas, never larger than
  before. The pixels drawn are unchanged.
- MIDI note velocity now follows the SoundFont 2 default curve, 40·log10(velocity/127) dB (velocity 64 is 11.9 dB
  down, was 6 dB), which is what FluidSynth and DLS synths play. Songs keep their loudest notes and gain dynamics.
- MIDI: a program change to a program the soundfont lacks (in the bank and in bank 0) plays program 0 instead of
  keeping the channel's previous instrument. DLS instruments are no longer cut at 128 regions.
- .NET: `AudioStream.Info` no longer allocates on every read: the last `StreamInfo` is reused while the native values
  are unchanged (a changed duration, length or bitrate still gives a fresh one), and `GetSpectrum(int)` on streams
  and devices shares one code path with the `Span<float>` overload. For library scans, `Tags.Read(path,
  TagReadOptions.NoPictures)` skips the cover art (the default still copies it).

- Tracker modules no longer use the Amiga resampler by default (`FA_TRACKER_AMIGA_RESAMPLER` 0, .NET
  `TrackerAmigaResampler` false), as in libopenmpt: it cost about 19 times the CPU on MOD files. MOD files now sound
  cleaner and brighter; set it to 1 for the Paula sound.
- Less CPU and memory, same sound (memory and speed review): device-rate conversion (44.1 kHz on a 48 kHz device)
  works its kernels out once per ratio, about five times cheaper (a resampled stream went from about 3.4% to
  0.6% of one core); the output limiter measures true peaks several frames at a time and skips quiet blocks
  (bit-identical, about 2x); the equalizer runs all bands frame by frame, left and right together (bit-identical,
  about 2x); Ogg Opus streams no longer hold a 184 KB buffer; FLAC in Matroska, MP4 and Ogg holds output for its
  own channels (not 8) and stops allocating per packet (about 190 KB less per stream); a resampled stream keeps
  32 KB less history; web streams no longer keep a 256 KB read-ahead on top of their download window; idle
  engine workers wake 20 times less; Windows (MSVC) builds read atomics with plain loads instead of locked
  compare-exchanges. .NET: `HasCdg` and the `Tags` lookups (`Cover`, the indexer) no longer allocate.
- Less memory per stream, same sound (memory review, part 2): a web stream that can't seek (radio, HLS, a server
  without byte ranges) keeps only the 2 MB ahead of playback, not a 4 MB window (about 2.1 MB less per radio
  stream); push and other forward-only streams reserve the tempo chain's half second of history only once tempo,
  pitch or rate is used (188 KB at 48 kHz stereo); decoded audio is converted straight into the decode-ahead
  buffer (16 KB less); a stream's own state is 14 KB instead of 24 KB (its tempo map, equalizer, push timestamps
  and the unused true-peak meter are made only when used); levels and spectrum keep 1 s plus the device's latency
  and callbacks instead of about 2.7 s (512 KB instead of 1 MiB at 44.1 and 48 kHz on most devices); HE-AAC keeps
  about 75 KB less SBR state per stereo element and 190 KB less for HE-AACv2 (parametric stereo); CD+G checkpoints
  take half the memory.
- HE-AAC decodes about 35% faster (its QMF analysis bank uses a fast DCT-IV; 16 KB less per decoder) and ALAC
  up to 30% faster (faster bit reading). The output is unchanged.
- Less engine CPU, same output: the mixer mixes a stream whose gains hold steady in one pass instead of every 16
  frames (bit-identical; 16 playing streams mix in about half the time); idle decode workers no longer each poll
  every 5 ms (one polls and wakes the others when there is work: about 320 wake-ups a second instead of 200 per
  worker); device-rate conversion of 5.1 and 7.1 sums its channels with SSE2, about 3x faster (per channel now
  cheaper than stereo); CD+G smooth scaling redraws 1080p 2.5 to 6 times faster (pixel-identical).
- Faster codecs, same output (bit-identical on x86-64; on ARM, where compilers fuse multiply-adds differently, within
  float rounding): the inverse MDCT's rotations run four at a time (SSE on x86, NEON on ARM), about 13%
  off AC-3 and AAC decoding; HE-AAC's high band works out its linear prediction in one pass and keeps its filter
  bank delay lines as rings instead of moving them every slot (with the MDCT, HE-AAC about 25% faster); ALAC sums
  its predictor in 32 bits where that gives the same result, as Apple's decoder does (about 10%); the FLAC encoder
  is about 1.7x faster with byte-identical files (32-bit residuals, a table CRC, all autocorrelation lags at once,
  the 5-bit Rice method skipped where it cannot win); the MIDI chorus turns its LFO instead of calling `sin` four
  times a frame (about 10% off MIDI rendering, within 4e-9 of before).
- A tempo, pitch or rate ramp's `FA_EVENT_RAMP_DONE` now comes when the ramp's end is heard, not up to the
  decode-ahead latency (150 ms) earlier when it was decoded, matching volume ramps. See
  [docs/features/tempo-pitch.md](docs/features/tempo-pitch.md).
- Playback at another rate than the device's (a 44.1 kHz file on a 48 kHz device) is resampled with the windowed-sinc
  resampler that rate and pitch use, instead of miniaudio's linear one: about -90 dB of aliasing and imaging, and
  high frequencies no longer dulled. See [docs/features/playback.md](docs/features/playback.md).
- Frames pushed at another rate than their push stream's (`fa_stream_push_frame`), and tracks whose rate changes
  partway (chained Ogg), are resampled with the same windowed-sinc resampler (a 17 kHz tone pushed at 44.1 kHz into
  a 48 kHz stream: its image went from 8 dB to 126 dB below the tone).
- Proxy settings (`proxy` in the net options) are read the same way on every platform: `host:port` or
  `http://user:pass@host:port/` (an IPv6 literal in brackets, port 80 when none, any path ignored); a setting that is not
  an http proxy is refused alike by WinHTTP, Android and Apple (before, each parsed it its own way).
- Memory: the lyric/text rasterizer gives its buffer back when a layer is much smaller than the biggest one before it
  (a damaged font's huge metrics no longer keep up to 265 MB for the surface's life); a mono HE-AAC v2 stream no longer
  allocates a whole channel structure (about 14 KB) for its right channel.
- Internal cleanup with identical output: one run-once helper, one Bessel/KBD window code, one device list publisher, one
  copy of the effects' tile, shadow and bevel code, and the dead exports and unused arguments removed.

### Fixed

- Float PCM (WAV, Wave64, RF64, AIFF-C, CAF, MP4/MOV, Matroska, ASF, push streams): NaN and infinite samples, and
  doubles too big for a float, now read as 0. One such sample in a damaged file used to stick in every filter, effect
  and meter after it. Found by fuzzing (`tests/fuzz/regress/stream/wav-float-nan.wav`).
- WAV/AIFF: a damaged header with a huge frame count overflowed the audio byte count (undefined behaviour); the byte
  count is now left unknown. Found by fuzzing.
- AC-3/E-AC-3: a track whose first packet was empty passed a null buffer to `memmove` (undefined behaviour, no crash
  seen). Found by fuzzing.
- A container that claims a sample rate over 768 kHz (a damaged ASF, MP4 or Matroska PCM track) is no longer decoded:
  the silence for a gap before its first packet could ask for gigabytes. A gap whose silence would pass 2^26 samples
  (more than 30 s of 32 channels at 48 kHz) is no longer filled, as a gap over 30 s already was not. Found by fuzzing.
- .NET for iOS: the binding also tries `@rpath/fatzaudio.framework/fatzaudio` when the plain library name is not found. CI now runs the binding in a real .NET for iOS app (simulator) and a .NET for Android app (emulator), see `tools/dotnet-apps/`.
- Ogg Opus and Ogg Vorbis: a song whose only audio page is also its last one (a short clip) came out 650 to 970 frames
  off when played straight through, on its own or chained after other songs: its last page was timed back from its
  granule without its end trim, while a seek timed it forward from its start. A chained file's later songs then played
  that much off against their seek positions. Playing through now times it as a seek does (`tests/data/ogg`
  `r128.opus`, `rg.ogg`, `surround.ogg` and `tags/tags.opus` now line up with FFmpeg's decode).
- FLAC: after a seek that failed partway (a damaged or cut-off `.flac`), the next seek back inside the frame that
  was playing could return samples from the wrong place in it. The vendored dr_flac now drops the decoded frame
  before any seek reads on past it (`third_party/miniaudio/PATCHES.md`).
- HLS: an `EXT-X-MEDIA-SEQUENCE` of 2^63-1 overflowed the segment counter while parsing (now treated as 0), and an `EXT-X-BYTERANGE` with an offset near 2^63 (or a negative or empty length) overflowed the next range's offset
  while the playlist was parsed; such a range is now ignored and the whole segment is fetched. Found by fuzzing.
- A damaged `.flac` could crash the decoder (a null read in dr_flac) when a seek followed a frame that failed to decode.
  Found by fuzzing the file readers (`tests/fuzz`); the vendored decoder now keeps no half-decoded frame
  (`third_party/miniaudio/PATCHES.md`).
- Threading (audit of 2026-10-27):
  - Deck loops: two loop changes in a row (a failed reversed jump, a beat jump while stopped) could let the audio thread
    read a half-written loop for one block, the old out point with the new in point. The loop is now read whole.
  - A deck loop change or loop jump whose seek fails (an I/O error) returns the error and keeps the old loop, instead of
    reporting the new one. The stream still ends where the change would have joined, as before.
  - Unloading a deck whose loop has turned no longer re-decodes the loop join (device wait, seek and 100 ms of decoding
    under the mixer lock) only to throw it away.
  - Lyric surfaces: turning render-ahead off while another thread drew a frame could make both render-ahead buffers
    again (up to about 66 MB at 2160p) and keep them while the feature was off.
  - Lyric drawing no longer allocates a buffer per scaled or moving line per frame (about 280 KB each at 3840 wide).
  - Opening the same font file from two threads at once: the second opener waits on a condition variable instead of
    polling every millisecond (about 15.6 ms per poll on Windows), and the list lock is a real mutex.
- Damaged and hostile files (audit of 2026-10-27):
  - HE-AAC with a 64, 88.2 or 96 kHz core (which the standard doesn't allow with SBR) could hang the decoder forever
    building its SBR tables. SBR data with a core above 48 kHz is now ignored and the core plays alone.
  - A damaged HE-AAC frame whose SBR envelope or noise levels step past the standard's range gave infinite, then NaN,
    samples that reached the output and stuck in the EQ and resampler. Such a frame now counts as damaged.
  - HE-AAC v2: a damaged parametric stereo header could leave a mode past the tables for the next frames (an
    out-of-bounds read), and a fine-mode level held over a switch to coarse steps read past its table.
  - MP3 in a packet stream (MPEG-TS, pushed streams) changing between mono and stereo or between rates: the first frame
    after the change was decoded twice and came out wrong.
  - Tags: at most 65,536 items, 1,024 pictures and 64 KB for a field joined from a repeated key are kept (a file with a million repeated
    `ARTIST` comments took minutes to read); an empty ID3v2 frame no longer ends the tag (the frames after it were
    lost); an APEv2 item's key is read in one go instead of byte by byte; a huge `WM/Track` number no longer overflows.
  - Matroska: Cues times and block durations so large they overflow once scaled are clamped, and a second Tracks
    element no longer leaks the first one's header-stripping bytes.
  - MP4: a chunk or fragment offset near the largest 64-bit value (a damaged `co64` or `tfhd`) is refused instead of
    overflowing.
  - A WAVE disc image whose data chunk claims more than the file holds (a cut-off file, or one written as a stream with
    length 0xFFFFFFFF) gets its length from the file instead of hours of missing sectors.
  - ZIP: an entry claiming more than 4 GB unpacked returns `FA_ERROR_TOO_LARGE` instead of filling the temp directory.
- Web streams (audit of 2026-10-27):
  - Single-file fMP4 HLS (`#EXT-X-MAP:URI="main.mp4",BYTERANGE="720@0"`) plays. The init section's byte range was
    ignored, so the whole file was fetched as the init section: over 2 MB the stream failed as unsupported, below it
    the audio was garbled. A malformed `BYTERANGE` now refuses the playlist.
  - Your `Authorization` and `Cookie` header lines are sent only to the stream URL's own server. A playlist (`.pls`,
    `.m3u`, HLS) naming entries, segments or keys on another server used to send them there too, although a redirect
    there already dropped them.
  - Timed waits inside the library (download, encoder and queue polls) no longer stretch when the wall clock is set
    back (NTP, the user changing the time): on Linux and Android they run on the monotonic clock, on Apple systems
    they are relative. Windows already was.
  - A seekable web file smaller than 4 MB gets a download buffer of its own size plus 64 KB instead of 4 MB
    (16 short jingles: about 7 MB instead of 64 MB).
- Looping a source whose length isn't known (a container without one, such as a FLAC written to an output that can't seek): the
  position now wraps and `FA_EVENT_LOOPED` is raised at each jump. Before, it looped where the decoder ended but the
  position ran on past the end and no event came. The end is taken as the length from the first jump on.
- `fa_encoder_close` on an encoder converting a web stream (`fa_encoder_encode_stream`) returns at once while the
  download stalls. It used to wait in the network read for data or the network timeout, holding the lock every encoder
  call takes. The encoder now reads the stream only once enough is downloaded, as playback does.
- Bitrate and `audio_bytes` of an MP3 with a Xing/LAME header frame (VBR files, and LAME's CBR ones) no longer count
  that frame as audio.
- AAC seeks: HE-AAC and HE-AAC v2 (SBR and PS) came out up to 0.35-0.86 off a straight decode for the first frames
  after a seek, because only one packet before the target was decoded. Now 12 packets are decoded and thrown away
  first, and on the test streams a seek gives exactly a straight decode's audio; a seek costs 0.1-0.5 ms more for
  stereo, about 1 ms for 5.1 HE-AAC. The noise generator for noise-filled bands now starts again on a seek to the
  start, so that is exact for every AAC stream. Deeper into a stream, noise-filled bands and SBR's noise are a
  different stretch of noise than a straight decode's (the generators run through the whole stream), and AAC Main
  and LTP still settle more slowly ([AAC](docs/features/streams.md#aac)).
- Web streams: credentials from the URL (`https://user:password@host/...`) were still sent after a redirect to
  another server on Windows (WinHTTP) and Apple (URLSession). Now they go only to the URL's own scheme, host and port,
  and the app's `Authorization` and `Cookie` header lines likewise, on every transport
  ([redirects](docs/features/web-streams.md#redirects)).
- Output limiter: after a reduction the gain now releases all the way back to 1. It used to stop about 0.002 dB short
  (float rounding in the release step), so everything after the first loud peak played that much quieter until the
  device was reset. Output after a reduction changes by that amount; audio that never reached the ceiling is unchanged.
- Text that claims to be UTF-8 (ID3v2, Vorbis, APE and MP4 tags, ICY stream titles and `icy-*` headers) is checked the
  same strict way MIDI text already was: overlong forms, UTF-16 surrogates and code points above U+10FFFF are no longer
  passed through as UTF-8 but read as Latin-1, as other text that is not UTF-8. Well-formed UTF-8 is unchanged.
- HTTPS: every backend (curl, WinHTTP, Apple, Android) now refuses a response whose headers exceed 64 KB with
  `FA_ERROR_NETWORK`, as plain HTTP already did. Before, only plain HTTP had a limit and the others took any size.
- MIDI: a GS, XG or universal SysEx message cut short (its F7 where the value goes) is ignored. It used to read the
  F7 as the value, so a broken GS master volume message nearly doubled the level.
- DJ mixer: a beat jump with a deck loop on can no longer leave the deck jumped with its loop off when a step fails:
  the loop and the position move in one join, or not at all. `fa_deck_load` of a stream on a deck of another mixer
  now takes it off that deck under that mixer's lock too (both locks in a fixed order, so two mixers loading each
  other's streams at once cannot deadlock); before, a load on the other mixer at the same moment could detach the
  stream just loaded, leaving a deck holding a stream that played nowhere. The audio thread reads a deck loop once
  per helper and once per stream mix, so a loop changed while playing no longer mixes the old out point with the new
  in point for a block.
- DJ mixer: changing the crossfader curve with the crossfader away from the ends glides over 5 ms instead of
  stepping the decks by up to 3 dB. `fa_stream_get_loop_region` no longer returns the in point of one change with the
  out point of another.
- DJ mixer: deck loops, beat jumps and sync's jumps no longer drop a playing deck out for a moment. A jump waiting for
  its target to be decoded no longer pauses the deck's mix on every retry (about 90 times per sync jump), the cut is
  made between two audio callbacks, and waits for a callback yield instead of sleeping (`Sleep(1)` on Windows can take
  15.6 ms). Seeks and closes wait less for the same reason.
- DJ mixer: a deck's stream moved elsewhere or detached (not only closed) now leaves its deck under the mixer's lock, so
  a deck call or the event thread can no longer use it after it is freed. `fa_mixer_destroy` racing the event thread,
  or a stream closed on another thread, could touch the freed device or mixer.
- DJ mixer: a tracker module's exact grid is rendered in the background instead of under the mixer's lock (which held
  up the event thread and every deck call for up to a second); see
  [docs/features/dj-mixer.md](docs/features/dj-mixer.md#sync-and-beat-loops).
- DJ mixer mics: removing a mic (an `InputLost` handler, say) while the setup check measures it no longer frees memory
  the check still reads. A mic on the default input is no longer reported lost when the system's default recording
  device changes (only a loss that lasts 300 ms is). `fa_mixer_set_latency_mode` can no longer slip in while a setup
  check starts. The mic bridge gives back the latency it added after underruns (1 ms per 30 s without one), drops
  what it held from before a stall instead of playing it late, and `mic_monitor` follows what it holds now. No more
  denormals as the echo dies away or a deck's isolator goes quiet.
- CD+G and lyric surfaces with render-ahead: `fa_surface_update_into` could copy from a frame buffer that another
  thread's `fa_surface_set_presentation_size` (and a new layout) or `fa_surface_set_render_ahead(s, 0)` was freeing.
- Lyric surfaces: a damaged font's vertical metrics (a tiny em with huge ascender and descender) made line layers and
  the intro card of gigabytes; such a font now gets the usual 0.9 em ascender and 1.2 em line, layers are never taller
  than the area, and a line that cannot be drawn is not tried again every frame. One glyph's outline is flattened
  into at most 2^20 pieces, so a crafted glyph cannot take seconds.
- Lyric surfaces: a crafted glyph of many long lines (400k lines across a 2160p layer took about 9 s to draw) now
  stops after 2^22 pixel cells, about 60 ms; the busiest glyph of a CJK font needs about 12k at 2160p, and every glyph
  of the test fonts and WenQuanYi Zen Hei draws with the same pixels as before.
- Lyric and CD+G surfaces: `fa_surface_update_into` read the layout revision without a lock while
  `fa_surface_get_layout` or a drawing call on another thread wrote it (a data race ThreadSanitizer reports; the value
  was right in practice).
- CAF: a `pakt` chunk is read in growing pieces, so a source of unknown length no longer allocates its claimed size
  (up to 256 MB) up front, and the packet table never has more entries than the chunk can hold.
- MIDI: a Set Tempo under 600 µs a quarter no longer makes the exact beat grid invalid (which made the analysis cache
  be rewritten on every analysis); a beats-only analysis of a MIDI file no longer renders the song to count its
  frames. Shift-JIS lyrics with a stray byte (at most one in 50 characters), or a character cut between two events,
  are no longer read as Windows-1252.
- .NET: `DjDeck.Load` holds a reference on the stream for the call, so a `Dispose` on another thread cannot free it
  under the native call. `DjMixer.Mics` no longer copies the list on every read, and `TrackAnalysis.Waveform` is
  read without a second copy.
- CD+G: copies of a memory preset (repeat count above 0) no longer clear the screen again when a border or tiles
  were drawn between the copies (reported by FatzBomb.MediaControl).
- CD+G from discs: packs that fail their P/Q parity are corrected (one damaged symbol) or dropped, instead of
  drawing garbage tiles from a scratched disc. Drives that return no parity are unchanged.
- CD+G: `fa_cdg_surface_update`, `fa_cdg_surface_update_at` and `fa_cdg_surface_destroy` could touch freed memory
  when the surface's stream was closed on another thread at the same moment; the close now waits for them.
- Transient mode (`FA_STRETCH_TRANSIENTS`) now also removes the pre-echo of a hit in the last ~100 ms of a track
  (-49 dB instead of -20 dB).

## 0.2.0 (2026-10-18)

The first packaged version (NuGet `FatzAudio` with `win-x64` and `linux-x64` natives).

### Breaking (against 0.1.0 builds from main)

- `fa_dsp_callback` gained a `sample_rate` argument: `(frames, count, channels, sample_rate, user)`.
  .NET: `DspProcessor(Span<float> samples, int channels, int sampleRate)`. User DSP never worked before
  this version (it returned `FA_ERROR_NOT_IMPLEMENTED`).
- `fa_ffmpeg_load` / `fa_ffmpeg_get_info` and .NET `FFmpeg.Load` / `FFmpeg.Info` were removed: FatzAudio
  never loads FFmpeg. Formats only FFmpeg decodes (DTS, TrueHD, WMA off Windows) return
  `FA_ERROR_UNSUPPORTED_FORMAT`; decode them with your own FFmpeg and use the packet hand-off.
- The MinGW build is now named `fatzaudio.dll` (was `libfatzaudio.dll`), like the MSVC build.
- The Linux `.so` exports only `fa_*` symbols and links the C++ runtime in.
- The Linux SONAME is now `libfatzaudio.so.0.2` (was `libfatzaudio.so.0`).

### Added

- Equalizer: up to 16 peak or shelf bands per stream, 13 presets, bypass (`fa_stream_set_eq`,
  `fa_eq_get_preset`; .NET `SetEqualizer`).
- User DSP: `fa_stream_add_dsp` / `fa_stream_remove_dsp` (.NET `AddDsp`).
- Looping on or off while playing (`fa_stream_set_loop`, `FA_EVENT_LOOPED`; .NET `Loop`, `Looped`).
- Mute for streams and devices that keeps the volume (`fa_stream_set_mute`, `fa_device_set_mute`; .NET `Muted`).
- Device changes: an unplugged device is reopened on the default with its streams playing on, and
  `FA_EVENT_DEVICE_CHANGED` (.NET `DeviceChanged`) is raised for reroutes and reopens.
- `fa_stream_get_clock` is lock-free for streams on a device and file streams, safe from a render loop.
- Tempo and rate on push and packet streams; the clock keeps the app's timestamps and runs at the speed heard.
- Seeking without waiting: `fa_stream_seek_async` (.NET `RequestSeek`), for seek bars and web streams.
- Tag reader with cover art and file properties (`fa_tags_*`; .NET `Tags`); bitrate, file size and audio
  size in `fa_stream_info`.
- MIDI: DLS soundfonts and the operating system's own soundfont as the default.
- MIDI: SF2 instrument attenuation follows the SoundFont spec (quiet instruments in some SF2 banks were
  up to 10x too loud).
- MIDI: GM/GM2/GS/XG SysEx (resets, drum parts on any channel, XG and GM2 drum banks, master volume and
  tuning); notes held across a seek start again at the target; tempo changes and time and key signatures
  (`fa_stream_get_midi_marks`; .NET `MidiMarks`).
- MIDI: SF3 soundfonts (SoundFont 2 with Ogg Vorbis samples) load with `fa_soundfont_open_path` /
  `fa_soundfont_open_memory` (.NET `SoundFont.FromFile` / `FromMemory`); samples are decoded to 16-bit at load.
- MIDI: built-in reverb and chorus, on by default and driven by CC91, CC93 and the soundfont's effect sends;
  `fa_stream_set_midi_effects` / `fa_stream_get_midi_effects` (.NET `AudioStream.MidiEffects`) turn them off.
- Levels and spectrum (FFT) for streams and devices (`fa_stream_get_levels`, `fa_stream_get_spectrum`,
  `fa_device_get_levels`, `fa_device_get_spectrum`; .NET `GetLevels`, `GetSpectrum`).
- ZIP64 archives (over 4 GB, or more than 65,535 entries) open like any other karaoke ZIP.
- Everything else built so far: decoding (MP3, AAC, ALAC, FLAC, Opus, Vorbis, AC-3/E-AC-3, WMA on Windows,
  MIDI), web streams and HLS, crossfades, markers, buses, R128 normalization, tempo and pitch, CD+G,
  disc playback, encoding (WAV, FLAC, MP3 via LAME) and the FFmpeg packet hand-off. See
  [docs/features](docs/features/README.md).

## 0.1.0

Development builds from main before packaging. Not released.
