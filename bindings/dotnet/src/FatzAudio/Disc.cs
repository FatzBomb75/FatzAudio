using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>An optical drive of this computer, as listed by <see cref="Disc.Drives"/>.</summary>
/// <param name="Id">Pass to <see cref="Disc.Open(string?, DiscOptions?)"/>: "D:" on Windows, "/dev/sr0" on Linux, "disk2" on macOS.</param>
/// <param name="Name">"Vendor Product", for showing in a list.</param>
/// <param name="Vendor">The drive's vendor.</param>
/// <param name="Product">The drive's model.</param>
/// <param name="Revision">The drive's firmware revision.</param>
public sealed record DiscDriveInfo(string Id, string Name, string Vendor, string Product, string Revision);

/// <summary>What is in a drive right now.</summary>
public enum DiscState
{
    /// <summary>A disc is in and readable.</summary>
    Ready = 0,
    /// <summary>The tray is closed and empty.</summary>
    NoDisc,
    /// <summary>The tray is open.</summary>
    TrayOpen,
    /// <summary>A disc was just inserted; it is ready in a moment.</summary>
    SpinningUp,
}

/// <summary>The state of a drive, from <see cref="Disc.Status"/>.</summary>
/// <param name="State">What is in the drive.</param>
/// <param name="Locked">True while the eject button is disabled by <see cref="Disc.Locked"/>.</param>
/// <param name="ChangeCount">Goes up by one each time a disc is removed or inserted, as seen by
/// <see cref="Disc.Status"/>. Poll it (every second or so) to notice a new disc.</param>
public readonly record struct DiscStatus(DiscState State, bool Locked, uint ChangeCount);

/// <summary>One track of a disc's table of contents.</summary>
/// <param name="Number">1..99, as printed on the cover. Pass it to <see cref="AudioEngine.OpenDisc"/>.</param>
/// <param name="IsAudio">False for a data track (the CD-ROM part of an Enhanced CD), which cannot be played.</param>
/// <param name="PreEmphasis">Recorded with pre-emphasis; de-emphasized automatically on playback.</param>
/// <param name="CopyPermitted">The copy-permitted flag of the track.</param>
/// <param name="FourChannel">A four-channel audio track (very rare; not playable).</param>
/// <param name="StartSector">Where the track starts, in sectors (75 per second).</param>
/// <param name="Sectors">The track's length in sectors.</param>
/// <param name="Frames">The track's length in sample frames (588 per sector), the length of its stream.</param>
/// <param name="Duration">The track's length.</param>
public sealed record DiscTrack(int Number, bool IsAudio, bool PreEmphasis, bool CopyPermitted, bool FourChannel,
                               long StartSector, long Sectors, long Frames, TimeSpan Duration);

/// <summary>A disc's table of contents, with the ids used to look the album up online.</summary>
public sealed class DiscToc
{
    internal DiscToc(int firstTrack, int lastTrack, IReadOnlyList<DiscTrack> tracks, int audioTrackCount, long leadoutSector,
                     TimeSpan duration, uint freedbId, string freedbQuery, string musicBrainzId, bool hasCdText)
    {
        FirstTrack = firstTrack;
        LastTrack = lastTrack;
        Tracks = tracks;
        AudioTrackCount = audioTrackCount;
        LeadoutSector = leadoutSector;
        Duration = duration;
        FreedbId = freedbId;
        FreedbQuery = freedbQuery;
        MusicBrainzId = musicBrainzId;
        HasCdText = hasCdText;
    }

    /// <summary>Number of the first track, usually 1.</summary>
    public int FirstTrack { get; }
    /// <summary>Number of the last track.</summary>
    public int LastTrack { get; }
    /// <summary>Every track, audio and data, in order.</summary>
    public IReadOnlyList<DiscTrack> Tracks { get; }
    /// <summary>How many tracks are audio tracks.</summary>
    public int AudioTrackCount { get; }
    /// <summary>The end of the last session, in sectors.</summary>
    public long LeadoutSector { get; }
    /// <summary>The length of all audio tracks.</summary>
    public TimeSpan Duration { get; }
    /// <summary>The freedb/CDDB disc id.</summary>
    public uint FreedbId { get; }
    /// <summary>The arguments of a CDDB "cddb query" command: id, track count, offsets and length.</summary>
    public string FreedbQuery { get; }
    /// <summary>The MusicBrainz disc id, for <c>https://musicbrainz.org/cdtoc/&lt;id&gt;</c>.</summary>
    public string MusicBrainzId { get; }
    /// <summary>True when <see cref="Disc.GetText"/> has titles from the disc's CD-Text.</summary>
    public bool HasCdText { get; }

