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
| Second device | `FA_CUE_DEVICE` | `CueOutput.Device(headphones)` | The cue on another output device, such as USB headphones or the laptop's own jack, chosen by `cue_device_id` (NULL = the default output). Its own clock, bridged and lined up with the master ([below](#second-device-cue)). Not on iOS |

The cue send is taken before the channel fader, so a deck with its fader down can still be heard in the headphones. `FA_MIX_CUE_MIX` blends from cue only (0, the default) to master only (1), and `FA_MIX_CUE_GAIN` sets the headphone level. The master part of the blend is taken after the limiter, as the crowd hears it.

### Second-device cue

```c
fa_mixer_config c;
fa_mixer_config_init(&c);
c.cue_mode = FA_CUE_DEVICE;
c.cue_device_id = headphones->id;              /* NULL = the default output */
fa_mixer* mixer;
fa_mixer_create(engine, &c, &mixer);
fa_mixer_set_param(mixer, FA_MIX_CUE_TRIM_MS, 8.0f);   /* the headphones sounded early */
fa_mixer_latency l = { sizeof l };
fa_mixer_get_latency(mixer, &l);               /* l.cue_align, l.cue_alignment */
```

```csharp
var headphones = engine.OutputDevices.First(d => d.Name.Contains("USB"));
using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Device(headphones) });
mixer.CueTrim = TimeSpan.FromMilliseconds(8);
Console.WriteLine($"{mixer.Latency.CueAligned}: {mixer.Latency.CueAlignment.TotalMilliseconds:F1} ms");
mixer.CueLost += (_, _) => Console.WriteLine("headphones gone; call SetCueDevice when they are back");
```

- **Drift.** The headphones run on their own crystal. The cue reaches them through a small buffer that a resampler keeps at a steady fill, moving its rate by at most 0.05% (about 1 cent, never heard), whatever the drift between the two devices or their rates (a 48 kHz master and 44.1 kHz headphones). The buffer holds one master period, one headphone period, the resampler's look-ahead and a millisecond; a gap in the headphones raises it by 2 ms (up to 50 ms), so a system with uneven callbacks settles at what it needs. The mic inputs use the same bridge.
- **Lining up.** The two outputs have different latencies, so the master heard in the headphones (`FA_MIX_CUE_MIX`) would come early or late against the speakers. The mixer delays the faster output by the difference (at most 500 ms): `fa_mixer_latency.cue_align` (`Latency.CueAligned`) says which (`FA_CUE_ALIGN_CUE` the headphones, `FA_CUE_ALIGN_MASTER` the master) and `cue_alignment` by how much. Within 1 ms nothing is delayed (`FA_CUE_ALIGN_NONE`). `FA_MIXER_CUE_LOWEST_LATENCY` (`CueOutput.Device(lineUp: false)`) delays neither (`FA_CUE_ALIGN_OFF`), for the lowest latency on both.
- **Monitored mics come first.** While any mic is monitored (`FA_MIC_MONITOR`), the master is never delayed: a singer must not hear their voice later. When the master is the faster output, alignment is skipped (`FA_CUE_ALIGN_SKIPPED`; the headphones are `cue` − `master` late), and a delay already on the master comes off the moment a mic is monitored. Delaying the headphones goes on as usual.
- **Heard positions.** A delay on the master is part of what the crowd hears, so it is part of `fa_stream_get_clock` (`Clock`), and with it CD+G and lyric surfaces and video that follows the clock, deck meters, `fa_device_get_latency` and `mic_monitor`. `fa_stream_get_position`, which counts frames handed to the device, has no output latency in it.
- **Trim.** The latencies the system reports are not exact. `FA_MIX_CUE_TRIM_MS` (`CueTrim`, −250..250 ms) is added to the master's: raise it when the headphones sound early against the speakers, lower it when late. It takes effect at once and can't be ramped.
- **Changes.** A change of alignment (a trim, a mic turned to monitored, other headphones) makes a short gap in the output it delays (5 ms fades on the master); changes under 5 ms in the headphones glide in.
- **Mode.** The headphones open in the mixer's latency mode (low when it is exclusive, which a second device can't share) and move with `fa_mixer_set_latency_mode`.
- **Lost headphones.** Headphones that go away go silent and raise `FA_EVENT_CUE_LOST` (`CueLost`) once; the master plays on untouched, keeping any delay. `fa_mixer_set_cue_device` (`SetCueDevice`) opens the same or other headphones (lined up anew). It is `FA_ERROR_INVALID_ARG` on a mixer made without `FA_CUE_DEVICE`.
- **Systems.** Windows, macOS, Linux and Android (where the system may route both outputs to one place). iOS is `FA_ERROR_NOT_IMPLEMENTED`: use `FA_CUE_SPLIT`.

