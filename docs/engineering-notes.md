# Engineering notes

Known issues and follow-up work that were found while doing something else and
kept out of that change so it stayed small. Add new entries at the top. When an
item is fixed, delete its entry in the same change and mention the PR there.

Each entry has a date, the area it touches, what's wrong or missing, and where it
was found (a PR, a review, or a thread).

| Date | Area | Issue | Found in |
|---|---|---|---|
| 2026-10-17 | Web streams | A live HTTP stream with no `icy-*` headers and no Content-Length that the server closes cleanly is treated as a finished file and not reconnected; the transports would need to report whether a body ended with a chunked terminator. | Web streams thread (/code-review) |
| 2026-10-17 | Web streams | AAC radio and AAC HLS segments (TS and fMP4) need an AAC/ADTS decoder, to add on the FFmpeg codec bridge once the FFmpeg hand-off is merged. | Web streams thread |
| 2026-10-17 | Web streams | Android has no HTTPS transport yet (HttpURLConnection through JNI); `https://` returns `FA_ERROR_PROVIDER_MISSING` there. | Web streams thread |
| 2026-10-17 | Web streams | The macOS/iOS URLSession backend (`src/net_https_apple.m`) compiles in CI but has not been run against a real server; worth a radio and HLS check on a Mac. | Web streams thread |
| 2026-10-17 | CI | Android and iOS build-only jobs are still TODOs in `.github/workflows/build.yml`. | CI trigger trim |
