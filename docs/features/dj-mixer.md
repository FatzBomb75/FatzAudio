# DJ mixer

A **DJ mixer** puts two decks on one output device, the way a two-channel DJ mixer does: each deck has a channel fader, a trim, a three-band isolator EQ with kills and a meter; a crossfader sits between them; then a master gain and the output limiter, which is always on. A headphone **cue** lets you hear a deck before the crowd does.

```
 deck: your stream (decode, tempo/pitch, its own EQ, volume, fades)
   -> trim -> isolator (low / mid / high) -+-> cue send (pre-fader)
                                           '-> fader -> meter -> crossfader side
 decks -> master gain -> limiter -> master output (channels 1-2)
 cue = (1 - cue mix) x cue sends + cue mix x master, x cue gain -> headphones
```

Decks hold ordinary streams. Play, pause, seek, tempo, pitch, markers, CD+G and lyrics work on them as on any stream, and both decks share the master device's clock, so `fa_stream_play_at` on two decks starts them on the same frame. The mixer opens and owns its output device: use `fa_mixer_get_device` for the clock, levels, spectrum and recording of the master mix.

## C

```c
fa_mixer_config cfg;
fa_mixer_config_init(&cfg);           /* 2 decks, default device, low latency, no cue */
cfg.cue_mode = FA_CUE_CHANNELS;       /* headphones on outputs 3-4 of a 4-output interface */

fa_mixer* mixer;
if (fa_mixer_create(engine, &cfg, &mixer) == FA_ERROR_NOT_ENOUGH_CHANNELS)
    cfg.cue_mode = FA_CUE_SPLIT, fa_mixer_create(engine, &cfg, &mixer);   /* Y-cable */

fa_deck *a, *b;
fa_mixer_get_deck(mixer, 0, &a);      /* side A of the crossfader */
fa_mixer_get_deck(mixer, 1, &b);      /* side B */

fa_deck_load(a, song_a, 0);           /* the deck doesn't own the stream */
fa_deck_load(b, song_b, 0);
fa_stream_play(song_a);

/* Pre-listen deck B in the headphones, bass cut, then bring it in. */
fa_deck_set_param(b, FA_DECK_CUE, 1);
fa_deck_set_param(b, FA_DECK_EQ_LOW_DB, FA_DECK_EQ_KILL_DB);
fa_stream_play(song_b);
fa_ramp shape = { FA_CURVE_LINEAR };
fa_mixer_ramp_param(mixer, FA_MIX_CROSSFADER, 1.0f, 8000000000LL, 0, &shape);   /* 8 s to B */
fa_deck_ramp_param(b, FA_DECK_EQ_LOW_DB, 0.0f, 8000000000LL, 0, &shape);         /* bass back in */

/* A four-beat loop at 128 BPM from the current position. */
fa_frames pos; fa_stream_get_position(song_b, &pos);
fa_frames beat = 44100 * 60 / 128;    /* the stream's sample rate (fa_stream_get_info) */
fa_deck_set_loop(b, pos, pos + 4 * beat);
fa_deck_set_loop(b, 0, 0);            /* off: plays on from where it is */

fa_mixer_destroy(mixer);              /* unloads the decks; close the streams yourself */
```

## C#

```csharp
using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Channels34 });
DjDeck a = mixer.Decks[0], b = mixer.Decks[1];

a.Load(songA);
b.Load(songB);
songA.Play();

b.Cue = true;                         // pre-listen B
b.Eq.Low = DeckEq.Kill;
songB.Play();
mixer.Crossfader.Ramp(1f, TimeSpan.FromSeconds(8));
b.Ramp(DeckParameter.EqLowDb, 0f, TimeSpan.FromSeconds(8));

b.CuePoint = songB.PositionFrames;    // CDJ cue: ReturnToCuePoint() pauses and goes back
long beat = songB.Info.Format.SampleRate * 60 / 128;
b.Loop = new DeckLoop(songB.PositionFrames, songB.PositionFrames + 4 * beat);
b.Loop = null;                        // off

var levels = a.GetLevels();           // the channel meter
```

`DjMixer.Dispose` unloads the decks (even playing ones) and closes the output; the streams stay open for you to dispose. A stream that is disposed, or played on another device or bus, leaves its deck by itself.

## Cue (headphones)

