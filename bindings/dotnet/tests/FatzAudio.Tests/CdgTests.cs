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
    public void CdgChanged_RaisedWithTheWholeScreenFirst()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        song.LoadCdg(SimpleCdg());
        using var got = new ManualResetEventSlim();
        CdgChangedEventArgs? first = null;
        EventHandler<CdgChangedEventArgs> handler = (_, e) => { first ??= e; got.Set(); };
        song.CdgChanged += handler;
        Assert.True(got.Wait(TimeSpan.FromSeconds(5)));
        Assert.Equal(new CdgRect(0, 0, 300, 216), first!.Area);
        Assert.True(first.PaletteChanged);
        song.CdgChanged -= handler;
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

    [Fact]
    public void UpdateInto_MatchesUpdateAndDrawsOnlyChanges()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        var tile = new byte[16];
        tile[0] = 1; tile[1] = 2; tile[2] = 4; tile[3] = 4;      // colours 1 / 2, row 4, column 4
        for (int i = 4; i < 16; i++) tile[i] = 0x01;
        song.LoadCdg([.. SimpleCdg(), .. Pack(6, tile)]);
        using var reference = song.CreateCdgSurface();
        IKaraokeSurface surface = song.CreateCdgSurface();

        var layout = surface.GetLayout();
        Assert.Equal((300, 216), (layout.Width, layout.Height));
        Assert.Equal(new PixelRect(0, 0, 300, 216), layout.Crop);
        Assert.Equal(SafetyZone.Show, layout.SafetyZone);
        Assert.Equal(1f, layout.Scale);
        Assert.Equal("", layout.FontUsed);
        Assert.Equal(1UL, layout.LayoutRevision);

        int stride = 300 * 4 + 8;
        var buffer = new byte[stride * 216];
        var first = surface.UpdateInto(buffer, stride, 300, 216, 0, AfterPacks(2));
        Assert.True(first.IsFullDraw);
        Assert.Equal(new PixelRect(0, 0, 300, 216), first.Dirty[0]);
        ulong revision = first.Revision;
        Assert.Equal(new CdgColor(255, 0, 0, 255), first.Palette[1]);

        var again = surface.UpdateInto(buffer, stride, 300, 216, revision, AfterPacks(2));
        Assert.Equal(0, again.Dirty.Length);
        Assert.Equal(revision, again.BaseRevision);

        var next = surface.UpdateInto(buffer, stride, 300, 216, revision, AfterPacks(3));
        Assert.False(next.IsFullDraw);
        Assert.Equal(new PixelRect(24, 48, 6, 12), next.Dirty[0]);
        Assert.True(next.DrawTime >= next.RenderTime);

        var frame = reference.UpdateAt(AfterPacks(3));
        for (int y = 0; y < 216; y++)
            Assert.True(frame.Pixels.Slice(y * frame.Stride, 1200).SequenceEqual(buffer.AsSpan(y * stride, 1200)));

        var error = Assert.Throws<FatzAudioException>(() => surface.UpdateInto(buffer, stride, 299, 216, 0));
        Assert.Equal(ErrorCode.SizeMismatch, error.ErrorCode);
        Assert.Throws<ArgumentException>(() => surface.UpdateInto(new byte[100], stride, 300, 216, 0));

        surface.PresentationSize = new PixelSize(1920, 1080);
        Assert.Equal(new PixelSize(1920, 1080), surface.PresentationSize);
        Assert.Throws<ArgumentException>(() => surface.PresentationSize = new PixelSize(0, 1080));
        surface.PresentationSize = new PixelSize(7680, 4320);
        Assert.Equal(new PixelSize(3840, 2160), surface.PresentationSize);
        surface.RenderAhead = true;
        Assert.True(surface.RenderAhead);
        Assert.Equal(300, surface.GetLayout().Width);    // the classic picture keeps its size

        surface.Dispose();
        Assert.Throws<ObjectDisposedException>(() => surface.GetLayout());
    }

    [Fact]
    public void Presentation_ScaledCropPaddingAndTransparentBackground()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        var tile = new byte[16];
        tile[0] = 1; tile[1] = 2; tile[2] = 4; tile[3] = 4;      // colours 1 / 2, row 4, column 4
        for (int i = 4; i < 16; i++) tile[i] = 0x01;              // the last pixel of each row green
        song.LoadCdg([.. SimpleCdg(), .. Pack(6, tile)]);
        using var cdg = song.CreateCdgSurface();
        Assert.Null(cdg.Presentation);

        cdg.Presentation = new CdgPresentation { Scale = true, SafetyZone = SafetyZone.Remove, Padding = 1 };
        cdg.PresentationSize = new PixelSize(1920, 1080);
        var layout = cdg.GetLayout();
        Assert.Equal(6f, layout.Scale);
        Assert.Equal(((288 + 4) * 6, (192 + 4) * 6), (layout.Width, layout.Height));
        Assert.Equal((6, 6), (layout.Margin, layout.Padding));
        Assert.Equal(new PixelRect(12, 12, 288 * 6, 192 * 6), layout.Content);
        Assert.Equal(SafetyZone.Remove, layout.SafetyZone);
        Assert.True(layout.LayoutRevision > 1);

        int stride = layout.Width * 4;
        var buffer = new byte[stride * layout.Height];
        var update = cdg.UpdateInto(buffer, stride, layout.Width, layout.Height, 0, AfterPacks(4));
        Assert.True(update.IsFullDraw);
        Assert.Equal(new CdgColor(255, 0, 0, 255), update.Background);   // red, drawn transparent
        // Screen pixel (29, 48) is green: B, G, R, A at its block.
        int x = layout.Content.X + (29 - 6) * 6 + 3, y = layout.Content.Y + (48 - 12) * 6 + 3;
        Assert.Equal(new byte[] { 0, 255, 0, 255 }, buffer.AsSpan(y * stride + x * 4, 4).ToArray());
        Assert.Equal(new byte[] { 0, 0, 0, 0 }, buffer.AsSpan((y - 6) * stride + x * 4, 4).ToArray());
        Assert.Equal(new byte[] { 0, 0, 0, 0 }, buffer.AsSpan(0, 4).ToArray());

        // Without scaling: the surface's own format, padding as the transparent index.
        using var indexed = song.CreateCdgSurface(new CdgSurfaceOptions { PixelFormat = CdgPixelFormat.Indexed8 });
        indexed.Presentation = new CdgPresentation { Padding = 2, TransparentBorder = true };
        var small = indexed.GetLayout();
        Assert.Equal((304, 220), (small.Width, small.Height));
        var pixels = new byte[304 * 220];
        indexed.UpdateInto(pixels, 304, 304, 220, 0, AfterPacks(4));
        Assert.Equal(CdgSurface.TransparentIndex, pixels[0]);
        Assert.Equal(CdgSurface.TransparentIndex, pixels[2 * 304 + 2]);   // the border
        Assert.Equal(1, pixels[(2 + 20) * 304 + 2 + 20]);                  // red picture area
        Assert.Equal(2, pixels[(2 + 48) * 304 + 2 + 29]);

        Assert.Throws<ArgumentOutOfRangeException>(() => cdg.Presentation = new CdgPresentation { Smoothness = 11 });
        Assert.Throws<ArgumentOutOfRangeException>(() => cdg.Presentation = new CdgPresentation { Padding = 33 });
        cdg.Presentation = null;
        Assert.Equal(300, cdg.GetLayout().Width);
    }

    [Fact]
    public void Effects_OutlineAroundTheShapeAndValidation()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        var tile = new byte[16];
        tile[0] = 1; tile[1] = 2; tile[2] = 4; tile[3] = 4;
        for (int i = 4; i < 16; i++) tile[i] = 0x01;              // a green line at screen x = 29, y 48..59
        song.LoadCdg([.. SimpleCdg(), .. Pack(6, tile)]);
        using var cdg = song.CreateCdgSurface();
        Assert.Null(cdg.Effects);
        cdg.Presentation = new CdgPresentation { Scale = true, SafetyZone = SafetyZone.Remove };
        cdg.PresentationSize = new PixelSize(1920, 1080);
        var layout = cdg.GetLayout();
        int stride = layout.Width * 4;
        var buffer = new byte[stride * layout.Height];
        int x = layout.Content.X + (30 - 6) * 6 + 1, y = layout.Content.Y + (52 - 12) * 6 + 3;   // just right of the line

        cdg.UpdateInto(buffer, stride, layout.Width, layout.Height, 0, AfterPacks(4));
        Assert.Equal(new byte[] { 0, 0, 0, 0 }, buffer.AsSpan(y * stride + x * 4, 4).ToArray());

        var effects = new SurfaceEffects { Outline = new OutlineEffect { Color = new CdgColor(0, 0, 255, 255), Size = 1 } };
        cdg.Effects = effects;
        Assert.Same(effects, cdg.Effects);
        var update = cdg.UpdateInto(buffer, stride, layout.Width, layout.Height, 0, AfterPacks(4));
        Assert.True(update.IsFullDraw);
        Assert.Equal(new byte[] { 255, 0, 0, 255 }, buffer.AsSpan(y * stride + x * 4, 4).ToArray());   // blue, B G R A

        Assert.Throws<ArgumentOutOfRangeException>(() => cdg.Effects = new SurfaceEffects { Outline = new OutlineEffect { Size = 1.5f } });
        Assert.Throws<ArgumentOutOfRangeException>(() => cdg.Effects = new SurfaceEffects { Shadow = new ShadowEffect { Opacity = float.NaN } });
        Assert.Throws<ArgumentOutOfRangeException>(() => cdg.Effects = new SurfaceEffects { Bevel = new BevelEffect { LightAngleDegrees = float.PositiveInfinity } });
        Assert.Same(effects, cdg.Effects);
        cdg.Effects = new SurfaceEffects
        {
            Shadow = new ShadowEffect { Size = 1, Fade = 1 },
            Outline = new OutlineEffect(),
            Bevel = new BevelEffect(),
        };
        cdg.Effects = null;
        Assert.Null(cdg.Effects);
        cdg.UpdateInto(buffer, stride, layout.Width, layout.Height, 0, AfterPacks(4));
        Assert.Equal(new byte[] { 0, 0, 0, 0 }, buffer.AsSpan(y * stride + x * 4, 4).ToArray());
    }
}
