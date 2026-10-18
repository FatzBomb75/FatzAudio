using System.IO.Compression;
using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>CD+G graphics: loading (files, pairs, ZIP), surfaces and the clock.</summary>
public class CdgTests
{
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    /// <summary>Colour table (0 black, 1 red, 2 green), then a red screen and a green border.</summary>
    private static byte[] SimpleCdg(int emptyPacksFirst = 0)
    {
        var packs = new List<byte[]>();
        for (int i = 0; i < emptyPacksFirst; i++) packs.Add(new byte[24]);
        var colors = new byte[16];
        colors[2] = 15 << 2;              // colour 1: red
        colors[4] = 3; colors[5] = 3 << 4; // colour 2: green
        packs.Add(Pack(30, colors));
        packs.Add(Pack(1, [1]));
        packs.Add(Pack(2, [2]));
        return packs.SelectMany(p => p).ToArray();
    }

    private static byte[] Pack(int instruction, byte[] data)
    {
        var p = new byte[24];
        p[0] = 9;
        p[1] = (byte)instruction;
        data.CopyTo(p, 4);
        return p;
    }

    private static TimeSpan AfterPacks(int n) => TimeSpan.FromTicks(n * 10_000_000L / 300 + 1);

    [Fact]
    public void LoadCdg_SurfaceDrawsBgraPixels()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.False(song.HasCdg);
        song.LoadCdg(SimpleCdg());
        Assert.True(song.HasCdg);

        using var surface = song.CreateCdgSurface();
        var frame = surface.UpdateAt(AfterPacks(3));
        Assert.True(frame.Changed);
        Assert.Equal((300, 216, 1200), (frame.Width, frame.Height, frame.Stride));
        Assert.Equal(new CdgRect(0, 0, 300, 216), frame.Dirty);
        var px = frame.Pixels;
        Assert.Equal(new byte[] { 0, 255, 0, 255 }, px.Slice(0, 4).ToArray());                // border, B G R A
        Assert.Equal(new byte[] { 0, 0, 255, 255 }, px.Slice(100 * 1200 + 400, 4).ToArray()); // red picture
        Assert.Equal(new CdgColor(255, 0, 0, 255), frame.Palette[1]);