| Mode | C | .NET | What you get |
|---|---|---|---|
| None | `FA_CUE_NONE` | `CueOutput.None` | Master only |
| Channels 3-4 | `FA_CUE_CHANNELS` | `CueOutput.Channels34`, `CueOutput.Channels(n)` | Master on outputs 1-2, cue on two more outputs of the same device (DJ controllers, 4-output USB interfaces). Same clock: no drift, no delay. `cue_channel` picks the first cue output (counted from 0, default 2). `FA_ERROR_NOT_ENOUGH_CHANNELS` when the device has too few |
| Split | `FA_CUE_SPLIT` | `CueOutput.Split` | One stereo output and a Y-cable: master in mono on the left, cue in mono on the right |
| Second device | `FA_CUE_DEVICE` | — | Planned (`FA_ERROR_NOT_IMPLEMENTED`) |

The cue send is taken before the channel fader, so a deck with its fader down can still be heard in the headphones. `FA_MIX_CUE_MIX` blends from cue only (0, the default) to master only (1), and `FA_MIX_CUE_GAIN` sets the headphone level. The master part of the blend is taken after the limiter, as the crowd hears it.

## Parameters

| Parameter | Range | Default |
|---|---|---|
| `FA_MIX_CROSSFADER` / `Crossfader.Position` | 0 (A) .. 1 (B) | 0.5 |
| `FA_MIX_MASTER_GAIN` / `MasterGain` | 0..4, linear, before the limiter | 1 |
| `FA_MIX_CUE_MIX` / `CueMix` | 0 (cue) .. 1 (master) | 0 |
| `FA_MIX_CUE_GAIN` / `CueGain` | 0..4, linear | 1 |
| `fa_mixer_set_crossfader_curve` / `Crossfader.Curve` | constant power (each side −3 dB in the middle, steady loudness) or linear (−6 dB, a slight dip) | constant power |
| `FA_DECK_GAIN` / `Gain` | 0..1, the channel fader | 1 |
| `FA_DECK_TRIM_DB` / `TrimDb` | −12..+12 dB, on top of loudness normalization | 0 |
| `FA_DECK_EQ_LOW_DB`, `_MID_DB`, `_HIGH_DB` / `Eq.Low`, `Eq.Mid`, `Eq.High` | `FA_DECK_EQ_KILL_DB` (−60, silent) .. +6 dB | 0 |
| `FA_DECK_XFADER_SIDE` / `Side` | thru (0), A (1), B (2) | deck 1 A, deck 2 B |
| `FA_DECK_CUE` / `Cue` | off / on | off |

## Rules

- Sets glide over 5 ms; ramps take any curve and an optional start time on the device clock, like stream ramps ([ramps](ramps.md)). Out-of-range values are clamped. Side and cue switch at once and can't be ramped. Mixer and deck ramps raise no events.
- The isolator splits the deck at 250 Hz and 2.5 kHz with fourth-order Linkwitz-Riley crossovers, so with every band at 0 dB the bands add back to the original exactly in level (the phase turns gently around the crossovers, as on hardware isolators). It runs always, so moving a knob never clicks.
- `fa_deck_load` attaches the stream to the mixer's device through the deck, from wherever it was, keeping its position. Loading over a stream that is playing and heard (fader up and the crossfader not closed on its side) is `FA_ERROR_BUSY` unless `FA_DECK_LOAD_FORCE` (`force: true`). Loading a stream that isn't playing also shortens its decode-ahead to 60 ms, so tempo and pitch moves are heard quickly.
- A deck holds one stream. `fa_crossfade` or `fa_stream_set_next` from a deck's stream to a stream that isn't attached yet is `FA_ERROR_INVALID_ARG`: load the next song on the other deck and use the crossfader. A crossfade between the streams of two decks works.
- Close (dispose) a deck's stream on the thread that drives the deck, or unload it first: closing it while another thread is inside a call on that deck isn't safe.
- The device mute (`fa_device_set_mute`, `Output.Muted`) silences the headphones too.
- The cue point (`fa_deck_set_cue_point`, `CuePoint`) defaults to the stream's CUE_IN marker, else 0. `fa_deck_cue` (`ReturnToCuePoint`) pauses the deck and goes back to it.
- The meter (`fa_deck_get_levels`) shows the deck after trim, EQ and fader, before the crossfader, as heard now.
- `latency` defaults to `FA_LATENCY_LOW`: on Windows the WASAPI low-latency shared mode at the device's own rate (needs a driver that offers small periods, most do), about 5 ms periods elsewhere. `FA_LATENCY_NORMAL` uses the engine's period. `FA_LATENCY_EXCLUSIVE` comes with the mic setup check.
- Closing the mixer's device, or the engine, destroys the mixer. In .NET, `DjMixer` keeps its `OutputDevice`, so dispose the mixer, not its `Output`.

## Deck loops

