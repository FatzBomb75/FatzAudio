# Opening and decoding streams

A stream is anything that produces audio. You open it from a file, a memory block, your own read callbacks, or a URL ([web streams](web-streams.md)), and FatzAudio works out the format from the data itself. There's one open call per kind of source and none per format: no plugins to load, no `BASS_FLAC_StreamCreateFile`.

## Supported formats

| Format | Containers / variants | Codec name in `fa_stream_info` |
|---|---|---|
| WAV | RIFF, RIFX, RF64, Wave64, AIFF, AIFF-C (PCM, float, ADPCM, A-law, µ-law) | `pcm` (container `wav` or `aiff`) |
| FLAC | Native FLAC, with or without a leading ID3v2 tag | `flac` |
| MP3 | MPEG-1, 2 and 2.5 Layer III, with or without ID3v2 tags | `mp3` |
| AAC (ADTS) | `.aac` files, AAC internet radio and AAC in HLS segments: AAC-LC, HE-AAC and HE-AAC v2 in ADTS frames, with or without ID3v2 tags | `aac` (container `adts`) |
| MP4, M4A, MOV | ISO MP4 and QuickTime, including fragmented MP4. Audio tracks only: video is never read | the track's codec: `aac`, `mp3`, `ac3`, `eac3`, `opus`, `flac`, `alac`, `pcm_*`, ... (container `mp4` or `mov`) |
| Matroska, WebM | MKV, MKA, WebM, including live layouts (unknown sizes, no Cues). Audio tracks only | `aac`, `mp3`, `ac3`, `eac3`, `opus`, `vorbis`, `flac`, `pcm_*`, ... (container `matroska` or `webm`) |

