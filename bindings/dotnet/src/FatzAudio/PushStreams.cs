using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>
/// A time base: the unit of a timestamp, in seconds (the same as FFmpeg's <c>AVRational</c>, for example
/// <c>AVStream.time_base</c>). The default value (0/0) means nanoseconds.
/// </summary>
/// <param name="Numerator">Numerator, for example 1.</param>
/// <param name="Denominator">Denominator, for example 48000 or 90000.</param>
public readonly record struct Rational(int Numerator, int Denominator)
{
    /// <summary>Timestamps in nanoseconds (0/0).</summary>
    public static Rational Nanoseconds => default;

    internal FaRational ToNative() => new() { Num = Numerator, Den = Denominator };
}

/// <summary>
/// Describes one compressed audio track for <see cref="AudioEngine.OpenPackets"/>: the fields of FFmpeg's
/// <c>AVCodecParameters</c> that decoding needs, plus the track's time base.
/// </summary>
/// <param name="Codec">FFmpeg codec name (<c>avcodec_get_name(codecpar->codec_id)</c>), for example <c>aac</c>,
/// <c>ac3</c>, <c>eac3</c>, <c>mp3</c>, <c>opus</c>, <c>flac</c> or <c>pcm_s16le</c>.</param>
/// <param name="SampleRate">Sample rate of the track (<c>codecpar->sample_rate</c>).</param>
/// <param name="Channels">Channel count (<c>codecpar->ch_layout.nb_channels</c>).</param>
/// <example>
/// <code>
/// // FFmpeg.AutoGen: st is the AVStream* of the audio track
/// var p = new CodecParameters(ffmpeg.avcodec_get_name(st->codecpar->codec_id),
///                             st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels)
/// {
///     ExtraData = new ReadOnlySpan&lt;byte&gt;(st->codecpar->extradata, st->codecpar->extradata_size).ToArray(),
///     BlockAlign = st->codecpar->block_align,
///     TimeBase = new Rational(st->time_base.num, st->time_base.den),
/// };
/// using var audio = engine.OpenPackets(p);
/// </code>
/// </example>
public sealed record CodecParameters(string Codec, int SampleRate, int Channels)
{
    /// <summary>Codec configuration (<c>codecpar->extradata</c>: the AAC config, the Opus header, ...); null when there is none.</summary>
    public byte[]? ExtraData { get; init; }
    /// <summary>Bit rate in bits per second; optional.</summary>
    public long BitRate { get; init; }
    /// <summary>Block alignment (<c>codecpar->block_align</c>); needed by WMA, ADPCM and PCM.</summary>
    public int BlockAlign { get; init; }
    /// <summary>Bits per coded sample (<c>codecpar->bits_per_coded_sample</c>); optional.</summary>
    public int BitsPerCodedSample { get; init; }
    /// <summary>The unit of packet timestamps and durations (<c>AVStream.time_base</c>); the default means nanoseconds.</summary>
    public Rational TimeBase { get; init; }
}

/// <summary>
/// A stream the application feeds: <see cref="PushAudioStream"/> (PCM or decoded frames) or
/// <see cref="PacketAudioStream"/> (compressed packets). Pushing never blocks.
/// </summary>
/// <remarks>
/// <para>
/// Keep about a second queued (check <see cref="Buffered"/> before reading more from your demuxer). When the
/// queue runs dry before <see cref="End"/>, <see cref="AudioStream.State"/> is <see cref="StreamState.Buffering"/>
/// (<see cref="AudioStream.Stalled"/> is raised once audio had played), and playback resumes by itself
/// (<see cref="AudioStream.Resumed"/>) when about 100 ms are queued again.
/// </para>
/// <para>
/// <see cref="AudioStream.Clock"/> reports your own timestamps: the pts of the audio leaving the speaker now.
/// Show the video frame whose pts matches it and the picture stays in sync, through pauses, stalls and
/// timestamp jumps. These streams cannot be seeked: seek your demuxer, call <see cref="Flush"/> and push from
/// the new position.
/// </para>
/// </remarks>
public abstract class FeedAudioStream : AudioStream
{
    /// <summary>"No timestamp" (FFmpeg's <c>AV_NOPTS_VALUE</c>): the data follows right after the previous push.</summary>
    public const long NoTimestamp = long.MinValue;

