using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Audio CDs, through a .cue/.bin disc image (CI machines have no optical drive).</summary>
public sealed class DiscTests : IDisposable
{
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "fatzaudio-disc-" + Guid.NewGuid().ToString("N"));
    private readonly string _cue;

    /// <summary>Disc sample i, left channel; the right channel is its negative.</summary>
    private static short Pattern(long i) => (short)(i % 20000 + 1);

    public DiscTests()
    {
        Directory.CreateDirectory(_dir);
        // Two tracks: 20 sectors and 30 sectors (588 frames each).
        var bin = new byte[50 * 2352];
        for (long i = 0; i < 50 * 588; i++)
        {
            short l = Pattern(i), r = (short)-l;
            BitConverter.TryWriteBytes(bin.AsSpan((int)(i * 4)), l);
            BitConverter.TryWriteBytes(bin.AsSpan((int)(i * 4 + 2)), r);
        }
        File.WriteAllBytes(Path.Combine(_dir, "disc.bin"), bin);
        _cue = Path.Combine(_dir, "disc.cue");
        File.WriteAllText(_cue,
            "PERFORMER \"Band\"\nTITLE \"Album\"\nFILE \"disc.bin\" BINARY\n" +
            "  TRACK 01 AUDIO\n    TITLE \"First\"\n    INDEX 01 00:00:00\n" +
            "  TRACK 02 AUDIO\n    TITLE \"Second\"\n    ISRC USXYZ2600002\n    INDEX 00 00:00:15\n    INDEX 01 00:00:20\n");
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, true); } catch (IOException) { }
    }

    [Fact]
    public void Image_TocAndText()
    {
        using var disc = Disc.Open(_cue);
        Assert.Equal(DiscState.Ready, disc.Status.State);
        var toc = disc.Toc;
        Assert.Equal(1, toc.FirstTrack);
        Assert.Equal(2, toc.LastTrack);
        Assert.Equal(2, toc.AudioTrackCount);
        Assert.Equal(20, toc.Tracks[0].Sectors);
        Assert.Equal(30 * 588, toc.Tracks[1].Frames);
        Assert.Equal(20, toc[2]!.StartSector);
        Assert.Equal(TimeSpan.FromSeconds(50 / 75.0), toc.Duration);
        Assert.Equal(28, toc.MusicBrainzId.Length);
        Assert.StartsWith(toc.FreedbId.ToString("x8"), toc.FreedbQuery);
        Assert.True(toc.HasCdText);
        Assert.Equal("Album", disc.GetText(0, DiscTextField.Title));
        Assert.Equal("Band", disc.GetText(0, DiscTextField.Performer));
        Assert.Equal("Second", disc.GetText(2, DiscTextField.Title));
        Assert.Equal("USXYZ2600002", disc.GetText(2, DiscTextField.Code));
        Assert.Null(disc.GetText(1, DiscTextField.Composer));
    }

    [Fact]
    public void Image_Pregap()
    {
        using var disc = Disc.Open(_cue);
        Assert.Equal(0, disc.GetPregap(1));
        Assert.Equal(5, disc.GetPregap(2));
        var e = Assert.Throws<FatzAudioException>(() => disc.GetPregap(3));
        Assert.Equal(ErrorCode.NotFound, e.ErrorCode);
    }

    [Fact]
    public void Image_TrackStreamReadsExactSamples()
    {
        using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
        using var disc = Disc.Open(_cue, new DiscOptions { ReadOffset = 6 });
        using var track = engine.OpenDisc(disc, 2, DecodeOnly);
        Assert.Equal("cdda", track.Info.Codec);
        Assert.Equal(44100, track.Info.Format.SampleRate);
        Assert.Equal(30 * 588, track.Info.LengthFrames);
        var buf = new float[2 * 1000];
        int got = 0;
        while (got < 1000) got += track.Read(buf.AsSpan(got * 2));
        for (int i = 0; i < 1000; i++)
        {
            long abs = 20 * 588 + 6 + i;
            Assert.Equal(Pattern(abs) / 32768f, buf[i * 2]);
            Assert.Equal(-Pattern(abs) / 32768f, buf[i * 2 + 1]);
        }
    }

    [Fact]
    public void Image_CdgFromSubcode()
    {
        // A 400-sector track whose .sub carries CD+G packs: the screen cleared to
        // colour 1, which is loaded as red.
        const int sectors = 400;
        File.WriteAllBytes(Path.Combine(_dir, "karaoke.bin"), new byte[sectors * 2352]);
        var sub = new byte[sectors * 96];
        for (int i = 0; i < 16; i++)
        {
            int p = i * 24;
            sub[p] = 0x09; sub[p + 1] = 1; sub[p + 4] = 1; sub[p + 5] = (byte)i;   // memory preset
        }
        sub[16 * 24] = 0x09; sub[16 * 24 + 1] = 30; sub[16 * 24 + 6] = 0x3C;      // colour 1 = red
        File.WriteAllBytes(Path.Combine(_dir, "karaoke.sub"), sub);
        var cue = Path.Combine(_dir, "karaoke.cue");
        File.WriteAllText(cue, "FILE \"karaoke.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");

        using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
        using var disc = Disc.Open(cue);
        using var track = engine.OpenDisc(disc, 1, DecodeOnly);
        using var surface = track.CreateCdgSurface();     // fine before the graphics are found
        var buf = new float[2 * 4096];
        while (track.Read(buf) > 0) { }
        Assert.True(track.HasCdg);
        var frame = surface.UpdateAt(TimeSpan.FromSeconds(1));
        int at = frame.Height / 2 * frame.Stride + frame.Width / 2 * 4;
        Assert.Equal(new byte[] { 0, 0, 255 }, frame.Pixels.Slice(at, 3).ToArray());   // B, G, R

        using var plain = Disc.Open(cue, new DiscOptions { ReadCdg = false });
        using var noGraphics = engine.OpenDisc(plain, 1, DecodeOnly);
        while (noGraphics.Read(buf) > 0) { }
        Assert.False(noGraphics.HasCdg);
    }

    [Fact]
    public void Errors()
    {
        using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
        using var disc = Disc.Open(_cue);
        var ex = Assert.Throws<FatzAudioException>(() => engine.OpenDisc(disc, 3));
        Assert.Equal(ErrorCode.NotFound, ex.ErrorCode);
        ex = Assert.Throws<FatzAudioException>(() => disc.Eject());
        Assert.Equal(ErrorCode.NotImplemented, ex.ErrorCode);
        ex = Assert.Throws<FatzAudioException>(() => Disc.Open(Path.Combine(_dir, "missing.cue")));
        Assert.Equal(ErrorCode.NotFound, ex.ErrorCode);
        Assert.NotNull(Disc.Drives);   // empty on machines without a drive
    }
}
