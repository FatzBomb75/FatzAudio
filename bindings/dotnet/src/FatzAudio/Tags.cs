using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>What <see cref="Tags.Read(string, TagReadOptions)"/> skips (<c>fa_tags_flags</c>).</summary>
[Flags]
public enum TagReadOptions
{
    /// <summary>Read the tags, the pictures and the file's properties.</summary>
    None = 0,
    /// <summary>Skip picture data (faster for library scans).</summary>
    NoPictures = 1 << 0,
    /// <summary>Skip the properties (<see cref="Tags.Info"/> is null): no decoder is opened, only the tags are read.</summary>
    NoInfo = 1 << 1,
}

/// <summary>The tag format an item or picture was found in (<c>fa_tag_format</c>).</summary>
public enum TagFormat
{
    /// <summary>ID3v2.2, 2.3 or 2.4 (MP3, AAC, AC-3, WAV, AIFF).</summary>
    Id3v2 = 1,
    /// <summary>ID3v1 at the end of the file.</summary>
    Id3v1 = 2,
    /// <summary>APEv2.</summary>
    Ape = 3,
    /// <summary>Vorbis comments and FLAC pictures (FLAC, Ogg Vorbis, Opus, Ogg FLAC).</summary>
    Vorbis = 4,
    /// <summary>MP4/M4A iTunes atoms.</summary>
    Mp4 = 5,
    /// <summary>ASF attributes (WMA).</summary>
    Asf = 6,
    /// <summary>WAV LIST/INFO chunks.</summary>
    Riff = 7,
    /// <summary>AIFF NAME, AUTH, (c) and ANNO chunks.</summary>
    Aiff = 8,
    /// <summary>Matroska/WebM tags and attachments.</summary>
    Matroska = 9,
    /// <summary>A MIDI file's title, artist and copyright.</summary>
    Midi = 10,
    /// <summary>A tracker module's title, artist and song message (from <see cref="AudioStream.GetTags"/>).</summary>
    Tracker = 11,
}

/// <summary>What a picture shows: the ID3v2 / FLAC picture types.</summary>
public enum PictureType
{
    /// <summary>Other.</summary>
    Other = 0,
    /// <summary>32×32 PNG file icon.</summary>
    FileIcon = 1,
    /// <summary>Other file icon.</summary>
    OtherFileIcon = 2,
    /// <summary>Front cover.</summary>
    FrontCover = 3,
    /// <summary>Back cover.</summary>
    BackCover = 4,
    /// <summary>Leaflet page.</summary>
    Leaflet = 5,
    /// <summary>Media (the label side of a CD).</summary>
    Media = 6,
    /// <summary>Lead artist or soloist.</summary>
    LeadArtist = 7,
    /// <summary>Artist or performer.</summary>
    Artist = 8,
    /// <summary>Conductor.</summary>
    Conductor = 9,
    /// <summary>Band or orchestra.</summary>
    Band = 10,
    /// <summary>Composer.</summary>
    Composer = 11,
    /// <summary>Lyricist or text writer.</summary>
    Lyricist = 12,
    /// <summary>Recording location.</summary>
    RecordingLocation = 13,
    /// <summary>During recording.</summary>
    DuringRecording = 14,
    /// <summary>During performance.</summary>
    DuringPerformance = 15,
    /// <summary>Movie or video screen capture.</summary>
    ScreenCapture = 16,
    /// <summary>A bright coloured fish.</summary>
    BrightColouredFish = 17,
    /// <summary>Illustration.</summary>
    Illustration = 18,
    /// <summary>Band or artist logotype.</summary>
    BandLogo = 19,
    /// <summary>Publisher or studio logotype.</summary>
    PublisherLogo = 20,
}

