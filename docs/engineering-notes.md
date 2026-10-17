# Engineering notes

Known issues and follow-up work that were found while doing something else and
kept out of that change so it stayed small. Add new entries at the top. When an
item is fixed, delete its entry in the same change and mention the PR there.

Each entry has a date, the area it touches, what's wrong or missing, and where it
was found (a PR, a review, or a thread).

| Date | Area | Issue | Found in |
|---|---|---|---|
| 2026-10-17 | Tests | `test_reconnect_resumes` in tests/test_net.c can fail under heavy CPU load (`st.finished` still 0 right after the last frame is read): the download thread may not have recorded the end yet. Fails on main too; the check needs a short wait for `finished`. | Tempo and pitch thread |
| 2026-10-17 | Tempo/pitch | A tempo, pitch or rate ramp raises `FA_EVENT_RAMP_DONE` when its end is applied on the decode side, up to `latency_ms` (150 ms) before that end is heard; volume and pan ramps report when it is heard. Queueing the event until the ring frame plays would make them match. Documented in the feature guide. | Tempo and pitch thread (/code-review) |
| 2026-10-17 | Tempo/pitch | The phase vocoder leaves a faint pre-echo before sharp hits (about -25 dB, measured by `tools/tempo-compare`); bass_fx and SoundTouch have none but move the hits by 2 to 24 ms. A transient-preserving mode (or evaluating the Bungee stretcher) could remove it for drum-heavy material. | Tempo and pitch thread |
| 2026-10-17 | Resampling | Device-rate conversion still uses miniaudio's linear resampler, while rate and pitch use the new windowed-sinc resampler (`src/resample.c`). Moving the device-rate converter onto the sinc resampler leaves one resampler and improves 44.1 to 48 kHz playback. | Tempo and pitch thread |
| 2026-10-17 | Tempo/pitch | `FA_PARAM_TEMPO` and `FA_PARAM_RATE` return `FA_ERROR_NOT_IMPLEMENTED` on push and packet streams (their timing is the app's); doing them needs the push queue and the clock to run at a different speed than the pts. Pitch works. | Tempo and pitch thread |
| 2026-10-17 | Tempo/pitch | A forward-only stream played at a very high tempo and then moved to another device may need more history than `hist_cap` keeps for re-attaching at the heard position; it then resumes slightly later. | Tempo and pitch thread (/code-review) |
| 2026-10-17 | Tempo/pitch | bass_fx's reverse playback (`BASS_FX_ReverseCreate`) and BPM/beat detection are not implemented yet. | Tempo and pitch thread |
| 2026-10-17 | CD+G | While a song uses the smooth-scroll offsets, every changed tile redraws the whole surface (the dirty rectangle is not shifted with the offsets). Rare in practice; map the shifted rectangle if a renderer ever needs it. | CD+G thread (/code-review) |
| 2026-10-17 | CD+G | CD+EG (extended graphics) packs are ignored, and the design's optional renderer helpers (D3D11 texture updater, CVPixelBuffer) are not built; apps upload the pixel buffer themselves. | CD+G thread |
| 2026-10-17 | CD+G | ZIP64, encrypted and multi-part archives are refused (`FA_ERROR_UNSUPPORTED_FORMAT`); karaoke downloads do not use them. | CD+G thread |
| 2026-10-17 | Web streams | The MP4 and Matroska demuxers don't know about network sources yet. A seekable fragmented MP4 URL walks every fragment at open (a range request each), and Matroska with Cues after the clusters costs two extra requests. Audio chunks far ahead of the download make a worker wait on the network while it holds the stream's decode lock, because the decode gate only counts bytes past the read position. They need to use `sidx`/`mfra` indexes and tell the gate where they will read next. | PR 8 merge review |
| 2026-10-17 | Decoding | Decoded audio takes the timestamp of the packet that produced it. A codec that holds input back (a PCM sample split across packets, an FFmpeg decoder with delay) can put the first audio of a push stream at the wrong pts, or a container track a few frames late; the codec layer should carry pts and a frame offset through with its output. The FFmpeg audio decoders used today (AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC) give one frame per packet with no delay, so nothing is wrong in practice yet. | PR 8 review |
| 2026-10-17 | Web streams | A live HTTP stream with no `icy-*` headers and no Content-Length that the server closes cleanly is treated as a finished file and not reconnected; the transports would need to report whether a body ended with a chunked terminator. | Web streams thread (/code-review) |
| 2026-10-17 | Web streams | MP4/MKV URLs and fMP4 HLS segments reach the MP4 and Matroska demuxers but are untested over the network so far. | Web streams thread, PR 8 merge |
| 2026-10-17 | Web streams | Android has no HTTPS transport yet (HttpURLConnection through JNI); `https://` returns `FA_ERROR_PROVIDER_MISSING` there. | Web streams thread |
| 2026-10-17 | Web streams | The macOS/iOS URLSession backend (`src/net_https_apple.m`) compiles in CI but has not been run against a real server; worth a radio and HLS check on a Mac. | Web streams thread |
| 2026-10-17 | CI | Android and iOS build-only jobs are still TODOs in `.github/workflows/build.yml`. | CI trigger trim |