    /// <summary>The track with this number, or null.</summary>
    /// <param name="number">The track number.</param>
    /// <returns>The track, or null when the disc has no such track.</returns>
    public DiscTrack? this[int number] => Tracks.FirstOrDefault(t => t.Number == number);
}

/// <summary>A CD-Text field, for <see cref="Disc.GetText"/>.</summary>
public enum DiscTextField
{
    /// <summary>Album or track title.</summary>
    Title = 0,
    /// <summary>Performer.</summary>
    Performer,
    /// <summary>Songwriter.</summary>
    Songwriter,
    /// <summary>Composer.</summary>
    Composer,
    /// <summary>Arranger.</summary>
    Arranger,
    /// <summary>Message from the content provider.</summary>
    Message,
    /// <summary>Genre (album only).</summary>
    Genre,
    /// <summary>Album: UPC/EAN (media catalog number). Track: ISRC. Read from the subcode when CD-Text has none.</summary>
    Code,
}

/// <summary>Options for <see cref="Disc.Open(string?, DiscOptions?)"/>.</summary>
public sealed class DiscOptions
{
    /// <summary>The drive's read offset correction in samples, as listed in the AccurateRip drive database (for example +6). Default 0.</summary>
    public int ReadOffset { get; set; }
    /// <summary>Re-reads of a damaged sector before it plays as read (or as silence when unreadable). Default 5.</summary>
    public int Retries { get; set; } = 5;
    /// <summary>De-emphasize tracks recorded with pre-emphasis. Default true.</summary>
    public bool DeEmphasis { get; set; } = true;
    /// <summary>Ask the drive for C2 error pointers to find damaged sectors. Turn off for drives that report them wrongly. Default true.</summary>
    public bool UseC2 { get; set; } = true;
    /// <summary>Read the subcode with the audio, so CD+G discs show their graphics (<see cref="AudioStream.HasCdg"/>). Turn off for drives that read it slowly. Default true.</summary>
    public bool ReadCdg { get; set; } = true;
}

/// <summary>
/// An audio CD drive (or a .cue disc image). Read its <see cref="Toc"/> and CD-Text, and play tracks with
/// <see cref="AudioEngine.OpenDisc"/>: a disc track is an ordinary stream, so crossfades, queues,
/// normalization and buses all work on it.
/// </summary>
/// <example>
/// <code>
/// using var disc = Disc.Open();                       // the first drive
/// var toc = disc.Toc;
/// Console.WriteLine($"{disc.GetText(0, DiscTextField.Title)}: {toc.AudioTrackCount} tracks");
/// using var track = engine.OpenDisc(disc, 1);
/// track.PlayOn(output);
/// </code>
/// </example>
public sealed class Disc : IDisposable
{
    private readonly DiscHandle _handle;

    private Disc(DiscHandle handle) => _handle = handle;

