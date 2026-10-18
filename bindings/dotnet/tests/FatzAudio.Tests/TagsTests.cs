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
