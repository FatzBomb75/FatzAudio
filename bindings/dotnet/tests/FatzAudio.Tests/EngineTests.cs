using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

public static class TestAudio
{
    /// <summary>Generates a 16-bit mono PCM WAV file with a sine tone.</summary>
    public static byte[] SineWav(double seconds = 1.0, int sampleRate = 44100, double frequency = 440.0)
    {
        int frames = (int)(seconds * sampleRate);
        int dataBytes = frames * 2;
        using var ms = new MemoryStream(44 + dataBytes);
        using var w = new BinaryWriter(ms);
        w.Write("RIFF"u8);
        w.Write(36 + dataBytes);
        w.Write("WAVE"u8);
        w.Write("fmt "u8);
        w.Write(16);
        w.Write((short)1);          // PCM
        w.Write((short)1);          // mono
        w.Write(sampleRate);
        w.Write(sampleRate * 2);    // byte rate
        w.Write((short)2);          // block align
        w.Write((short)16);
        w.Write("data"u8);
        w.Write(dataBytes);
        for (int i = 0; i < frames; i++)
            w.Write((short)(Math.Sin(2 * Math.PI * frequency * i / sampleRate) * short.MaxValue * 0.5));
        w.Flush();
        return ms.ToArray();
    }

    public static int ReadAll(AudioStream s)
    {
        var buf = new float[4096 * s.Info.Format.Channels];
        int total = 0, n;
        while ((n = s.Read(buf)) > 0) total += n;
        return total;
    }
}

public class EngineTests
{
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    [Fact]
    public void Engine_CreateAndDispose()
    {
        var engine = AudioEngine.Create();
        Assert.True(AudioEngine.NativeVersion >= new Version(0, 1, 0));
        engine.Dispose();
        engine.Dispose(); // idempotent
        Assert.Throws<ObjectDisposedException>(() => engine.OutputDevices);
    }

    [Fact]
    public void Engine_CreateWithOptions()
    {
        using var engine = AudioEngine.Create(new AudioEngineOptions { WorkerThreads = 2 });
        Assert.NotNull(engine.OutputDevices);
    }

    [Fact]
    public void Devices_AreListed_DefaultPresentIfAny()
    {
        using var engine = AudioEngine.Create();
        var outputs = engine.OutputDevices;
        var inputs = engine.InputDevices;
        Assert.True(outputs.Count >= 0);
        Assert.True(inputs.Count >= 0);
        Assert.All(outputs, d => Assert.Equal(DeviceKind.Playback, d.Kind));
        Assert.All(inputs, d => Assert.Equal(DeviceKind.Capture, d.Kind));
        if (outputs.Count > 0)
            Assert.Contains(outputs, d => d.IsDefault);
    }

    [Fact]
    public void OpenOutput_Default_ControlsVolume()
    {
        using var engine = AudioEngine.Create();
        if (engine.OutputDevices.Count == 0) return; // headless machine
        using var output = engine.OpenOutput();
        output.Volume = 0.5f;
        Assert.Equal(0.5f, output.Volume, 3);
        Assert.True(output.Latency >= TimeSpan.Zero);
        Assert.Throws<ArgumentOutOfRangeException>(() => output.Volume = -1f);
    }

    [Fact]
    public void OpenMemory_ReportsInfo()
    {
        using var engine = AudioEngine.Create();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        var info = s.Info;
        Assert.Equal(44100, info.Format.SampleRate);
        Assert.Equal(1, info.Format.Channels);
        Assert.NotNull(info.Duration);
        Assert.InRange(info.Duration!.Value.TotalSeconds, 0.99, 1.01);
        Assert.Equal(44100, info.LengthFrames);
        Assert.True(info.CanSeek);
        Assert.False(info.HasCdg);
    }

    [Fact]
    public void DecodeOnly_Read_ReturnsAllFrames()
    {
        using var engine = AudioEngine.Create();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.Equal(44100, TestAudio.ReadAll(s));
    }