`fa_deck_set_loop(deck, in, out)` (`Loop`) loops between two source frames, at any time, also while the deck plays. The loop is made where the deck is decoded, so it is seamless (no gap, no fade), follows tempo and pitch, raises `FA_EVENT_LOOPED` (`Looped`) at each turn, and the position and clock report it as heard.

- Setting an out point the deck has only just passed (up to 0.5 s ago, as a "loop out" button does) loops back at once and keeps the loop's phase. An out point further behind jumps to the in point.
- `out <= in` turns the loop off; playback carries on from where it is. A seek past the out point also leaves the loop; a seek inside it keeps it.
- While on, the deck loop replaces the CUE_IN..CUE_OUT loop of `fa_stream_set_loop`. Unloading the deck clears it.
- Only seekable streams loop (`FA_ERROR_NOT_SEEKABLE`): files, memory and discs, not web radio.

## Microphones

Up to two karaoke mics (`FA_MIC_MAX`), mixed into the master before the master gain and the limiter, so the limiter catches a shout:

```
 input device -> bridge (drift-corrected) -> low cut -> gain -> gate -> EQ -> meter
   -> mute -> pan -+-> master (when monitored)
                   +-> reverb send -> shared reverb -+-> master
                   '-> echo send ---> shared echo ---'
 host mics -> ducker -> gain on the music (the decks)
 mics with their effects -> vocals recording tap
```

```c
fa_mic_config mc;
fa_mic_config_init(&mc);            /* the default input, all its channels summed, heard */
mc.device_id = info.id;             /* an FA_DEVICE_CAPTURE device from fa_device_get_info */
mc.channel = 0;                     /* input 1 of a 2-input interface; a second mic takes 1 */

fa_mic* mic;
fa_result r = fa_mixer_add_mic(mixer, &mc, &mic);   /* FA_ERROR_PERMISSION_DENIED: ask for it first */
fa_mic_set_param(mic, FA_MIC_GAIN_DB, 6);
fa_eq_band eq[10]; uint32_t n;
fa_eq_get_preset(FA_EQ_PRESET_LIVE_VOCAL, eq, &n);
fa_mic_set_eq(mic, eq, n);

float peak, rms;
fa_mic_get_levels(mic, 50000000, &peak, &rms);       /* the singer's meter */

fa_mixer_latency lat = { sizeof lat };
fa_mixer_get_latency(mixer, &lat);  /* lat.mic_monitor[0]: voice to speaker, in ns */
fa_mic_remove(mic);
```

```csharp
var input = engine.InputDevices.First(d => d.Name.Contains("Scarlett"));
using var mic = mixer.AddMic(new MicOptions { Device = input, Channel = 0 });
mic.GainDb = 6;
mic.SetEqualizer(EqualizerPreset.LiveVocal);
mixer.InputLost += (_, e) => e.Mic.Dispose();   // unplugged: add it again when it is back
MicLevels level = mic.GetLevels();
TimeSpan delay = mic.MonitorLatency;            // voice to speaker
```

| Setting | C | .NET | Range | Default |
|---|---|---|---|---|
| Gain | `FA_MIC_GAIN_DB` | `GainDb` | −20..+40 dB | 0 |
| Mute | `FA_MIC_MUTE` | `Muted` | off / on, glides over 5 ms | off |
| Pan | `FA_MIC_PAN` | `Pan` | −1 (left) .. 1 (right), balance | 0 |
| Gate | `FA_MIC_GATE_DB` | `GateDb` | −100 (off) .. 0 dB | −50 |
| Low cut | `FA_MIC_LOW_CUT` | `LowCut` | an 80 Hz high-pass, on / off | on |
| Monitor | `FA_MIC_MONITOR`, `fa_mic_config.monitor` | `Monitor`, `MicOptions.Monitor` | heard through the master, or metered only (a mic that goes through a hardware mixer) | heard |
| EQ | `fa_mic_set_eq` | `SetEqualizer` | the stream EQ's bands and presets ([equalizer](equalizer.md)); `FA_EQ_PRESET_LIVE_VOCAL` is made for mics | off |
| Reverb send | `FA_MIC_REVERB_SEND` | `ReverbSend` | 0..1 into the shared reverb | 0.2 |
| Echo send | `FA_MIC_ECHO_SEND` | `EchoSend` | 0..1 into the shared echo | 0 |

