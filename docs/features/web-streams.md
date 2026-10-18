# Web streams and HLS

`fa_stream_open_url` plays what is behind an `http://` or `https://` URL. The result is an ordinary stream: attach it, play it, crossfade it, queue it, normalize it, or decode it without playback, exactly like a file.

| What's behind the URL | What you get |
|---|---|
| An audio file on a web server | Plays while it downloads. Seekable when the server supports byte ranges (almost all do), and only the parts you play are downloaded |
| An Icecast or Shoutcast radio station | Live audio, with the song titles as `FA_EVENT_METADATA` at the moment each song is heard |
| An HLS playlist (`.m3u8`), on demand or live | The best audio variant, MPEG-TS, fMP4 or plain audio segments, AES-128 encryption. On-demand HLS is seekable to the frame |
| A `.pls` or `.m3u` playlist (what radio sites usually link to) | Its first entry is played |

The format is detected from the content, as for files ([streams](streams.md#how-detection-works)): WAV, FLAC, MP3, Ogg Opus, Ogg Vorbis, Ogg FLAC, AAC, AC-3 and E-AC-3, also inside HLS segments (MPEG-TS, fMP4 or packed audio). AAC, which most HLS and many radio stations use, including HE-AAC and HE-AAC v2, decodes with FatzAudio's own decoder on every platform, with no FFmpeg ([AAC](streams.md#aac)).

## C

```c
#include <fatzaudio/fatzaudio.h>

static void FA_CALL on_event(const fa_event* ev, void* user)
{
    fa_metadata md;
    switch (ev->type) {
    case FA_EVENT_METADATA:                 /* a new song is heard now */
        fa_stream_get_metadata(ev->stream, &md);
        printf("Now playing: %s - %s\n", md.artist, md.title);
        break;
    case FA_EVENT_STALLED: printf("buffering...\n"); break;
    case FA_EVENT_RESUMED: printf("playing again\n"); break;
    case FA_EVENT_ENDED:   printf("the station went away\n"); break;
    default: break;
    }
}

fa_stream* radio;
fa_result r = fa_stream_open_url(engine, "https://stream.example.com/live.mp3", NULL, &radio);
if (r != FA_OK) { printf("can't open: %s\n", fa_result_string(r)); return; }

fa_stream_set_event_callback(radio, on_event, NULL);
fa_stream_attach(radio, device, NULL);
fa_stream_play(radio);

fa_metadata md;
fa_stream_get_metadata(radio, &md);
printf("%s (%s, %u kbps)\n", md.station, md.genre, md.bitrate_kbps);
```

### Connection settings

```c
fa_net_options net;
fa_net_options_init(&net);
net.headers = "Authorization: Bearer abc123\r\nCookie: session=xyz";
net.prebuffer_ms = 3000;        /* ride out a weak mobile connection */
net.hls_max_bandwidth = 192000; /* HLS: at most 192 kbit/s */

fa_open_options opts;
fa_open_options_init(&opts);
opts.net = &net;                /* copied by the call */
fa_stream_open_url(engine, "https://cdn.example.com/show/master.m3u8", &opts, &s);
```

| `fa_net_options` field | Default | Meaning |
|---|---|---|
| `user_agent` | `FatzAudio/<version>` | The User-Agent header |
| `headers` | none | Extra request headers, `Name: value` lines separated by `\r\n` or `\n`. Sent with every request, HLS segments and keys included |
| `proxy` | the system's | `NULL` uses the `http_proxy`/`https_proxy`/`no_proxy` environment variables and the Windows and macOS proxy settings. `""` connects directly. Or `"host:port"`, `"http://user:password@host:port"` |
| `timeout_ms` | 10000 | How long connecting may take, and the longest the server may stay silent before the connection counts as dropped |
| `prebuffer_ms` | 1000 | Audio buffered before playback starts, and again before it resumes after a stall |
| `reconnect_ms` | 30000 | How long to keep reconnecting after the connection drops |
| `flags` | 0 | `FA_NET_NO_RECONNECT`: end the stream when the connection drops. `FA_NET_NO_ICY`: don't ask radio servers for song titles |
| `hls_max_bandwidth` | 0 (best) | HLS: the best variant up to this many bits per second. Audio-only variants are preferred when the playlist has them |

### Status and headers

```c
fa_net_status st;
fa_stream_get_net_status(s, &st);   /* FA_ERROR_INVALID_ARG for streams that aren't web streams */
printf("%.1f s buffered, %lld bytes downloaded, %u reconnects, now at %s\n",
       st.buffered / 1e9, (long long)st.downloaded, st.reconnects, st.url);

char type[128];
if (fa_stream_get_http_header(s, "Content-Type", type, sizeof(type)) == FA_OK)
    printf("served as %s\n", type);
```

`fa_net_status` also says whether the stream is live (`is_live`), HLS (`is_hls`) or seekable, whether a connection is open right now, the last HTTP status, the file size (`content_length`, -1 for live), and why the last connection failed (`last_error`).

## C#

```csharp
using var radio = engine.OpenUrl("https://stream.example.com/live.mp3");
radio.MetadataChanged += (_, e) => Console.WriteLine($"Now playing: {e.Metadata.Artist} - {e.Metadata.Title}");
radio.Stalled += (_, _) => Console.WriteLine("buffering...");
radio.Resumed += (_, _) => Console.WriteLine("playing again");
radio.PlayOn(output);

Console.WriteLine($"{radio.Metadata.Station}, {radio.Metadata.BitrateKbps} kbps");
Console.WriteLine($"{radio.NetworkStatus!.Buffered.TotalSeconds:F1} s buffered");
```

With settings:

```csharp
var opts = new StreamOptions
{
    Network = new NetworkOptions
    {
        Headers = new Dictionary<string, string> { ["Authorization"] = "Bearer abc123" },
        Prebuffer = TimeSpan.FromSeconds(3),
        HlsMaxBandwidth = 192_000,
    },
};
using var show = engine.OpenUrl("https://cdn.example.com/show/master.m3u8", opts);
```

| .NET | C |
|---|---|
| `engine.OpenUrl(string or Uri, options)` | `fa_stream_open_url` |
| `StreamOptions.Network` (`NetworkOptions`) | `fa_open_options.net` (`fa_net_options`); `Reconnect = false` and `SongTitles = false` are the two flags |
| `stream.Metadata` (`StreamMetadata`) | `fa_stream_get_metadata` |
| `stream.MetadataChanged` (with the new `Metadata`) | `FA_EVENT_METADATA` |
| `stream.NetworkStatus` (null for other streams) | `fa_stream_get_net_status` |
| `stream.GetHttpHeader(name)` (null when absent) | `fa_stream_get_http_header` |
| `stream.Stalled`, `stream.Resumed` | `FA_EVENT_STALLED`, `FA_EVENT_RESUMED` |

A URL that isn't an absolute http or https URL throws `ArgumentException`. Other failures throw `FatzAudioException` with the codes below.

## Opening

`fa_stream_open_url` connects, follows redirects (up to 8) and playlists, and waits for the first few KB so the format can be detected. That takes one round trip or a few, and at most twice `timeout_ms` when the server is slow, so call it off your UI thread. Everything after that runs on the stream's own download thread.

| Result | Why |
|---|---|
| `FA_OK` | Connected and recognised |
| `FA_ERROR_INVALID_ARG` | Not an absolute `http://` or `https://` URL |
| `FA_ERROR_NETWORK` | The server can't be reached, refused the connection, or timed out |
| `FA_ERROR_NOT_FOUND` | The server answered with an error (404, 403, 500, ...) |
| `FA_ERROR_UNSUPPORTED_FORMAT` | What came back isn't audio FatzAudio can decode (a web page, say) |
| `FA_ERROR_PROTECTED_CONTENT` | The stream is DRM-protected: HLS with `SAMPLE-AES` or another DRM key method, or an encrypted file |
| `FA_ERROR_PROVIDER_MISSING` | `https://` on a Linux system without libcurl |

## Buffering, stalls and reconnects

- The download thread keeps up to 4 MB per stream, at most 2 MB ahead of playback. Decoding only starts on data that's already downloaded, so neither the audio thread nor the decode workers ever wait for the network.
- After `fa_stream_play` the stream stays silent, in `FA_STATE_BUFFERING`, until `prebuffer_ms` of audio are in, then plays. No event is raised for that first fill.
- When the network can't keep up and the buffer runs dry, the stream goes to `FA_STATE_BUFFERING` and raises `FA_EVENT_STALLED`. Once `prebuffer_ms` are buffered again it plays on from where it stopped and raises `FA_EVENT_RESUMED`. Pause, stop and seek work in either state.
- A dropped connection is re-established by itself, with waits growing from 250 ms to 4 s, for up to `reconnect_ms`. A file continues at the byte where it stopped (with a Range request; if the server ignores ranges, the bytes already played are skipped). Radio picks up the live stream. If reconnecting fails for that long, the stream plays what it has and ends with `FA_EVENT_ENDED`.

## Seeking

| Stream | Seekable |
|---|---|
| File, server supports byte ranges | Yes. A seek outside what's downloaded reconnects at the right byte |
| File, no byte ranges | No (`FA_ERROR_NOT_SEEKABLE`), plays from start to end |
| Radio, live HLS | No |
| On-demand HLS | Yes, to the exact frame: the download restarts at the segment, and decoding starts there |

MP3 and AAC files over the network seek by estimated byte position, as most players do for files without an index, and land within a few frames of the target. The reported position is the one you asked for. WAV and FLAC seek exactly.

## Song titles (ICY metadata)

Radio servers send the title of the song inside the audio data, every few KB. The title is downloaded seconds before you hear it (the buffer), so FatzAudio keeps it until playback reaches the byte where it was sent, then raises `FA_EVENT_METADATA`. `value` is the title's `seq` (1, 2, ...), and `fa_stream_get_metadata` returns the title heard now, never one that is still in the buffer. Events are delivered on the event thread, so when titles change faster than that (a decode-only stream read faster than real time), `fa_stream_get_metadata` in the handler can already return a newer title: compare its `seq` with the event's `value`. The .NET `MetadataChanged` skips such stale events and raises only the latest title.

Ogg radio (Opus or Vorbis) has no ICY titles: every song starts a new Ogg link with its own comment header, and its `ARTIST` and `TITLE` become `stream_title` ("Artist - Title"), announced the same way at the song's first sample.

`stream_title` is the title exactly as the station sends it; `artist` and `title` split it at the first `" - "`. Titles in Latin-1, which older servers send, are converted to UTF-8. Station details (`station`, `genre`, `description`, `station_url`, `bitrate_kbps`) come from the `icy-*` response headers.

## HLS

- **Variant choice:** the highest bandwidth up to `hls_max_bandwidth`, audio-only variants first. A playlist with a separate audio rendition (`EXT-X-MEDIA TYPE=AUDIO`) plays that rendition, the default one first.
- **Live:** starts three segments from the end, as players usually do, and reloads the playlist every half target duration.
- **Segments:** MPEG-TS (the audio track is taken out of the transport stream), fMP4 with `EXT-X-MAP`, and packed audio (ID3 timestamps skipped). `EXT-X-BYTERANGE` and `EXT-X-KEY METHOD=AES-128` (keys fetched once and cached) are supported; SAMPLE-AES (DRM) isn't and returns `FA_ERROR_PROTECTED_CONTENT`.
- `fa_stream_get_info` reports container `hls` and the codec of the segments. On-demand playlists have an exact length from the `EXTINF` durations.

## With the rest of the engine

- **Normalization:** web streams can't be scanned ahead, so they use the running loudness estimate, which adapts as the program changes ([normalization](normalization.md)). If your app knows a station's loudness, give it with `fa_stream_set_loudness`.
- **Crossfades and queues:** work as for files. A web stream queued with `fa_stream_set_next` connects and prebuffers when you open it, so it's ready at the mix point.
- **Decode only:** `fa_stream_read` waits for the network when the buffer is empty, as a file read waits for the disk.

## HTTPS

FatzAudio never bundles a TLS library. HTTPS goes through the system's stack, so certificates, corporate proxies and the system's security settings apply as they do for every other app:

| Platform | Stack |
|---|---|
| Windows | WinHTTP (SChannel) |
| macOS, iOS | URLSession; App Transport Security rules apply |
| Linux | libcurl, loaded at runtime (`libcurl.so.4`, installed almost everywhere). Without it, `https://` returns `FA_ERROR_PROVIDER_MISSING`; `http://` still works |
| Android | 🗓️ (HttpURLConnection through JNI) |

Plain `http://` uses FatzAudio's own small HTTP/1.1 client on every platform. It understands Shoutcast's `ICY 200 OK` answers, which some HTTP stacks reject.

## Not provided yet

- A callback that receives the raw downloaded bytes (BASS's `DOWNLOADPROC`), for recording a station to a file.
- Opening asynchronously with a cancel call (`BASS_StreamCancel`).
