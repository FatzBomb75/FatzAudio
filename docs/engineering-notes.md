# Engineering notes

Known issues and follow-up work that were found while doing something else and
kept out of that change so it stayed small. Add new entries at the top. When an
item is fixed, delete its entry in the same change and mention the PR there.

Each entry has a date, the area it touches, what's wrong or missing, and where it
was found (a PR, a review, or a thread).

| Date | Area | Issue | Found in |
|---|---|---|---|
| 2026-10-17 | Decoding | Decoded audio takes the timestamp of the packet that produced it. A codec that holds input back (a PCM sample split across packets, an FFmpeg decoder with delay) can put the first audio of a push stream at the wrong pts, or a container track a few frames late; the codec layer should carry pts and a frame offset through with its output. The FFmpeg audio decoders used today (AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC) give one frame per packet with no delay, so nothing is wrong in practice yet. | PR 8 review |
| 2026-10-17 | CI | Android and iOS build-only jobs are still TODOs in `.github/workflows/build.yml`. | CI trigger trim |