/// <summary>
/// One tag item exactly as the file stores it. <see cref="Key"/> is the format's own name: an ID3v2 frame ID
/// (<c>TIT2</c>; user-defined TXXX items use their description), a Vorbis or APE key (<c>ARTIST</c>), an MP4 atom
/// (<c>©nam</c>; freeform atoms use their name), an ASF attribute (<c>WM/AlbumTitle</c>), a RIFF or AIFF chunk ID
/// (<c>INAM</c>) or a Matroska tag name (<c>ALBUM/TITLE</c> for album-level tags).
/// </summary>
/// <param name="Key">The format's own key.</param>
/// <param name="Value">The value as text; a list of values is joined with "; ".</param>
/// <param name="Format">Where it was found.</param>
public sealed record TagItem(string Key, string Value, TagFormat Format);

/// <summary>An embedded picture (cover art).</summary>
/// <param name="Type">What it shows.</param>
/// <param name="MimeType">Such as <c>image/jpeg</c> or <c>image/png</c>; "" when neither stated nor recognisable from the data.</param>
/// <param name="Description">The picture's description, "" if none.</param>
/// <param name="Data">The encoded image, as stored (decode it with any image library).</param>
/// <param name="Format">Where it was found.</param>
public sealed record TagPicture(PictureType Type, string MimeType, string Description, byte[] Data, TagFormat Format);

/// <summary>
/// The tags, cover art and properties of a file. Read a file directly with <see cref="Read(string, TagReadOptions)"/>
/// (no engine or stream needed, so a media library can scan fast), or an open stream's with
/// <see cref="AudioStream.GetTags"/>. The common fields are the same whatever the format: ID3v2/ID3v1 (MP3, AAC,
/// AC-3, WAV, AIFF), APEv2, Vorbis comments (FLAC, Ogg, Opus), MP4/M4A, WMA, WAV INFO, AIFF text chunks and
/// Matroska/WebM. A ZIP (MP3+G) is read through the audio file inside it.
/// </summary>
/// <remarks>
/// With several tag formats in one file the richest wins: ID3v2 before APEv2 before ID3v1; in WAV and AIFF, ID3v2
/// before the native chunks. Everything is copied out of the native library, so the object needs no disposing and
/// is safe to share between threads.
/// </remarks>
/// <example>
/// <code>
/// var tags = Tags.Read("song.mp3");
/// Console.WriteLine($"{tags.Artist} - {tags.Title} ({tags.Year}), track {tags.Track} of {tags.TrackTotal}");
/// Console.WriteLine($"{tags.Duration} at {tags.Bitrate / 1000} kbps, {tags.Channels} channels, {tags.SampleRate} Hz");
/// if (tags.Cover is { } cover) File.WriteAllBytes("cover.jpg", cover.Data);
///
/// // A fast library scan: tags only, no pictures and no decoder.
/// var quick = Tags.Read(path, TagReadOptions.NoPictures | TagReadOptions.NoInfo);
/// </code>
/// </example>
public sealed class Tags
{
    private readonly string?[] _fields;

    private Tags(string?[] fields, TagItem[] items, TagPicture[] pictures, StreamInfo? info)
    {
        _fields = fields;
        Items = items;
        Pictures = pictures;
        Info = info;
    }

