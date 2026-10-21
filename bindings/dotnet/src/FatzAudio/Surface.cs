using FatzAudio.Native;

namespace FatzAudio;

/// <summary>A size in pixels.</summary>
/// <param name="Width">Width in pixels.</param>
/// <param name="Height">Height in pixels.</param>
public readonly record struct PixelSize(int Width, int Height)
{
    /// <summary>No size (0 x 0).</summary>
    public static PixelSize Unset => default;
    /// <summary>True for 0 x 0.</summary>
    public bool IsUnset => Width == 0 && Height == 0;
}

/// <summary>A rectangle of pixels.</summary>
/// <param name="X">Left edge.</param>
/// <param name="Y">Top edge.</param>
/// <param name="Width">Width; 0 when empty.</param>
/// <param name="Height">Height; 0 when empty.</param>
public readonly record struct PixelRect(int X, int Y, int Width, int Height)
{
    /// <summary>True when the rectangle covers no pixel.</summary>
    public bool IsEmpty => Width <= 0 || Height <= 0;
}

/// <summary>Which part of the 300x216 CD+G screen a surface shows.</summary>
public enum SafetyZone
{
    /// <summary>All of it, border included.</summary>
    Show = 0,
    /// <summary>The 288x192 picture area (<see cref="CdgSurfaceOptions.HideBorder"/>).</summary>
    Remove,
    /// <summary>Reserved for the CD+G presentation options.</summary>
    Auto,
    /// <summary>Reserved for the CD+G presentation options.</summary>
    Super,
}

/// <summary>What a surface draws: the buffer <see cref="IKaraokeSurface.UpdateInto(nint, int, int, int, ulong, TimeSpan?)"/>
/// wants and where it goes (<see cref="IKaraokeSurface.GetLayout"/>).</summary>
/// <param name="Width">Width of the buffer to draw into, in pixels.</param>
/// <param name="Height">Height of the buffer, in pixels.</param>
/// <param name="BitmapX">Left edge of the buffer inside the presentation area (0 without a presentation size).</param>
/// <param name="BitmapY">Top edge of the buffer inside the presentation area.</param>
/// <param name="Content">The picture inside the buffer, without padding and margin.</param>
/// <param name="Scale">CD+G: the whole-number multiple shown (1 for the classic picture, whatever its output size).</param>
/// <param name="Crop">CD+G: the part of the 300x216 screen shown, in CD+G pixels.</param>
/// <param name="SafetyZone">CD+G: <see cref="FatzAudio.SafetyZone.Show"/>, or <see cref="FatzAudio.SafetyZone.Remove"/>
/// with <see cref="CdgSurfaceOptions.HideBorder"/>.</param>
/// <param name="SafetyZoneResolved">True (false is reserved for zones a disc can't work out).</param>
/// <param name="FontUsed">Lyrics: the font family drawn with; empty for CD+G.</param>
/// <param name="Margin">In buffer pixels; 0 for the classic picture.</param>
/// <param name="Padding">In buffer pixels; 0 for the classic picture.</param>
/// <param name="LayoutRevision">Goes up whenever any of the above changes: then reallocate, and the next draw is a full one.</param>
public sealed record SurfaceLayout(int Width, int Height, int BitmapX, int BitmapY, PixelRect Content, float Scale,
    PixelRect Crop, SafetyZone SafetyZone, bool SafetyZoneResolved, string FontUsed, int Margin, int Padding,
    ulong LayoutRevision);

