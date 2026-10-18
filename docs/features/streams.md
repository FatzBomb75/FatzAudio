# Opening and decoding streams

A stream is anything that produces audio. You open it from a file, a memory block, your own read callbacks, or a URL ([web streams](web-streams.md)), and FatzAudio works out the format from the data itself. There's one open call per kind of source and none per format: no plugins to load, no `BASS_FLAC_StreamCreateFile`.

## Supported formats

| Format | Containers / variants | Codec name in `fa_stream_info` |
|---|---|---|
| WAV | RIFF, RIFX, RF64, Wave64, AIFF, AIFF-C (PCM, float, ADPCM, A-law, µ-law) | `pcm` (container `wav` or `aiff`) |
| FLAC | Native FLAC, with or without a leading ID3v2 tag. Ogg FLAC and FLAC tracks in MP4 and Matroska decode with the same built-in decoder | `flac` |
| MP3 | MPEG-1, 2 and 2.5 Layer III, with or without ID3v2 tags | `mp3` |
| AAC (ADTS) | `.aac` files, AAC internet radio and AAC in HLS segments: AAC-LC, HE-AAC, HE-AAC v2, AAC Main and AAC-LTP in ADTS frames, with or without ID3v2 tags ([AAC](#aac)) | `aac` (container `adts`) |
| Ogg Opus, Ogg Vorbis, Ogg FLAC | `.opus`, `.ogg`, `.oga`, including chained files (one song after another) and Ogg files with several streams ([Ogg](#ogg-opus-and-ogg-vorbis)) | `opus`, `vorbis` or `flac` (container `ogg`) |
| MP4, M4A, MOV | ISO MP4 and QuickTime, including fragmented MP4. Audio tracks only: video is never read | the track's codec: `aac`, `mp3`, `ac3`, `eac3`, `opus`, `flac`, `alac`, `pcm_*`, ... (container `mp4` or `mov`) |
| Matroska, WebM | MKV, MKA, WebM, including live layouts (unknown sizes, no Cues). Audio tracks only | `aac`, `mp3`, `ac3`, `eac3`, `opus`, `vorbis`, `flac`, `pcm_*`, ... (container `matroska` or `webm`) |
| Dolby Digital, Dolby Digital Plus | Raw `.ac3` and `.ec3` streams, up to 7.1 ([Dolby Digital](dolby.md)) | `ac3` or `eac3` (container `ac3` or `ec3`) |
| WMA, ASF | `.wma`, `.asf` and `.wmv` files (Advanced Systems Format), local or over HTTP: Windows Media Audio 1, 2, 9 Professional, 9 Lossless and Voice, plus PCM, MP3 and AC-3 in ASF. Audio streams only ([WMA](#wma)) | `wmav1`, `wmav2`, `wmapro`, `wmalossless`, `wmavoice`, `pcm_*`, `mp3`, ... (container `asf`) |
| MIDI | Standard MIDI Files (format 0, 1, 2), karaoke `.kar`, RIFF `.rmi`; played with a SoundFont 2 you load ([MIDI](midi.md)) | `midi` (container `smf` or `rmid`) |

Inside MP4, Matroska and ASF, PCM, MP3, AC-3, E-AC-3, Opus, Vorbis, FLAC and ALAC tracks decode built in (ALAC to the bit, all channel layouts up to 7.1 and every bit depth). AAC (AAC-LC, HE-AAC, HE-AAC v2, Main and LTP, up to 7.1) decodes built in as well, on every platform ([AAC](#aac)). Every other codec (DTS, TrueHD, WMA outside Windows) uses the FFmpeg your app already has, loaded at runtime ([loading FFmpeg](ffmpeg.md#loading-ffmpeg)). Without it those tracks return `FA_ERROR_PROVIDER_MISSING`.

More formats are added to the same registry (built-in or OS decoders for the codecs above follow the build order in the design doc). Ask the library at runtime with `fa_format_count` / `fa_format_get_info`, or `AudioEngine.SupportedFormats` in .NET.

Apps that demux files themselves (for example with FFmpeg, for the video) can push compressed packets or decoded frames instead of opening a file: see [FFmpeg hand-off and A/V sync](ffmpeg.md).

### How detection works

1. The first 8 KB of the source are read once and kept, so detection works even on sources that can't seek.
2. A leading ID3v2 tag is skipped. If the tag is bigger than the 8 KB window (cover art often is), detection reads the 8 KB right after it instead. A forward-only source can't do that, so there the file name or `mime_hint` picks the decoder.
3. Every decoder in the registry scores the data from 0 to 100: magic bytes for WAV/AIFF/FLAC/Ogg/ASF, two chained valid frame headers for MP3.
4. The best-scoring decoder opens the stream. If it rejects the data, the next candidate gets a turn.
5. The file extension and `mime_hint` only break ties. A FLAC file named `song.mp3` still opens as FLAC.

If nothing matches, the open returns `FA_ERROR_UNSUPPORTED_FORMAT`.

Protected content returns `FA_ERROR_PROTECTED_CONTENT` instead, so the app can tell the user the file is copy-protected rather than broken. That covers DRM-protected WMA/ASF, MP4/M4A with Common Encryption (`enca`) or iTunes FairPlay (`drms`), Matroska/WebM tracks with ContentEncryption, and HLS with `SAMPLE-AES` or another DRM key method. FatzAudio never decrypts these: the licences live with the platform's DRM components.

Karaoke files: a ZIP archive opened with `fa_stream_open_path` or `fa_stream_open_memory` plays the audio file inside it, and `fa_stream_open_path` on an audio file also loads a `.cdg` of the same name next to it (turn that off with `FA_OPEN_NO_CDG`). See [CD+G](cdg.md).

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

## WAV and AIFF

- 16, 24 and 32-bit integer and 32-bit float PCM in RIFF, RF64 and Wave64 is read straight into your buffer in one call and widened to float in place: about as fast as BASS (over 10000× real time for 16-bit stereo), with the same values (x / 2^(bits-1)).
- Other variants (AIFF, RIFX, 8-bit, ADPCM, A-law, µ-law, 64-bit float) go through dr_wav's own conversion, a little slower.
- A forward-only source (or a web stream) is read in order, so trailing chunks after the audio are never sought to.

## FLAC

- Seeks are sample exact, with or without a SEEKTABLE. A file without one (FatzAudio's own FLAC encoder writes none, nor does FFmpeg's) is searched: each probe decodes one frame near where the target should be and narrows the range, and two to four probes usually find it, about a millisecond per seek. A SEEKTABLE narrows the search to the stretch between two of its points.
- Audio whose compression changes sharply, such as long digital silence followed by music, can take more probes (about a dozen) but still lands exactly.

## MP3

MP3 files decode built in (minimp3, inside miniaudio), in 32-bit float: nothing is rounded to 16 bits, so the output matches a reference float decoder to about 124 dB.

- **Start and length:** a LAME/Xing header's encoder delay and padding are removed, so the audio starts on the first sample the encoder was given and the length is exact. Without that header the decoder's own 529-frame delay is still removed (the encoder's delay is not recorded anywhere, so a LAME-encoded file then starts 576 frames late, as in BASS).
- **Seeking is exact and fast**, on files and memory: a seek lands on the frame asked for, with exactly the samples a straight decode gives there. The decoder finds frames by hopping from header to header (no decoding) and keeps the position of every 16th; after a seek it decodes only the few frames before the target that the bit reservoir and the filter bank need, so a seek takes about 0.3 ms. A file with a Xing/LAME header is hopped lazily, up to where you seek, so the first seek far into a long file reads the headers on the way (about 6 ms for 10 minutes; later seeks don't); a file without one is hopped once at open to learn its length.
- **`FA_OPEN_PRESCAN`** hops the whole file at open: the length then comes from the frames themselves rather than the header's frame count, and no later seek has to read ahead. It is never needed for exact seeks.
- **Damaged files:** junk between frames is skipped, the same way when playing and when seeking; a frame that can't be decoded plays as silence so the timeline never shifts. A file cut off mid-frame plays up to its last whole frame.
- **Over the network** the length comes from the Xing header (or the bitrate) and seeks jump to the estimated byte, as nothing past the playing position has been downloaded. Free-format files (no bitrate in the header) also use that decoder.
- **Loudness scans** cut MP3 files into chunks on all cores like other formats ([normalization](normalization.md#why-the-scan-is-fast-even-on-large-video-files)).

## AAC

AAC decodes with FatzAudio's own decoder, the same on every platform, with no FFmpeg and no system codec. It plays `.aac` (ADTS) files, AAC radio and HLS, and AAC tracks in MP4/M4A/MOV, Matroska and video files, plus AAC packets you push ([FFmpeg hand-off](ffmpeg.md)).

| Profile | Supported |
|---|---|
| AAC-LC | ✅ mono to 7.1, every sampling rate from 8 to 96 kHz, channel layouts from a program config element (PCE) |
| HE-AAC (SBR) | ✅ mono to 7.1, signalled in the configuration or found in the stream (implicit) |
| HE-AAC v2 (parametric stereo) | ✅ every PS mode: 10, 20 and 34 bands, fine and coarse IID, both mixing procedures, IPD/OPD phase |
| AAC Main | ✅ with backward-adaptive prediction |
| AAC-LTP | ✅ with long-term prediction |
| xHE-AAC (USAC) | Through the system's decoder where there is one (Media Foundation, AudioToolbox, MediaCodec). Elsewhere it returns `FA_ERROR_UNSUPPORTED_FORMAT`; an app that must play it can decode it with its own FFmpeg and hand FatzAudio the frames ([FFmpeg hand-off](ffmpeg.md)) |
| AAC SSR, low delay and the other error-resilient (ER) profiles, 960-sample frames | Not supported. MP4/MKV tracks with such a configuration return `FA_ERROR_UNSUPPORTED_FORMAT`; an `.aac` (ADTS) file of AAC SSR, the only one of these that ADTS can carry, returns `FA_ERROR_UNSUPPORTED_FORMAT` |

The output matches FFmpeg's AAC decoder to float precision: about 136 dB SNR for AAC-LC, AAC Main and AAC-LTP and 90 to 130 dB for HE-AAC and HE-AAC v2 (measured on streams from Fraunhofer's encoder and FFmpeg's; the tests check every profile and PS mode). For PS phase data (IPD/OPD) the lowest QMF band differs slightly from FFmpeg, which conjugates the phase of its negative-frequency part only at the start of each interpolation; FatzAudio follows the standard there, as FAAD2 does.

```c
fa_stream* s;
fa_stream_open_path(engine, "song.aac", NULL, &s);   /* or .m4a, .mp4, .mkv, an AAC radio URL */
fa_stream_info info;
fa_stream_get_info(s, &info);   /* info.codec "aac"; info.format.sample_rate is the rate after SBR */
```

```csharp
using var s = engine.OpenFile("song.aac");
Console.WriteLine($"{s.Info.Format.SampleRate} Hz, {s.Info.Format.Channels} channels");
```

- **Rate:** HE-AAC plays at its full rate after SBR, so `format.sample_rate` can be twice what the container or ADTS header states. A file that only signals HE-AAC inside its audio (implicit signalling, common in ADTS and in many MP4 files) is found from its first frame.
- **Channels** come out in the standard speaker order ([channel order](dolby.md#channels)). Mono HE-AAC plays as stereo from the start unless its configuration rules parametric stereo out, because PS can switch on in any frame (the two channels are identical until it does). Quadraphonic sound plays as 5 channels with a silent centre.
- **ADTS details:** frames with several raw data blocks, CRC-protected frames and layouts in a program config element all decode. When the headers change mid-stream (an HLS variant switch), the decoder follows them and the stream keeps its first format, converting the rest. Damaged frames play as silence of their length, so the timeline never shifts, and the stream carries on with the next good frame.
- **Start and length:** ADTS has no field for the encoder delay, so the first 1024 samples (one frame: the encoder's priming in a file, half a frame's overlap when joining a radio stream) are dropped, as bass_aac does; the audio then starts on the first real sample of files from the usual encoders (FFmpeg, Apple, Fraunhofer), and the length is one frame shorter than the frames hold. MP4 files use their edit list, so they start on the first sample exactly.
- **Indexing and seeking:** an `.aac` file on disk or in memory is indexed when it opens (a quick pass over the frame headers, without decoding), so its length and seeks are exact. Over the network, and from your own read callbacks when they can seek, the length and seek positions are estimated from the frame sizes at the start, as for MP3 without a seek table. Junk between frames, such as a radio stream joined in the middle of a frame or ID3 tags between HLS segments, is skipped.
- **Pushed packets:** give AAC its AudioSpecificConfig as `extradata` (from MP4 or Matroska), or push whole ADTS frames (from MPEG-TS) with no `extradata` ([FFmpeg hand-off](ffmpeg.md)).
- Not used: dynamic range control data in the stream (AAC's DRC, unlike Dolby's, is rarely sent), and coupling channel elements (a tool no current encoder writes).

## Ogg Opus and Ogg Vorbis

Opus and Vorbis decode with libopus and libvorbis, compiled into the library: no FFmpeg, no extra DLL. Ogg FLAC (`.oga`) uses the same FLAC decoder as `.flac` files and gives exactly the same samples. The same decoders play Opus and Vorbis tracks in WebM, Matroska and MP4, and raw Opus packets pushed by your app ([FFmpeg hand-off](ffmpeg.md)), which replaces bassopus's `BASS_OPUS_StreamCreate` + `BASS_StreamPutData`.

```c
fa_stream* s;
fa_stream_open_path(engine, "song.opus", NULL, &s);     /* or .ogg, .oga */
```

```csharp
using var s = engine.OpenFile("song.opus");
```

- **Length and seeking** are sample exact. The length comes from the last page's granule position, Opus pre-skip is dropped, and the end is trimmed where the encoder padded it. Opus restarts 300 ms before a seek target (the format asks for at least 80 ms, which can leave the first audio after a seek only 30 dB from a straight decode; 300 ms brings it within about 80 dB). A seek decodes only the packets it needs: the rest of the page it lands on is skipped, so a seek takes about a millisecond.
- **Output gain** in the Opus header is applied, and ReplayGain / R128 tags in the comment header (`R128_TRACK_GAIN` in OpusTags, `REPLAYGAIN_*` in Vorbis comments) feed normalization like any other tags ([normalization](normalization.md)).
- **Channels:** up to 8 channels come out in the standard speaker order (see [Dolby Digital](dolby.md#channels)), not Ogg's own order. Quadraphonic sound plays as 5 channels with a silent centre. Opus ambisonics (mapping family 2) plays its channels as they are; family 3 needs FFmpeg.
- **Chained files** (songs joined one after another, each with its own headers) play straight through. The first song sets the stream's rate and channels; later songs with a different format are converted. On a seekable local file the length and seeking cover every song (the file's page headers are read once at open). A chained file over HTTP plays through but can't seek.
- **Several streams in one file** (for example Ogg with video, or two audio languages): `audio_track` picks among the audio streams that start the file, as for MP4 and Matroska.
- **Web radio:** Ogg Opus and Ogg Vorbis stations work, and each song's title comes from its comment header ([web streams](web-streams.md#song-titles-icy-metadata)).
- **Ogg FLAC** gets everything above: exact length and seeking, chained files, `audio_track`, loudness tags and radio titles from its comment block.
- Not supported: Ogg Speex, and Ogg streams that start without their headers.

## WMA

`.wma` files (and `.asf`/`.wmv`, audio only) open with the ordinary calls; there is no plugin to load. FatzAudio reads the ASF container itself and hands each audio frame to a decoder:

- **Windows**: the system's own Windows Media Audio decoders, through Media Foundation. Nothing to install or ship. On Windows N and KN editions without the Media Feature Pack (and on Server Core) Media Foundation is missing; then the app's FFmpeg is used, as elsewhere.
- **macOS, Linux, Android, iOS**: the FFmpeg your app already has, loaded at runtime ([loading FFmpeg](ffmpeg.md#loading-ffmpeg)). Without it a WMA track returns `FA_ERROR_PROVIDER_MISSING`. PCM, MP3 and AC-3 inside ASF decode built in everywhere.

Details:

- **Length** comes from the file's play duration less its preroll. In files with several streams (video, other languages) the length is taken from the chosen track's own last frame instead.
- **Seeking** needs a seekable source. A file's Simple Index (written for video) narrows the search when there is one; otherwise the packets' send times are searched. Seeks are sample exact for WMA 1 and 2 and for PCM (tested); other codecs restart a packet or two earlier the same way, but have no test files yet.
- **Several audio streams** (several languages, or multiple-bitrate files): `audio_track` picks one, counting audio streams in the order of the header; -1 is the first. Video streams are skipped without being read.
- **Web streams**: an ASF file on a web server plays with `fa_stream_open_url`, seekable when the server takes byte ranges, straight through when it doesn't. MMS (`mms://`) and Windows Media Server's HTTP streaming protocol are not supported: the servers that spoke them are gone.
- **ReplayGain**: `REPLAYGAIN_TRACK_GAIN`, `REPLAYGAIN_ALBUM_GAIN` and the peaks (and `R128_*`) in a WMA file's attributes are used for [normalization](normalization.md), like ID3v2 and FLAC tags.
- **DRM**: protected files (a Content Encryption object in the header, or an encrypted stream, as with old store purchases and subscription downloads) return `FA_ERROR_PROTECTED_CONTENT`. Only Windows' own DRM components hold the licences to decrypt them.
- **Not supported**: WMA encoding, and scrambled audio ("audio spread" with a span above 1) has not been tested against real files.

```c
fa_stream* s;
fa_stream_open_path(engine, "song.wma", NULL, &s);
fa_stream_info info;
fa_stream_get_info(s, &info);          /* info.codec "wmav2", info.container "asf" */
```

```csharp
using var s = engine.OpenFile("song.wma");
Console.WriteLine($"{s.Info.Codec} in {s.Info.Container}");
```

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
