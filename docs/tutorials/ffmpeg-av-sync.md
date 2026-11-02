# Tutorial: audio for a video player (FFmpeg A/V sync)

FatzAudio never links FFmpeg and plays no video. If your app demuxes video with FFmpeg, it can hand the audio packets to FatzAudio and let its clock drive the picture. FatzAudio's clock reports the time of the audio leaving the speaker right now, in your own timeline, so the video follows what is heard.

## 1. Describe the audio track and play it

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
```

## 2. Feed it and render by the clock

```csharp
while (ffmpeg.av_read_frame(fmt, pkt) >= 0)
{
    if (pkt->stream_index == audioIndex)
        audio.PushPacket((nint)pkt->data, pkt->size, pkt->pts, pkt->duration);
    ffmpeg.av_packet_unref(pkt);
}
audio.End();

// in the render loop:
TimeSpan now = audio.Clock;   // show the video frame for this time
```

Pushing never blocks, so keep about a second queued (`Buffered`). When the queue runs dry the stream stalls and the clock holds.

## 3. Seeking and codecs FatzAudio does not play

Seek your demuxer, call `audio.Flush()`, and push from the new position. For DTS, TrueHD or anything else FatzAudio rejects, decode with FFmpeg and push frames with `engine.CreatePushStream(...)` and `PushFrame(...)`.

Full details, the C calls and the error cases: [FFmpeg hand-off and A/V sync](../features/ffmpeg.md).