- **Inputs.** A mic reads one channel of a capture device (`channel`, from 0) or all of them summed (−1, the default). Two mics on one 2-input interface share the device: it is opened once. A channel the device does not have is `FA_ERROR_NOT_ENOUGH_CHANNELS`; a third mic is `FA_ERROR_BUSY`.
- **Clocks.** The input runs on its own clock and often at its own rate (a USB mic at 44.1 kHz into a 48 kHz master). It reaches the master through a small buffer that a resampler keeps at a steady fill, adjusting the speed by at most 0.2% (not audible), so drift never builds up into a delay or a click. If the input stalls, the buffer grows by 2 ms (up to 50 ms) and starts again.
- **The gate** opens at the threshold within 1 ms, stays open while the voice is at most 6 dB below it, and closes over 150 ms after 100 ms below that, so breaths and room noise stay out between phrases without chopping words.
- **The meter** (`fa_mic_get_levels`, `GetLevels`) reads the mic after gain, gate and EQ, before mute and pan, so a muted singer still sees their level. The first call starts measuring and reads silence.
- **Fade-in.** A new mic is silent for 50 ms, then fades in over 10 ms, so adding one never pops.
- **Unprocessed capture.** Mics ask for the input without the system's voice processing (automatic gain, noise suppression, echo cancelling: made for calls, they spoil singing and add latency). For now that is Android's UNPROCESSED input preset; Windows' raw mode is planned with the setup check, and macOS, iOS and Linux give the input as the system delivers it. `FA_MIC_OS_PROCESSING` (`KeepOsProcessing`) keeps the processing; two mics on one device share its input, so the first mic added decides for both.
- **Permission.** The app asks for the microphone permission before adding a mic: Android needs `RECORD_AUDIO` in the manifest and the runtime prompt; macOS and iOS need `NSMicrophoneUsageDescription` (and a sandboxed macOS app the audio-input entitlement). Where the system refuses at open, adding a mic is `FA_ERROR_PERMISSION_DENIED` (`ErrorCode.PermissionDenied`); some systems deliver silence instead.
- **Input lost.** An input that goes away (unplugged, taken by another app) goes silent, the mix carries on, and the mixer raises `FA_EVENT_INPUT_LOST` once per mic on `fa_mixer_set_event_callback` (`InputLost`), on the engine's event thread. Remove the mic there or later and add it again when the device is back. `fa_mic_remove` works inside the callback, also for the event's own mic.
- **Host mics.** `FA_MIC_DUCKS_MUSIC` (`DucksMusic`) marks a host's mic: its voice ducks the music (below). Leave it off for singers.

### Reverb and echo

Both mics share one reverb and one echo, like the effects unit of a karaoke mixer; each mic sets how much it sends (`FA_MIC_REVERB_SEND`, `FA_MIC_ECHO_SEND`). Their returns join the mics in the master, in the middle. They take what is heard: a mic with monitor off sends nothing.

```c
fa_mic_effects fx;
fa_mic_effects_init(&fx);              /* the defaults below */
fx.reverb_size = 0.7f;
fx.echo_time_ms = 180;
fa_mixer_set_mic_effects(mixer, &fx);
fa_mic_set_param(mic, FA_MIC_ECHO_SEND, 0.3f);
```

```csharp
mixer.MicEffects = new MicEffects { ReverbSize = 0.7f, EchoTime = TimeSpan.FromMilliseconds(180) };
mic.EchoSend = 0.3f;
```

| Setting | C (`fa_mic_effects`) | .NET (`MicEffects`) | Range | Default |
|---|---|---|---|---|
| Reverb size | `reverb_size` | `ReverbSize` | 0 (small room) .. 1 (hall) | 0.5 |
| Reverb damping | `reverb_damping` | `ReverbDamping` | 0 (bright) .. 1 (dark) | 0.5 |
| Echo time | `echo_time_ms` | `EchoTime` | 20 ms .. 1 s | 250 ms |
| Echo feedback | `echo_feedback` | `EchoFeedback` | 0 .. 0.9, how much of each repeat comes back | 0.35 |
| Echo tone | `echo_tone` | `EchoTone` | 0 (dark) .. 1 (bright): a low-pass in the feedback from 1 to 16 kHz, so later repeats get duller | 0.6 |

The reverb is the one the MIDI synthesizer uses (eight combs and four allpasses per side, after the published Freeverb design), with Freeverb's size and damping ranges. An echo time change glides over about 50 ms, bending the repeats for a moment as a tape echo does, so it never clicks. Out-of-range values are clamped. The tails ring out after a mic is removed.

### Ducking

