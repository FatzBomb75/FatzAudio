# FatzAudio: mic latency spike results (DJ mixer plan, step 0)

For FatzBomb.MediaControl, answering "if Windows cannot get mic to speaker near 20 ms in LOW mode on the
owner's interface, tell us before building the mic PRs".

**Answer: it cannot on Keith's PC as it is set up today. Default to monitoring off.** Details below, then
what FatzAudio proposes.

## Setup measured (2026-10-23)

- Windows, WASAPI. Mic: Yeti Nano (USB). Output: LG ULTRAWIDE monitor speakers over HDMI (NVIDIA HD Audio),
  the default device. Mic close to the speaker.
- No audio interface or DJ controller connected; nothing on the Realtek analog output; the Yeti's
  headphone jack not tested.
- Tool: `tools/mic-latency` on branch `spike/mic-latency` (standalone, miniaudio 0.11.25 as the library
  uses it). It measures the round trip with callback timestamps and the real monitoring path (capture,
  ring, playback) from the repeats of an injected chirp, sample-exact. A simulated self-test passes.
  Raw results and recordings: `C:\development\fatzaudio-plans\mic-latency\` on Keith's PC.

## Results

| Mode | Periods (capture / playback) | Round trip, out + in + air | Mic to speaker (monitor loop) |
|---|---|---|---|
| NORMAL (shared, today's default) | 10 ms / 10 ms | 71-76 ms (4 runs) | about 90-100 ms (*estimate*: round trip plus about 20 ms of ring; the loop measurement locked onto wrong peaks here) |
| LOW (shared, IAudioClient3) | 10 ms / 10 ms | 77-89 ms | same as NORMAL |
| EXCLUSIVE (reference) | 3 ms / 3.1 ms | 30-32 ms median (22-40) | **41.9 ms measured** (40.5-41.9) |

What this says:

1. **LOW gives nothing on this hardware.** Neither the Yeti Nano's driver nor the NVIDIA HDMI driver offers
   shared-mode periods below 10 ms, so IAudioClient3 runs at 10 ms, the same as NORMAL. LOW only helps on
   drivers that offer small periods (the Windows inbox HD Audio and USB Audio 2 class drivers usually do).
2. **The HDMI display is most of the delay.** Even with 3 ms periods the round trip is about 30 ms, so the
   monitor's own audio path adds roughly 20 ms. A sound card or USB interface feeding the speaker rig
   should do much better; that is not measured yet.
3. **Exclusive mode bypasses the Windows volume slider.** The chirp arrived about 25 times louder in
   exclusive mode than in shared mode (Keith's system volume is low). In exclusive mode the app's own
   volume is the only control.
4. **Feedback comes early.** With the mic next to the speaker, the monitoring loop howled at a broadband
   loop gain of 0.12 (and at 0.5): room and speaker resonances sit well above the average level. A full
   speaker rig with mics in the room needs a measured safe monitor gain, not a guess.

## What FatzAudio proposes

- **Monitoring off by default** on Windows. Mics still record, still duck the music, and still go to the
  master when monitoring is on.
- **A built-in setup check** (Keith asked for it 2026-10-23): `fa_mixer_measure_latency` plays a few chirps
  through the real speakers, listens on the real mic, and reports the measured mic-to-speaker delay per
  available mode, the highest monitor gain that stays clear of feedback, and a recommendation (monitor on
  in a given mode, or off, with the reason). The mixer then uses the measured delays instead of the OS's
  reported latency for vocal recording alignment and cue alignment. Part of the mic step, about 8 KB.
- **WASAPI exclusive as the opt-in "live monitoring" mode** (pinned as future in your reply; we ask to move
  it into the mic step). On this PC it is the only mode under 50 ms. Costs: other apps lose the output
  device while it runs, and the Windows volume slider no longer applies.
- **Re-measure on the real rig** (sound card or interface to the PA) when it is connected, with the setup
  check itself. Expect about 15-25 ms in exclusive mode and 35-45 ms in shared mode on a driver with
  10 ms periods (*estimate*).
