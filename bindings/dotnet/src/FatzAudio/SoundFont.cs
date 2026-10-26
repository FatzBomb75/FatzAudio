using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>
/// SoundFont 2 (.sf2) or DLS (.dls) instruments for playing MIDI files. Load one once and share it: set it as the
/// engine's <see cref="AudioEngine.DefaultSoundFont"/> for every MIDI stream, or pass it in
/// <see cref="StreamOptions.SoundFont"/> for one stream. No soundfont is built in. A MIDI file opened without one
/// plays with the instruments the OS ships (<see cref="FromSystem"/>: gm.dls on Windows, gs_instruments.dls on
/// macOS, a General MIDI .sf2 package on Linux); where there is none, or with
/// <see cref="AudioEngineOptions.NoSystemSoundFont"/>, the open throws <see cref="FatzAudioException"/> with
/// <see cref="ErrorCode.NoSoundFont"/>.
/// </summary>
/// <remarks>
/// The native soundfont is reference-counted: the engine and every stream using it keep their own reference, so
/// disposing this object at any time is safe and the instruments are freed when the last user is done. Samples
/// are held in memory as 32-bit float (about twice the size of the file). All members are thread-safe.
/// </remarks>
/// <example>
/// <code>
/// using var engine = AudioEngine.Create();
/// using (var gm = SoundFont.FromFile("GeneralUser.sf2"))
///     engine.DefaultSoundFont = gm;           // the engine keeps its own reference
/// using var song = engine.OpenFile("song.kar");
/// </code>
/// </example>
public sealed class SoundFont : IDisposable
{
    private readonly SoundFontHandle _handle;

    private SoundFont(SoundFontHandle handle)
    {
        _handle = handle;
        Info = ReadInfo();
    }

    internal SoundFontHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    /// <summary>Loads a SoundFont 2 (.sf2) or DLS (.dls) bank.</summary>
    /// <param name="path">Path of the .sf2 or .dls file.</param>
    /// <returns>A soundfont that must be disposed.</returns>
    /// <exception cref="FatzAudioException">The file is missing (<see cref="ErrorCode.NotFound"/>) or not a usable bank
    /// (<see cref="ErrorCode.UnsupportedFormat"/>; SF3 and SFZ are not supported yet).</exception>
    public static SoundFont FromFile(string path)
    {
        ArgumentNullException.ThrowIfNull(path);
        NativeLoader.EnsureInitialized();
        return Wrap(NativeMethods.fa_soundfont_open_path(path, out nint native), native);
    }

    /// <summary>Loads a SoundFont 2 or DLS bank from memory (the bytes are copied).</summary>
    /// <param name="data">The .sf2 or .dls file's bytes.</param>
    /// <returns>A soundfont that must be disposed.</returns>
    /// <exception cref="FatzAudioException">The data is not a usable bank (<see cref="ErrorCode.UnsupportedFormat"/>).</exception>
    public static unsafe SoundFont FromMemory(ReadOnlySpan<byte> data)
    {
        NativeLoader.EnsureInitialized();
        nint native;
        int rc;
        fixed (byte* p = data) rc = NativeMethods.fa_soundfont_open_memory(p, (nuint)data.Length, out native);
        return Wrap(rc, native);
    }

    /// <summary>
    /// Loads the instruments the operating system ships for its own MIDI synthesizer: gm.dls on Windows,
    /// gs_instruments.dls on macOS, the first General MIDI .sf2 package found on Linux.
    /// </summary>
    /// <returns>The OS's bank (dispose it), or null where there is none (Android, iOS, Linux without one).</returns>
    /// <exception cref="FatzAudioException">The bank is there but not usable.</exception>
    public static SoundFont? FromSystem()
    {
        NativeLoader.EnsureInitialized();
        int rc = NativeMethods.fa_soundfont_open_system(out nint native);
        if (rc == (int)ErrorCode.NotFound) return null;
        return Wrap(rc, native);
    }