    private protected FeedAudioStream(AudioEngine engine, StreamHandle handle, AudioFormat? format)
        : base(engine, handle, null, null, format)
    {
    }

    /// <summary>
    /// No more data will come: the stream plays what is queued, then ends (<see cref="AudioStream.Ended"/>).
    /// Pushing after this fails with <see cref="ErrorCode.EndOfStream"/> until <see cref="Flush"/>.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public void End() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_push_end(Handle));

    /// <summary>
    /// Drops everything queued and decoded ahead, at once; call it after seeking your demuxer. The clock holds
    /// where it was until audio of the new position plays. A playing stream goes on with whatever is pushed next.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    /// <example>
    /// <code>
    /// ffmpeg.av_seek_frame(fmt, -1, target, ffmpeg.AVSEEK_FLAG_BACKWARD);
    /// audio.Flush();
    /// // keep pushing packets from the new position
    /// </code>
    /// </example>
    public void Flush() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_push_flush(Handle));

    /// <summary>
    /// How much pushed audio has not been heard yet: queued, decoded ahead and in the device buffer. It is
    /// counted from durations, never timestamps, so a jump in the timeline adds nothing; a queued packet without
    /// a duration counts like the one before it until it is decoded.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TimeSpan Buffered
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_buffered(Handle, out long ns));
            return TimeConvert.FromNanoseconds(ns);
        }
    }

    internal static long ToNativePts(TimeSpan? pts) => pts is { } t ? TimeConvert.ToNanoseconds(t) : NoTimestamp;
}

/// <summary>
/// A stream fed with PCM by the application (<see cref="AudioEngine.CreatePushStream"/>): interleaved samples
/// with <see cref="Push(ReadOnlySpan{float}, TimeSpan?)"/>, or decoded frames in any format with
/// <see cref="PushFrame"/>. See <see cref="FeedAudioStream"/> for buffering and the clock.
/// </summary>
/// <example>
/// <code>
/// using var push = engine.CreatePushStream(AudioFormat.Float32(48000, 2));
/// push.PlayOn(output);
/// push.Push(interleavedSamples);
/// </code>
/// </example>
public sealed unsafe class PushAudioStream : FeedAudioStream
{
    private readonly AudioFormat _format;

    internal PushAudioStream(AudioEngine engine, StreamHandle handle, AudioFormat format)
        : base(engine, handle, format) => _format = format;

    /// <summary>The stream's format: what <see cref="Push(ReadOnlySpan{float}, TimeSpan?)"/> accepts and what frames are converted to.</summary>
    public AudioFormat Format => _format;

    /// <summary>Pushes interleaved 32-bit float frames. The stream must have been created with <see cref="SampleFormat.Float32"/>.</summary>
    /// <param name="samples">Interleaved samples; the length must be a multiple of the channel count.</param>
    /// <param name="presentationTime">Presentation time of the first frame; null continues right after the previous push.</param>
    /// <exception cref="InvalidOperationException">The stream format is not <see cref="SampleFormat.Float32"/>.</exception>
    /// <exception cref="ArgumentException">The sample count is not a multiple of the channel count.</exception>
    /// <exception cref="FatzAudioException">The stream was ended (<see cref="ErrorCode.EndOfStream"/>) or more than 256 MB are queued (<see cref="ErrorCode.Busy"/>).</exception>
    public void Push(ReadOnlySpan<float> samples, TimeSpan? presentationTime = null)
    {
        if (_format.SampleFormat != SampleFormat.Float32)
            throw new InvalidOperationException("This push stream was not created with SampleFormat.Float32.");
        fixed (float* p = samples) PushCore(p, samples.Length, presentationTime);
    }