    /// <summary>Reads the tags of a file.</summary>
    /// <param name="path">Path of the file.</param>
    /// <param name="options">What to skip.</param>
    /// <returns>The tags; empty (all fields null) for a file without any.</returns>
    /// <exception cref="FatzAudioException">The file is missing (<see cref="ErrorCode.NotFound"/>) or cannot be read.</exception>
    public static Tags Read(string path, TagReadOptions options = TagReadOptions.None)
    {
        ArgumentNullException.ThrowIfNull(path);
        NativeLoader.EnsureInitialized();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_tags_read_path(path, (uint)options, out nint native));
        return Take(native);
    }

    /// <summary>Reads the tags of a file in memory.</summary>
    /// <param name="data">The file's bytes (only used during the call).</param>
    /// <param name="options">What to skip.</param>
    /// <returns>The tags; empty for data without any.</returns>
    /// <exception cref="ArgumentException"><paramref name="data"/> is empty.</exception>
    public static unsafe Tags Read(ReadOnlySpan<byte> data, TagReadOptions options = TagReadOptions.None)
    {
        if (data.IsEmpty) throw new ArgumentException("No data.", nameof(data));
        NativeLoader.EnsureInitialized();
        nint native;
        int rc;
        fixed (byte* p = data) rc = NativeMethods.fa_tags_read_memory(p, (nuint)data.Length, (uint)options, out native);
        FatzAudioException.ThrowIfFailed(rc);
        return Take(native);
    }

    internal static Tags FromStream(StreamHandle stream, TagReadOptions options)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_tags(stream, (uint)options, out nint native));
        return Take(native);
    }

    /// <summary>Copies everything out of the native tag object and closes it.</summary>
    private static Tags Take(nint native)
    {
        try
        {
            var fields = new string?[(int)TagField.Count];
            for (int f = 0; f < fields.Length; f++)
            {
                string s = Marshal.PtrToStringUTF8(NativeMethods.fa_tags_get(native, f)) ?? "";
                fields[f] = s.Length > 0 ? s : null;
            }
            var items = new TagItem[NativeMethods.fa_tags_get_item_count(native)];
            for (uint i = 0; i < items.Length; i++)
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_tags_get_item(native, i, out FaTagItem it));
                items[i] = new TagItem(Marshal.PtrToStringUTF8(it.Key) ?? "", Marshal.PtrToStringUTF8(it.Value) ?? "", (TagFormat)it.Format);
            }
            var pictures = new TagPicture[NativeMethods.fa_tags_get_picture_count(native)];
            for (uint i = 0; i < pictures.Length; i++)
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_tags_get_picture(native, i, out FaTagPicture p));
                var data = new byte[checked((int)p.Size)];
                Marshal.Copy(p.Data, data, 0, data.Length);
                pictures[i] = new TagPicture((PictureType)p.Type, Marshal.PtrToStringUTF8(p.Mime) ?? "",
                    Marshal.PtrToStringUTF8(p.Description) ?? "", data, (TagFormat)p.Format);
            }
            StreamInfo? info = NativeMethods.fa_tags_get_info(native, out FaStreamInfo n) == 0 ? StreamInfo.FromNative(n) : null;
            return new Tags(fields, items, pictures, info);
        }
        finally
        {
            NativeMethods.fa_tags_close(native);
        }
    }

    private string? Field(TagField f) => _fields[(int)f];

    private static int? Number(string? s)
    {
        if (s is null) return null;
        int end = 0;
        while (end < s.Length && char.IsAsciiDigit(s[end])) end++;
        return end > 0 && int.TryParse(s.AsSpan(0, end), out int v) ? v : null;
    }

    /// <summary>Title.</summary>
    public string? Title => Field(TagField.Title);
    /// <summary>Artist (several artists are joined with "; ").</summary>
    public string? Artist => Field(TagField.Artist);
    /// <summary>Album.</summary>
    public string? Album => Field(TagField.Album);
    /// <summary>Album artist.</summary>
    public string? AlbumArtist => Field(TagField.AlbumArtist);
    /// <summary>Composer.</summary>
    public string? Composer => Field(TagField.Composer);
    /// <summary>Genre (ID3 and MP4 genre numbers are turned into names).</summary>
    public string? Genre => Field(TagField.Genre);
    /// <summary>Date as written: "2024", "2024-05-17", ...</summary>
    public string? Date => Field(TagField.Date);
    /// <summary>The year at the start of <see cref="Date"/>.</summary>
    public int? Year => Date is { Length: >= 4 } d && int.TryParse(d.AsSpan(0, 4), out int y) ? y : null;
    /// <summary>Track number.</summary>
    public int? Track => Number(Field(TagField.Track));
    /// <summary>Number of tracks on the album.</summary>
    public int? TrackTotal => Number(Field(TagField.TrackTotal));
    /// <summary>Disc number.</summary>
    public int? Disc => Number(Field(TagField.Disc));
    /// <summary>Number of discs.</summary>
    public int? DiscTotal => Number(Field(TagField.DiscTotal));
    /// <summary>Comment.</summary>
    public string? Comment => Field(TagField.Comment);
    /// <summary>Unsynchronised lyrics.</summary>
    public string? Lyrics => Field(TagField.Lyrics);
    /// <summary>Copyright.</summary>
    public string? Copyright => Field(TagField.Copyright);
    /// <summary>The software that made the file.</summary>
    public string? Encoder => Field(TagField.Encoder);
    /// <summary>Beats per minute.</summary>
    public int? Bpm => Number(Field(TagField.Bpm));

    /// <summary>Every tag item in file order, with the format's own keys.</summary>
    public IReadOnlyList<TagItem> Items { get; }

    /// <summary>Embedded pictures in file order (none with <see cref="TagReadOptions.NoPictures"/>).</summary>
    public IReadOnlyList<TagPicture> Pictures { get; }

    /// <summary>The front cover, else the first picture; null when there is none.</summary>
    public TagPicture? Cover
    {
        get
        {
            var pictures = Pictures;
            for (int i = 0; i < pictures.Count; i++)
                if (pictures[i].Type == PictureType.FrontCover) return pictures[i];
            return pictures.Count > 0 ? pictures[0] : null;
        }
    }

    /// <summary>
    /// The file's properties (codec, duration, bitrate, sizes, ...), as <see cref="AudioStream.Info"/> gives them
    /// for a stream of the file. Null with <see cref="TagReadOptions.NoInfo"/>, or when no decoder recognised the
    /// file (MIDI files need a soundfont: read a MIDI stream's tags with <see cref="AudioStream.GetTags"/>).
    /// </summary>
    public StreamInfo? Info { get; }

    /// <summary>Duration, or null when unknown.</summary>
    public TimeSpan? Duration => Info?.Duration;
    /// <summary>Average bits per second, or null when unknown.</summary>
    public int? Bitrate => Info is { Bitrate: > 0 } i ? i.Bitrate : null;
    /// <summary>Channels.</summary>
    public int? Channels => Info?.Format.Channels;
    /// <summary>Sample rate in Hz.</summary>
    public int? SampleRate => Info?.Format.SampleRate;
    /// <summary>Bit depth of PCM and lossless files, null for lossy codecs.</summary>
    public int? BitsPerSample => Info is { BitsPerSample: > 0 } i ? i.BitsPerSample : null;
    /// <summary>Codec name such as <c>mp3</c>, <c>aac</c> or <c>flac</c>.</summary>
    public string? Codec => Info?.Codec;
    /// <summary>Bytes of the file.</summary>
    public long? FileSize => Info?.FileSize;
    /// <summary>Bytes of the audio data, without tags and headers.</summary>
    public long? AudioSize => Info?.AudioBytes;
    /// <summary>A CD+G graphics file is next to the file or inside its ZIP.</summary>
    public bool HasCdg => Info?.HasCdg ?? false;

    /// <summary>The value of the first item with this key (case-insensitive), or null.</summary>
    /// <param name="key">The format's own key, such as <c>TXXX</c> description, <c>REPLAYGAIN_TRACK_GAIN</c> or <c>WM/Mood</c>.</param>
    public string? this[string key]
    {
        get
        {
            var items = Items;
            for (int i = 0; i < items.Count; i++)
                if (string.Equals(items[i].Key, key, StringComparison.OrdinalIgnoreCase)) return items[i].Value;
            return null;
        }
    }

    private enum TagField
    {
        Title, Artist, Album, AlbumArtist, Composer, Genre, Date, Track, TrackTotal, Disc, DiscTotal,
        Comment, Lyrics, Copyright, Encoder, Bpm, Count,
    }
}
