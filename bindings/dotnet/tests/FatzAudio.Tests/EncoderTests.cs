using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Encoding to WAV, FLAC and MP3 (when LAME is installed) from every kind of source.</summary>
public class EncoderTests
{
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static string TempFile(string ext) => Path.Combine(Path.GetTempPath(), $"fatzaudio-{Guid.NewGuid():N}.{ext}");

    private static float[] Sine(int frames, int rate, int channels, double freq = 440, double amp = 0.5)
    {
        var x = new float[frames * channels];
        for (int i = 0; i < frames; i++)
            for (int c = 0; c < channels; c++)
                x[i * channels + c] = (float)(amp * Math.Sin(2 * Math.PI * freq * i / rate + c));
        return x;
    }

    private static float[] ReadAll(AudioStream s, int channels)
    {
        var all = new List<float>();
        var buf = new float[4096 * channels];
        int frames;
        while ((frames = s.Read(buf)) > 0) all.AddRange(buf.AsSpan(0, frames * channels).ToArray());
        return [.. all];
    }

    [Theory]
    [InlineData(EncodeFormat.Wav, 32)]
    [InlineData(EncodeFormat.Wav, 24)]
    [InlineData(EncodeFormat.Flac, 24)]
    public void Write_RoundTripsThroughTheDecoder(EncodeFormat format, int bits)
    {
        var path = TempFile(format == EncodeFormat.Flac ? "flac" : "wav");
        try
        {
            var x = Sine(30000, 44100, 2);
            using (var enc = AudioEncoder.Create(path, new EncoderOptions { Format = format, BitsPerSample = bits, Title = "Test" }))
            {
                enc.Write(x.AsSpan(0, 20000), 44100, 2);
                enc.Write(x.AsSpan(20000), 44100, 2);
                var st = enc.Status;
                Assert.Equal(EncoderState.Running, st.State);
                Assert.Equal(new AudioFormat(SampleFormat.Float32, 44100, 2), st.Input);
                enc.Close();
            }
            using var engine = AudioEngine.Create();
            using var s = engine.OpenFile(path, DecodeOnly);
            Assert.Equal(30000, s.Info.LengthFrames);
            var y = ReadAll(s, 2);
            Assert.Equal(x.Length, y.Length);
            double tol = bits == 32 ? 0 : 1.0 / (1 << 23);
            for (int i = 0; i < x.Length; i++) Assert.InRange(y[i] - x[i], -tol, tol);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void ConvertFile_ResamplesAndMixesToMono()
    {
        var src = TempFile("wav");
        var dst = TempFile("flac");
        try
        {
            using (var enc = AudioEncoder.Create(src, new EncoderOptions { BitsPerSample = 32 }))
                enc.Write(Sine(48000, 48000, 2), 48000, 2);
            using var engine = AudioEngine.Create();
            AudioEncoder.ConvertFile(engine, src, dst, new EncoderOptions { Format = EncodeFormat.Flac, SampleRate = 44100, Channels = 1 });
            using var s = engine.OpenFile(dst, DecodeOnly);
            Assert.Equal(44100, s.Info.Format.SampleRate);
            Assert.Equal(1, s.Info.Format.Channels);
            Assert.Equal(44100, s.Info.LengthFrames);
        }
        finally
        {
            File.Delete(src);
            File.Delete(dst);
        }
    }

    [Fact]
    public void Encode_ToAStream_ClosesItUnlessLeftOpen()
    {
        var ms = new MemoryStream();
        using (var enc = AudioEncoder.Create(ms, new EncoderOptions { Format = EncodeFormat.Flac }, leaveOpen: true))
        {
            enc.Write(Sine(10000, 32000, 1), 32000, 1);
            enc.Close();
        }
        Assert.True(ms.CanRead);   // left open
        using var engine = AudioEngine.Create();
        using var s = engine.OpenMemory(ms.ToArray(), DecodeOnly);
        Assert.Equal(10000, s.Info.LengthFrames);   // the header was completed through Seek
        Assert.Equal(10000, ReadAll(s, 1).Length);
    }

    [Fact]
    public void Encode_ToAStream_StartsAtItsPosition()
    {
        var ms = new MemoryStream();
        ms.Write(new byte[100]);   // the app's own preamble stays untouched
        using (var enc = AudioEncoder.Create(ms, new EncoderOptions(), leaveOpen: true))
            enc.Write(Sine(1000, 44100, 2), 44100, 2);
        Assert.Equal(ms.Length, ms.Position);   // left at the end
        var bytes = ms.ToArray();
        Assert.All(bytes[..100], b => Assert.Equal(0, b));
        using var engine = AudioEngine.Create();
        using var s = engine.OpenMemory(bytes[100..], DecodeOnly);
        Assert.Equal(1000, s.Info.LengthFrames);
    }

    [Fact]
    public void Wait_RejectsNegativeTimeouts()
    {
        using var enc = AudioEncoder.Create(new MemoryStream());
        Assert.Throws<ArgumentOutOfRangeException>(() => enc.Wait(TimeSpan.FromMilliseconds(-5)));
        Assert.False(enc.Wait(TimeSpan.FromMilliseconds(120)));
    }

    [Fact]
    public void Record_Device_CapturesTheOutput()
    {
        var path = TempFile("wav");
        try
        {
            using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
            using var output = engine.OpenOutput();
            using var enc = AudioEncoder.Create(path);
            enc.Record(output);
            Assert.Equal(ErrorCode.Busy, Assert.Throws<FatzAudioException>(() => enc.Record(output)).ErrorCode);
            output.Start();
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
            while (enc.Status.Frames < 4800 && DateTime.UtcNow < deadline) Thread.Sleep(10);
            enc.IsPaused = true;
            Assert.True(enc.IsPaused);
            enc.Close();
            Assert.True(new FileInfo(path).Length > 4800 * 4);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void Options_AreValidated()
    {
        var path = TempFile("flac");
        try
        {
            var ex = Assert.Throws<FatzAudioException>(() => AudioEncoder.Create(path, new EncoderOptions { Format = EncodeFormat.Flac, BitsPerSample = 32 }));
            Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
            Assert.Throws<ArgumentOutOfRangeException>(() => AudioEncoder.Create(path, new EncoderOptions { SampleRate = -1 }));
            using var enc = AudioEncoder.Create(path);
            enc.Write(new float[200], 44100, 2);
            Assert.Throws<ArgumentException>(() => enc.Write(new float[3], 44100, 2));
            Assert.Equal(ErrorCode.InvalidArgument,
                         Assert.Throws<FatzAudioException>(() => enc.Write(new float[2], 48000, 2)).ErrorCode);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void Mp3_WhenLameIsInstalled()
    {
        if (!Lame.Load()) return;   // LAME is the app's to ship; CI machines may not have it
        Assert.True(Lame.Info.IsLoaded);
        Assert.False(string.IsNullOrEmpty(Lame.Info.Version));
        var path = TempFile("mp3");
        try
        {
            using (var enc = AudioEncoder.Create(path, new EncoderOptions { Format = EncodeFormat.Mp3, Artist = "FatzAudio" }))
            {
                enc.Write(Sine(44100, 44100, 2), 44100, 2);
                enc.Close();
            }
            using var engine = AudioEngine.Create();
            using var s = engine.OpenFile(path, DecodeOnly);
            Assert.Equal("mp3", s.Info.Codec);
            Assert.InRange(ReadAll(s, 2).Length / 2, 44100 - 1152, 44100 + 1152);
        }
        finally
        {
            File.Delete(path);
        }
    }
}