    internal DiscHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    /// <summary>The optical drives of this computer, built fresh on every access. On macOS only drives with an audio CD in them are listed.</summary>
    /// <exception cref="FatzAudioException">The drives could not be listed.</exception>
    public static unsafe IReadOnlyList<DiscDriveInfo> Drives
    {
        get
        {
            NativeLoader.EnsureInitialized();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_drive_count(out uint count));
            var list = new List<DiscDriveInfo>((int)count);
            for (uint i = 0; i < count; i++)
            {
                if (NativeMethods.fa_disc_drive_get_info(i, out FaDiscDriveInfo d) < 0) continue;
                list.Add(new DiscDriveInfo(Str(d.Id), Str(d.Name), Str(d.Vendor), Str(d.Product), Str(d.Revision)));
            }
            return list;
        }
    }

    /// <summary>Opens a drive, with or without a disc in it, or a disc image.</summary>
    /// <param name="driveId">A <see cref="DiscDriveInfo.Id"/>, the path of a .cue disc image, or null for the first drive.</param>
    /// <param name="options">Read options, or null for the defaults.</param>
    /// <returns>The drive; dispose it when done (streams opened from it keep working).</returns>
    /// <exception cref="FatzAudioException">No such drive (<see cref="ErrorCode.NotFound"/>), or no permission to open it (<see cref="ErrorCode.Device"/>).</exception>
    public static unsafe Disc Open(string? driveId = null, DiscOptions? options = null)
    {
        NativeLoader.EnsureInitialized();
        FaDiscOptions o;
        NativeMethods.fa_disc_options_init(&o);
        if (options is not null)
        {
            o.ReadOffset = options.ReadOffset;
            o.Retries = (uint)Math.Clamp(options.Retries, 0, 100);
            o.Flags = (options.DeEmphasis ? 0u : 1u) | (options.UseC2 ? 0u : 2u) | (options.ReadCdg ? 0u : 4u);
        }
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_open(driveId, &o, out nint native));
        var h = new DiscHandle();
        h.Initialize(native);
        return new Disc(h);
    }

    /// <summary>Asks the drive what is in it now (a few milliseconds).</summary>
    /// <exception cref="FatzAudioException">The drive did not answer.</exception>
    public DiscStatus Status
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_get_status(Handle, out FaDiscStatus s));
            return new DiscStatus((DiscState)s.State, s.Locked != 0, s.ChangeCount);
        }
    }

    /// <summary>The disc's table of contents. Read once per disc, so later reads are free.</summary>
    /// <exception cref="FatzAudioException">No readable disc is in the drive (<see cref="ErrorCode.NoDisc"/>).</exception>
    public unsafe DiscToc Toc
    {
        get
        {
            FaDiscToc* t = (FaDiscToc*)NativeMemory.AllocZeroed((nuint)sizeof(FaDiscToc));
            try
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_get_toc(Handle, t));
                var tracks = new List<DiscTrack>(t->TrackCount);
                FaDiscTrack* raw = (FaDiscTrack*)t->Tracks;
                for (int i = 0; i < t->TrackCount && i < FaDiscToc.MaxTracks; i++)
                {
                    FaDiscTrack n = raw[i];
                    tracks.Add(new DiscTrack(n.Number, n.IsAudio != 0, n.Preemphasis != 0, n.CopyPermitted != 0, n.FourChannel != 0,
                                             n.StartSector, n.Sectors, n.Frames, TimeSpan.FromTicks(n.Duration / 100)));
                }
                return new DiscToc(t->FirstTrack, t->LastTrack, tracks, t->AudioTrackCount, t->LeadoutSector,
                                   TimeSpan.FromTicks(t->Duration / 100), t->FreedbId, Str(t->FreedbQuery), Str(t->MusicBrainzId),
                                   t->HasCdText != 0);
            }
            finally
            {
                NativeMemory.Free(t);
            }
        }
    }

    /// <summary>CD-Text of the album (track 0) or of a track.</summary>
    /// <param name="track">0 for the album, else the track number.</param>
    /// <param name="field">Which text.</param>
    /// <returns>The text, or null when the disc has none for it.</returns>
    /// <exception cref="FatzAudioException">No readable disc is in the drive (<see cref="ErrorCode.NoDisc"/>).</exception>
    public unsafe string? GetText(int track, DiscTextField field)
    {
        byte* buf = stackalloc byte[1024];
        int rc = NativeMethods.fa_disc_get_text(Handle, track, (int)field, buf, 1024);
        if (rc == (int)ErrorCode.NotFound) return null;
        FatzAudioException.ThrowIfFailed(rc);
        return Marshal.PtrToStringUTF8((nint)buf);
    }

    /// <summary>Opens the tray (unlocking it first).</summary>
    /// <exception cref="FatzAudioException">The drive refused (<see cref="ErrorCode.Device"/>, or <see cref="ErrorCode.Busy"/> on macOS while Finder has the disc mounted).</exception>
    public void Eject() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_eject(Handle));

    /// <summary>Closes the tray. Slot-loading and laptop drives cannot (<see cref="ErrorCode.NotImplemented"/> or <see cref="ErrorCode.Device"/>).</summary>
    /// <exception cref="FatzAudioException">The drive cannot close its tray.</exception>
    public void Load() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_load(Handle));

    /// <summary>Disables the drive's eject button while true; reset when the disc is disposed.</summary>
    /// <exception cref="FatzAudioException">The drive refused.</exception>
    public bool Locked
    {
        get => Status.Locked;
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_set_locked(Handle, value ? 1 : 0));
    }

    /// <summary>
    /// Read speed as a multiple of normal playback speed (1 = 176.4 kB/s); set 0 for the fastest. Slower is
    /// quieter and often reads damaged discs better. The drive picks the nearest speed it supports.
    /// </summary>
    /// <exception cref="FatzAudioException">The drive does not report or accept a speed.</exception>
    public float Speed
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_get_speed(Handle, out float s));
            return s;
        }
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_disc_set_speed(Handle, value));
    }

    /// <summary>Releases the drive. Streams opened from it keep working until they are disposed.</summary>
    public void Dispose() => _handle.Dispose();

    private static unsafe string Str(byte* p) => Marshal.PtrToStringUTF8((nint)p) ?? "";
}