    private static SoundFont Wrap(int rc, nint native)
    {
        FatzAudioException.ThrowIfFailed(rc);
        var h = new SoundFontHandle();
        Marshal.InitHandle(h, native);
        return new SoundFont(h);
    }

    private unsafe SoundFontInfo ReadInfo()
    {
        FaSoundFontInfo n;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_soundfont_get_info(Handle, &n));
        return new SoundFontInfo(NativeText.Read(n.Name), (int)n.PresetCount, (long)n.SampleMemory);
    }

    /// <summary>The bank's name, preset count and memory use.</summary>
    public SoundFontInfo Info { get; }

    /// <summary>Releases this reference. Engines and streams using the soundfont keep theirs.</summary>
    public void Dispose() => _handle.Dispose();
}

/// <summary>Information about a <see cref="SoundFont"/>.</summary>
/// <param name="Name">The bank's name from the file, "" if it has none.</param>
/// <param name="PresetCount">Instruments and drum kits in the bank.</param>
/// <param name="SampleMemory">Bytes of sample data held in memory.</param>
public sealed record SoundFontInfo(string Name, int PresetCount, long SampleMemory);

/// <summary>Kind of a MIDI text event (<see cref="MidiText.Type"/>).</summary>
public enum MidiTextType
{
    /// <summary>A syllable to sing: lyric events, or the words of a .kar file.</summary>
    Lyric = 0,
    /// <summary>Any other text event (for .kar files, the "@" header lines).</summary>
    Text = 1,
    /// <summary>A marker: a song section ("Verse", "Chorus").</summary>
    Marker = 2,
    /// <summary>A cue point.</summary>
    Cue = 3,
    /// <summary>The name of a track.</summary>
    TrackName = 4,
    /// <summary>The name of an instrument.</summary>
    InstrumentName = 5,
    /// <summary>A copyright notice.</summary>
    Copyright = 6,
}

/// <summary>Layout marks of a lyric syllable (<see cref="MidiText.Flags"/>).</summary>
[Flags]
public enum MidiTextFlags
{
    /// <summary>No mark: the syllable continues the current line.</summary>
    None = 0,
    /// <summary>A new line starts with this syllable.</summary>
    NewLine = 1 << 0,
    /// <summary>A new verse or page starts with this syllable.</summary>
    NewParagraph = 1 << 1,
}

/// <summary>
/// One text event of a MIDI file. Lyrics come cleaned up: the line and paragraph marks of .kar files and lyric
/// events are removed from <see cref="Text"/> and given as <see cref="Flags"/>, so a karaoke screen joins the
/// syllables of a line as they are and breaks before the flagged ones.
/// </summary>
/// <param name="Index">Position in <see cref="AudioStream.MidiTexts"/>.</param>
/// <param name="Type">What kind of text it is.</param>
/// <param name="Text">The text, UTF-8 decoded (Latin-1/Windows-1252 files are converted), at most 255 bytes.</param>
/// <param name="Flags">Line and paragraph marks of lyrics.</param>
/// <param name="Position">Where it is heard.</param>
/// <param name="Frame">Where it is heard, in frames.</param>
/// <param name="Track">The track (MTrk chunk) it came from, 0-based.</param>
public sealed record MidiText(int Index, MidiTextType Type, string Text, MidiTextFlags Flags, TimeSpan Position, long Frame, int Track);

/// <summary>Kind of a <see cref="MidiMark"/>.</summary>
public enum MidiMarkType
{
    /// <summary>A tempo change: <see cref="MidiMark.Value"/> is microseconds per quarter note; the text "120 bpm".</summary>
    Tempo = 0,
    /// <summary>A time signature: <see cref="MidiMark.Value"/> / <see cref="MidiMark.Value2"/>; the text "3/4".</summary>
    TimeSignature = 1,
    /// <summary>
    /// A key signature: <see cref="MidiMark.Value"/> is the number of sharps (negative: flats), <see cref="MidiMark.Value2"/>
    /// 1 for minor and 0 for major; the text "E minor".
    /// </summary>
    KeySignature = 2,
}