    /// <summary>Pushes interleaved signed 16-bit frames. The stream must have been created with <see cref="SampleFormat.Int16"/>.</summary>
    /// <param name="samples">Interleaved samples; the length must be a multiple of the channel count.</param>
    /// <param name="presentationTime">Presentation time of the first frame; null continues right after the previous push.</param>
    /// <exception cref="InvalidOperationException">The stream format is not <see cref="SampleFormat.Int16"/>.</exception>
    /// <exception cref="ArgumentException">The sample count is not a multiple of the channel count.</exception>
    /// <exception cref="FatzAudioException">The stream was ended (<see cref="ErrorCode.EndOfStream"/>) or more than 256 MB are queued (<see cref="ErrorCode.Busy"/>).</exception>
    public void Push(ReadOnlySpan<short> samples, TimeSpan? presentationTime = null)
    {
        if (_format.SampleFormat != SampleFormat.Int16)
            throw new InvalidOperationException("This push stream was not created with SampleFormat.Int16.");
        fixed (short* p = samples) PushCore(p, samples.Length, presentationTime);
    }

    private void PushCore(void* data, int sampleCount, TimeSpan? pts)
    {
        if (_format.Channels <= 0 || sampleCount % _format.Channels != 0)
            throw new ArgumentException("Sample count must be a multiple of the channel count.");
        void* plane = data;
        long frames = sampleCount / _format.Channels;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_push_pcm(Handle, &plane, frames, ToNativePts(pts)));
    }

    /// <summary>
    /// Pushes a decoded frame as it is: any sample format, planar or interleaved, any rate and channel count
    /// (converted to <see cref="Format"/> on a worker thread, so no swresample step is needed). The samples are
    /// copied before this returns, so the frame can be unreferenced right after.
    /// </summary>
    /// <param name="frameFormat">The frame's format (<see cref="FFmpeg.ToSampleFormat"/> maps <c>AVFrame.format</c>).</param>
    /// <param name="data">Pointer to the array of plane pointers: <c>AVFrame.extended_data</c> (one per channel when
    /// planar, one for interleaved data).</param>
    /// <param name="frames">Samples per channel (<c>AVFrame.nb_samples</c>).</param>
    /// <param name="pts">Timestamp in <paramref name="timeBase"/> units, or <see cref="FeedAudioStream.NoTimestamp"/>.</param>
    /// <param name="timeBase">Unit of <paramref name="pts"/> (<c>AVStream.time_base</c>); the default means nanoseconds.</param>
    /// <exception cref="ArgumentNullException"><paramref name="frameFormat"/> is null.</exception>
    /// <exception cref="ArgumentException"><paramref name="data"/> is zero or <paramref name="frames"/> is negative.</exception>
    /// <exception cref="FatzAudioException">The format is invalid (<see cref="ErrorCode.InvalidArgument"/>), the stream was ended or the queue is full.</exception>
    /// <example>
    /// <code>
    /// // FFmpeg.AutoGen, inside the avcodec_receive_frame loop
    /// var fmt = new AudioFormat(FFmpeg.ToSampleFormat(frame->format)!.Value, frame->sample_rate, frame->ch_layout.nb_channels);
    /// push.PushFrame(fmt, (nint)frame->extended_data, frame->nb_samples, frame->best_effort_timestamp,
    ///                new Rational(st->time_base.num, st->time_base.den));
    /// </code>
    /// </example>
    public void PushFrame(AudioFormat frameFormat, nint data, int frames, long pts = NoTimestamp, Rational timeBase = default)
    {
        ArgumentNullException.ThrowIfNull(frameFormat);
        if (data == 0) throw new ArgumentException("The plane pointer array is null.", nameof(data));
        if (frames < 0) throw new ArgumentException("Frame count must not be negative.", nameof(frames));
        var f = new FaAudioFrame
        {
            Format = frameFormat.ToNative(),
            Data = (void**)data,
            Frames = frames,
            Pts = pts,
            TimeBase = timeBase.ToNative(),
        };
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_push_frame(Handle, &f));
    }
}

