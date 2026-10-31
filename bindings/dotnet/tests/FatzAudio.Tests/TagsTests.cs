using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Tags, cover art and file properties, read from the tagged files of tests/data/tags.</summary>
public class TagsTests
{
    private static string FindTestFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            string p = Path.Combine(dir.FullName, relative);
            if (File.Exists(p)) return p;
        }
        throw new FileNotFoundException(relative);
    }

    [Theory]
    [InlineData("id3v23.mp3", "mp3", true)]
    [InlineData("tags.flac", "flac", true)]
    [InlineData("tags.m4a", "aac", true)]
    [InlineData("tags.opus", "opus", false)]
    [InlineData("tags.mka", "opus", true)]
    public void Read_CommonFieldsCoverAndProperties(string file, string codec, bool hasCover)
    {
        var tags = Tags.Read(FindTestFile("tests/data/tags/" + file));
        Assert.Equal("The Artist", tags.Artist);
        Assert.Equal("Great Album", tags.Album);
        Assert.Equal("Rock", tags.Genre);
        Assert.Equal(2024, tags.Year);
        Assert.Equal(3, tags.Track);
        Assert.Equal(12, tags.TrackTotal);
        Assert.Equal(hasCover, tags.Cover is not null);
        if (tags.Cover is { } cover) Assert.StartsWith("image/", cover.MimeType);
        Assert.Equal(codec, tags.Codec);
        Assert.NotNull(tags.Duration);
        Assert.InRange(tags.Duration!.Value.TotalSeconds, 0.47, 0.53);
        Assert.True(tags.Bitrate > 0);
        Assert.Equal(2, tags.Channels);
        Assert.True(tags.AudioSize > 0 && tags.AudioSize < tags.FileSize);
        Assert.False(tags.HasCdg);
    }

    [Fact]
    public void Read_Utf16TitleAndRawItems()
    {
        var tags = Tags.Read(FindTestFile("tests/data/tags/id3v23.mp3"));
        Assert.Equal("Sóng Tïtle", tags.Title);
        Assert.Contains(tags.Items, i => i.Key == "TIT2" && i.Format == TagFormat.Id3v2);
        Assert.Equal("Composer Name", tags["tcom"]);
        Assert.Equal(PictureType.FrontCover, tags.Cover!.Type);
    }

    [Fact]
    public void Read_OptionsSkipPicturesAndInfo()
    {
        string path = FindTestFile("tests/data/tags/tags.m4a");
        var quick = Tags.Read(path, TagReadOptions.NoPictures | TagReadOptions.NoInfo);
        Assert.Equal("Song Title", quick.Title);
        Assert.Empty(quick.Pictures);
        Assert.Null(quick.Info);
        Assert.Null(quick.Duration);

        var fromMemory = Tags.Read(File.ReadAllBytes(path));
        Assert.Equal("Song Title", fromMemory.Title);
        Assert.Single(fromMemory.Pictures);
    }

    [Fact]
    public void Read_PcmPropertiesAreExact()
    {
        var tags = Tags.Read(FindTestFile("tests/data/tags/tags.aiff"));
        Assert.Equal(8000L, tags.AudioSize);
        Assert.Equal(128000, tags.Bitrate);
        Assert.Equal(16, tags.BitsPerSample);
        Assert.Equal(8000, tags.SampleRate);
    }

    [Fact]
    public void Read_SyncedLyricsAsTimedLines()
    {
        var tags = Tags.Read(FindTestFile("tests/data/tags/sylt.mp3"), TagReadOptions.NoInfo);
        Assert.Equal("[00:01.000]Hello\n[00:01.500] world\n\n[01:02.340]Second line", tags["SYLT:Karaoke"]);
    }

    [Fact]
    public void Read_FlacCueSheetAndVendor()
    {
        var tags = Tags.Read(FindTestFile("tests/data/tags/cue.flac"), TagReadOptions.NoInfo);
        Assert.Equal("0123456789012", tags["CUESHEET/CATALOG"]);
        Assert.Equal("00:00.000", tags["CUESHEET/TRACK01"]);
        Assert.Equal("USRC17607839", tags["CUESHEET/TRACK01/ISRC"]);
        Assert.Equal("00:00.510", tags["CUESHEET/TRACK02"]);
        Assert.Null(tags["CUESHEET/TRACK170"]);
        Assert.Equal("ffmpeg", tags["VENDOR"]);
    }

    [Fact]
    public void Read_Lyrics3AtTheEndOfAnMp3()
    {
        byte[] body = System.Text.Encoding.Latin1.GetBytes("IND0000210" + "LYR00014Just the words" + "ETT00005Title");
        var file = new List<byte> { 0xFF, 0xFB, 0x90, 0x00 };
        file.AddRange(new byte[200]);
        file.AddRange(System.Text.Encoding.Latin1.GetBytes("LYRICSBEGIN"));
        file.AddRange(body);
        file.AddRange(System.Text.Encoding.Latin1.GetBytes((body.Length + 11).ToString("D6") + "LYRICS200"));
        var tags = Tags.Read(file.ToArray(), TagReadOptions.NoInfo);
        Assert.Equal("Just the words", tags.Lyrics);
        Assert.Equal("Title", tags.Title);
        Assert.Contains(tags.Items, i => i.Key == "LYR" && i.Format == TagFormat.Lyrics3);
    }

    [Fact]
    public void Read_MidiFileGivesTagsAndLengthWithoutASoundfont()
    {
        byte[] track =
        {
            0, 0xFF, 0x03, 4, (byte)'S', (byte)'o', (byte)'n', (byte)'g',
            0, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0, 0, 0xFF, 0x2F, 0,
        };
        var file = new List<byte>();
        file.AddRange("MThd"u8.ToArray());
        file.AddRange(new byte[] { 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0 });
        file.AddRange("MTrk"u8.ToArray());
        file.AddRange(new byte[] { 0, 0, 0, (byte)track.Length });
        file.AddRange(track);
        var tags = Tags.Read(file.ToArray());
        Assert.Equal("Song", tags.Title);
        Assert.Equal("midi", tags.Codec);
        Assert.InRange(tags.Duration!.Value.TotalSeconds, 0.4, 4);
    }

    [Fact]
    public void Read_MissingFileThrows()
    {
        var ex = Assert.Throws<FatzAudioException>(() => Tags.Read(Path.Combine(Path.GetTempPath(), "no-such-file.mp3")));
        Assert.Equal(ErrorCode.NotFound, ex.ErrorCode);
    }

    [Fact]
    public void Stream_GetTagsMatchesInfo()
    {
        using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
        using var s = engine.OpenFile(FindTestFile("tests/data/tags/tags.flac"), new StreamOptions { DecodeOnly = true });
        var tags = s.GetTags();
        Assert.Equal("Song Title", tags.Title);
        Assert.Equal(s.Info.Bitrate, tags.Bitrate);
        Assert.Equal(s.Info.AudioBytes, tags.AudioSize);
        Assert.Equal(24, s.Info.BitsPerSample);
        Assert.NotNull(s.Info.FileSize);
    }
}
