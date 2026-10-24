# Feature guides

One page per feature, each with: what it does, C example, C# example, options, threading notes and pitfalls. Pages are added as features land (see the [status legend](../README.md#status-legend)).

| Feature | Page | Status |
|---|---|---|
| Devices (output, start/stop, silent output) | [devices.md](devices.md) | ✅ output and default following; input 🗓️ |
| Opening and decoding streams (format detection, custom I/O, MP4 and MKV audio tracks, built-in AAC, Ogg Opus, Vorbis and FLAC, FLAC in MP4 and MKV) | [streams.md](streams.md) | ✅ |
| Playback, buffering and threads (decode-ahead, transport, device clock) | [playback.md](playback.md) | ✅ |
| Volume, pan and ramps (7 curves, scheduled starts, ramp-done) | [ramps.md](ramps.md) | ✅ volume and pan |
| Events | [events.md](events.md) | ✅ ended, ramp done, marker, started, loudness final, metadata, stalled/resumed, decode error, MIDI text |
| Loudness normalization (R128 scans, tags, cache, limiter) | [normalization.md](normalization.md) | ✅ |
| Track analysis (beat grid and BPM, musical key, waveform overview; exact grids from MIDI and tracker files; cache) | [analysis.md](analysis.md) | ✅ (tempo maps and bar detection from audio 🗓️) |
| Crossfades, markers, queues and gapless, scheduled starts | [crossfade.md](crossfade.md) | ✅ |
| Buses (decks, groups) and the channel matrix | [buses.md](buses.md) | ✅ |
| DJ mixer (two decks, isolator EQ with kills, crossfader curves, headphone cue on outputs 3-4 or a Y-cable, seamless deck loops, cue points, meters, two karaoke mics with gate, EQ, reverb, echo, host ducking, latency report, a measured setup check with feedback-safe gain, opt-in Windows exclusive mode and aligned stem recording, a headphone cue on a second device, beat sync with lock, beat loops and auto-mix) | [dj-mixer.md](dj-mixer.md) | ✅ (more decks 🗓️) |
| Tempo, pitch and rate (time-stretch, key change, varispeed, formants) | [tempo-pitch.md](tempo-pitch.md) | ✅ |
| FFmpeg hand-off and A/V sync (packets, frames, presentation clock) | [ffmpeg.md](ffmpeg.md) | ✅ |
| Web streams and HLS (HTTP/HTTPS, radio titles, reconnects, HLS) | [web-streams.md](web-streams.md) | ✅ |
| CD+G karaoke graphics (MP3+G, ZIP, CD+G discs, surfaces for any renderer) | [cdg.md](cdg.md) | ✅ |
| MIDI lyric surface (karaoke lyrics drawn for you: lines, pages, wipes, countdown, presets, effects) | [lyrics.md](lyrics.md) | ✅ (intro and ending pages 🗓️) |
| Dolby Digital and Dolby Digital Plus (AC-3, E-AC-3, up to 7.1, dynamic range) | [dolby.md](dolby.md) | ✅ (passthrough 🗓️) |
| Disc playback (audio CDs, CD+G discs and `.cue` images) | [disc.md](disc.md) | ✅ |
| MIDI and karaoke files (SoundFont 2 and DLS, the OS's gm.dls by default, lyrics, transpose, channel mute) | [midi.md](midi.md) | ✅ (SF3, SFZ and MIDI input 🗓️) |
| Tracker modules (MOD, XM, S3M, IT, MPTM and more via libopenmpt; info and names, order/row seeking, interpolation, stereo separation, channel mute) | [trackers.md](trackers.md) | ✅ (currently-heard order/row and row events 🗓️) |
| Recording and encoding (WAV, FLAC, MP3 via LAME; device, stream or file sources) | [recording-encoding.md](recording-encoding.md) | ✅ WAV/FLAC/MP3; Opus, AAC and casting 🗓️ |
| Equalizer, presets and user DSP (parametric/graphic EQ, 14 presets, `fa_stream_add_dsp`) | [equalizer.md](equalizer.md) | ✅ |
| Levels and spectrum (VU meters, FFT for visualizers; per stream or the whole output) | [levels-spectrum.md](levels-spectrum.md) | ✅ |
| Tags, cover art and file properties (ID3v1/v2, APEv2, Vorbis comments, MP4, ASF, RIFF, AIFF, Matroska; bitrate, sizes) | [tags.md](tags.md) | ✅ |
