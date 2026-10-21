using System.Text;
using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>The MIDI lyric surface: styles and presets, layout, drawing, errors.</summary>
public class LyricTests
{
    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    private static string FindTestFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            string p = Path.Combine(dir.FullName, relative);
            if (File.Exists(p)) return p;
        }
        throw new FileNotFoundException(relative);
    }

    private static void VarLen(List<byte> t, int v)
    {
        var tmp = new Stack<byte>();
        tmp.Push((byte)(v & 0x7F));
        while ((v >>= 7) != 0) tmp.Push((byte)(0x80 | (v & 0x7F)));
        t.AddRange(tmp);
    }

    private static void Lyric(List<byte> t, int delta, string text)
    {
        var b = Encoding.ASCII.GetBytes(text);
        VarLen(t, delta); t.Add(0xFF); t.Add(0x05); VarLen(t, b.Length); t.AddRange(b);
    }

    /// <summary>960 ticks a second; lyrics in the test font's letters (A B O V): "ABBA VO" at 1, 1.5, 2 s and "OBAV" at
    /// 3, 3.5 s.</summary>
    private static byte[] Song()
    {
        var t = new List<byte>();
        VarLen(t, 0); t.AddRange(new byte[] { 0x90, 69, 1 });
        Lyric(t, 960, "AB");
        Lyric(t, 480, "BA ");
        Lyric(t, 480, "VO");
        Lyric(t, 960, "\rOB");
        Lyric(t, 480, "AV");
        VarLen(t, 960); t.AddRange(new byte[] { 0x80, 69, 0 });
        VarLen(t, 0); t.AddRange(new byte[] { 0xFF, 0x2F, 0 });
        var f = new List<byte>();
        f.AddRange("MThd"u8.ToArray()); f.AddRange(new byte[] { 0, 0, 0, 6, 0, 0, 0, 1, 480 >> 8, 480 & 255 });
        f.AddRange("MTrk"u8.ToArray());
        f.AddRange(new[] { (byte)(t.Count >> 24), (byte)(t.Count >> 16), (byte)(t.Count >> 8), (byte)t.Count });
        f.AddRange(t);
        return f.ToArray();
    }

    private static (AudioEngine, SoundFont, AudioStream) Open()
    {
        var engine = CreateHeadless();
        var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        var s = engine.OpenMemory(Song(), new StreamOptions { DecodeOnly = true, SoundFont = font });
        return (engine, font, s);
    }

    private static LyricStyle TestFont(LyricPreset preset) =>
        LyricStyle.FromPreset(preset) with { FontPath = FindTestFile("tests/data/fonts/tt.ttf") };

    [Fact]
    public void Presets_HaveTheDesignNumbers()
    {
        var classic = new LyricStyle();
        Assert.Equal(LyricStyle.FromPreset(LyricPreset.Classic), classic with { });
        Assert.Equal(72f, classic.FontSize);
        Assert.Equal(700, classic.FontWeight);
        Assert.Equal(new CdgColor(255, 255, 0, 255), classic.SungColor);
        Assert.Equal(LyricWipe.Crossfade, classic.Wipe);
        Assert.Equal(TimeSpan.FromMilliseconds(600), classic.WipeCap);
        Assert.Equal(TimeSpan.FromMilliseconds(400), classic.Lead);
        Assert.Equal(TimeSpan.FromSeconds(1.5), classic.Scroll);
        Assert.Equal(LyricCountdown.Clock, classic.Countdown);
        Assert.Equal(4, classic.CountdownCount);
        Assert.Equal(0.25f, classic.AboveOpacity2);
        Assert.NotNull(classic.Effects?.Outline);
        Assert.NotNull(classic.Effects?.Shadow);
        Assert.Null(classic.Effects?.Bevel);
        Assert.Equal(LyricWipe.Sweep, LyricStyle.FromPreset(LyricPreset.Modern).Wipe);
        Assert.Equal(LyricCountdown.Off, LyricStyle.FromPreset(LyricPreset.Minimal).Countdown);
        var sub = LyricStyle.FromPreset(LyricPreset.Subtitle);
        Assert.True(sub.Panel);
        Assert.True(sub.SubtitleMode);
        Assert.Equal(0, sub.LinesAbove);
        Assert.Throws<ArgumentOutOfRangeException>(() => LyricStyle.FromPreset((LyricPreset)99));
    }

    [Fact]
    public void Surface_DrawsTheSungLine()
    {
        var (engine, font, s) = Open();
        using (engine) using (font) using (s)
        {
            using var lyrics = s.CreateLyricSurface(TestFont(LyricPreset.Classic));
            lyrics.PresentationSize = new PixelSize(1920, 1080);
            var layout = lyrics.GetLayout();
            Assert.Equal("FA Test", layout.FontUsed);
            Assert.Equal(1f, layout.Scale);
            Assert.InRange(layout.Width, (int)(0.85 * 1920), 1919);
            Assert.Equal(960, layout.BitmapX + layout.Width / 2, 1.0);
            int stride = layout.Width * 4;
            var buffer = new byte[stride * layout.Height];
            // "AB" sung, "BA" half way: yellow and white pixels.
            var update = lyrics.UpdateInto(buffer, stride, layout.Width, layout.Height, 0, TimeSpan.FromSeconds(1.3));
            Assert.True(update.IsFullDraw);
            Assert.Equal(layout.LayoutRevision, update.LayoutRevision);
            int yellow = 0, white = 0;
            for (int i = 0; i < buffer.Length; i += 4)
            {
                if (buffer[i + 3] < 240) continue;
                if (buffer[i] < 16 && buffer[i + 1] > 240 && buffer[i + 2] > 240) yellow++;
                if (buffer[i] > 240 && buffer[i + 1] > 240 && buffer[i + 2] > 240) white++;
            }
            Assert.True(yellow > 500, $"yellow {yellow}");
            Assert.True(white > 500, $"white {white}");
            // Nothing new at the same position.
            var again = lyrics.UpdateInto(buffer, stride, layout.Width, layout.Height, update.Revision, TimeSpan.FromSeconds(1.3));
            Assert.Equal(update.Revision, again.Revision);
            Assert.Equal(0, again.Dirty.Length);
            var ex = Assert.Throws<FatzAudioException>(() =>
                lyrics.UpdateInto(new byte[16], 4, 1, 1, 0, TimeSpan.FromSeconds(1)));
            Assert.Equal(ErrorCode.SizeMismatch, ex.ErrorCode);
        }
    }

    [Fact]
    public void Style_AndEffects_RoundTrip()
    {
        var (engine, font, s) = Open();
        using (engine) using (font) using (s)
        {
            using var lyrics = s.CreateLyricSurface(TestFont(LyricPreset.Modern) with { FontSize = 60 });
            Assert.Equal(60f, lyrics.Style.FontSize);
            Assert.Throws<ArgumentException>(() => lyrics.Style = lyrics.Style with { BoxWidth = 2 });
            Assert.Equal(60f, lyrics.Style.FontSize);
            var effects = new SurfaceEffects { Bevel = new BevelEffect() };
            lyrics.Effects = effects;
            Assert.Same(effects, lyrics.Style.Effects);
            lyrics.Style = lyrics.Style with { Countdown = LyricCountdown.Off };
            Assert.Equal(LyricCountdown.Off, lyrics.Style.Countdown);
            var data = File.ReadAllBytes(FindTestFile("tests/data/fonts/tt.ttf"));
            lyrics.Style = LyricStyle.FromPreset(LyricPreset.Neon) with { FontData = data };
            Assert.Equal("FA Test", lyrics.GetLayout().FontUsed);
        }
    }

    [Fact]
    public void Errors()
    {
        var (engine, font, s) = Open();
        using (engine) using (font) using (s)
        {
            Assert.Throws<ArgumentException>(() => s.CreateLyricSurface(TestFont(LyricPreset.Classic) with { FontSize = 1 }));
            var missing = Assert.Throws<FatzAudioException>(() =>
                s.CreateLyricSurface(LyricStyle.FromPreset(LyricPreset.Classic) with { FontPath = "/no/such/font.ttf" }));
            Assert.Equal(ErrorCode.NotFound, missing.ErrorCode);
            var junk = Assert.Throws<FatzAudioException>(() =>
                s.CreateLyricSurface(LyricStyle.FromPreset(LyricPreset.Classic) with { FontData = new byte[] { 1, 2, 3 } }));
            Assert.Equal(ErrorCode.UnsupportedFormat, junk.ErrorCode);
        }
        // Not MIDI.
        using var e2 = CreateHeadless();
        using var wav = e2.OpenMemory(TestAudio.SineWav(0.1), new StreamOptions { DecodeOnly = true });
        Assert.Throws<ArgumentException>(() => wav.CreateLyricSurface());
    }

    [Fact]
    public void Surface_OutlivesItsStream()
    {
        var (engine, font, s) = Open();
        using (engine) using (font)
        {
            var lyrics = s.CreateLyricSurface(TestFont(LyricPreset.Classic));
            s.Dispose();
            Assert.Throws<ObjectDisposedException>(() => lyrics.GetLayout());
            lyrics.Dispose();
            lyrics.Dispose();
        }
    }
}