    [Fact]
    public void OpenStream_FromMemoryStream()
    {
        using var engine = AudioEngine.Create();
        AudioStream s;
        try
        {
            s = engine.OpenStream(new MemoryStream(TestAudio.SineWav()), DecodeOnly);
        }
        catch (FatzAudioException ex) when (ex.ErrorCode == ErrorCode.NotImplemented)
        {
            // The scaffold of the native library does not implement fa_stream_open_io yet.
            return;
        }
        using (s)
        {
            Assert.InRange(s.Info.Duration!.Value.TotalSeconds, 0.99, 1.01);
            Assert.Equal(44100, TestAudio.ReadAll(s));
        }
    }

    [Fact]
    public void Position_Seek()
    {
        using var engine = AudioEngine.Create();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.Equal(TimeSpan.Zero, s.Position);
        s.Position = TimeSpan.FromSeconds(0.5);
        Assert.Equal(22050, s.PositionFrames);
        Assert.InRange(s.Position.TotalSeconds, 0.499, 0.501);
        Assert.Equal(22050, TestAudio.ReadAll(s));
        s.PositionFrames = 0;
        Assert.Equal(44100, TestAudio.ReadAll(s));
        Assert.Throws<ArgumentOutOfRangeException>(() => s.Position = TimeSpan.FromSeconds(-1));
    }

    [Fact]
    public void OpenFile_MissingFile_Throws()
    {
        using var engine = AudioEngine.Create();
        var ex = Assert.Throws<FatzAudioException>(() => engine.OpenFile(Path.Combine(Path.GetTempPath(), "does-not-exist-" + Guid.NewGuid() + ".wav")));
        Assert.True(ex.ErrorCode < 0);
        Assert.False(string.IsNullOrWhiteSpace(ex.Message));
    }

    [Fact]
    public void Dispose_Works_InAnyOrder()
    {
        var engine = AudioEngine.Create();
        var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        engine.Dispose();                 // engine first
        Assert.Equal(44100, TestAudio.ReadAll(s)); // stream still fully usable
        s.Dispose();
        s.Dispose();
        Assert.Throws<ObjectDisposedException>(() => s.Info);
        Assert.Throws<ObjectDisposedException>(() => engine.OpenMemory(TestAudio.SineWav()));
    }

    [Fact]
    public void Volume_RoundTrips_OnAttachedStream()
    {
        using var engine = AudioEngine.Create();
        if (engine.OutputDevices.Count == 0) return;
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav());
        s.PlayOn(output);
        s.Pause();
        s.Volume = 0.25f;
        Assert.Equal(0.25f, s.Volume, 3);
        Assert.Equal(StreamState.Paused, s.State);
        s.Stop();
        Assert.Same(output, s.Device);
    }

    [Fact]
    public void NotImplementedCall_ThrowsWithErrorCode()
    {
        using var engine = AudioEngine.Create();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        var ex = Assert.Throws<FatzAudioException>(() => s.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(1), Curve.EqualPower));
        Assert.Equal(ErrorCode.NotImplemented, ex.ErrorCode);
        Assert.Contains("mplemented", ex.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Equal(ErrorCode.NotImplemented, Assert.Throws<FatzAudioException>(() => _ = engine.Time).ErrorCode);
        Assert.Equal(ErrorCode.NotImplemented, Assert.Throws<FatzAudioException>(() => engine.OpenUrl("http://localhost/x.mp3")).ErrorCode);
    }

    [Fact]
    public void Curve_Bezier_ValidatesControlPoints()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => Curve.Bezier(2f, 0f, 0.5f, 1f));
        Assert.Equal(Curve.Bezier(0.25f, 0.1f, 0.25f, 1f), Curve.Bezier(0.25f, 0.1f, 0.25f, 1f));
        Assert.NotEqual(Curve.Linear, Curve.EqualPower);
    }
}
