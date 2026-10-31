# Dolby Digital and Dolby Digital Plus

FatzAudio decodes Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) itself, in every place audio comes from: `.ac3` and `.ec3` files, AC-3 and E-AC-3 tracks in MP4, MOV and Matroska video files, web streams and HLS, and packets pushed from your own FFmpeg demuxer. There's no add-on to load and no FFmpeg needed. It replaces bass_ac3.

| What | Supported |
|---|---|
| AC-3 (Dolby Digital) | Every mode: mono to 5.1, 32, 44.1 and 48 kHz, all bit rates |
| E-AC-3 (Dolby Digital Plus) | Every coding tool: independent and dependent substreams (7.1), 1 to 6 blocks per frame, spectral extension, enhanced coupling, the adaptive hybrid transform, all bit allocation options. Transient pre-noise processing data is read and not applied, as in most decoders |
| Atmos (Dolby Digital Plus with JOC) | The 5.1 or 7.1 bed plays; the height objects are not rendered |
| Dolby TrueHD | Not supported. If you need it, we recommend decoding it with FFmpeg in your app and handing FatzAudio the audio ([FFmpeg hand-off](ffmpeg.md)) |

`fa_stream_get_info` reports codec `ac3` or `eac3`. The container is `ac3` or `ec3` for raw files, or the video file's (`mp4`, `matroska`, `hls`, ...).

## Channels

Streams decode to the standard speaker order for their channel count:

| Channels | Order | Used for |
|---|---|---|
| 1 | C | Mono |
| 2 | L R | Stereo, dual mono |
| 3 | L R C | 3.0 |
| 4 | L R C S | 2.1 and 3.1 (rear centre surround; C silent for 2.1) |
| 5 | L R C BL BR | 2.2 and 3.2 without LFE (C silent for 2.2) |
| 6 | L R C LFE SL SR | 5.1, and any layout above with LFE |
| 7 | L R C LFE BC SL SR | 6.1 |
| 8 | L R C LFE BL BR SL SR | 7.1 (E-AC-3 with a dependent substream) |

A layout without its own count uses the next larger one with silent channels, so a stream with an LFE always has at least 6 channels (stereo plus LFE plays as 5.1 with C and the surrounds silent). On a stereo device these channels are mixed down with the stream's own levels ([stereo downmix](#stereo-downmix)). For your own downmix (louder centre for dialogue, no LFE, music only), set a channel matrix ([buses and the channel matrix](buses.md#channel-matrix)); it always wins.

`fa_stream_read` (decode-only streams) always hands out every channel as decoded: the downmix is part of playback.

## Stereo downmix

A Dolby stream says how it should sound on two speakers: how loud the centre and the surrounds go into left and right, and which of two kinds of stereo the mastering engineer preferred. FatzAudio follows it, as ETSI TS 102 366 describes (clauses 6.8 and E.2.9, Annex D):

| Mode | What you hear | Use it for |
|---|---|---|
| `FA_DOLBY_DOWNMIX_AUTO` (default) | The kind the stream prefers; Lo/Ro when it names none (most streams) | Everything |
| `FA_DOLBY_DOWNMIX_LORO` | Lo/Ro, plain stereo: left gets L, the centre at its level and the left surround at the surround level; right the same | Headphones, speakers, a mono mix later |
| `FA_DOLBY_DOWNMIX_LTRT` | Lt/Rt, matrix surround: the surrounds go into left out of phase and into right in phase, so a Dolby Pro Logic decoder downstream (an AV receiver on the analog or stereo output) can bring them back | Feeding a surround receiver from a stereo output |
| `FA_DOLBY_DOWNMIX_ENGINE` | The engine's own downmix, as for every other format: centre and surrounds at -6 dB, no LFE, the stream's levels ignored | The downmix FatzAudio had before this setting |

The levels come from the stream itself, frame by frame:

| Stream | Centre level | Surround level | Preference |
|---|---|---|---|
| AC-3 | `cmixlev`: -3, -4.5 or -6 dB | `surmixlev`: -3 dB, -6 dB or off | none (Lo/Ro) |
| AC-3 with the extended header (Annex D) | separate Lo/Ro and Lt/Rt levels, +3 dB to off | the same, -1.5 dB to off | `dmixmod` |
| E-AC-3 with mixing metadata | separate Lo/Ro and Lt/Rt levels | the same | `dmixmod` |
| E-AC-3 without mixing metadata | -4.5 dB (Lo/Ro), -3 dB (Lt/Rt) | -6 dB (Lo/Ro), -3 dB (Lt/Rt) | none (Lo/Ro) |