/// <summary>
/// A stream fed with compressed packets of one audio track (<see cref="AudioEngine.OpenPackets"/>), straight
/// from your demuxer (<c>av_read_frame</c>) with no audio decoding in your app. FatzAudio decodes them on its
/// worker threads: PCM and MP3 built in, everything else through the FFmpeg your app already loaded.
/// See <see cref="FeedAudioStream"/> for buffering and the clock.
/// </summary>
/// <example>
/// <code>
/// using var audio = engine.OpenPackets(parameters);
/// audio.PlayOn(output);
/// while (ffmpeg.av_read_frame(fmt, pkt) >= 0)
/// {
///     if (pkt->stream_index == audioIndex)
///         audio.PushPacket((nint)pkt->data, pkt->size, pkt->pts, pkt->duration);
///     ffmpeg.av_packet_unref(pkt);
/// }
/// audio.End();
/// // video renderer: show the frame whose pts is closest to audio.Clock
/// </code>
/// </example>
public sealed unsafe class PacketAudioStream : FeedAudioStream
{
    internal PacketAudioStream(AudioEngine engine, StreamHandle handle, CodecParameters parameters)
        : base(engine, handle, null) => Parameters = parameters;

    /// <summary>The parameters the stream was opened with.</summary>
    public CodecParameters Parameters { get; }

    /// <summary>Queues one compressed packet; the data is copied. A packet that fails to decode is skipped
    /// (<see cref="AudioStream.DecodeError"/>) and playback continues.</summary>
    /// <param name="data">The packet's bytes.</param>
    /// <param name="pts">Timestamp in <see cref="CodecParameters.TimeBase"/> units, or <see cref="FeedAudioStream.NoTimestamp"/>.</param>
    /// <param name="duration">Duration in the same units; 0 when unknown.</param>
    /// <exception cref="FatzAudioException">The stream was ended (<see cref="ErrorCode.EndOfStream"/>) or more than 256 MB are queued (<see cref="ErrorCode.Busy"/>).</exception>
    public void PushPacket(ReadOnlySpan<byte> data, long pts = NoTimestamp, long duration = 0)
    {
        fixed (byte* p = data) PushCore(p, data.Length, pts, duration);
    }

    /// <summary>Queues one compressed packet from native memory (<c>AVPacket.data</c>); the data is copied.</summary>
    /// <param name="data">Pointer to the packet's bytes.</param>
    /// <param name="size">Size in bytes (<c>AVPacket.size</c>).</param>
    /// <param name="pts">Timestamp in <see cref="CodecParameters.TimeBase"/> units (<c>AVPacket.pts</c>), or <see cref="FeedAudioStream.NoTimestamp"/>.</param>
    /// <param name="duration">Duration in the same units (<c>AVPacket.duration</c>); 0 when unknown.</param>
    /// <exception cref="ArgumentException"><paramref name="size"/> is negative, or <paramref name="data"/> is zero with a non-zero size.</exception>
    /// <exception cref="FatzAudioException">The stream was ended (<see cref="ErrorCode.EndOfStream"/>) or more than 256 MB are queued (<see cref="ErrorCode.Busy"/>).</exception>
    public void PushPacket(nint data, int size, long pts = NoTimestamp, long duration = 0)
    {
        if (size < 0 || (data == 0 && size != 0)) throw new ArgumentException("Invalid packet buffer.", nameof(size));
        PushCore((void*)data, size, pts, duration);
    }

    private void PushCore(void* data, int size, long pts, long duration)
    {
        var p = new FaPacket { Data = data, Size = (nuint)size, Pts = pts, Duration = Math.Max(0, duration) };
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_push_packet(Handle, &p));
    }
}