/// <summary>
/// What one <see cref="IKaraokeSurface.UpdateInto(nint, int, int, int, ulong, TimeSpan?)"/> call drew. Keep
/// <see cref="Revision"/> with the buffer and pass it back the next time that buffer is drawn into. The spans belong to
/// the surface and stay valid until its next update.
/// </summary>
public readonly ref struct SurfaceUpdate
{
    internal SurfaceUpdate(FaSurfaceFrame f, ReadOnlySpan<PixelRect> dirty, ReadOnlySpan<CdgColor> palette,
        ReadOnlySpan<byte> transparency, CdgColor background)
    {
        Revision = f.Revision;
        BaseRevision = f.BaseRevision;
        LayoutRevision = f.LayoutRevision;
        Dirty = dirty;
        DirtyBounds = new PixelRect(f.BoundsX, f.BoundsY, f.BoundsWidth, f.BoundsHeight);
        Time = TimeConvert.FromNanoseconds(f.Time);
        DrawTime = TimeSpan.FromTicks(f.DrawNs / 100);
        RenderTime = TimeSpan.FromTicks(f.RenderNs / 100);
        Behind = f.Behind != 0;
        BackgroundIndex = f.BackgroundIndex;
        BorderIndex = f.BorderIndex;
        Background = background;
        Palette = palette;
        Transparency = transparency;
        PaletteRevision = f.PaletteRevision;
    }

    /// <summary>The revision of the picture now in the buffer.</summary>
    public ulong Revision { get; }
    /// <summary>The <see cref="Dirty"/> rectangles are the change from this revision (the one passed in); 0 = a full draw.</summary>
    public ulong BaseRevision { get; }
    /// <summary>True when every pixel was drawn.</summary>
    public bool IsFullDraw => BaseRevision == 0;
    /// <summary>Of the layout drawn with (<see cref="SurfaceLayout.LayoutRevision"/>).</summary>
    public ulong LayoutRevision { get; }
    /// <summary>The pixels drawn, at most 8 rectangles: upload these. Empty when the buffer was already up to date.</summary>
    public ReadOnlySpan<PixelRect> Dirty { get; }
    /// <summary>The box around <see cref="Dirty"/>.</summary>
    public PixelRect DirtyBounds { get; }
    /// <summary>Position on the graphics timeline shown (as <see cref="CdgFrame.Time"/>).</summary>
    public TimeSpan Time { get; }
    /// <summary>How long the call took.</summary>
    public TimeSpan DrawTime { get; }
    /// <summary>Of that, drawing pixels (with render-ahead: the time the frame took on the worker).</summary>
    public TimeSpan RenderTime { get; }
    /// <summary>Render-ahead: true when this is an older finished frame.</summary>
    public bool Behind { get; }
    /// <summary>CD+G: as <see cref="CdgFrame.BackgroundIndex"/>.</summary>
    public int BackgroundIndex { get; }
    /// <summary>CD+G: as <see cref="CdgFrame.BorderIndex"/>.</summary>
    public int BorderIndex { get; }
    /// <summary>CD+G: as <see cref="CdgFrame.Background"/>.</summary>
    public CdgColor Background { get; }
    /// <summary>CD+G: the 16 colours, to colour <see cref="CdgPixelFormat.Indexed8"/> pixels on the GPU.</summary>
    public ReadOnlySpan<CdgColor> Palette { get; }
    /// <summary>CD+G: as <see cref="CdgFrame.Transparency"/>.</summary>
    public ReadOnlySpan<byte> Transparency { get; }
    /// <summary>CD+G: as <see cref="CdgFrame.PaletteRevision"/>.</summary>
    public ulong PaletteRevision { get; }
}

/// <summary>
/// What every karaoke surface can do (<see cref="CdgSurface"/>; the MIDI lyric surface will be one too): draw into a
/// buffer of yours, such as a mapped upload buffer, drawing only what changed since the frame that buffer holds.
/// One thread at a time per surface.
/// </summary>
/// <example>
/// Three buffers in rotation:
/// <code>
/// var layout = surface.GetLayout();
/// int stride = layout.Width * 4;
/// var buffers = new byte[3][];
/// var revisions = new ulong[3];
/// for (int i = 0; i &lt; 3; i++) buffers[i] = new byte[stride * layout.Height];
/// // every frame, with buffer b:
/// var update = surface.UpdateInto(buffers[b], stride, layout.Width, layout.Height, revisions[b]);
/// revisions[b] = update.Revision;
/// foreach (var r in update.Dirty) UploadRect(buffers[b], r);
/// </code>
/// </example>
public interface IKaraokeSurface : IDisposable
{
    /// <summary>The size of the area the surface is shown in, in physical pixels, for the scaled presentations (CD+G
    /// presentation options, lyrics); <see cref="PixelSize.Unset"/> by default. The shape is kept and the size brought
    /// into 300x216 .. 3840x2160; the getter returns the size in use. The classic CD+G picture ignores it.</summary>
    PixelSize PresentationSize { get; set; }

    /// <summary>Off by default. With it on, surfaces that render into a bitmap of their own render the next frames ahead on
    /// the engine's workers. The classic CD+G picture is drawn straight into your buffer and ignores it.</summary>
    bool RenderAhead { get; set; }

    /// <summary>The buffer size to draw into, and where the picture goes.</summary>
    SurfaceLayout GetLayout();

    /// <summary>
    /// Draws the picture into <paramref name="destination"/>: only what changed since <paramref name="destinationRevision"/>
    /// when that is one of the last 8 revisions, else all of it. The rest of the buffer is not touched, so it must still
    /// hold that frame.
    /// </summary>
    /// <param name="destination">The first pixel of the buffer.</param>
    /// <param name="stride">Bytes per row, at least a row of pixels.</param>
    /// <param name="width">Must be <see cref="SurfaceLayout.Width"/>.</param>
    /// <param name="height">Must be <see cref="SurfaceLayout.Height"/>.</param>
    /// <param name="destinationRevision">The <see cref="SurfaceUpdate.Revision"/> the buffer holds; 0 = unknown.</param>
    /// <param name="time">Position on the stream's timeline; null = the audio heard now.</param>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.SizeMismatch"/> when the size is not the layout's.</exception>
    /// <exception cref="ObjectDisposedException">The surface or its stream was disposed.</exception>
    SurfaceUpdate UpdateInto(nint destination, int stride, int width, int height, ulong destinationRevision, TimeSpan? time = null);

    /// <summary>As <see cref="UpdateInto(nint, int, int, int, ulong, TimeSpan?)"/>, into a span that holds the whole buffer.</summary>
    SurfaceUpdate UpdateInto(Span<byte> destination, int stride, int width, int height, ulong destinationRevision, TimeSpan? time = null);
}
