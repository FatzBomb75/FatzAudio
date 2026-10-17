# Feature guides

One page per feature, each with: what it does, C example, C# example, options, threading notes and pitfalls. Pages are added as features land (see the [status legend](../README.md#status-legend)).

| Feature | Page | Status |
|---|---|---|
| Devices (output, start/stop, silent output) | [devices.md](devices.md) | ✅ output; input and default following 🗓️ |
| Opening and decoding streams (format detection, custom I/O, MP4 and MKV audio tracks) | [streams.md](streams.md) | ✅ |
| Playback, buffering and threads (decode-ahead, transport, device clock) | [playback.md](playback.md) | ✅ |
| Volume, pan and ramps (7 curves, scheduled starts, ramp-done) | [ramps.md](ramps.md) | ✅ volume and pan |
| Events | [events.md](events.md) | ✅ ended, ramp done, marker, started, loudness final, metadata, stalled/resumed, decode error |
| Loudness normalization (R128 scans, tags, cache, limiter) | [normalization.md](normalization.md) | ✅ |
| Crossfades, markers, queues and gapless, scheduled starts | [crossfade.md](crossfade.md) | ✅ |
| Buses (decks, groups) and the channel matrix | [buses.md](buses.md) | ✅ |
| Tempo, pitch and rate (time-stretch, key change, varispeed, formants) | [tempo-pitch.md](tempo-pitch.md) | ✅ (tempo and rate on push streams 🗓️) |
| FFmpeg hand-off and A/V sync (packets, frames, presentation clock) | [ffmpeg.md](ffmpeg.md) | ✅ |
| Web streams and HLS (HTTP/HTTPS, radio titles, reconnects, HLS) | [web-streams.md](web-streams.md) | ✅ |
| CD+G karaoke graphics (MP3+G, ZIP, surfaces for any renderer) | [cdg.md](cdg.md) | ✅ files; disc 🗓️ |
| Dolby Digital and Dolby Digital Plus (AC-3, E-AC-3, up to 7.1, dynamic range) | [dolby.md](dolby.md) | ✅ (passthrough 🗓️) |
| Disc playback | disc.md | 🗓️ |
| Recording and encoding | recording-encoding.md | 🗓️ |
