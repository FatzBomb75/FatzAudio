# Dolby Digital and Dolby Digital Plus

FatzAudio decodes Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) itself, in every place audio comes from: `.ac3` and `.ec3` files, AC-3 and E-AC-3 tracks in MP4, MOV and Matroska video files, web streams and HLS, and packets pushed from your own FFmpeg demuxer. There's no add-on to load and no FFmpeg needed. It replaces bass_ac3.

| What | Supported |
|---|---|
| AC-3 (Dolby Digital) | Every mode: mono to 5.1, 32, 44.1 and 48 kHz, all bit rates |
| E-AC-3 (Dolby Digital Plus) | Independent and dependent substreams (7.1), 1 to 6 blocks per frame, spectral extension, transient pre-noise processing, all bit allocation options |
| Atmos (Dolby Digital Plus with JOC) | The 5.1 or 7.1 bed plays; the height objects are not rendered |
| Adaptive hybrid transform, enhanced coupling | Rare E-AC-3 tools: these frames go through the app's FFmpeg when it is loaded, and are silent without it |
| Dolby TrueHD | Through the app's FFmpeg (MP4/MKV tracks and packets) |

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

A layout without its own count uses the next larger one with silent channels, so a stream with an LFE always has at least 6 channels (stereo plus LFE plays as 5.1 with C and the surrounds silent). On a stereo device the engine downmixes automatically, like any other multichannel stream. For your own downmix (louder centre for dialogue, no LFE, music only), set a channel matrix ([buses and the channel matrix](buses.md#channel-matrix)).

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

Raw `.ac3` and `.ec3` files are indexed when they're opened (one hop per frame header, no decoding), so the length is exact and seeks land on the exact sample. Over HTTP the length is estimated from the bit rate and seeks jump by byte position to the nearest frame. In MP4 and Matroska the container's index is used.

## Damaged data

A frame that fails its checksum, or doesn't parse, plays as silence for its length (a few to 32 ms) and decoding goes on with the next one: a scratch in a broadcast recording is a short gap, not an error. Junk between frames (a damaged file, or a radio stream joined mid-frame) is skipped.

## Threads

Decoding runs on the engine's worker threads like every other format. `fa_stream_set_dynamic_range` and `fa_stream_get_dynamic_range` may be called from any thread.

## Pitfalls

- Loudness normalization scans decode with full dynamic range, so the measured loudness is the same whichever mode you play in. The compressed modes play a little louder in quiet passages than the scan says.
- The stream's own downmix levels (centre and surround mix levels, Lt/Rt matrix-surround hints) and its dialogue level ("dialnorm") are not applied. Normalization takes care of level, and the channel matrix of the downmix.
- `FA_ERROR_PROVIDER_MISSING` is never returned for AC-3 or E-AC-3. A rare stream that needs the adaptive hybrid transform or enhanced coupling plays those frames silent when FFmpeg isn't loaded.
- Passthrough (sending the compressed stream to an AV receiver over S/PDIF or HDMI) is not supported.
