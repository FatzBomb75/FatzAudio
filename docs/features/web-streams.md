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
| `headers` | none | Extra request headers, `Name: value` lines separated by `\r\n` or `\n`. Sent with every request, HLS segments and keys included ([rules](#request-headers)) |
| `proxy` | the system's | `NULL` uses the `http_proxy`/`https_proxy`/`no_proxy` environment variables and the Windows, macOS and Android proxy settings. `""` connects directly. Or `"host:port"`, `"http://user:password@host:port"` |
| `timeout_ms` | 10000 | How long connecting may take, and the longest the server may stay silent before the connection counts as dropped |
| `prebuffer_ms` | 1000 | Audio buffered before playback starts, and again before it resumes after a stall |
| `reconnect_ms` | 30000 | How long to keep reconnecting after the connection drops |
| `flags` | 0 | `FA_NET_NO_RECONNECT`: end the stream when the connection drops. `FA_NET_NO_ICY`: don't ask radio servers for song titles |
| `hls_max_bandwidth` | 0 (best) | HLS: the best variant up to this many bits per second. Audio-only variants are preferred when the playlist has them; a variant with an audio group plays that group's rendition ([HLS](#hls)) |
| `download_proc`, `download_user` | none | Receives the stream's raw bytes from the first one on ([saving a station](#saving-a-station-as-it-plays)) |

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

A response whose headers exceed 64 KB is refused as a network error, with every HTTP and HTTPS backend alike.

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
| `NetworkOptions.Downloaded`, `stream.SetDownloadHandler(handler)` | `fa_net_options.download_proc`, `fa_stream_set_download_callback` ([saving a station](#saving-a-station-as-it-plays)) |

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
| `FA_ERROR_PROVIDER_MISSING` | `https://` on a Linux system without libcurl, or on Android before the Java VM is known ([Android](#android)) |

## Buffering, stalls and reconnects

- The download thread keeps up to 4 MB per stream, at most 2 MB ahead of playback; a seekable file smaller than that gets a buffer of its size plus 64 KB. A stream that can't seek (radio, HLS, a server without byte ranges) keeps only the part ahead, about 2 MB. Radio that states its bitrate (`icy-br`) keeps less: twice `prebuffer_ms` plus 5 s of audio, at least 256 KB (a 128 kbps station with the default prebuffer: 256 KB). A station whose `icy-br` is too low gets a shorter prebuffer than asked, at most 3/4 of that window. Decoding only starts on data that's already downloaded, so neither the audio thread nor the decode workers ever wait for the network.
- After `fa_stream_play` the stream stays silent, in `FA_STATE_BUFFERING`, until `prebuffer_ms` of audio are in, then plays. No event is raised for that first fill.
- When the network can't keep up and the buffer runs dry, the stream goes to `FA_STATE_BUFFERING` and raises `FA_EVENT_STALLED`. Once `prebuffer_ms` are buffered again it plays on from where it stopped and raises `FA_EVENT_RESUMED`. Pause, stop and seek work in either state.
- A dropped connection is re-established by itself, with waits growing from 250 ms to 4 s, for up to `reconnect_ms`. A file continues at the byte where it stopped (with a Range request; if the server ignores ranges, the bytes already played are skipped). Radio picks up the live stream. If reconnecting fails for that long, the stream plays what it has and ends with `FA_EVENT_ENDED`.
- A live stream without radio headers, length or chunked encoding that the server closes in an orderly way is reconnected too: such a close can't be told from a dropped connection, so it isn't taken as the end of a file. A body with a `Content-Length` or chunked encoding (or over HTTP/2) ends when it says it does.
- `fa_stream_close` (.NET `Dispose`) returns at once, also while a connection is being made or a host name looked up: a lookup that hangs is left to finish on its own.

## Seeking

| Stream | Seekable |
|---|---|
| File, server supports byte ranges | Yes. A seek outside what's downloaded reconnects at the right byte |
| File, no byte ranges | No (`FA_ERROR_NOT_SEEKABLE`), plays from start to end |
| Radio, live HLS | No |
| On-demand HLS | Yes, to the exact frame: the download restarts at the segment, and decoding starts there |

A seek outside what's downloaded waits for a network round trip. From a UI thread or a seek bar, use `fa_stream_seek_async` (.NET `RequestSeek`), which returns at once and lets a decode worker wait for the server instead ([what a seek costs](playback.md#what-a-seek-costs)).

MP3 and AAC files over the network seek by estimated byte position, as most players do for files without an index, and land within a few frames of the target. The reported position is the one you asked for. WAV and FLAC seek exactly.

## Song titles (ICY metadata)

Radio servers send the title of the song inside the audio data, every few KB. The title is downloaded seconds before you hear it (the buffer), so FatzAudio keeps it until playback reaches the byte where it was sent, then raises `FA_EVENT_METADATA`. `value` is the title's `seq` (1, 2, ...), and `fa_stream_get_metadata` returns the title heard now, never one that is still in the buffer. Events are delivered on the event thread, so when titles change faster than that (a decode-only stream read faster than real time), `fa_stream_get_metadata` in the handler can already return a newer title: compare its `seq` with the event's `value`. The .NET `MetadataChanged` skips such stale events and raises only the latest title.

Ogg radio (Opus or Vorbis) has no ICY titles: every song starts a new Ogg link with its own comment header, and its `ARTIST` and `TITLE` become `stream_title` ("Artist - Title"), announced the same way at the song's first sample.

`stream_title` is the title exactly as the station sends it; `artist` and `title` split it at the first `" - "`. Titles in Latin-1, which older servers send, are converted to UTF-8 (anything that is not well-formed UTF-8 is taken as Latin-1). Station details (`station`, `genre`, `description`, `station_url`, `bitrate_kbps`) come from the `icy-*` response headers.

## Saving a station as it plays

The bytes a stream downloads can go to your code as they arrive (BASS's `DOWNLOADPROC`), to record a station to a
file while it plays. They are the bytes the decoder reads, so written to a file they play as one:

| Stream | What the callback gets |
|---|---|
| A file | The file, byte for byte as the server sends it |
| Radio | The audio, without the ICY title blocks the server puts in between |
| HLS | The audio of the segments, decrypted; MPEG-TS segments as the audio stream inside (ADTS AAC, MP3, AC-3) |

```c
static void FA_CALL save(void* user, const void* data, size_t size)
{
    fwrite(data, 1, size, (FILE*)user);    /* the download waits while this runs: keep it quick */
}

FILE* f = fopen("recording.mp3", "wb");
fa_net_options net;
fa_net_options_init(&net);
net.download_proc = save;                  /* from the first byte on */
net.download_user = f;
/* ... open with opts.net = &net, play ... */

fa_stream_set_download_callback(radio, NULL, NULL);   /* stop: not called again after this returns */
fclose(f);
```

`fa_stream_set_download_callback` starts, changes or stops it while the stream plays (from the next downloaded byte;
the first bytes only arrive through `download_proc`). It may also be called from inside the callback. The callback
runs on the stream's download thread, one call at a time, in order. After a seek that restarts the download
elsewhere in a file, the bytes go on from there.

```csharp
var file = File.Create("recording.mp3");
var radio = engine.OpenUrl(url, new StreamOptions { Network = new NetworkOptions { Downloaded = data => file.Write(data) } });
// ... later, while it plays:
radio.SetDownloadHandler(null);   // or SetDownloadHandler(handler) to start recording mid-stream
file.Dispose();
```

## HLS

- **Variant choice:** the highest bandwidth up to `hls_max_bandwidth`, audio-only variants first. When the chosen variant names an audio group (`AUDIO=` and `EXT-X-MEDIA TYPE=AUDIO`), the group's rendition plays: its `DEFAULT=YES` one, else its `AUTOSELECT=YES` one, else its first. So a playlist with a 64 kbit/s audio group for its low tiers and a 192 kbit/s one for its high tiers plays the low one under a `hls_max_bandwidth` that picks a low tier. A rendition without a URI (the audio is muxed into the variant) plays the variant itself.
- **Live:** starts three segments from the end, as players usually do, and reloads the playlist every half target duration.
- **Segments:** MPEG-TS (the audio track is taken out of the transport stream), fMP4 with `EXT-X-MAP` (also a `BYTERANGE` of it, as single-file fMP4 playlists use; only that range is fetched), and packed audio (ID3 timestamps skipped). `EXT-X-BYTERANGE` and `EXT-X-KEY METHOD=AES-128` (keys fetched once and cached) are supported; SAMPLE-AES (DRM) isn't and returns `FA_ERROR_PROTECTED_CONTENT`.
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
| Android | `HttpURLConnection` (Conscrypt), called through JNI on the app's Java VM. The app's network security config applies |

Plain `http://` uses FatzAudio's own small HTTP/1.1 client on every platform. It understands Shoutcast's `ICY 200 OK` answers, which some HTTP stacks reject.

### Connections

The requests of one stream share their connections. An HLS stream fetches a master playlist, a media playlist,
maybe a key, and then a segment every few seconds; over `https://` all of them go over one connection per server,
so only the first pays for the TCP and TLS handshakes. With a 100 ms round trip that makes an HLS start about half
a second faster (measured on Linux: 1.2 s to 0.7 s to the first audio, five connections down to one). A server that
closed the kept connection in the meantime gets a new one, without an error. A seek in the middle of a download
closes that connection (the rest of the old body isn't read), so the request after it connects again. Plain
`http://` opens a connection per request. Streams never share connections with each other.

### Redirects

FatzAudio follows redirects itself, up to 8, the same way on every platform and stack, also from `https://` to
`http://`. Credentials go along only to where they belong:

- The URL's own credentials (`https://user:password@host/...`) are sent to the URL's own origin (scheme, host and
  port) only. A redirect to the same server keeps them; a redirect to another one doesn't send them.
- Your `Authorization` and `Cookie` header lines are likewise sent to the stream URL's origin only: not after a
  redirect to another server, and not to entries, HLS playlists, segments or keys a playlist names on another
  server. Every other line of yours goes to every request. If a CDN host behind your playlist needs a token, put it
  in the segment URLs (as signed URLs do) or in a header line of another name.
- Cookies a server sets go back to it on the stream's later requests ([cookies](#cookies)).

### Cookies

Each stream keeps the cookies its servers set (`Set-Cookie` on a playlist, a login redirect, a segment) and sends them
back on its later requests, as a browser would: to the host that set them (or, with `Domain=`, its subdomains), under
their `Path`, `Secure` ones over `https://` only, until they expire (`Max-Age`, `Expires`). That is what HLS services
that hand out a session cookie with the playlist and check it on every segment need. They are joined with your own
`Cookie` line, which goes first, into one `Cookie` header. The cookies live as long as the stream and are never
stored; streams don't share them. The system stacks' own cookie stores stay off, so this works the same on every
platform.

### Request headers

Every request sends the same headers on every platform and stack, in this order:

| Header | When |
|---|---|
| `User-Agent` | `fa_net_options.user_agent`, or `FatzAudio/<version>` |
| `Accept: */*` | always |
| `Accept-Encoding: identity` | always: the bytes as the server stores them, so byte ranges match |
| `Icy-MetaData: 1` | radio song titles, unless `FA_NET_NO_ICY` |
| `Range: bytes=N-` | seeks and resumed downloads; `bytes=N-M` for an HLS segment that is a byte range |
| `Authorization: Basic ...` | the URL has credentials (`https://user:password@host/...`); sent up front, without waiting for a challenge, and only to that URL's origin ([redirects](#redirects)) |
| your `headers` lines | as given, after the ones above |
| `Cookie` | the [cookies](#cookies) servers set, joined to your own `Cookie` line when you send one |

A line of yours replaces the header above with the same name (any case), so `"User-Agent: MyApp"` or
`"Accept: audio/*"` works. Spaces and tabs around the name and the value are trimmed. Lines are dropped when they
have no `:`, a name that isn't a valid header name, a value with anything but printable ASCII and tabs (encode
other text, as RFC 8187 or percent-encoding does), or more than 8192 bytes, and
so are `Host`, `Connection`, `Content-Length` and `Transfer-Encoding`, which the stack sets itself. A user agent
that breaks the same rules is not sent.

### Android

HTTPS runs on the Java VM every Android app already has, so `libfatzaudio.so` still needs nothing but libc. The VM is found like this:

- **Android 12 (API 31) and later:** by itself. Nothing to do.
- **.NET for Android, any version:** the binding hands .NET's VM to the library the first time it's used. Nothing to do. If your app hosts .NET some other way and `https://` fails with `ProviderMissing`, call `AudioEngine.SetAndroidJavaVM(vm)` once.
- **Java or Kotlin app on Android 7 to 11 (API 24 to 30):** call `fa_android_set_java_vm` once from your own JNI code, before the first `https://` URL. Until then, `https://` returns `FA_ERROR_PROVIDER_MISSING` and `http://` works.

```c
#include <jni.h>
#include <fatzaudio/fatzaudio.h>

/* In the app's own JNI library, loaded with System.loadLibrary. */
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    fa_android_set_java_vm(vm);
    return JNI_VERSION_1_6;
}
```

What else to know on Android:

- The certificates, user-added CAs and certificate pinning of your app's [network security config](https://developer.android.com/privacy-and-security/security-config) apply to `https://`, as they do for the app's own requests.
- The network security config's cleartext rule does not apply to `http://`: that goes through FatzAudio's own client. Use `https://` URLs if your app must not send cleartext.
- The proxy of the active network (Wi-Fi proxy settings, with its exclusion list) is used for `http://` and `https://` when `proxy` is `NULL`.
- Credentials in `proxy` (`"http://user:password@host:port"`) are sent for `http://` only, as on Windows: `https://` through a proxy that asks for a password fails with `FA_ERROR_NETWORK`.
- Android's HTTPS stack rejects the `ICY 200 OK` status line of old Shoutcast v1 servers, so such a server works over `http://` only. Icecast and Shoutcast v2 answer `HTTP/1.x` and work over both.
- Requests run on the stream's own download thread, never on the app's main thread, so `NetworkOnMainThreadException` can't happen.

## Not provided yet

- Opening asynchronously with a cancel call (`BASS_StreamCancel`).