While a host mic speaks above the threshold, the music (the decks after the crossfader, and any other stream on the mixer's device) goes down by the depth; it comes back after the hold and the release. It is on with these defaults from the start, so marking a mic `FA_MIC_DUCKS_MUSIC` is enough; `fa_mixer_set_ducking(mixer, NULL)` (`Ducking = null`) turns it off.

| Setting | C (`fa_ducking`) | .NET (`Ducking`) | Range | Default |
|---|---|---|---|---|
| Depth | `depth_db` | `DepthDb` | −40..0 dB (0 = off) | −12 dB |
| Attack | `attack` | `Attack` | 1 ms .. 1 s | 50 ms |
| Hold | `hold` | `Hold` | 0 .. 5 s | 300 ms |
| Release | `release` | `Release` | 10 ms .. 5 s | 600 ms |
| Threshold | `threshold_db` | `ThresholdDb` | −60..0 dB, the voice level that ducks | −40 dB |

The music is everything the decks and other streams on the mixer's device play; the mics and their effects are not ducked. The ducker follows the host's voice after the gate, so room noise below the gate never ducks. A muted host does not duck; a host with monitor off (heard through a hardware mixer) still does. Singers are never marked, so the music never dips under a singer.

### Recording

A mixer has two recording taps. Encoders attached to them record nothing until `fa_mixer_start_recording` starts them together on one device frame, so the files line up in any editor:

| Tap | C | .NET | What it records |
|---|---|---|---|
| Master | `FA_MIXER_TAP_MASTER` | `MixerTap.Master` | what the speakers play, after the limiter |
| Vocals | `FA_MIXER_TAP_VOCALS` | `MixerTap.Vocals` | both mics with their reverb and echo, before the master gain; a mic with monitor off is recorded dry |

```c
fa_encoder *master, *vocals;
fa_encoder_create_path("night.flac", &flac_options, &master);
fa_encoder_create_path("vocals.wav", NULL, &vocals);
fa_encoder_attach_mixer(master, mixer, FA_MIXER_TAP_MASTER);
fa_encoder_attach_mixer(vocals, mixer, FA_MIXER_TAP_VOCALS);
fa_encoder* both[] = { master, vocals };
fa_mixer_start_recording(mixer, both, 2, 0, 0);   /* 0 = with the next audio block */
/* ... the show ... */
fa_encoder_close(master);
fa_encoder_close(vocals);
```

```csharp
using var master = AudioEncoder.Create("night.flac", new EncoderOptions { Format = EncodeFormat.Flac });
using var vocals = AudioEncoder.Create("vocals.wav");
master.Record(mixer, MixerTap.Master);
vocals.Record(mixer, MixerTap.Vocals);
mixer.StartRecording([master, vocals]);
```

- **Lined up as the singer heard it.** A singer sings along with music that reached the speakers a little late, and the voice reaches the mixer a little late again. So the vocals file starts later than the master file by that round trip (the mic's input latency plus the master's output latency, the larger of two mics): put both files at 0 in an editor and the voice sits on the beat it was sung to. `FA_MIXER_REC_RAW_TIMING` (`rawTiming: true`) starts both with what the mixer made on the same frame instead (the voice as it reached the mixer). The round trip is the reported latency; the setup check will measure it.
- `device_time` (`startAt`) starts on a frame of the mixer device's clock (`fa_device_get_time`, `Output.Time`). 0, or a time less than one mixer block (2048 frames, about 43 ms at 48 kHz) ahead, starts one block ahead, so a block already in the audio thread never splits the stems.
- Formats, options and closing are those of every encoder ([recording and encoding](recording-encoding.md)). Destroying the mixer ends the recordings' input; close the encoders to complete the files.

### Latency

`fa_mixer_get_latency` (`Latency`, `Mic.InputLatency`, `Mic.MonitorLatency`) reports, in ns:

| Field | What it is |
|---|---|
| `master` | the master output's buffer plus the limiter's look-ahead (about 2 ms) |
| `cue` | the headphones: the output's buffer (the cue is not limited, so it has no look-ahead) |
| `cue_alignment` | delay added to line up a second-device cue (planned; 0) |
| `mic_input[i]` | the input device's buffer plus the bridge's fill; −1 when slot `i` has no mic |
| `mic_monitor[i]` | voice to speaker: `mic_input[i]` plus `master`; −1 when no mic |

Times the system does not report are estimated from the buffer sizes. Below about 20 ms voice to speaker a singer does not notice; at 30 ms and more it sounds like a slap-back. With the low-latency default the bridge holds about one input period, one master period and a millisecond. A measured check of the real total, speaker to mic, comes with the setup check.

## Planned

A measured setup check and exclusive mode, more than two decks, a second-device cue, beat sync and beat loops (BPM analysis), and auto-mix are the next steps of the [DJ mixer plan](../requests/2026-10-23_FatzAudio_Plan_DjMixer.md).