## Parameters

| Parameter | Range | Default |
|---|---|---|
| `FA_MIX_CROSSFADER` / `Crossfader.Position` | 0 (A) .. 1 (B) | 0.5 |
| `FA_MIX_MASTER_GAIN` / `MasterGain` | 0..4, linear, before the limiter | 1 |
| `FA_MIX_CUE_MIX` / `CueMix` | 0 (cue) .. 1 (master) | 0 |
| `FA_MIX_CUE_GAIN` / `CueGain` | 0..4, linear | 1 |
| `FA_MIX_CUE_TRIM_MS` / `CueTrim` | −250..250 ms, [second-device cue](#second-device-cue) only, not rampable | 0 |
| `fa_mixer_set_crossfader_curve` / `Crossfader.Curve` | constant power (each side −3 dB in the middle, steady loudness) or linear (−6 dB, a slight dip) | constant power |
| `FA_DECK_GAIN` / `Gain` | 0..1, the channel fader | 1 |
| `FA_DECK_TRIM_DB` / `TrimDb` | −12..+12 dB, on top of loudness normalization | 0 |
| `FA_DECK_EQ_LOW_DB`, `_MID_DB`, `_HIGH_DB` / `Eq.Low`, `Eq.Mid`, `Eq.High` | `FA_DECK_EQ_KILL_DB` (−60, silent) .. +6 dB | 0 |
| `FA_DECK_XFADER_SIDE` / `Side` | thru (0), A (1), B (2) | deck 1 A, deck 2 B |
| `FA_DECK_CUE` / `Cue` | off / on | off |
| `FA_DECK_SYNC_RANGE` / `SyncRange` | 0.01..1: the most [sync](#sync-and-beat-loops) moves the deck's tempo away from 1, not rampable | 0.16 (±16%) |

## Rules

- Sets glide over 5 ms; ramps take any curve and an optional start time on the device clock, like stream ramps ([ramps](ramps.md)). Out-of-range values are clamped. Side and cue switch at once and can't be ramped. Mixer and deck ramps raise no events.
- The isolator splits the deck at 250 Hz and 2.5 kHz with fourth-order Linkwitz-Riley crossovers, so with every band at 0 dB the bands add back to the original exactly in level (the phase turns gently around the crossovers, as on hardware isolators). It runs always, so moving a knob never clicks.
- `fa_deck_load` attaches the stream to the mixer's device through the deck, from wherever it was, keeping its position. Loading over a stream that is playing and heard (fader up and the crossfader not closed on its side) is `FA_ERROR_BUSY` unless `FA_DECK_LOAD_FORCE` (`force: true`). Loading a stream that isn't playing also shortens its decode-ahead to 60 ms, so tempo and pitch moves are heard quickly.
- A deck holds one stream. `fa_crossfade` or `fa_stream_set_next` from a deck's stream to a stream that isn't attached yet is `FA_ERROR_INVALID_ARG`: load the next song on the other deck and use the crossfader. A crossfade between the streams of two decks works.
- A deck's stream may be closed (disposed) from any thread, also while it is loaded: it leaves its deck first, under the mixer's lock, so a deck call on another thread (or the engine's event thread driving [sync](#sync-and-beat-loops) and [auto-mix](#auto-mix)) finishes before it goes.
- The device mute (`fa_device_set_mute`, `Output.Muted`) silences the headphones too.
- The cue point (`fa_deck_set_cue_point`, `CuePoint`) defaults to the stream's CUE_IN marker, else 0. `fa_deck_cue` (`ReturnToCuePoint`) pauses the deck and goes back to it.
- The meter (`fa_deck_get_levels`) shows the deck after trim, EQ and fader, before the crossfader, as heard now.
- `latency` defaults to `FA_LATENCY_LOW`: on Windows the WASAPI low-latency shared mode at the device's own rate (needs a driver that offers small periods, most do), about 5 ms periods elsewhere. `FA_LATENCY_NORMAL` uses the engine's period. `FA_LATENCY_EXCLUSIVE` (opt-in) is Windows WASAPI exclusive with 3 ms periods for the output and the mics: other apps can't use those devices meanwhile and the Windows volume no longer applies (the mixer's gains and `fa_device_set_volume` still do); `FA_ERROR_NOT_IMPLEMENTED` on other systems, `FA_ERROR_DEVICE` when another app holds the device. `fa_mixer_set_latency_mode` (`LatencyMode`) moves a running mixer to another mode: the devices close and open again (a moment of silence) and playback carries on. When the new mode can't be had the mixer stays in its own and returns that error; a mic input that won't open in the new mode raises `FA_EVENT_INPUT_LOST`.
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
- **Unprocessed capture.** Mics ask for the input without the system's voice processing (automatic gain, noise suppression, echo cancelling: made for calls, they spoil singing and add latency). That is WASAPI's raw mode on Windows (`AUDCLNT_STREAMOPTIONS_RAW`, where the driver offers it) and Android's UNPROCESSED input preset; macOS, iOS and Linux give the input as the system delivers it. `FA_MIC_OS_PROCESSING` (`KeepOsProcessing`) keeps the processing; two mics on one device share its input, so the first mic added decides for both.
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

- **Lined up as the singer heard it.** A singer sings along with music that reached the speakers a little late, and the voice reaches the mixer a little late again. So the vocals file starts later than the master file by that round trip (the mic's input latency plus the master's output latency, the larger of two mics): put both files at 0 in an editor and the voice sits on the beat it was sung to. `FA_MIXER_REC_RAW_TIMING` (`rawTiming: true`) starts both with what the mixer made on the same frame instead (the voice as it reached the mixer). The round trip is the reported latency until the [setup check](#setup-check) has measured that mic in that mode; then it is the measured one.
- `device_time` (`startAt`) starts on a frame of the mixer device's clock (`fa_device_get_time`, `Output.Time`). 0, or a time less than one mixer block (2048 frames, about 43 ms at 48 kHz) ahead, starts one block ahead, so a block already in the audio thread never splits the stems.
- Formats, options and closing are those of every encoder ([recording and encoding](recording-encoding.md)). Destroying the mixer ends the recordings' input; close the encoders to complete the files.

### Latency

`fa_mixer_get_latency` (`Latency`, `Mic.InputLatency`, `Mic.MonitorLatency`) reports, in ns:

| Field | What it is |
|---|---|
| `master` | the master output's buffer plus the limiter's look-ahead (about 2 ms) and any alignment delay on it |
| `cue` | the headphones: the output's buffer (the cue is not limited, so it has no look-ahead); on a second device also the bridge and any alignment delay |
| `cue_alignment` | the delay added to line up a [second-device cue](#second-device-cue) |
| `cue_align` | which output `cue_alignment` delays: `FA_CUE_ALIGN_NONE`, `_CUE`, `_MASTER`, `_SKIPPED` (a mic is monitored) or `_OFF` |
| `mic_input[i]` | the input device's buffer plus the bridge's fill; −1 when slot `i` has no mic |
| `mic_monitor[i]` | voice to speaker: measured by the setup check, else `mic_input[i]` plus `master` (with any delay on it); −1 when no mic |

Times the system does not report are estimated from the buffer sizes. Below about 20 ms voice to speaker a singer does not notice; at 30 ms and more it sounds like a slap-back. With the low-latency default the bridge holds about one input period, one master period and a millisecond. The [setup check](#setup-check) measures the real total; after it, `mic_monitor[i]` is the measured time for that mic in the mode measured (`mic_input[i]` stays the reported one).

### Setup check

`fa_mixer_measure_latency` (`MeasureLatencyAsync`) is a test mode for the app's setup screen. With the mic where the singer will stand, it plays short test sounds through the speakers, listens on the mic, and reports for each latency mode it measured how long the mic takes to reach the speakers, and how much mic gain the room takes before it feeds back. Then it advises whether to monitor the mics, and in which mode.

```c
fa_latency_check_config cc;
fa_latency_check_config_init(&cc);        /* -12 dBFS at most, 25 ms still feels live */
cc.flags = FA_CHECK_ALL_MODES;            /* normal, low and (Windows) exclusive */
fa_latency_check res = { sizeof res };
fa_result r = fa_mixer_measure_latency(mixer, mic, &cc, &res);   /* blocks 3-6 s per mode */
if (r == FA_OK && res.advice == FA_CHECK_MONITOR) {
    fa_mixer_set_latency_mode(mixer, res.advised_mode);
    fa_mic_set_param(mic, FA_MIC_MONITOR, 1);
}
/* res.modes[i]: .result, .mic_to_speaker, .spread, .reported, .max_gain_db, .level_db */
```

```csharp
var check = await mixer.MeasureLatencyAsync(mic, new LatencyCheckOptions { AllModes = true });
if (check.Advice == CheckAdvice.Monitor)
{
    mixer.LatencyMode = check.AdvisedMode;
    mic.Monitor = true;
}
foreach (var m in check.Modes)
    Console.WriteLine($"{m.Mode}: {m.Error?.ToString() ?? $"{m.MicToSpeaker.TotalMilliseconds:F1} ms, gain up to {m.MaxGainDb:F0} dB"}");
```

- **What it plays.** The music fades out over 10 ms and the mics are silenced while the test sounds play, then everything fades back in. After a switch of mode it first waits until the input has run for half a second without a gap (at most 3 s), and a mode not heard right after a switch is tried once more. First a probe: three 5 ms chirps (1-6 kHz), 150 ms apart. It starts at −40 dBFS and gets 10 dB louder each time until the mic hears every chirp clearly (or 10 dB more no longer makes them clearer: a room's reflections set a limit, and the quieter level is kept), up to `max_level_db` (`MaxLevelDb`, −40..−6, default −12). Then at that level five chirps and 1.5 s of pink noise 10 dB below them. A mode takes 3-6 s.
- **The time.** `mic_to_speaker` (`MicToSpeaker`) is the median of the chirps' times, mic to speaker: the input, the bridge, the limiter, the output, and the air back to the mic (3 ms per metre). `spread` is how far the chirps differed (a steady system: well under 1 ms); `reported` is what the system's figures add up to, for comparison. The mixer uses the measured time for that mic and mode from then on (`fa_mixer_get_latency`, the vocals' recording alignment), until the mic is removed. While a check runs the bridge holds its speed, so its drift correction can't bend the time.
- **The gain.** From the pink noise, the loudest 1/24 octave of the loop speaker to mic between 100 Hz and 10 kHz (the room's worst resonance) gives `max_gain_db` (`MaxGainDb`): the most mic gain that stays 6 dB clear of feedback, with the master gain as it is now and the mic's EQ flat (an EQ boost takes that much off), −20..40.
- **The advice.** `FA_CHECK_MONITOR` (`Monitor`): monitor in `advised_mode`, the first of low, normal and exclusive that measured within `max_monitor` (`MaxMonitor`, default 25 ms), so shared modes come before exclusive, which takes the devices from other apps. `FA_CHECK_FEEDBACK`: that mode is fast enough, but the room feeds back below 0 dB of gain: move the mic away from the speakers or point it away. `FA_CHECK_SLOW`: every mode measured is slower than `max_monitor` (an audible slap-back), `advised_mode` is the fastest; leave the mics unmonitored or use a hardware mixer. `FA_CHECK_NOT_HEARD`: the mic didn't hear the speakers (is it on and unmuted, are the speakers up, is it a headset?).
- **Each mode.** `modes[i].result` is `FA_OK`, `FA_ERROR_NOT_IMPLEMENTED` (exclusive off Windows), `FA_ERROR_DEVICE` (it would not open, or the output isn't running) or `FA_ERROR_NOT_FOUND` (not heard). `FA_CHECK_ALL_MODES` switches the mixer to each mode with `fa_mixer_set_latency_mode`, waits half a second for it to settle, measures, and returns to the mixer's own mode (if that fails, the mixer stays in the last mode measured and the call returns the error, with the measurements filled in). Without it only the mixer's own mode is measured, and a failure other than not heard is also the call's result.
- **Trying a rig.** `tools/mic-check/fa_mic_check` runs the check from a command line: `fa_mic_check --list`, then `fa_mic_check --mic Yeti --all` (`--out NAME` for the output, `--level DB`, `--repeat N`).
- **Rules.** The call blocks (in .NET it runs on a thread pool thread); call it from a thread of your own, not the event callback. A second check, or `fa_mixer_set_latency_mode`, while one runs is `FA_ERROR_BUSY`; a mic of another mixer is `FA_ERROR_INVALID_ARG`. Keep the mic until it returns. Exclusive mode ignores the Windows volume, so a level that was quiet in a shared mode can be much louder there.

## Sync and beat loops

Sync needs a beat grid on both decks: one from [track analysis](analysis.md) set with `fa_stream_set_beat_grid` (`AudioStream.BeatGrid`), one the user corrected, or the exact grid of a MIDI file or tracker module (no analysis needed). Without one the calls return `FA_ERROR_NOT_FOUND`. Beats are measured where the decks are heard (the presentation clock), so they line up in the room, including any delay on the master.

```c
fa_stream_set_beat_grid(song_a, &grid_a, NULL, 0);   /* from fa_analysis_get_beat_grid */
fa_stream_set_beat_grid(song_b, &grid_b, NULL, 0);
fa_stream_play(song_a);

/* B starts on A's next downbeat, at A's tempo, and stays locked to it. */
fa_deck_play_synced(b, a, FA_SYNC_TEMPO | FA_SYNC_BAR | FA_SYNC_LOCK);

fa_deck_set_loop_beats(b, 4);         /* a four-beat loop from the beat heard now */
fa_deck_set_loop_beats(b, 2);         /* halved: the same start */
fa_deck_set_loop_beats(b, 0);         /* off */

fa_deck_beat_info info = { sizeof(info) };
fa_deck_get_beat_info(b, &info);      /* bpm, beat, bar, phase_error_ms, locked */
fa_deck_sync(b, NULL, 0);             /* unlock: the tempo stays */
```

```csharp
songA.BeatGrid = (await engine.AnalyzeAsync(pathA)).Beats;
songB.BeatGrid = (await engine.AnalyzeAsync(pathB)).Beats;
songA.Play();
b.PlaySynced(a, SyncMode.Tempo | SyncMode.Bar | SyncMode.Lock);
b.LoopBeats(4);
var info = b.BeatInfo;                // Bpm, Beat, Bar, PhaseErrorMs, Locked, Synced
b.Unsync();
```

- **Tempo** (`FA_SYNC_TEMPO`): the deck's `FA_PARAM_TEMPO` becomes the leader's heard BPM (its grid's BPM where it plays × its tempo × its rate) over the deck's grid BPM × its rate. The pitch stays (the stretcher). When that is more than `FA_DECK_SYNC_RANGE` from 1, half or double time is tried (a 170 BPM leader plays an 85 BPM deck at its own tempo); still out of reach is `FA_ERROR_OUT_OF_RANGE` (`ErrorCode.OutOfRange`) and nothing changes.
- **Phase** (`FA_SYNC_BEAT`, `FA_SYNC_BAR`): on a playing deck, an error under 30 ms is pulled in by bending the tempo by up to 2% for about two beats (not audible); a larger one is fixed by a jump on the deck's next beat, joined seamlessly like a loop. `FA_SYNC_BAR` lines up downbeats (it implies BEAT); bars are known for exact grids, others count beat 0 as a downbeat. On a deck that isn't playing, `fa_deck_sync` sets the tempo and the phase follows once it plays; `fa_deck_play_synced` starts it in phase instead.
- **`fa_deck_play_synced`** (`PlaySynced`) starts a stopped deck from its cue point (a paused one from where it is) so that its next beat (BAR: downbeat) lands exactly on one of the leader's, at least 50 ms ahead. It is scheduled on the device clock, so it is exact to the frame. The leader must be playing (`FA_ERROR_INVALID_ARG`); a deck that plays already is synced as `fa_deck_sync` does.
- **Lock** (`FA_SYNC_LOCK`): the engine's event thread looks about 20 times a second and keeps the deck on the leader's beat: it follows the leader's tempo moves and tempo-mapped songs, with a target error under 1 ms. The deck's `FA_PARAM_TEMPO` then moves by itself; set it by hand only after unlocking. Without LOCK a sync is done once (a phase pull ends within 1 ms, or after about eight beats).
- **Ending it**: `fa_deck_sync(deck, NULL, 0)` (`Unsync`), or loading a stream on either deck. The tempo stays where it is.
- **Beat loops** (`fa_deck_set_loop_beats`, `LoopBeats`): 1/16..32 beats on the grid, from the beat heard now (a loop shorter than a beat starts on the last whole part of the beat). While a deck loop is on the new one keeps its in point, so halving and doubling work like a DJ's loop buttons. 0 turns the loop off. It is an ordinary [deck loop](#deck-loops), read back with `fa_deck_get_loop` (`Loop`) in frames.
- **Beat info** (`fa_deck_get_beat_info`, `BeatInfo`): the heard BPM, the position in beats and bars, and while synced the phase error against the leader in ms (positive: this deck is ahead). `FA_ERROR_NOT_FOUND` (null in .NET) for an empty deck or one without a grid.

## Auto-mix

Auto-mix makes the two decks take turns: while one plays, the other starts at the playing deck's mix point and the crossfader moves across. Everything is scheduled on the device clock a couple of seconds ahead, so it is sample-accurate however busy the app's UI thread is. The app owns the playlist and loads the next song when asked.

```c
static void FA_CALL on_mixer(const fa_mixer_event* e, void* user)
{
    if (e->type == FA_EVENT_DECK_EMPTY) {          /* the idle deck needs the next song */
        fa_deck* d; fa_mixer_get_deck(e->mixer, e->index, &d);
        fa_stream* old; fa_deck_get_stream(d, &old);
        fa_deck_load(d, next_song(user), FA_DECK_LOAD_FORCE);
        if (old) fa_stream_close(old);
    }
}

fa_mixer_set_event_callback(mixer, on_mixer, playlist);
fa_automix_options o;
fa_automix_options_init(&o);          /* 8 s, match tempo, align beats */
o.length_beats = 32;                  /* when both grids are good */
fa_mixer_set_automix(mixer, &o);
fa_stream_play(first_song);           /* on either deck: auto-mix takes it from here */
fa_mixer_automix_now(mixer);          /* skip: the next transition now */
fa_mixer_set_automix(mixer, NULL);    /* off */
```

```csharp
mixer.AutoMix.DeckEmpty += (s, e) =>
{
    var old = e.Deck.Stream;
    e.Deck.Load(engine.OpenFile(queue.Dequeue()), force: true);
    old?.Dispose();
};
mixer.AutoMix.Start(new AutoMixOptions { LengthBeats = 32 });
mixer.AutoMix.Skip();
```

- **The mix point** follows the rules of `fa_stream_set_next`: the outgoing song's `MIX_POINT` marker, else its `FADE_OUT_START`, else its end (`CUE_OUT`, or the length) minus the transition's length. The incoming deck starts at its cue point (`CUE_IN` unless set).
- **Beat-aligned** when `FA_AUTOMIX_ALIGN_BEATS` is set, both grids have at least `min_confidence` and the tempo is within reach: the incoming deck starts on its first downbeat at or after its cue point, on the outgoing deck's downbeat nearest the mix point, and the transition lasts whole bars. With `FA_AUTOMIX_MATCH_TEMPO` the incoming deck is first brought to the outgoing BPM (within `max_tempo_change`, half or double time allowed), and it stays locked to the outgoing deck until the transition is done. Without MATCH_TEMPO beats align only when the tempos already match within 0.2%.
- **Otherwise** (no grid, a low confidence, tempos too far apart) it is a plain timed crossfade over `length`: auto-mix never fails unattended.
- **The crossfader** is ramped over the transition toward the incoming deck's side, so it follows the crossfader curve. Decks on the same side, or thru, are started but not faded.
- **When it is done** the outgoing deck is stopped. The incoming deck keeps the tempo it was mixed in at.
- **Events** (mixer events, on the engine's event thread): `FA_EVENT_AUTOMIX_STARTED` when the incoming deck is heard and `FA_EVENT_AUTOMIX_DONE` when the crossfader is across (index and value: the incoming deck; `Started`, `Done`), and `FA_EVENT_DECK_EMPTY` (index: the idle deck; `DeckEmpty`) when a transition is done, and again 30 s before the next mix point while the idle deck still holds the song that was mixed out, or nothing. If nothing new is loaded, the outgoing song plays to its end.
- **Skip** (`fa_mixer_automix_now`, `Skip`): the next transition now (on the next bar when beat-aligned). `FA_ERROR_NOT_FOUND` when auto-mix is off, no deck plays, or the idle deck has nothing new; `FA_ERROR_BUSY` while a transition runs.
- Auto-mix needs the outgoing song's length (a file, memory or disc track, not a live web stream) unless it has a `MIX_POINT` or `FADE_OUT_START` marker. Turning it off (`NULL`, `Stop`) lets a transition already scheduled finish, without events.

| Option | Range | Default |
|---|---|---|
| `length` / `Length` | 0.5..60 s | 8 s |
| `length_beats` / `LengthBeats` | 0 (use `length`) or 1..256, when both grids are good | 0 |
| `flags` / `MatchTempo`, `AlignBeats` | `FA_AUTOMIX_MATCH_TEMPO`, `FA_AUTOMIX_ALIGN_BEATS` (`BASS_SWAP` and `RESTORE_TEMPO` are planned: `FA_ERROR_INVALID_ARG`) | both |
| `max_tempo_change` / `MaxTempoChange` | 0..0.5 | 0.08 |
| `min_confidence` / `MinConfidence` | 0..1 | 0.5 |

## Planned

More than two decks, beat jumps, a bass swap and a tempo glide back after an auto-mix are pinned in section 19 of the [DJ mixer plan](../requests/2026-10-23_FatzAudio_Plan_DjMixer.md).
