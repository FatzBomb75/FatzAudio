# Callback streams and capture streams

Two stream sources that are not files: your own function that makes audio on demand, and an input device (a microphone, a line input). Both are ordinary streams: they play on an output, decode with `fa_stream_read`, record with an encoder, and take tempo, effects, levels and loudness meters like any file.

| Source | C | .NET | BASS |
|---|---|---|---|
| Your function, asked for audio when the stream wants more | `fa_stream_open_callback` | `engine.OpenCallback(rate, channels, fill)` | `BASS_StreamCreate` with a `STREAMPROC` |
| An input device | `fa_stream_open_capture` | `engine.OpenCapture(capture)` | `BASS_RecordInit` + `BASS_RecordStart` |

Audio you already have in hand (decoded frames, FFmpeg packets) goes to a push stream instead ([FFmpeg hand-off](ffmpeg.md)). A karaoke or DJ mic you sing or talk through belongs on the [DJ mixer](dj-mixer.md#microphones), which corrects the drift between input and output and keeps the latency low.

## Callback streams

The stream calls your function on a decode worker whenever its buffer wants topping up. The function writes up to `frames` interleaved frames in the stream's format and returns how many it wrote:

| Return | Meaning |
|---|---|
| `1..frames` | That many frames. Fewer than asked is fine |
| `0` | Nothing right now: it is asked again about 5 ms later. A playing stream that runs dry meanwhile goes `FA_STATE_BUFFERING` (`FA_EVENT_STALLED`), and plays on once audio comes |
| `FA_STREAM_PROC_END` (any negative value) | No more audio: the stream plays what it has, then ends (`FA_EVENT_ENDED`). Return the last frames first |

### C

```c
#include <fatzaudio/fatzaudio.h>
#include <math.h>

typedef struct { double phase; } tone_gen;

/* A 440 Hz tone, stereo float. Runs on a decode worker. */
static int64_t FA_CALL make_tone(void* buffer, fa_frames frames, void* user)
{
    tone_gen* t = (tone_gen*)user;
    float* out = (float*)buffer;
    for (fa_frames i = 0; i < frames; i++) {
        out[i * 2] = out[i * 2 + 1] = (float)(0.2 * sin(t->phase));
        t->phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
    }
    return frames;
}

tone_gen gen = { 0 };
fa_audio_format format = { FA_FORMAT_F32, 48000, 2, 0 };
fa_stream* tone;
fa_stream_open_callback(engine, &format, make_tone, &gen, NULL, &tone);
fa_stream_attach(tone, output, NULL);
fa_stream_play(tone);
/* ... */
fa_stream_close(tone);   /* make_tone is never called again: gen may go now */
```

Any interleaved sample format works (`FA_FORMAT_S16`, `S24`, `S32`, `U8`, `F32`, `F64`); planar formats are refused with `FA_ERROR_INVALID_ARG`. Float samples that are not finite (NaN, infinity) become 0, as in files.

### C#

```csharp
using FatzAudio;

double phase = 0;
using var tone = engine.OpenCallback(48000, 2, buffer =>
{
    for (int i = 0; i < buffer.Length; i += 2, phase += 2 * Math.PI * 440 / 48000)
        buffer[i] = buffer[i + 1] = (float)(0.2 * Math.Sin(phase));
    return buffer.Length / 2;          // frames written; 0 = nothing now; AudioEngine.EndOfStream = the end
});
tone.PlayOn(output);
```

The .NET callback always gets float samples (`buffer.Length` is frames times channels). An exception thrown in it ends the stream.

### Threads and limits

- The function runs on a decode worker, ahead of what is heard by the stream's buffer (500 ms by default, `fa_open_options.buffer_ms`). For `FA_OPEN_DECODE_ONLY` streams it runs on your thread, inside `fa_stream_read`.
- It is never called on the audio thread, never twice at once, and never after `fa_stream_close` returns. It may take locks and do real work, but should not block: the stream runs dry while it waits.
- Something you change for the audio (a synth's note) is heard a buffer later. Use a short `buffer_ms` (50 to 100 ms) for an instrument that should answer quickly.
- The stream cannot seek (`FA_ERROR_NOT_SEEKABLE`) or loop (`FA_OPEN_LOOP` is `FA_ERROR_INVALID_ARG`), and its length is unknown (`length_frames` -1).

## Capture streams

`fa_stream_open_capture` opens an input as a stream in its own format (or the rate and channel count `fa_capture_options` asks for: the system converts). The input runs from the open, and the stream begins with the audio of its first read: the first `fa_stream_read`, an encoder's first read, or `fa_stream_attach` (an attached stream reads ahead at once, so attach it just before `fa_stream_play` to start with live input). What came before is dropped, and so is input nobody reads for about a second (a paused stream): reading then goes on with live input, not a backlog.

| Use | How |
|---|---|
| Record to a file | Open with `FA_OPEN_DECODE_ONLY`, then `fa_encoder_encode_stream`. `fa_encoder_close` ends the recording; close the stream after |
| Read the samples (a tuner, a level check, your own processing) | `FA_OPEN_DECODE_ONLY` + `fa_stream_read`, from a timer or a loop. It returns `FA_OK` with 0 frames while nothing new has arrived |
| Hear it | `fa_stream_attach` + `fa_stream_play`, to check an input. The input and the output run on two clocks and nothing corrects the drift, so the delay wanders and the stream rebuffers now and then. Use a [mixer mic](dj-mixer.md#microphones) to sing through |
| Meter it | `fa_stream_get_levels`, `fa_stream_get_loudness_meter` on a playing capture stream ([levels and loudness](levels-spectrum.md)) |

### C

```c
/* Record the default input to a FLAC file. */
fa_open_options o;
fa_open_options_init(&o);
o.flags = FA_OPEN_DECODE_ONLY;
fa_stream* mic;
fa_result r = fa_stream_open_capture(engine, NULL, &o, &mic);
if (r == FA_ERROR_PERMISSION_DENIED) { /* ask the user for the microphone first */ }

fa_encoder_options eo;
fa_encoder_options_init(&eo);
eo.format = FA_ENCODE_FLAC;
fa_encoder* rec;
fa_encoder_create_path("memo.flac", &eo, &rec);
fa_encoder_encode_stream(rec, mic);
/* ... when the user presses stop: */
fa_encoder_close(rec);    /* completes the file */
fa_stream_close(mic);

/* A chosen input, at 48 kHz stereo. */
fa_capture_options c;
fa_capture_options_init(&c);
c.device_id = info.id;    /* fa_device_get_info(engine, FA_DEVICE_CAPTURE, i, &info) */
c.sample_rate = 48000;
c.channels = 2;
fa_stream_open_capture(engine, &c, &o, &mic);
```

### C#

```csharp
using var mic = engine.OpenCapture(new CaptureOptions { Device = engine.InputDevices[0], SampleRate = 48000 },
                                   new StreamOptions { DecodeOnly = true });
var rec = AudioEncoder.Create("memo.flac", new EncoderOptions { Format = EncodeFormat.Flac });
rec.Encode(mic);
// ... when the user presses stop:
rec.Close();        // completes the file
```

### Options and behaviour

| `fa_capture_options` | .NET `CaptureOptions` | Default |
|---|---|---|
| `device_id` (an `FA_DEVICE_CAPTURE` device's `id`) | `Device` | NULL: the default input |
| `sample_rate` (8000 to 384000) | `SampleRate` | 0: the input's own |
| `channels` (1 to 32) | `Channels` | 0: the input's own |
| `FA_CAPTURE_OS_PROCESSING` in `flags` | `KeepOsProcessing` | Off: the system's echo cancelling, noise suppression and automatic gain stay off where the system allows (Windows raw capture on drivers that offer it, Android's unprocessed input), as for mixer mics. Turn it on for calls |

- Permission: `FA_ERROR_PERMISSION_DENIED` (.NET `ErrorCode.PermissionDenied`) when the system refuses the microphone (macOS, iOS, Android and the Windows privacy settings). Ask the user for it before opening. `FA_ERROR_DEVICE` when the input cannot be opened.
- An input that goes away (unplugged) ends the stream with `FA_EVENT_ENDED` once what it gave is read, unless the system moved a default input to the new default within about a second.
- The stream cannot seek or loop and its length is unknown. Its position counts the frames read or played since the first read.
- Each capture stream opens the input on its own. Opening one input twice (a mixer mic and a capture stream, say) works where the system shares an input between apps, as shared mode does.

## Pitfalls

- A callback that blocks (waits for a network, a lock held for long) starves its stream: it rebuffers, and other streams are served by the other workers meanwhile.
- `fa_stream_read` on a capture stream returns 0 frames with `FA_OK` most of the time if you call it in a tight loop: sleep 10 ms or so between empty reads.
- Hearing a capture stream adds its buffer and the output's latency to the input's: fine to check a cable, too slow to sing through.
