using System.Runtime.InteropServices;
using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Push streams, packet streams and the presentation clock (the FFmpeg hand-off).</summary>
public class PushStreamTests
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    private static async Task WaitFor(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + Timeout;
        while (!condition() && DateTime.UtcNow < deadline) await Task.Delay(10);
        Assert.True(condition());
    }

    private static float[] ReadToEnd(AudioStream s, int channels)
    {
        var all = new List<float>();
        var buf = new float[1024 * channels];
        int frames;
        while ((frames = s.Read(buf)) > 0) all.AddRange(buf.AsSpan(0, frames * channels).ToArray());
        return [.. all];
    }

    private static float[] Constant(int frames, int channels, float value)
    {
        var x = new float[frames * channels];
        Array.Fill(x, value);
        return x;
    }

    [Fact]
    public void Push_DecodeOnly_ReturnsExactlyWhatWasPushed()
    {
        using var engine = AudioEngine.Create();
        using var s = engine.CreatePushStream(AudioFormat.Float32(48000, 2), DecodeOnly);
        var a = Constant(1000, 2, 0.25f);
        s.Push(a);
        s.Push(Constant(500, 2, -0.5f));
        s.End();
        var got = ReadToEnd(s, 2);
        Assert.Equal(3000, got.Length);
        Assert.Equal(0.25f, got[0]);
        Assert.Equal(-0.5f, got[^1]);
        Assert.Equal(ErrorCode.EndOfStream, Assert.Throws<FatzAudioException>(() => s.Push(a)).ErrorCode);
        s.Flush();
        s.Push(a); // pushable again after a flush
    }

    [Fact]
    public unsafe void PushFrame_ConvertsPlanarDoubleAtAnotherRate()
    {
        using var engine = AudioEngine.Create();
        using var s = engine.CreatePushStream(AudioFormat.Float32(48000, 2), DecodeOnly);
        const int frames = 2400; // 100 ms at 24 kHz
        double* plane = (double*)NativeMemory.Alloc(frames, sizeof(double));
        try
        {
            for (int i = 0; i < frames; i++) plane[i] = 0.5;
            double** planes = stackalloc double*[1];
            planes[0] = plane;
            var fmt = new AudioFormat(SampleFormat.Float64Planar, 24000, 1);
            s.PushFrame(fmt, (nint)planes, frames, 0, new Rational(1, 24000));
            s.PushFrame(fmt, (nint)planes, frames); // no timestamp: follows on
        }
        finally
        {
            NativeMemory.Free(plane);
        }
        s.End();
        var got = ReadToEnd(s, 2);
        Assert.InRange(got.Length / 2, 9600 - 200, 9600 + 200);
        Assert.Equal(0.5f, got[got.Length / 2], 2);
    }

    [Fact]
    public void FFmpeg_ToSampleFormat_MapsAvSampleFormat()
    {
        Assert.Equal(SampleFormat.Float32Planar, FFmpeg.ToSampleFormat(8));
        Assert.Equal(SampleFormat.Int16, FFmpeg.ToSampleFormat(1));
        Assert.Null(FFmpeg.ToSampleFormat(5));   // U8P
        Assert.Null(FFmpeg.ToSampleFormat(10));  // S64
    }

    [Fact]
    public void OpenPackets_PcmPacketsSplitAnywhere()
    {
        using var engine = AudioEngine.Create();
        using var s = engine.OpenPackets(new CodecParameters("pcm_s16le", 44100, 2) { TimeBase = new Rational(1, 44100) }, DecodeOnly);
        Assert.Equal(44100, s.Info.Format.SampleRate);
        Assert.Equal("pcm_s16le", s.Info.Codec);
        var bytes = new byte[4410 * 4];
        for (int i = 0; i < bytes.Length; i += 2) BitConverter.TryWriteBytes(bytes.AsSpan(i), (short)16384);
        // odd sizes: a sample split across packets is carried over
        int at = 0, pts = 0;
        foreach (int size in new[] { 1, 7, 1000, 3, bytes.Length })
        {
            int n = Math.Min(size, bytes.Length - at);
            if (n <= 0) break;
            s.PushPacket(bytes.AsSpan(at, n), pts);
            at += n;
            pts = at / 4;
        }
        s.End();
        var got = ReadToEnd(s, 2);
        Assert.Equal(4410 * 2, got.Length);
        Assert.Equal(0.5f, got[100], 3);
    }

    [Fact]
    public void OpenPackets_RejectsBadParameters()
    {
        using var engine = AudioEngine.Create();
        Assert.Throws<ArgumentNullException>(() => engine.OpenPackets(null!));
        Assert.Equal(ErrorCode.InvalidArgument,
            Assert.Throws<FatzAudioException>(() => engine.OpenPackets(new CodecParameters("pcm_s16le", 0, 2))).ErrorCode);
        Assert.Equal(ErrorCode.InvalidArgument,
            Assert.Throws<FatzAudioException>(() => engine.OpenPackets(new CodecParameters("pcm_s16le", 48000, 2), new StreamOptions { Loop = true })).ErrorCode);
        var e = Assert.Throws<FatzAudioException>(() => engine.OpenPackets(new CodecParameters("no-such-codec", 48000, 2)));
        Assert.True(e.ErrorCode is ErrorCode.UnsupportedFormat or ErrorCode.ProviderMissing);
    }

    [Fact]
    public async Task Playing_BuffersStallsResumesAndEnds()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.CreatePushStream(AudioFormat.Float32(48000, 2));
        int stalled = 0, resumed = 0;
        var ended = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        s.Stalled += (_, _) => Interlocked.Increment(ref stalled);
        s.Resumed += (_, _) => Interlocked.Increment(ref resumed);
        s.Ended += (_, _) => ended.TrySetResult();

        s.PlayOn(output);
        await WaitFor(() => s.State == StreamState.Buffering); // nothing pushed yet

        s.Push(Constant(14400, 2, 0.1f), TimeSpan.FromSeconds(10)); // 300 ms in the app's timeline
        // Wait for the clock, not for State == Playing: that lasts only until the 300 ms run out, and the
        // null output plays them in one go when a busy runner gives the test the CPU back.
        await WaitFor(() => s.Clock > TimeSpan.FromSeconds(10.05));
        Assert.True(s.Buffered < TimeSpan.FromMilliseconds(350));

        await WaitFor(() => s.State == StreamState.Buffering); // ran dry
        await WaitFor(() => Volatile.Read(ref stalled) == 1);
        var held = s.Clock;
        Assert.InRange(held.TotalSeconds, 10.25, 10.31);

        s.Push(Constant(9600, 2, 0.1f)); // continues the timeline
        s.End();
        await ended.Task.WaitAsync(Timeout);
        Assert.Equal(1, Volatile.Read(ref resumed));
        Assert.InRange(s.Clock.TotalSeconds, 10.45, 10.51);
    }

    [Fact]
    public async Task Flush_DropsQueuedAudioAndKeepsPlaying()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.CreatePushStream(AudioFormat.Float32(48000, 2));
        s.Push(Constant(48000 * 2, 2, 0.1f), TimeSpan.Zero);
        Assert.True(s.Buffered > TimeSpan.FromSeconds(1)); // before playing, so nothing has drained yet
        s.PlayOn(output);
        await WaitFor(() => s.Clock > TimeSpan.FromMilliseconds(50)); // playing (the clock moves)
        s.Flush();
        Assert.True(s.Buffered < TimeSpan.FromMilliseconds(200));
        s.Push(Constant(24000, 2, 0.1f), TimeSpan.FromSeconds(30)); // the app seeked to 0:30
        await WaitFor(() => s.Clock >= TimeSpan.FromSeconds(30));
        Assert.True(s.State is StreamState.Playing or StreamState.Buffering);
    }
}