/// <summary>
/// A tempo change or a time or key signature of a MIDI file (<see cref="AudioStream.MidiMarks"/>). Positions are where it is
/// heard in the file; <see cref="AudioStream.Tempo"/> stretches the stream on top of the file's own tempo.
/// </summary>
/// <param name="Type">What it is.</param>
/// <param name="Value">Its first number (see <see cref="MidiMarkType"/>).</param>
/// <param name="Value2">Its second number, 0 for a tempo.</param>
/// <param name="Text">A readable form: "120 bpm", "3/4", "E minor".</param>
/// <param name="Position">Where it is heard.</param>
/// <param name="Frame">Where it is heard, in frames.</param>
/// <param name="Track">The track (MTrk chunk) it came from, 0-based.</param>
public sealed record MidiMark(MidiMarkType Type, int Value, int Value2, string Text, TimeSpan Position, long Frame, int Track);

/// <summary>Information about a MIDI stream (<see cref="AudioStream.MidiInfo"/>).</summary>
/// <param name="Title">The .kar title line, else the name of the first track.</param>
/// <param name="Artist">The second .kar title line, "" if none.</param>
/// <param name="Copyright">The copyright event, "" if none.</param>
/// <param name="Format">Standard MIDI File format: 0, 1 or 2.</param>
/// <param name="Tracks">Number of tracks.</param>
/// <param name="TextCount">Entries of <see cref="AudioStream.MidiTexts"/> (all types).</param>
/// <param name="LyricCount">Of those, <see cref="MidiTextType.Lyric"/>.</param>
/// <param name="IsKaraoke">A karaoke (.kar) file: "@K"/"@T" header lines or a "Words" track.</param>
public sealed record MidiInfo(string Title, string Artist, string Copyright, int Format, int Tracks, int TextCount, int LyricCount, bool IsKaraoke);

/// <summary>
/// A MIDI file's song information (<see cref="AudioStream.MidiCredits"/>), for intro and ending pages: the .kar "@T" and
/// "@I" header lines and the file's own copyright, tempo and signature events. Empty strings for what the file does not say.
/// </summary>
/// <param name="CodePage">How the file's text was read.</param>
/// <param name="Title">The first "@T" line, else "@I Title", else the name of the first track.</param>
/// <param name="Artist">The second "@T" line, else "@I Artist" or "Performer".</param>
/// <param name="Writer">"@I Written by", "Composer", "Music by", "Writer", "Words and music by".</param>
/// <param name="Lyricist">"@I Lyrics by", "Lyricist", "Lyrics", "Words and music by".</param>
/// <param name="Year">"@I Year", "Date", or an unlabelled year 1800-2200.</param>
/// <param name="Copyright">The copyright event, else a copyright "@I" line or third "@T" line.</param>
/// <param name="Key">The first key signature ("E minor"), else "@I Key".</param>
/// <param name="Tempo">The first tempo ("120 bpm"), else "@I Tempo" or "BPM".</param>
/// <param name="TimeSignature">The first time signature ("3/4"), else "@I Timing".</param>
/// <param name="Other">Other "@I" lines and "@T" lines after the second, without repeats.</param>
public sealed record MidiCredits(TextCodePage CodePage, string Title, string Artist, string Writer, string Lyricist, string Year,
    string Copyright, string Key, string Tempo, string TimeSignature, IReadOnlyList<string> Other);

/// <summary>Event data for <see cref="AudioStream.MidiTextReached"/>.</summary>
public sealed class MidiTextEventArgs : EventArgs
{
    internal MidiTextEventArgs(MidiText text) => Text = text;

    /// <summary>The lyric or other text being heard now.</summary>
    public MidiText Text { get; }
}
