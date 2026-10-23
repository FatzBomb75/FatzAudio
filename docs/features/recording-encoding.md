# Recording and encoding

An encoder writes audio to a file, or to your own output (a network upload, a pipe, memory), in one of three formats. It runs on its own thread, so encoding never slows playback down.

| Format | `fa_encode_format` | .NET `EncodeFormat` | What you get |
|---|---|---|---|
| WAV | `FA_ENCODE_WAV` | `Wav` | 16 or 24-bit PCM, or 32-bit float. More than two channels use `WAVE_FORMAT_EXTENSIBLE` with the standard speaker layout; files past 4 GB become RF64 automatically |
| FLAC | `FA_ENCODE_FLAC` | `Flac` | Lossless, 16 or 24 bit, up to 8 channels, with the MD5 of the audio. Built into the library (no libFLAC) |
| MP3 | `FA_ENCODE_MP3` | `Mp3` | Through LAME, which your app ships and FatzAudio loads at runtime (see [MP3 and LAME](#mp3-and-lame)). VBR or CBR, with a LAME header for exact length and gapless playback |

Each encoder takes its audio from one source:

| Source | C | .NET | What is recorded |
|---|---|---|---|
| A device's output | `fa_encoder_attach_device` | `encoder.Record(output)` | Everything the device plays (its master mix after the limiter), in stereo at the device rate, including silence while nothing plays |
| A DJ mixer's master or vocals | `fa_encoder_attach_mixer` + `fa_mixer_start_recording` | `encoder.Record(mixer, tap)` + `mixer.StartRecording(...)` | Stems that start on one frame, the vocals lined up as the singer heard the music ([DJ mixer recording](dj-mixer.md#recording)) |
| One playing stream | `fa_encoder_attach_stream` | `encoder.Record(stream)` | The stream as heard: volume, pan, fades, crossfades, normalization and its bus applied. Only what it plays: nothing before it starts or after it ends, and nothing while it is paused or stalled |
| A stream that is not playing | `fa_encoder_encode_stream` | `encoder.Encode(stream)` | The stream from its position to its end, as fast as it decodes, at its own rate and channels (file conversion). Tempo, pitch and rate apply, as for `fa_stream_read` |
| Your own samples | `fa_encoder_write` | `encoder.Write(span, rate, channels)` | Interleaved float frames from any thread. It blocks while the queue is full, so nothing is lost |

Several encoders can share a source, for example an MP3 for the web and a WAV archive of the same mix.

## C

Record the output to MP3 while it plays:

```c
fa_encoder_options o;
fa_encoder_options_init(&o);
o.format = FA_ENCODE_MP3;          /* VBR quality 2 (~190 kbps) unless bitrate_kbps is set */
o.title = "Friday night";
o.artist = "DJ Fatz";

fa_encoder* rec;
fa_result r = fa_encoder_create_path("friday.mp3", &o, &rec);
if (r == FA_ERROR_PROVIDER_MISSING) { /* ship libmp3lame with the app */ }
fa_encoder_attach_device(rec, device);
/* ... */
r = fa_encoder_close(rec);         /* completes the file; FA_OK or the first error (disk full, ...) */
```

Convert a file to FLAC as fast as possible:

```c
fa_stream* s;
fa_open_options so;
fa_open_options_init(&so);
so.flags = FA_OPEN_DECODE_ONLY;
fa_stream_open_path(engine, "song.wav", &so, &s);

fa_encoder_options o;
fa_encoder_options_init(&o);
o.format = FA_ENCODE_FLAC;
o.bits_per_sample = 24;            /* 16 (default) or 24 */

fa_encoder* enc;
fa_encoder_create_path("song.flac", &o, &enc);
fa_encoder_encode_stream(enc, s);
fa_encoder_wait(enc, -1);          /* or poll fa_encoder_get_status for a progress bar */
fa_encoder_close(enc);
fa_stream_close(s);
```

Encode your own samples, at another rate and channel count than they come in:

```c
fa_encoder_options o;
fa_encoder_options_init(&o);
o.sample_rate = 44100;             /* input at 48 kHz is resampled */
o.channels = 1;                    /* and mixed to mono */
fa_encoder_create_path("voice.wav", &o, &enc);
fa_encoder_write(enc, samples, frames, 48000, 2);   /* interleaved float */
fa_encoder_close(enc);
```

Your own output: `write` takes the bytes, `seek` (optional) lets the encoder complete its header at the end, `close` (optional) is called once when the encoder closes.

```c
static fa_result FA_CALL upload_write(void* user, const void* data, size_t size) { /* send */ return FA_OK; }

fa_encoder_output out = { upload_write, NULL, NULL };   /* forward only */
fa_encoder_create_output(&out, my_connection, &o, &enc);
```

## C#

```csharp
// Record the output
using var rec = AudioEncoder.Create("friday.mp3", new EncoderOptions
{
    Format = EncodeFormat.Mp3,
    Title = "Friday night",
    Artist = "DJ Fatz",
});
rec.Record(output);
// ...
rec.Close();     // completes the file; throws FatzAudioException on errors (Dispose doesn't throw)

// Record one deck
using var deck = AudioEncoder.Create("deck-a.wav", new EncoderOptions { BitsPerSample = 24 });
deck.Record(trackA);       // trackA plays on a device

// Convert a file
AudioEncoder.ConvertFile(engine, "song.wav", "song.flac", new EncoderOptions { Format = EncodeFormat.Flac });

// Encode to any writable stream
using var ms = new MemoryStream();
using (var enc = AudioEncoder.Create(ms, new EncoderOptions { Format = EncodeFormat.Flac }, leaveOpen: true))
{
    enc.Write(samples, 48000, 2);
}

// Progress
var st = rec.Status;
Console.WriteLine($"{st.Duration} recorded, {st.Bytes / 1024} KB, {st.DroppedFrames} frames dropped");
```

## Options

| `fa_encoder_options` | .NET `EncoderOptions` | Default | Meaning |
|---|---|---|---|
| `format` | `Format` | WAV | The output format |
| `sample_rate` | `SampleRate` | 0 = the input's | Output rate; other rates are resampled. MP3 only has 8, 11.025, 12, 16, 22.05, 24, 32, 44.1 and 48 kHz: with 0 it keeps the input's rate when it is one of those, otherwise 44.1 kHz for multiples of 44.1/11.025 kHz and 48 kHz for the rest |
| `channels` | `Channels` | 0 = the input's | Output channels; any count is mixed to any other (miniaudio's standard speaker mixing). MP3 has at most 2, so surround input becomes stereo; FLAC at most 8 |
| `bits_per_sample` | `BitsPerSample` | 16 | WAV: 16, 24 or 32 (float). FLAC: 16 or 24. MP3 ignores it |
| `bitrate_kbps` | `BitrateKbps` | 0 = VBR | MP3: a constant bitrate, 8 to 320 kbps |
| `quality` | `Quality` | -1 = the format's default | MP3 VBR: 0 (best, ~245 kbps) to 9 (smallest), default 2 (~190 kbps). FLAC: 0 (fastest) to 8 (smallest), default 5 |
| `queue_ms` | `QueueLength` | 2000 ms | Input buffered ahead of the encoder thread |
| `title`, `artist`, `album`, `genre`, `date`, `comment`, `track_number` | `Title` … `TrackNumber` | none | Tags: ID3v2.3 (UTF-16) in MP3, Vorbis comments in FLAC, a LIST/INFO chunk in WAV |

Invalid values (FLAC at 32 bits, MP3 at 96 kHz, an unknown format) fail at creation with `FA_ERROR_INVALID_ARG`.

## Status, waiting and closing

`fa_encoder_get_status` (.NET `encoder.Status`) reports the state, the first error, how many frames and bytes were written, the frames dropped, and the input and output formats.

| State | Meaning |
|---|---|
| `FA_ENCODER_IDLE` | No source yet |
| `FA_ENCODER_RUNNING` | Encoding |
| `FA_ENCODER_FINISHED` | `fa_encoder_encode_stream` reached the end of the stream |
| `FA_ENCODER_FAILED` | Stopped by an error (`status.error`): the output failed (disk full, network gone) or the stream could not be decoded |

`fa_encoder_wait(enc, timeout)` (.NET `Wait`) returns when the encoder is finished or failed, or `FA_ERROR_BUSY` after the timeout. `fa_encoder_close` stops the source, encodes what is still queued, completes the header (WAV sizes, FLAC length, frame sizes and MD5, the MP3's LAME/Xing frame) and closes the file. It returns the first error the encoder hit; `FA_OK` means the file is complete. An encoder closed before it got any audio still leaves a valid, empty file.

## How it behaves

- **The audio thread never waits for the encoder.** Recording a device or a stream only copies the audio into the encoder's queue. If the encoder falls more than `queue_ms` behind real time (a network output that stalls), what does not fit is dropped and counted in `dropped_frames`; the rest of the file stays valid. `fa_encoder_write` waits instead, so nothing is lost on that path.
- **Pausing.** `fa_encoder_set_paused` (.NET `IsPaused`) skips the device's or stream's audio until resumed: the file just continues where it stopped, with no gap of silence.
- **When the source goes away.** Closing the device or stream being recorded (or encoded) ends the input; the encoder stays open and its file is completed when you close it. A recorded stream moved to a device of another sample rate is not recorded while it plays there (the input format is fixed when the source is attached).
- **Exact lengths.** Resampled output has exactly `input × out_rate / in_rate` frames (rounded), with the resampler's look-ahead flushed at the end. FLAC and WAV keep every frame; MP3 files carry LAME's delay and padding in the LAME header, so decoders that read it (FatzAudio, FFmpeg, most players) give back exactly the frames encoded.
- **Dither.** 16-bit output (WAV or FLAC) gets TPDF dither, except for samples that already are exact 16-bit values. So a 16-bit source played or converted at unity gain stays bit exact: WAV to FLAC is lossless, and so is FLAC to WAV. 24-bit output is rounded without dither.
- **Outputs that cannot seek.** When `fa_encoder_output.seek` is NULL (or the .NET stream can't seek), the header can't be completed at the end. WAV keeps "length unknown" sizes and FLAC an unknown length and no MD5 (both decode fine, including with FatzAudio); MP3 is written without the LAME header.
- **Threads.** One thread per encoder does the conversion, encoding and writing; your output callbacks run there. FLAC level 5 encodes 44.1 kHz stereo at about 90× real time on one core (level 8: about 20×), so recording costs almost nothing. The calls are thread-safe; `fa_encoder_write` calls must not overlap each other.

## FLAC compression

FatzAudio's FLAC encoder is written from the format specification and tries, for every block and channel, a constant, raw samples, the fixed predictors and LPC of every order up to the level's maximum, plus the four stereo decorrelations, and keeps whichever is smallest. Measured on 30 s of 16-bit stereo test music against the reference encoder (`flac` 1.4):

| Level | FatzAudio | flac | 
|---|---|---|
| 0 | 3.34 MB | 3.31 MB |
| 5 (default) | 2.17 MB | 2.19 MB |
| 8 | 2.11 MB | 2.11 MB |

Every file it writes passes `flac -t` (CRCs and MD5), which the tests check when `flac` is installed.

## MP3 and LAME

LAME is LGPL, so FatzAudio never links or bundles it (the same model as FFmpeg). Ship libmp3lame with your app and FatzAudio finds it:

1. one already loaded in the process, else
2. one the system loader finds: `libmp3lame.dll`, `libmp3lame-0.dll` or `lame_enc.dll` (Windows, for example next to the exe), `libmp3lame.0.dylib` (macOS, also in the Homebrew and MacPorts folders), `libmp3lame.so.0` (Linux, Android).

`fa_lame_load(directory)` (.NET `Lame.Load(dir)`) loads it from a folder of your choice; call it before the first MP3 encoder. `fa_lame_get_info` (.NET `Lame.Info`) says which version is in use and where it came from. Without LAME, creating an MP3 encoder returns `FA_ERROR_PROVIDER_MISSING`. LAME 3.98 and later work; 3.100 is the current release.

## Not yet

- Opus and AAC encoding (planned in the design: Opus with the built-in Opus codec, AAC through the operating system's encoders), the Windows Media Foundation MP3 encoder as a fallback when LAME is missing, and broadcasting to Icecast/Shoutcast servers. See [engineering notes](../engineering-notes.md).
- Recording from input devices (microphones, line in) comes with capture devices; until then, feed captured samples to `fa_encoder_write`.
