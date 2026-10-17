# FFmpeg hand-off and A/V sync

For apps that already read the file themselves, usually with FFmpeg because of the video. The old way with BASS is: decode the audio to PCM, convert it, push it, and keep correcting the video against the audio by hand. With FatzAudio you hand over what you already have, and the video follows one clock.

| You have | Use | What FatzAudio does |
|---|---|---|
| Compressed packets from `av_read_frame` | `fa_stream_open_packets` + `fa_stream_push_packet` (.NET `OpenPackets`, `PushPacket`) | Decodes them on its worker threads. Your app does no audio decoding at all |
| Decoded `AVFrame`s from your own decoder | `fa_stream_open_push` + `fa_stream_push_frame` (.NET `CreatePushStream`, `PushFrame`) | Takes them as they are (planar or interleaved, any sample format, any rate) and converts on a worker thread. No swresample step |
| Interleaved PCM | `fa_stream_push_pcm` (.NET `Push`) | Plays it |
| A video renderer to keep in sync | `fa_stream_get_clock` (.NET `Clock`) | Reports the pts of the audio leaving the speaker right now, in your own timeline |

FatzAudio never links or ships FFmpeg. Packet streams decode PCM, MP3, AC-3 and E-AC-3 (Dolby Digital and Dolby Digital Plus, see [Dolby Digital](dolby.md)), Opus, Vorbis and FLAC themselves and use the FFmpeg your app already loaded for everything else: AAC, DTS, TrueHD, WMA and so on.

FLAC packets (`codec = "flac"`) take FFmpeg's extradata as it is: the 34-byte STREAMINFO, with or without a `fLaC` marker and block header in front. Without extradata the frame headers and the `sample_rate`, `channels` and `bits_per_coded_sample` you give are enough.