Inside MP4 and Matroska, PCM and MP3 tracks decode built in; every other codec (AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC, ALAC, DTS, TrueHD) uses the FFmpeg your app already has, loaded at runtime ([loading FFmpeg](ffmpeg.md#loading-ffmpeg)). Without it those tracks return `FA_ERROR_PROVIDER_MISSING`. The same goes for `.aac` files and AAC streams.

An `.aac` file on disk or in memory is indexed when it opens (a quick pass over the frame headers, without decoding), so its length and seeks are exact. Over the network, and from your own read callbacks when they can seek, the length and seek positions are estimated from the frame sizes at the start, as for MP3 without a seek table. Junk between frames, such as a radio stream joined in the middle of a frame or ID3 tags between HLS segments, is skipped.

More formats are added to the same registry (Ogg Vorbis and Opus, raw AC-3 files, and built-in or OS decoders for the codecs above follow the build order in the design doc). Ask the library at runtime with `fa_format_count` / `fa_format_get_info`, or `AudioEngine.SupportedFormats` in .NET.

Apps that demux files themselves (for example with FFmpeg, for the video) can push compressed packets or decoded frames instead of opening a file: see [FFmpeg hand-off and A/V sync](ffmpeg.md).

### How detection works

1. The first 8 KB of the source are read once and kept, so detection works even on sources that can't seek.
2. A leading ID3v2 tag is skipped. If the tag is bigger than the 8 KB window (cover art often is), detection reads the 8 KB right after it instead. A forward-only source can't do that, so there the file name or `mime_hint` picks the decoder.
3. Every decoder in the registry scores the data from 0 to 100: magic bytes for WAV/AIFF/FLAC, two chained valid frame headers for MP3.
4. The best-scoring decoder opens the stream. If it rejects the data, the next candidate gets a turn.
5. The file extension and `mime_hint` only break ties. A FLAC file named `song.mp3` still opens as FLAC.

If nothing matches, the open returns `FA_ERROR_UNSUPPORTED_FORMAT`.

## C

```c
#include <fatzaudio/fatzaudio.h>

fa_engine* engine;
fa_engine_create(NULL, &engine);

/* From a path (UTF-8 on every platform, including Windows). */
fa_stream* song;
fa_result r = fa_stream_open_path(engine, "C:/Music/track.flac", NULL, &song);
if (r != FA_OK) printf("can't open: %s\n", fa_result_string(r));

fa_stream_info info;
fa_stream_get_info(song, &info);
printf("%s in %s, %u Hz, %u ch, %.3f s\n", info.codec, info.container,
       info.format.sample_rate, info.format.channels, info.duration / 1e9);

/* What can this build open? */
for (uint32_t i = 0; i < fa_format_count(); i++) {
    fa_format_info f;
    fa_format_get_info(i, &f);
    printf("%s: %s (%s)\n", f.name, f.description, f.extensions);
}
```

### Decoding without playback

Open with `FA_OPEN_DECODE_ONLY`, then pull interleaved 32-bit float frames:

```c
fa_open_options opts;
fa_open_options_init(&opts);
opts.flags = FA_OPEN_DECODE_ONLY;

fa_stream* s;
fa_stream_open_path(engine, "track.mp3", &opts, &s);
fa_stream_get_info(s, &info);

float* buf = malloc(4096 * info.format.channels * sizeof(float));
fa_frames got;
for (;;) {
    r = fa_stream_read(s, buf, 4096, &got);
    if (r == FA_ERROR_END_OF_STREAM) break;  /* got == 0 here */
    if (r != FA_OK) { /* I/O or decode error */ break; }
    /* use `got` frames (can be fewer than 4096) */
}
fa_stream_close(s);
```

`fa_stream_read` rules:

- `FA_OK` with 1 or more frames. A short read doesn't mean the end; keep calling.
- `FA_ERROR_END_OF_STREAM` with 0 frames once everything has been read. Further calls return the same until you seek.
- `FA_ERROR_IO` (or whatever error your `read` callback returned) if reading the source failed. The frames decoded before the failure are returned first, so the error arrives with 0 frames, never instead of real audio.
- `FA_ERROR_BUSY` if the stream is attached to a device, because the device pulls it then.

### Custom I/O (your own reader)

```c
typedef struct { FILE* f; } my_reader;

static int64_t FA_CALL my_read(void* user, void* buf, size_t bytes)
{ return (int64_t)fread(buf, 1, bytes, ((my_reader*)user)->f); }

static fa_result FA_CALL my_seek(void* user, int64_t offset)
{ return fseek(((my_reader*)user)->f, (long)offset, SEEK_SET) == 0 ? FA_OK : FA_ERROR_IO; }

static void FA_CALL my_close(void* user)
{ fclose(((my_reader*)user)->f); free(user); }

my_reader* rd = malloc(sizeof *rd);
rd->f = fopen("track.flac", "rb");
fa_io_callbacks io = { my_read, my_seek, NULL /* length unknown */, my_close };
if (fa_stream_open_io(engine, &io, rd, NULL, &s) != FA_OK) {
    fclose(rd->f); free(rd);   /* on failure you still own `rd` */
}
```

Ownership rules for `user`:

- The stream owns `user` **only if `fa_stream_open_io` returns `FA_OK`**. On failure `close` is not called, so clean up yourself.
- After a successful open, `close` is called **exactly once**, from `fa_stream_close`, after the last `read`/`seek`/`length` call.
- The callbacks are never called at the same time for one stream, but they can be called from engine threads.
- `seek` may be `NULL` for forward-only sources such as pipes or sockets. The stream still opens and plays (a `length` callback is fine too), but `fa_stream_seek` returns `FA_ERROR_NOT_SEEKABLE` and `fa_stream_info.seekable` is 0.
- A forward-only stream that is detached while playing keeps the audio it had decoded ahead and plays it first when you attach it again, so nothing is skipped, even on a device with a different sample rate. Its position is where you stopped hearing it, as with any other stream.
- `seek` is called by `fa_stream_seek` itself, so a seek your callback refuses fails right there.

## C#

```csharp
using FatzAudio;

using var engine = AudioEngine.Create();

using var song = engine.OpenFile(@"C:\Music\track.flac");
var info = song.Info;
Console.WriteLine($"{info.Codec} in {info.Container}, {info.Format.SampleRate} Hz, {info.Duration}");

foreach (var f in AudioEngine.SupportedFormats)
    Console.WriteLine($"{f.Name}: {f.Description} ({string.Join(", ", f.Extensions)})");

// Decode to PCM without playing
using var pcm = engine.OpenFile("track.mp3", new StreamOptions { DecodeOnly = true });
var buffer = new float[4096 * pcm.Info.Format.Channels];
int frames;
while ((frames = pcm.Read(buffer)) > 0)
{
    // buffer holds `frames` interleaved frames
}

// Any System.IO.Stream (disposed with the audio stream unless leaveOpen: true)
using var fromStream = engine.OpenStream(File.OpenRead("track.wav"));
```

`Read` returns 0 at the end of the stream. If `OpenStream` throws, the `System.IO.Stream` you passed in is left open.

## Stream info

| Field | Meaning |
|---|---|
| `format` | The source's own format: always 32-bit float interleaved, at the file's rate and channel count. It doesn't change when the stream is attached to a device. |
| `codec`, `container` | For example `flac`/`flac`, `pcm`/`aiff`, `mp3`/`mp3`, `aac`/`mp4`, `opus`/`webm` |
| `audio_track_count` | Audio tracks in the file (1 for audio-only formats) |
| `length_frames` | Length in frames, -1 if unknown (live streams) |
| `duration` | Length in nanoseconds, -1 if unknown. In .NET, `Info.Duration` (null if unknown) |
| `seekable` | 0 for forward-only sources |

## Attaching to a device

`fa_stream_attach(stream, device, NULL)` routes the stream to a device. The decoder isn't reopened: position, format and parameters stay as they are. Attaching to the device the stream is already on does nothing. Attaching to a different device moves the stream there and it continues from the same position. `fa_stream_detach` takes it off the device and keeps the position. Decode-only streams can't be attached.

In .NET, `stream.PlayOn(device)` attaches (or moves) and plays, and `stream.Detach()` detaches.

## Pitfalls

- `fa_stream_open_memory` copies the data, so you can free your buffer right after the call.
- Paths are UTF-8. On Windows FatzAudio converts them to UTF-16 internally, so non-ASCII file names work.
- `audio_track` greater than 0 returns `FA_ERROR_NOT_FOUND` for single-track formats.

## Video files and audio tracks

Open a video file like any other: FatzAudio reads only its audio. `fa_open_options.audio_track` (.NET `StreamOptions.AudioTrack`) picks the track, counting audio tracks only from 0; -1 is the first. `fa_stream_info.audio_track_count` (.NET `Info.AudioTrackCount`) says how many there are, and a track that doesn't exist returns `FA_ERROR_NOT_FOUND`.

```c
fa_open_options o;
fa_open_options_init(&o);
o.audio_track = 1;                      /* the second language */
fa_stream_open_path(engine, "movie.mkv", &o, &s);
```

```csharp
using var s = engine.OpenFile("movie.mp4", new StreamOptions { AudioTrack = 1 });
Console.WriteLine(s.Info.AudioTrackCount);
```

- Seeking is sample exact: the codec is restarted a little earlier (pre-roll) and the extra audio is dropped. Encoder delay, edit lists, Matroska CodecDelay and DiscardPadding are applied, so an AAC or Opus track starts on its first real sample and has its exact length.
- Only the audio's own bytes are read, so reading a track of a large video is fast: about 1000x real time for a 720p file, reading 3 to 7 percent of it. Normalization scans use the same path ([normalization](normalization.md)).
- Forward-only sources work when the MP4 has its index in front (faststart or fragmented); otherwise `FA_ERROR_NOT_SEEKABLE`. Matroska always works.
- Not supported: encrypted tracks (CENC), compressed Matroska tracks (zlib, bzip2, LZO), chained Matroska segments, and MP4 edit lists beyond a leading gap and one edit.
- When your app demuxes the video itself with FFmpeg, you can still open the file a second time for the audio, or hand FatzAudio the packets you already read ([FFmpeg hand-off](ffmpeg.md)).