/// <summary>Where FatzAudio found FFmpeg (<see cref="FFmpeg.Info"/>).</summary>
/// <param name="IsLoaded">True when FFmpeg is in use.</param>
/// <param name="AvcodecVersion">libavcodec version, or null when not loaded.</param>
/// <param name="AvutilVersion">libavutil version, or null when not loaded.</param>
/// <param name="AvcodecPath">Where libavcodec was found (a file name if the full path is unknown), or null.</param>
public sealed record FFmpegInfo(bool IsLoaded, Version? AvcodecVersion, Version? AvutilVersion, string? AvcodecPath);

/// <summary>
/// FFmpeg at runtime. FatzAudio never links or bundles FFmpeg; packet streams whose codec it has no decoder of
/// its own for use the libavcodec of your app: the one already loaded in the process (for example by
/// FFmpeg.AutoGen), else the one the system finds, or the one in the folder given to <see cref="Load"/>.
/// FFmpeg 4.0 to 8.x is supported.
/// </summary>
public static unsafe class FFmpeg
{
    /// <summary>
    /// Loads FFmpeg (libavcodec and libavutil) now. Without a folder it uses the copy already loaded in the
    /// process or the one the system loader finds; packet streams do this by themselves when needed, so call it
    /// only to use a specific folder or to check up front. Once loaded, FFmpeg stays loaded.
    /// </summary>
    /// <param name="directory">Folder with the FFmpeg libraries, or null.</param>
    /// <returns>True when FFmpeg is (already) loaded; false when no usable libavcodec was found.</returns>
    /// <example>
    /// <code>
    /// if (!FFmpeg.Load(@"C:\MyApp\ffmpeg"))
    ///     Console.WriteLine("AAC and Opus packets need FFmpeg");
    /// </code>
    /// </example>
    public static bool Load(string? directory = null)
    {
        NativeLoader.EnsureInitialized();
        int rc = NativeMethods.fa_ffmpeg_load(directory);
        if (rc == (int)ErrorCode.ProviderMissing) return false;
        FatzAudioException.ThrowIfFailed(rc);
        return true;
    }

    /// <summary>Which FFmpeg is in use, if any.</summary>
    public static FFmpegInfo Info
    {
        get
        {
            NativeLoader.EnsureInitialized();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_ffmpeg_get_info(out FaFfmpegInfo n));
            if (n.Loaded == 0) return new FFmpegInfo(false, null, null, null);
            return new FFmpegInfo(true, ToVersion(n.AvcodecVersion), ToVersion(n.AvutilVersion),
                                  Marshal.PtrToStringUTF8((nint)n.AvcodecPath));
        }
    }

    private static Version ToVersion(uint v) => new((int)(v >> 16), (int)((v >> 8) & 0xFF), (int)(v & 0xFF));

    /// <summary>
    /// Maps an FFmpeg <c>AVSampleFormat</c> (<c>AVFrame.format</c>) to the matching <see cref="SampleFormat"/>, for
    /// <see cref="PushAudioStream.PushFrame"/>.
    /// </summary>
    /// <param name="avSampleFormat">The <c>AVSampleFormat</c> value.</param>
    /// <returns>The sample format, or null for formats FatzAudio has no layout for (planar 8-bit, 64-bit integer).</returns>
    public static SampleFormat? ToSampleFormat(int avSampleFormat) => avSampleFormat switch
    {
        0 => SampleFormat.UInt8,        // AV_SAMPLE_FMT_U8
        1 => SampleFormat.Int16,        // S16
        2 => SampleFormat.Int32,        // S32
        3 => SampleFormat.Float32,      // FLT
        4 => SampleFormat.Float64,      // DBL
        6 => SampleFormat.Int16Planar,  // S16P
        7 => SampleFormat.Int32Planar,  // S32P
        8 => SampleFormat.Float32Planar,// FLTP
        9 => SampleFormat.Float64Planar,// DBLP
        _ => null,
    };
}