Raw Opus packets without a container (bassopus's `BASS_OPUS_StreamCreate`) need no FFmpeg either: give `codec = "opus"`, the rate (48000) and channels, and the 19-byte `OpusHead` as `extradata` when you have one (it carries the pre-skip and gain); without it, mono or stereo is assumed with no pre-skip.

## Packets straight from the demuxer (C)

Include `fatzaudio_ffmpeg.h` after your FFmpeg headers. It fills FatzAudio's structs from FFmpeg's in one line each and compiles against whatever FFmpeg version you build with (4.0 to 8.x).

```c
#include <libavformat/avformat.h>
#include <fatzaudio/fatzaudio.h>
#include <fatzaudio/fatzaudio_ffmpeg.h>

int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
AVStream* st = fmt->streams[ai];

fa_codec_params params;
fa_codec_params_from_av(&params, st->codecpar, st->time_base);
fa_stream* audio;
if (fa_stream_open_packets(engine, &params, NULL, &audio) != FA_OK) { /* see "Errors" */ }
fa_stream_attach(audio, device, NULL);
fa_stream_play(audio);   /* buffers until the first ~100 ms arrive, then plays */

/* demux thread */
AVPacket* pkt = av_packet_alloc();
for (;;) {
    fa_time queued;
    fa_stream_get_buffered(audio, &queued);
    if (queued > 1000000000LL && video_queue_full()) { av_usleep(5000); continue; }  /* ~1 s ahead is plenty */
    if (av_read_frame(fmt, pkt) < 0) { fa_stream_push_end(audio); break; }
    if (pkt->stream_index == ai) {
        fa_packet p;
        fa_packet_from_av(&p, pkt);
        fa_stream_push_packet(audio, &p);      /* copies; never blocks */
    } else if (pkt->stream_index == vi) {
        queue_video_packet(pkt);
    }
    av_packet_unref(pkt);
}
```

Without the helper header, fill `fa_codec_params` yourself: `codec` is `avcodec_get_name(codecpar->codec_id)`, plus the sample rate, channel count, `extradata`, `block_align` and the stream's `time_base`. `fa_codec_params_init` sets the defaults first.

## Following the clock with the video

```c
/* render thread, every display refresh */
fa_time now_ns;
fa_stream_get_clock(audio, &now_ns);
int64_t now_pts = av_rescale_q(now_ns, (AVRational){1, 1000000000}, video_st->time_base);
show_latest_frame_with_pts_at_most(now_pts);
```

The clock is the audio actually heard:

- It includes the device latency (the device buffer and the output limiter's look-ahead), so it is not the time decoded or mixed.
- It moves smoothly between audio callbacks, interpolated from the device's sample clock, so it can be read every video frame.
- It is in **your** timeline: the packet pts you pushed, converted to nanoseconds. A stream whose first packet has pts 10 s starts its clock at 10 s.
- It holds still while the stream is paused, buffering or ended, and continues when playback does. When the audio has a gap or a jump in its timestamps, the clock jumps with it at the moment that audio is heard. Jitter under 15 ms is ignored.
- The clock is recorded as the audio is mixed, from the timestamps the mixed audio carries, so pitch changes need nothing extra from your app. Push and packet streams take `FA_PARAM_PITCH` only; their speed is the app's ([tempo and pitch](tempo-pitch.md)).

That's the whole sync loop. There's no drift to correct, because the video is never compared with a second clock.

## Decoded frames instead (C)

When your app decodes the audio itself (for a filter graph, or because it needs the PCM for something else), push the frames as they come out of the decoder:

```c
fa_audio_format want = { FA_FORMAT_F32, 48000, 2, 0 };    /* the stream's own format */
fa_stream_open_push(engine, &want, NULL, &audio);

while (avcodec_receive_frame(dec, frame) >= 0) {
    fa_audio_frame f;
    if (fa_audio_frame_from_av(&f, frame, st->time_base) == FA_OK)
        fa_stream_push_frame(audio, &f);
    av_frame_unref(frame);
}
```

The frame's own format can differ from the stream's, and may change from frame to frame: FatzAudio converts on its worker threads. Only 64-bit integer and planar 8-bit samples are not accepted (`FA_ERROR_UNSUPPORTED_FORMAT`).

## .NET

The binding takes plain pointers, so it works with FFmpeg.AutoGen, Sdcb.FFmpeg or your own P/Invoke.

```csharp
// st is the AVStream* of the audio track (FFmpeg.AutoGen)
var p = new CodecParameters(ffmpeg.avcodec_get_name(st->codecpar->codec_id),
                            st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels)
{
    ExtraData = new ReadOnlySpan<byte>(st->codecpar->extradata, st->codecpar->extradata_size).ToArray(),
    BlockAlign = st->codecpar->block_align,
    TimeBase = new Rational(st->time_base.num, st->time_base.den),
};
using var audio = engine.OpenPackets(p);
audio.PlayOn(output);

while (ffmpeg.av_read_frame(fmt, pkt) >= 0)
{
    if (pkt->stream_index == audioIndex)
        audio.PushPacket((nint)pkt->data, pkt->size, pkt->pts, pkt->duration);
    ffmpeg.av_packet_unref(pkt);
}
audio.End();

// render loop
TimeSpan now = audio.Clock;   // show the video frame for this time
```

Decoded frames:

```csharp
using var audio = engine.CreatePushStream(AudioFormat.Float32(48000, 2));
var fmt = new AudioFormat(FFmpeg.ToSampleFormat(frame->format)!.Value, frame->sample_rate, frame->ch_layout.nb_channels);
audio.PushFrame(fmt, (nint)frame->extended_data, frame->nb_samples, frame->best_effort_timestamp,
                new Rational(st->time_base.num, st->time_base.den));
```

`FeedAudioStream.NoTimestamp` (`long.MinValue`, the same as `AV_NOPTS_VALUE`) means "right after the previous push". `Push(ReadOnlySpan<float>)` takes interleaved PCM in the stream's own format.

## Buffering, the end and seeking

| Call | What happens |
|---|---|
| `fa_stream_get_buffered` / `Buffered` | Pushed audio not heard yet: queued, decoded ahead and in the device buffer. Counted from durations, never timestamps, so a jump in the timeline adds nothing; a queued packet without a duration counts like the one before it until it is decoded. Keep about a second queued |
| The queue runs dry | The stream is `FA_STATE_BUFFERING`, the clock holds, and `FA_EVENT_STALLED` (`Stalled`) is raised once audio had played. It resumes by itself when about 100 ms are queued (`FA_EVENT_RESUMED`, `Resumed`) |
| `fa_stream_push_end` / `End` | Plays what is queued, including what the decoder still holds (codecs with delayed output), then ends (`FA_EVENT_ENDED`). Pushing after it returns `FA_ERROR_END_OF_STREAM` |
| `fa_stream_push_flush` / `Flush` | Drops everything queued and decoded ahead, at once, including audio kept by a detach. The clock holds where it was until audio from the new position is heard, also while paused. A playing stream keeps playing whatever comes next. Also makes an ended stream pushable again |

Push streams can't be seeked with `fa_stream_seek` (`FA_ERROR_NOT_SEEKABLE`): seek your demuxer, flush, and push from the new position.

```c
av_seek_frame(fmt, -1, target, AVSEEK_FLAG_BACKWARD);
fa_stream_push_flush(audio);
/* drop queued video too, then read and push from the new position */
```

Pushing never blocks. More than 256 MB queued returns `FA_ERROR_BUSY`.

## Loading FFmpeg

When a packet stream needs a codec it has no decoder of its own for, FatzAudio looks for libavcodec and libavutil:

1. the ones already loaded in your process (your app's FFmpeg), then
2. the ones the system loader finds (next to the executable, `PATH`, the library path).

`fa_ffmpeg_load(directory)` (.NET `FFmpeg.Load(folder)`) loads them from a folder instead; call it before opening packet streams. `fa_ffmpeg_get_info` (`FFmpeg.Info`) says which version is in use and where it was found. FFmpeg 4.0 to 8.x (libavcodec 58 to 62) are supported. FatzAudio reads no FFmpeg struct layouts beyond the parts that have been stable since 4.0, so an FFmpeg upgrade doesn't need a new FatzAudio build.

```c
if (fa_ffmpeg_load("C:\\MyApp\\ffmpeg") != FA_OK) { /* only PCM and MP3 packets will work */ }
fa_ffmpeg_info info;
fa_ffmpeg_get_info(&info);
printf("libavcodec %u.%u from %s\n", info.avcodec_version >> 16, (info.avcodec_version >> 8) & 255, info.avcodec_path);
```

## Errors

| Result | Meaning |
|---|---|
| `FA_ERROR_PROVIDER_MISSING` from `fa_stream_open_packets` | The codec needs FFmpeg and none was found |
| `FA_ERROR_UNSUPPORTED_FORMAT` | FFmpeg has no decoder for the codec, or the frame's sample format isn't supported |
| `FA_ERROR_INVALID_ARG` | No codec name, a rate or channel count of 0, `FA_OPEN_LOOP`, or `push_frame` on a packet stream |
| `FA_EVENT_DECODE_ERROR` (`DecodeError`) | A packet failed to decode. It is skipped and playback continues |

## Threading

- Push calls copy the data and return; they never wait for the audio thread or a decoder. Call them from your demux thread.
- Decoding and format conversion run on the decode workers. The audio thread only mixes, as for every stream.
- `fa_stream_get_clock` takes no locks the audio thread uses and can be called from a render thread at any rate.
- `fa_stream_open_packets` with an FFmpeg codec takes a short global lock around `avcodec_open2`, which FFmpeg requires.

## Pitfalls

- Pass the **stream's** time base (`st->time_base`), not the codec's. The clock is in that timeline.
- Don't stop pushing while the video queue is full and the audio queue is empty, or the audio stalls and the video waits on it. Size the two queues by time, not by packet count.
- Decode-only packet streams (`FA_OPEN_DECODE_ONLY`) work too. `fa_stream_read` returns `FA_OK` with 0 frames while nothing is queued, and `FA_ERROR_END_OF_STREAM` after `fa_stream_push_end` once everything is read.
