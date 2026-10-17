# Engineering notes

Known issues and follow-up work that were found while doing something else and
kept out of that change so it stayed small. Add new entries at the top. When an
item is fixed, delete its entry in the same change and mention the PR there.

Each entry has a date, the area it touches, what's wrong or missing, and where it
was found (a PR, a review, or a thread).

| Date | Area | Issue | Found in |
|---|---|---|---|
| 2026-10-17 | Disc | Real drives have not been tested yet: the MMC layer, CD-Text, C2 retries and the Linux and Windows backends are covered by a simulated drive and `.cue` images in `tests/test_disc.c`. Worth a check with a real drive (and an Enhanced CD and a scratched disc) on Windows and Linux. | Disc playback thread |
| 2026-10-17 | Disc | The macOS backend (`src/disc_mac.c`, DKIOCCD ioctls) only compiles in CI and has never run against a drive. It lists only drives with a disc in them, and cannot close the tray or lock it. | Disc playback thread |
| 2026-10-17 | Disc | No jitter correction (cdparanoia-style overlap matching) for old drives without accurate stream; reads rely on C2 retries and the drive's own correction. Track pregaps (`BASS_CD_GetTrackPregap`) are not reported. | Disc playback thread |
| 2026-10-17 | CD+G | While a song uses the smooth-scroll offsets, every changed tile redraws the whole surface (the dirty rectangle is not shifted with the offsets). Rare in practice; map the shifted rectangle if a renderer ever needs it. | CD+G thread (/code-review) |
| 2026-10-17 | CD+G | CD+EG (extended graphics) packs are ignored, and the design's optional renderer helpers (D3D11 texture updater, CVPixelBuffer) are not built; apps upload the pixel buffer themselves. | CD+G thread |
| 2026-10-17 | CD+G | ZIP64, encrypted and multi-part archives are refused (`FA_ERROR_UNSUPPORTED_FORMAT`); karaoke downloads do not use them. | CD+G thread |
| 2026-10-17 | Web streams | The MP4 and Matroska demuxers don't know about network sources yet. A seekable fragmented MP4 URL walks every fragment at open (a range request each), and Matroska with Cues after the clusters costs two extra requests. Audio chunks far ahead of the download make a worker wait on the network while it holds the stream's decode lock, because the decode gate only counts bytes past the read position. They need to use `sidx`/`mfra` indexes and tell the gate where they will read next. | PR 8 merge review |
| 2026-10-17 | Decoding | Decoded audio takes the timestamp of the packet that produced it. A codec that holds input back (a PCM sample split across packets, an FFmpeg decoder with delay) can put the first audio of a push stream at the wrong pts, or a container track a few frames late; the codec layer should carry pts and a frame offset through with its output. The FFmpeg audio decoders used today (AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC) give one frame per packet with no delay, so nothing is wrong in practice yet. | PR 8 review |
| 2026-10-17 | Web streams | A live HTTP stream with no `icy-*` headers and no Content-Length that the server closes cleanly is treated as a finished file and not reconnected; the transports would need to report whether a body ended with a chunked terminator. | Web streams thread (/code-review) |
| 2026-10-17 | Web streams | AAC radio and AAC in MPEG-TS HLS segments need an ADTS reader on top of the FFmpeg codec bridge (merged with the FFmpeg hand-off). MP4/MKV URLs and fMP4 HLS segments now reach the MP4 and Matroska demuxers, untested over the network so far. | Web streams thread, PR 8 merge |
| 2026-10-17 | Web streams | Android has no HTTPS transport yet (HttpURLConnection through JNI); `https://` returns `FA_ERROR_PROVIDER_MISSING` there. | Web streams thread |
| 2026-10-17 | Web streams | The macOS/iOS URLSession backend (`src/net_https_apple.m`) compiles in CI but has not been run against a real server; worth a radio and HLS check on a Mac. | Web streams thread |
| 2026-10-17 | CI | Android and iOS build-only jobs are still TODOs in `.github/workflows/build.yml`. | CI trigger trim |