E-AC-3 can also give an LFE level; then the LFE is mixed in at it (4.5 dB under the stated level, E.2.9). AC-3 has no such field, and its LFE stays out of the stereo mix, as the standard allows. A single rear surround (3/1, 2/1) goes in at 0.7 times the surround level in each side (Lo/Ro). 7.1 mixes its back pair like its side pair.

All the weights are then turned down alike until neither output can pass full scale (6.8.1). A typical 5.1 film with the centre and surrounds at -3 dB plays its front left and right at 0.41 (-7.7 dB), so it is quieter than with `FA_DOLBY_DOWNMIX_ENGINE`, which can clip a loud 5.1 mix. With [normalization](normalization.md) on, the gain gives the scaling back (it is added to `fa_loudness.gain_db`), and the output limiter catches the peaks the downmix would have clipped: normalized films play at the same level in every mode. Only channels that carry audio count, so a stream that is stereo plus LFE still plays its left and right at full level.

Set the mode when opening, or change it while the stream plays. Between AUTO, LORO and LTRT the change applies to audio decoded from then on (after `buffer_ms`, like the dynamic range). To or from ENGINE the stream rebuilds its chain and carries on where it plays, as `fa_stream_set_matrix` does; a live stream refuses that while attached (`FA_ERROR_NOT_SEEKABLE`), so change it while detached.

### C

```c
fa_open_options o;
fa_open_options_init(&o);
o.dolby_downmix = FA_DOLBY_DOWNMIX_LTRT;   /* the stereo output feeds a Pro Logic receiver */
fa_stream* film = NULL;
fa_stream_open_path(engine, "film.mkv", &o, &film);
fa_stream_attach(film, device, NULL);
fa_stream_play(film);

/* Back to plain stereo for headphones. */
fa_stream_set_dolby_downmix(film, FA_DOLBY_DOWNMIX_LORO);
```

### C#

```csharp
using var film = engine.OpenFile("film.mkv", new StreamOptions { DolbyDownmix = DolbyDownmix.LtRt });
film.PlayOn(output);

film.DolbyDownmix = DolbyDownmix.LoRo;
```

## Dialogue level (dialnorm)

Every Dolby stream states its dialogue level, -1 to -31 dBFS, which broadcasters set to the programme's loudness (ATSC A/85). FatzAudio can use it for [normalization](normalization.md) instead of scanning: turn it on once with `fa_engine_set_dialnorm(engine, 1)` (.NET `engine.SetDialnorm(true)`). A file's dialnorm then counts as its loudness when there is no value from your app, no ReplayGain or R128 tag and no earlier scan in the cache, and it is final at once (`FA_LOUDNESS_DIALNORM`), so a two-hour film needs no scan.

It is off by default because the value is only as good as whoever set it: -31, which encoders write when nobody chose a value, is never used, but a wrong value would be. Live streams and app I/O keep the running estimate.

```c
fa_engine_set_normalization(engine, FA_NORMALIZE_TRACK, -16);
fa_engine_set_dialnorm(engine, 1);
fa_stream_open_path(engine, "film.ac3", NULL, &film);   /* loudness = its dialnorm, e.g. -24 LUFS */
```

## Dynamic range

Dolby streams carry the broadcaster's compression gains for every block. You choose whether to use them:

| Mode | What you hear | Use it for |
|---|---|---|
| `FA_DYNAMIC_RANGE_FULL` (default) | The mix as it was mastered, no compression | Music, a good room |
| `FA_DYNAMIC_RANGE_NORMAL` | The stream's own compression ("line mode"): quiet dialogue lifted, explosions tamed, as a home AV receiver does | Films on TV speakers |
| `FA_DYNAMIC_RANGE_NIGHT` | Heavy compression ("RF mode") for low volume; streams without heavy gains use the normal ones | Late-night listening |

Set it when opening, or change it while the stream plays. A change applies to audio decoded from then on, so what is already buffered (up to `buffer_ms`) plays out first; seek to the current position to hear it at once. Other formats ignore the setting.