        Assert.False(surface.UpdateAt(AfterPacks(3)).Changed);
    }

    [Fact]
    public void Frame_ReportsDecoderStateRevisionsAndInfo()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.Equal(CdgSource.None, song.CdgInfo.Source);
        var transparent = new byte[16];
        transparent[2] = 63;              // colour 2 invisible
        var bytes = SimpleCdg().Concat(Pack(28, transparent)).ToArray();
        song.LoadCdg(bytes);
        var info = song.CdgInfo;
        Assert.Equal(CdgSource.File, info.Source);
        Assert.Equal(4, info.PackCount);
        Assert.Equal(TimeSpan.FromSeconds(10), info.CheckpointInterval);
        Assert.Null(info.DiscParity);

        using var indexed = song.CreateCdgSurface(new CdgSurfaceOptions { PixelFormat = CdgPixelFormat.Indexed8 });
        var f = indexed.UpdateAt(AfterPacks(0));
        Assert.Equal(-1, f.BackgroundIndex);
        Assert.Equal(-1, f.BorderIndex);
        Assert.Equal(1UL, f.Revision);
        f = indexed.UpdateAt(AfterPacks(4));
        Assert.Equal(1, f.BackgroundIndex);
        Assert.Equal(2, f.BorderIndex);
        Assert.Equal(63, f.Transparency[2]);
        Assert.Equal(new CdgColor(255, 0, 0, 255), f.Background);
        Assert.Equal(255, f.Palette[2].A);    // no option: alpha untouched
        Assert.Equal(2UL, f.Revision);
        ulong rev = f.Revision, prev = f.PaletteRevision;
        f = indexed.UpdateAt(AfterPacks(4));
        Assert.Equal((rev, prev), (f.Revision, f.PaletteRevision));

        using var bgOnly = song.CreateCdgSurface(new CdgSurfaceOptions { TransparentBackgroundOnly = true });
        f = bgOnly.UpdateAt(AfterPacks(4));
        Assert.Equal(0, f.Palette[1].A);
        Assert.Equal(255, f.Palette[2].A);    // the transparent-colour pack is ignored

        using var custom = song.CreateCdgSurface(new CdgSurfaceOptions
            { BackgroundColor = new CdgColor(1, 2, 3, 255), BackgroundOpacity = 40 });
        f = custom.UpdateAt(AfterPacks(4));
        Assert.Equal(new CdgColor(1, 2, 3, 102), f.Palette[1]);
        Assert.Equal(f.Palette[1], f.Background);

        using var hidden = song.CreateCdgSurface(new CdgSurfaceOptions { HideUntilPreset = true });
        Assert.Equal(0, hidden.UpdateAt(AfterPacks(0)).Pixels[3]);
        Assert.Throws<ArgumentOutOfRangeException>(() => song.CreateCdgSurface(new CdgSurfaceOptions { BackgroundOpacity = 101 }));
    }

    [Fact]
    public void SurfaceOptions_ScaleCropAndTransparency()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        song.LoadCdg(SimpleCdg());
        using var big = song.CreateCdgSurface(new CdgSurfaceOptions { Width = 576, Height = 384, HideBorder = true, PixelFormat = CdgPixelFormat.Rgba32 });
        var f = big.UpdateAt(AfterPacks(3));
        Assert.Equal((576, 384), (f.Width, f.Height));
        Assert.Equal(new byte[] { 255, 0, 0, 255 }, f.Pixels.Slice(0, 4).ToArray());

        using var overlay = song.CreateCdgSurface(new CdgSurfaceOptions { Transparent = true, PixelFormat = CdgPixelFormat.Indexed8 });
        f = overlay.UpdateAt(AfterPacks(3));
        Assert.Equal(300, f.Stride);
        Assert.Equal(1, f.Pixels[100 * 300 + 100]);
        Assert.Equal(0, f.Palette[1].A);      // the background is see-through
        Assert.Equal(255, f.Palette[2].A);

        Assert.Throws<ArgumentException>(() => song.CreateCdgSurface(new CdgSurfaceOptions { Width = 10000 }));
    }

    [Fact]
    public void OffsetShiftsGraphics()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        song.LoadCdg(SimpleCdg());
        song.CdgOffset = TimeSpan.FromSeconds(1);
        Assert.Equal(TimeSpan.FromSeconds(1), song.CdgOffset);
        using var surface = song.CreateCdgSurface(new CdgSurfaceOptions { PixelFormat = CdgPixelFormat.Indexed8 });
        var f = surface.UpdateAt(TimeSpan.FromSeconds(0.5));
        Assert.Equal(0, f.Pixels[100 * 300 + 100]);
        Assert.Equal(TimeSpan.FromSeconds(-0.5), f.Time);
        f = surface.UpdateAt(TimeSpan.FromSeconds(1) + AfterPacks(3));
        Assert.Equal(1, f.Pixels[100 * 300 + 100]);
    }

    [Fact]
    public void OpenFile_FindsMatchingCdgAndZip()
    {
        string dir = Path.Combine(Path.GetTempPath(), "fa-cdg-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            string mp3g = Path.Combine(dir, "Artist - Song.wav");
            File.WriteAllBytes(mp3g, TestAudio.SineWav());
            File.WriteAllBytes(Path.Combine(dir, "Artist - Song.cdg"), SimpleCdg());
            string zip = Path.Combine(dir, "Artist - Song.zip");
            using (var z = ZipFile.Open(zip, ZipArchiveMode.Create))
            {
                using (var e = z.CreateEntry("Artist - Song.cdg", CompressionLevel.Optimal).Open()) e.Write(SimpleCdg(1000));
                using (var e = z.CreateEntry("Artist - Song.wav", CompressionLevel.Optimal).Open()) e.Write(TestAudio.SineWav());
            }

            using var engine = CreateHeadless();
            using (var pair = engine.OpenFile(mp3g, DecodeOnly)) Assert.True(pair.HasCdg);
            using (var plain = engine.OpenFile(mp3g, DecodeOnly with { NoCdg = true })) Assert.False(plain.HasCdg);
            using var zipped = engine.OpenFile(zip, DecodeOnly);
            Assert.True(zipped.HasCdg);
            Assert.Equal(44100, zipped.Info.Format.SampleRate);
            Assert.True(TestAudio.ReadAll(zipped) > 40000);
            using var surface = zipped.CreateCdgSurface(new CdgSurfaceOptions { PixelFormat = CdgPixelFormat.Indexed8 });
            Assert.Equal(0, surface.UpdateAt(TimeSpan.FromSeconds(3)).Pixels[0]);   // still empty packs
            Assert.Equal(2, surface.UpdateAt(AfterPacks(1003)).Pixels[0]);
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void OpenFile_BigCompressedZip_NeedsATempDirectory()
    {
        string dir = Path.Combine(Path.GetTempPath(), "fa-cdg-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            byte[] wav = TestAudio.SineWav(20);
            Assert.True(wav.Length > 1 << 20);
            string zip = Path.Combine(dir, "Big.zip");
            using (var z = ZipFile.Open(zip, ZipArchiveMode.Create))
            {
                using (var e = z.CreateEntry("Big.cdg", CompressionLevel.Optimal).Open()) e.Write(SimpleCdg());
                using (var e = z.CreateEntry("Big.wav", CompressionLevel.Optimal).Open()) e.Write(wav);
            }

            using (var inMemory = AudioEngine.Create(new AudioEngineOptions { NullOutput = true, ZipMemoryLimitMb = 1 }))
                Assert.Equal(ErrorCode.TooLarge, Assert.Throws<FatzAudioException>(() => inMemory.OpenFile(zip, DecodeOnly)).ErrorCode);

            string temp = Path.Combine(dir, "temp");
            using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true, ZipMemoryLimitMb = 1, TempDirectory = temp });
            using (var song = engine.OpenFile(zip, DecodeOnly))
            {
                Assert.True(song.HasCdg);
                Assert.Equal(20 * 44100, TestAudio.ReadAll(song));
            }
            // Deleted with the stream (Windows: delete-on-close; elsewhere the name is gone at once).
            Assert.Empty(Directory.GetFiles(Path.Combine(temp, "fatzaudio-tmp")));
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void Surface_FollowsTheAudioClock()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var song = engine.OpenMemory(TestAudio.SineWav(seconds: 3));
        song.LoadCdg(SimpleCdg(emptyPacksFirst: 150)); // red from 0.5 s
        using var surface = song.CreateCdgSurface(new CdgSurfaceOptions { PixelFormat = CdgPixelFormat.Indexed8 });
        song.PlayOn(output);
        var until = DateTime.UtcNow + TimeSpan.FromSeconds(8);
        int index;
        TimeSpan shown;
        do
        {
            Thread.Sleep(20);
            var f = surface.Update();
            index = f.Pixels[100 * 300 + 100];
            shown = f.Time;
        } while (shown < TimeSpan.FromSeconds(0.6) && DateTime.UtcNow < until);
        Assert.True(shown >= TimeSpan.FromSeconds(0.6));
        Assert.Equal(1, index);
        song.Pause();
        var clock = song.Clock;
        var drawn = surface.Update().Time;
        // The clock still moves briefly while the device plays out its buffer.
        Assert.InRange(drawn, clock, clock + TimeSpan.FromMilliseconds(200));
    }

    [Fact]
    public void DisposingTheStream_EndsItsSurfaces()
    {
        using var engine = CreateHeadless();
        var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        song.LoadCdg(SimpleCdg());
        var surface = song.CreateCdgSurface();
        var other = song.CreateCdgSurface();
        other.Dispose();
        song.Dispose();
        Assert.Throws<ObjectDisposedException>(() => surface.Update());
        surface.Dispose();
        surface.Dispose();
    }

    [Fact]
    public void LoadCdg_Errors()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.Equal(ErrorCode.NotFound, Assert.Throws<FatzAudioException>(() => song.LoadCdg("missing.cdg")).ErrorCode);
        Assert.Equal(ErrorCode.UnsupportedFormat, Assert.Throws<FatzAudioException>(() => song.LoadCdg(new byte[10])).ErrorCode);
        song.LoadCdg(SimpleCdg());
        song.RemoveCdg();
        Assert.False(song.HasCdg);
    }
}
