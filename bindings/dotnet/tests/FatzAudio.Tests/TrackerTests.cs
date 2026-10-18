using System.Text;
using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Tracker modules: a 4-channel MOD built in code (decode-only, no audio hardware needed).</summary>
public class TrackerTests
{
    private const int Rate = 44100;
    private const double RowSeconds = 0.06;   // speed 3 at tempo 125
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true, MixSampleRate = Rate });

    private static void Cell(byte[] pat, int row, int ch, int sample, int period, int fx, int param)
    {
        int o = (row * 4 + ch) * 4;
        pat[o] = (byte)((sample & 0xF0) | (period >> 8));
        pat[o + 1] = (byte)(period & 0xFF);
        pat[o + 2] = (byte)(((sample & 0x0F) << 4) | fx);
        pat[o + 3] = (byte)param;
    }

    private static void Name(BinaryWriter w, string s, int n)
    {
        var b = new byte[n];
        Encoding.ASCII.GetBytes(s).CopyTo(b, 0);
        w.Write(b);
    }

    private static void BE16(BinaryWriter w, int v) { w.Write((byte)(v >> 8)); w.Write((byte)v); }

    /// <summary>
    /// "Test Module": sample 1 "Sine", two orders of 16 rows; order 0 plays on channel 0 (left), order 1 on channel 1
    /// (right), each note cut at row 8.
    /// </summary>
    internal static byte[] Module()
    {
        using var ms = new MemoryStream();
        using (var w = new BinaryWriter(ms))
        {
            Name(w, "Test Module", 20);
            for (int i = 0; i < 31; i++)
            {
                Name(w, i == 0 ? "Sine" : "", 22);
                BE16(w, i == 0 ? 16 : 0);
                w.Write((byte)0);
                w.Write((byte)(i == 0 ? 64 : 0));
                BE16(w, 0);
                BE16(w, i == 0 ? 16 : 1);
            }
            w.Write((byte)2);
            w.Write((byte)127);
            var orders = new byte[128];
            orders[1] = 1;
            w.Write(orders);
            w.Write(Encoding.ASCII.GetBytes("M.K."));
            for (int p = 0; p < 2; p++)
            {
                var pat = new byte[64 * 4 * 4];
                Cell(pat, 0, p, 1, 428, 0, 0);
                Cell(pat, 0, 3, 0, 0, 0xF, 3);
                Cell(pat, 8, p, 0, 0, 0xC, 0);
                Cell(pat, 15, 2, 0, 0, 0xD, 0);
                w.Write(pat);
            }
            for (int i = 0; i < 32; i++) w.Write((sbyte)Math.Round(Math.Sin(2 * Math.PI * i / 32) * 120));
        }
        return ms.ToArray();
    }

    private static float[] ReadAll(AudioStream s, long frames)
    {
        var all = new float[frames * 2];
        int at = 0, n;
        while (at < all.Length && (n = s.Read(all.AsSpan(at, Math.Min(4096, all.Length - at)))) > 0) at += n * 2;
        return all;
    }

    private static double Rms(float[] x, double from, double to, int ch)
    {
        int a = (int)(from * Rate), b = (int)(to * Rate);
        double sum = 0;
        for (int i = a; i < b; i++) sum += x[i * 2 + ch] * (double)x[i * 2 + ch];
        return Math.Sqrt(sum / (b - a));
    }

    [Fact]
    public void Info_NamesAndTags()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(Module(), DecodeOnly);
        var info = s.TrackerInfo;
        Assert.NotNull(info);
        Assert.Equal("mod", info!.Type);
        Assert.Equal("Test Module", info.Title);
        Assert.Equal(4, info.Channels);
        Assert.Equal(2, info.Orders);
        Assert.Equal(31, info.Samples.Count);
        Assert.Equal("Sine", info.Samples[0]);
        Assert.Equal(32 * RowSeconds, info.Duration.TotalSeconds, 2);
        Assert.Equal("Test Module", s.GetTags(TagReadOptions.NoInfo).Title);
    }

    [Fact]
    public void Plays_SeeksByOrderRow_AndMutes()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(Module(), DecodeOnly);
        var all = ReadAll(s, (long)(32 * RowSeconds * Rate));
        Assert.True(Rms(all, 0.05, 0.4, 0) > 2 * Rms(all, 0.05, 0.4, 1));
        Assert.True(Rms(all, 1.0, 1.4, 1) > 2 * Rms(all, 1.0, 1.4, 0));
        Assert.Equal(16 * RowSeconds, s.GetTrackerTime(1, 0)!.Value.TotalSeconds, 3);
        Assert.Null(s.GetTrackerTime(5, 0));
        s.SeekTracker(1, 0);
        Assert.Equal(16 * RowSeconds, s.Position.TotalSeconds, 3);
        s.SetTrackerChannelMuted(1, true);
        Assert.True(s.IsTrackerChannelMuted(1));
        s.TrackerInterpolation = TrackerInterpolation.Linear;
        Assert.Equal(TrackerInterpolation.Linear, s.TrackerInterpolation);
        Assert.Equal(100, s.TrackerStereoSeparation);
        Assert.Throws<ArgumentOutOfRangeException>(() => s.TrackerStereoSeparation = 201);
        Assert.True(s.TrackerAmigaResampler);
        s.Position = TimeSpan.Zero;
        var muted = ReadAll(s, (long)(32 * RowSeconds * Rate));
        Assert.True(Rms(muted, 1.0, 1.4, 0) + Rms(muted, 1.0, 1.4, 1) < 1e-4);
    }

    [Fact]
    public void NotAModule_HasNoTrackerInfo()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.Null(s.TrackerInfo);
        Assert.Throws<FatzAudioException>(() => s.TrackerStereoSeparation);
    }
}
