using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Callback streams, capture streams, loudness meters and engine stats, on the null output and input.</summary>
public class LiveStreamTests
{
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    /// <summary>Waits until the output has played <paramref name="duration"/> more (its clock, not the wall clock).</summary>
    private static async Task PlayFor(OutputDevice output, TimeSpan duration)
    {
        var start = output.Time;
        var deadline = DateTime.UtcNow + duration * 10;
        while (output.Time - start < duration && DateTime.UtcNow < deadline) await Task.Delay(10);
    }

    [Fact]
    public void OpenCallback_DecodeOnly_ReadsWhatTheCallbackWrites()
    {
        using var engine = AudioEngine.Create();
        int next = 0, calls = 0;
        using var s = engine.OpenCallback(22050, 2, buffer =>
        {
            if (++calls % 2 == 1) return 0;          // nothing this time
            if (next >= 1000) return AudioEngine.EndOfStream;
            int n = Math.Min(Math.Min(buffer.Length / 2, 1000 - next), 100);
            for (int i = 0; i < n; i++)
            {
                buffer[i * 2] = (next + i) / 1000f;
                buffer[i * 2 + 1] = -(next + i) / 1000f;
            }
            next += n;
            return n;
        }, DecodeOnly);
        Assert.Equal(22050, s.Info.Format.SampleRate);
        var buf = new float[512];
        int total = 0;
        for (int guard = 0; guard < 1000; guard++)
        {
            int frames = s.Read(buf);
            if (frames == 0 && next >= 1000) break;
            for (int i = 0; i < frames; i++, total++)
            {
                Assert.Equal(total / 1000f, buf[i * 2]);
                Assert.Equal(-total / 1000f, buf[i * 2 + 1]);
            }
        }
        Assert.Equal(1000, total);
    }

    [Fact]
    public void OpenCallback_AnExceptionEndsTheStream()
    {
        using var engine = AudioEngine.Create();
        using var s = engine.OpenCallback(48000, 1, _ => throw new InvalidOperationException("boom"), DecodeOnly);
        Assert.Equal(0, s.Read(new float[256]));
        Assert.Throws<ArgumentNullException>(() => engine.OpenCallback(48000, 1, null!));
        Assert.Equal(ErrorCode.InvalidArgument,
            Assert.Throws<FatzAudioException>(() => engine.OpenCallback(48000, 1, _ => 0, new StreamOptions { Loop = true })).ErrorCode);
    }

    [Fact]
    public async Task LoudnessMeter_ReadsTheR128ToneAndStatsCountTheWork()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        double phase = 0, amp = Math.Pow(10, -23 / 20.0);
        using var tone = engine.OpenCallback(48000, 2, buffer =>
        {
            for (int i = 0; i < buffer.Length; i += 2, phase += 2 * Math.PI * 997 / 48000)
                buffer[i] = buffer[i + 1] = (float)(amp * Math.Sin(phase));
            return buffer.Length / 2;
        }, new StreamOptions { BufferLength = TimeSpan.FromSeconds(2) });   // a loaded runner must not run it dry
        Assert.True(double.IsNegativeInfinity(tone.GetLoudnessMeter().Momentary));
        Assert.True(double.IsNegativeInfinity(output.GetLoudnessMeter().ShortTerm));
        tone.PlayOn(output);
        await PlayFor(output, TimeSpan.FromSeconds(3.5));
        var m = tone.GetLoudnessMeter();
        var d = output.GetLoudnessMeter();
        for (int i = 0; i < 20 && (Math.Abs(m.Momentary + 23) > 0.2 || Math.Abs(m.ShortTerm + 23) > 0.2 || Math.Abs(d.ShortTerm + 23) > 0.2); i++)
        {
            // A gap from a stalled runner leaves the windows within a few seconds.
            await PlayFor(output, TimeSpan.FromSeconds(0.25));
            m = tone.GetLoudnessMeter();
            d = output.GetLoudnessMeter();
        }
        Assert.InRange(m.Momentary, -23.2, -22.8);
        Assert.InRange(m.ShortTerm, -23.2, -22.8);
        Assert.InRange(d.ShortTerm, -23.2, -22.8);

        var stats = engine.GetStats();
        Assert.Equal(1, stats.Devices);
        Assert.Equal(1, stats.AttachedStreams);
        Assert.True(stats.Workers >= 1);
        Assert.InRange(stats.AudioLoad, double.Epsilon, 100);
        Assert.True(stats.AudioLoadPeak > 0);
        Assert.InRange(stats.WorkerLoad, 0, 100.0 * stats.Workers);
    }

    [Fact]
    public async Task OpenCapture_ReadsTheInput()
    {
        using var engine = CreateHeadless();
        using var mic = engine.OpenCapture(new CaptureOptions { SampleRate = 48000, Channels = 2 }, DecodeOnly);
        Assert.Equal(48000, mic.Info.Format.SampleRate);
        Assert.Equal(2, mic.Info.Format.Channels);
        var buf = new float[1024];
        int total = 0;
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (total < 4800 && DateTime.UtcNow < deadline)
        {
            int frames = mic.Read(buf);
            total += frames;
            if (frames == 0) await Task.Delay(10);
        }
        Assert.True(total >= 4800, $"read {total} frames");
        Assert.Throws<ArgumentException>(() => engine.OpenCapture(new CaptureOptions { Device = engine.OutputDevices.FirstOrDefault() ?? throw new InvalidOperationException() }));
    }
}