### C

```c
fa_open_options o;
fa_open_options_init(&o);
o.dynamic_range = FA_DYNAMIC_RANGE_NORMAL;
fa_stream* film = NULL;
fa_stream_open_path(engine, "film.mkv", &o, &film);   /* the E-AC-3 track decodes natively */
fa_stream_attach(film, device, NULL);
fa_stream_play(film);

/* Later: night mode from now on. */
fa_stream_set_dynamic_range(film, FA_DYNAMIC_RANGE_NIGHT);

fa_dynamic_range mode;
fa_stream_get_dynamic_range(film, &mode);
```

Packets from your own demuxer work the same way (see [FFmpeg hand-off](ffmpeg.md)):

```c
fa_codec_params p;
fa_codec_params_init(&p);
p.codec = "eac3";              /* or "ac3" */
p.sample_rate = 48000;
p.channels = 6;
p.time_base = (fa_rational){ 1, 90000 };
fa_stream_open_packets(engine, &p, NULL, &s);
/* fa_stream_push_packet for each AVPacket; packets may hold any number of
 * frames, or cut a frame anywhere */
```

### C#

```csharp
using var film = engine.OpenFile("film.mkv", new StreamOptions { DynamicRange = DynamicRange.Normal });
Console.WriteLine($"{film.Info.Codec}, {film.Info.Format.Channels} channels");   // eac3, 6 channels
film.PlayOn(output);

film.DynamicRange = DynamicRange.Night;
```

## Seeking and length

Raw `.ac3` and `.ec3` files have an exact length and seeks that land on the exact sample. Nearly every file has a constant bit rate (every frame the same size, one substream): the open then checks a few frames and works out every frame's position, so it takes well under a millisecond even for a film. Other files (variable bit rate, 7.1 with a dependent substream, junk inside) are indexed when they're opened, one hop per frame header without decoding. Over HTTP the length is estimated from the bit rate and seeks jump by byte position to the nearest frame. In MP4 and Matroska the container's index is used.

## Damaged data

A frame that fails its checksum, or doesn't parse, plays as silence for its length (a few to 32 ms) and decoding goes on with the next one: a scratch in a broadcast recording is a short gap, not an error. Junk between frames (a damaged file, or a radio stream joined mid-frame) is skipped.

## Speed and memory

The decoder runs at about 400 times real time for 5.1 and 900 times for stereo on one core (the black-box comparison in `tools/bass-compare`, where bass_ac3 does about 350 and 820), and a 5.1 stream uses about 90 KB (bass_ac3 140 KB, each measured alone under Wine), including its read buffer and one access unit of output. A unit of one syncframe, which is every AC-3 stream and E-AC-3 with a single substream, is mixed block by block straight into that output. 7.1 and other E-AC-3 with more than one substream (a dependent one adding channels, or other programs) also keep the whole unit per speaker, about 6 KB more per channel.

## Threads

Decoding runs on the engine's worker threads like every other format. `fa_stream_set_dynamic_range` and `fa_stream_get_dynamic_range` may be called from any thread.

## Pitfalls

- Loudness normalization scans decode with full dynamic range, so the measured loudness is the same whichever mode you play in. The compressed modes play a little louder in quiet passages than the scan says.
- Without normalization, the stereo downmix plays Dolby 5.1 quieter than the engine's own downmix did (see [stereo downmix](#stereo-downmix)); use normalization, or `FA_DOLBY_DOWNMIX_ENGINE` for the old level.
- Lt/Rt summed to mono loses the surrounds (they cancel). Use Lo/Ro for anything that ends in mono.
- The downmix follows the levels of the frames decoded last, so a stream that changes them mid-way (rare: broadcast programmes joined together) changes about one decode chunk later.
- Dual mono (1+1, two languages) always plays as stereo, one programme per side; there is no setting to pick one.
- Encoding a 5.1 Dolby file read with `fa_stream_read` into a stereo file uses the encoder's own downmix, not the stream's levels. A playing stream recorded with `fa_encoder_attach_stream` has the stream's downmix.
- A damaged frame plays as silence for its 1 to 6 blocks, and decoding carries on with the next frame.
- Passthrough (sending the compressed stream to an AV receiver over S/PDIF or HDMI) is not supported.
