using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Byte order of the pixels a <see cref="CdgSurface"/> produces.</summary>
public enum CdgPixelFormat
{
    /// <summary>B, G, R, A bytes: Direct3D/DXGI <c>B8G8R8A8</c>, WPF <c>Bgra32</c>/<c>Pbgra32</c>, SkiaSharp <c>Bgra8888</c>, GDI.</summary>
    Bgra32 = 0,
    /// <summary>R, G, B, A bytes: OpenGL/Vulkan/Metal <c>RGBA8</c>, Android <c>ARGB_8888</c> bitmaps.</summary>
    Rgba32 = 1,
    /// <summary>One byte per pixel, an index 0..15 into <see cref="CdgFrame.Palette"/>.</summary>
    Indexed8 = 2,
}

/// <summary>How a <see cref="CdgSurface"/> draws: pixel format, size and options.</summary>
public sealed record CdgSurfaceOptions
{
    /// <summary>Pixel format of <see cref="CdgFrame.Pixels"/> (default <see cref="CdgPixelFormat.Bgra32"/>).</summary>
    public CdgPixelFormat PixelFormat { get; init; } = CdgPixelFormat.Bgra32;
    /// <summary>Output width in pixels; 0 = native (300, or 288 with <see cref="HideBorder"/>). At most 8192.</summary>
    public int Width { get; init; }
    /// <summary>Output height in pixels; 0 = native (216, or 192 with <see cref="HideBorder"/>). At most 8192.</summary>
    public int Height { get; init; }
    /// <summary>Show only the 288x192 picture area, without the border.</summary>
    public bool HideBorder { get; init; }
    /// <summary>
    /// The background colour (that of the last screen clear) is fully transparent, and so are colours the disc
    /// marks as transparent: for lyrics drawn over video.
    /// </summary>
    public bool Transparent { get; init; }
    /// <summary>Colour multiplied by alpha (WPF <c>Pbgra32</c>, Direct2D, most compositors); otherwise straight alpha.</summary>
    public bool Premultiplied { get; init; }
    /// <summary>Bilinear scaling instead of sharp pixels. Ignored for <see cref="CdgPixelFormat.Indexed8"/>.</summary>
    public bool Smooth { get; init; }
    /// <summary>
    /// Like <see cref="Transparent"/>, but only the background colour is transparent: the disc's transparent-colour
    /// packs are ignored (some discs set them in ways that punch holes in the lyrics). Wins over <see cref="Transparent"/>.
    /// </summary>
    public bool TransparentBackgroundOnly { get; init; }
    /// <summary>Every colour is fully transparent until the first screen clear, so a picture or video underneath
    /// shows instead of a black screen.</summary>
    public bool HideUntilPreset { get; init; }
    /// <summary>Replaces the background colour, alpha included; null keeps the disc's colour.</summary>
    public CdgColor? BackgroundColor { get; init; }
    /// <summary>How opaque the background is, 0..100 % (of 255, or of <see cref="BackgroundColor"/>'s alpha),
    /// whatever <see cref="Transparent"/> makes it; null leaves it to the other options.</summary>
    public int? BackgroundOpacity { get; init; }
}

/// <summary>
/// How a <see cref="CdgSurface"/> presents the picture, on top of its <see cref="CdgSurfaceOptions"/>: see
/// <see cref="CdgSurface.Presentation"/>. The defaults change nothing.
/// </summary>
/// <remarks>
/// <para>
/// With <see cref="Scale"/> off the surface keeps its pixel format, size and options, with the <see cref="SafetyZone"/>'s
/// crop and the <see cref="Padding"/> (transparent: alpha 0, or <see cref="CdgSurface.TransparentIndex"/> in
/// <see cref="CdgPixelFormat.Indexed8"/>).
/// </para>
/// <para>
/// With it on, the picture is drawn at the whole-number multiple nearest <see cref="IKaraokeSurface.PresentationSize"/>,
/// rounded up (1920x1080: <see cref="FatzAudio.SafetyZone.Show"/> is 5x, 1080 high; <see cref="FatzAudio.SafetyZone.Remove"/>
/// is 6x, 1152 high), as premultiplied colour (<see cref="CdgPixelFormat.Rgba32"/> with an RGBA surface,
/// <see cref="CdgPixelFormat.Bgra32"/> otherwise) on a transparent background; <see cref="SurfaceUpdate.Background"/> is
/// the colour to fill behind it. From the outside in: padding, a transparent margin of one CD+G pixel, the picture.
/// <see cref="CdgSurfaceOptions.Width"/>, <see cref="CdgSurfaceOptions.Height"/>, <see cref="CdgSurfaceOptions.Smooth"/>,
/// <see cref="CdgSurfaceOptions.Premultiplied"/> and the background options do not apply; the transparency options and
/// <see cref="CdgSurfaceOptions.HideUntilPreset"/> do.
/// </para>
/// </remarks>
public sealed record CdgPresentation
{
    /// <summary>Full colour at a whole-number multiple (off: the surface's own format and size).</summary>
    public bool Scale { get; init; }
    /// <summary>Which part of the screen is shown (default <see cref="FatzAudio.SafetyZone.Show"/>); replaces
    /// <see cref="CdgSurfaceOptions.HideBorder"/>.</summary>
    public SafetyZone SafetyZone { get; init; } = SafetyZone.Show;
    /// <summary><see cref="FatzAudio.SafetyZone.Auto"/>: drawing in the border before this time on the graphics timeline
    /// does not count (title cards). 0 .. 24 hours.</summary>
    public TimeSpan AutoIgnore { get; init; }
    /// <summary><see cref="FatzAudio.SafetyZone.Super"/>: the same crop on both sides.</summary>
    public bool SuperCentered { get; init; }
    /// <summary>CD+G pixels of transparent space on each side, 0..32.</summary>
    public int Padding { get; init; }
    /// <summary>0..10, with <see cref="Scale"/> only: 0 sharp pixels, 1-4 softer (4 = bilinear). 5-10 are planned and draw
    /// as 4 in this version.</summary>
    public int Smoothness { get; init; }
    /// <summary>With <see cref="Scale"/> only: anti-aliases stair steps along the edges between colours (morphological
    /// anti-aliasing), without blurring anything else.</summary>
    public bool EdgeBlend { get; init; }
    /// <summary>The border is transparent too; its colour is still reported in <see cref="SurfaceUpdate.BorderIndex"/>.</summary>
    public bool TransparentBorder { get; init; }
}

/// <summary>Where a stream's CD+G graphics come from.</summary>
public enum CdgSource
{
    /// <summary>The stream has no graphics (a disc track until its graphics are found).</summary>
    None = 0,
    /// <summary>A <c>.cdg</c> file, a ZIP entry or bytes loaded with <see cref="AudioStream.LoadCdg(ReadOnlySpan{byte})"/>.</summary>
    File = 1,
    /// <summary>A CD+G disc's subcode, read while the track plays.</summary>
    Disc = 2,
}

/// <summary>A stream's CD+G graphics: see <see cref="AudioStream.CdgInfo"/>.</summary>
/// <param name="Source">Where they come from.</param>
/// <param name="PackCount">Packs, 300 per second (a disc track's whole length).</param>
/// <param name="CheckpointInterval">A seek replays at most this much (10 s, longer for graphics over 30 minutes).</param>
/// <param name="CheckpointCount">Checkpoints built so far. Files: all of them once the stream's first surface exists.
/// Discs: as the packs arrive.</param>
/// <param name="DiscParity">Discs: true when the packs' parity chose the subcode layout, false when instruction counts
/// did (drives that zero the parity). When true, a pack with one damaged symbol is corrected and one with more is
/// dropped; when false the packs are applied as read. Null for other sources.</param>
public readonly record struct CdgInfo(CdgSource Source, long PackCount, TimeSpan CheckpointInterval, int CheckpointCount, bool? DiscParity);

/// <summary>What changed in the CD+G picture: see <see cref="AudioStream.CdgChanged"/>.</summary>
public sealed class CdgChangedEventArgs : EventArgs
{
    internal CdgChangedEventArgs(CdgRect area, bool paletteChanged, long packs)
    {
        Area = area;
        PaletteChanged = paletteChanged;
        Packs = packs;
    }

    /// <summary>The changed area in native 300x216 screen pixels (a surface's <see cref="CdgFrame.Dirty"/> is in its
    /// own pixels). Empty when only colours changed.</summary>
    public CdgRect Area { get; }
    /// <summary>True when colours or transparency changed.</summary>
    public bool PaletteChanged { get; }
    /// <summary>The number of CD+G packs now shown (300 per second).</summary>
    public long Packs { get; }
}

/// <summary>A palette colour of a CD+G picture, straight (not premultiplied) alpha.</summary>
/// <param name="R">Red, 0..255.</param>
/// <param name="G">Green, 0..255.</param>
/// <param name="B">Blue, 0..255.</param>
/// <param name="A">Alpha, 0 (transparent) .. 255 (opaque).</param>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct CdgColor(byte R, byte G, byte B, byte A);

/// <summary>A rectangle of output pixels.</summary>
/// <param name="X">Left edge.</param>
/// <param name="Y">Top edge.</param>
/// <param name="Width">Width; 0 when empty.</param>
/// <param name="Height">Height; 0 when empty.</param>
public readonly record struct CdgRect(int X, int Y, int Width, int Height)
{
    /// <summary>True when the rectangle covers no pixel.</summary>
    public bool IsEmpty => Width <= 0 || Height <= 0;
}

/// <summary>
/// One picture from <see cref="CdgSurface.Update"/>. The pixels belong to the surface: they stay valid and unchanged
/// until the next update of that surface or its disposal, so copy them to your renderer before then.
/// </summary>
public readonly unsafe ref struct CdgFrame
{
    private readonly byte* _pixels;

    internal CdgFrame(FaCdgFrame f, ReadOnlySpan<CdgColor> palette, ReadOnlySpan<byte> transparency)
    {
        _pixels = (byte*)f.Pixels;
        Width = (int)f.Width;
        Height = (int)f.Height;
        Stride = f.Stride;
        PixelFormat = (CdgPixelFormat)f.PixelFormat;
        Changed = f.Changed != 0;
        PaletteChanged = f.PaletteChanged != 0;
        Dirty = new CdgRect((int)f.DirtyX, (int)f.DirtyY, (int)f.DirtyWidth, (int)f.DirtyHeight);
        Palette = palette;
        Time = TimeConvert.FromNanoseconds(f.Time);
        BackgroundIndex = f.BackgroundIndex;
        BorderIndex = f.BorderIndex;
        Transparency = transparency;
        Background = new CdgColor(f.Background[0], f.Background[1], f.Background[2], f.Background[3]);
        Revision = f.Revision;
        PaletteRevision = f.PaletteRevision;
    }

    /// <summary>All pixels, <see cref="Height"/> rows of <see cref="Stride"/> bytes.</summary>
    public ReadOnlySpan<byte> Pixels => new(_pixels, Stride * Height);
    /// <summary>Address of the first pixel, for APIs that take a pointer (WPF <c>WritePixels</c>, texture updates).</summary>
    public nint PixelPointer => (nint)_pixels;
    /// <summary>Width in pixels.</summary>
    public int Width { get; }
    /// <summary>Height in pixels.</summary>
    public int Height { get; }
    /// <summary>Bytes from one row to the next.</summary>
    public int Stride { get; }
    /// <summary>Byte layout of <see cref="Pixels"/>.</summary>
    public CdgPixelFormat PixelFormat { get; }
    /// <summary>True when the pixels or the palette differ from the previous update (always true for the first one).</summary>
    public bool Changed { get; }
    /// <summary>True when <see cref="Palette"/> differs from the previous update. For <see cref="CdgPixelFormat.Indexed8"/>
    /// this alone changes the colours on screen, with an empty <see cref="Dirty"/> rectangle.</summary>
    public bool PaletteChanged { get; }
    /// <summary>The pixels that changed, in output pixels; upload just this. Empty when no pixel changed.</summary>
    public CdgRect Dirty { get; }
    /// <summary>The 16 colours of the picture; for <see cref="CdgPixelFormat.Indexed8"/> pixels.</summary>
    public ReadOnlySpan<CdgColor> Palette { get; }
    /// <summary>Position on the graphics timeline that is shown (the audio clock minus <see cref="AudioStream.CdgOffset"/>).</summary>
    public TimeSpan Time { get; }
    /// <summary>Colour index of the last screen clear (memory preset); -1 before the first one, or without graphics.
    /// For apps that apply their own transparency to <see cref="CdgPixelFormat.Indexed8"/> pixels.</summary>
    public int BackgroundIndex { get; }
    /// <summary>Colour index of the last border preset; -1 before one.</summary>
    public int BorderIndex { get; }
    /// <summary>The disc's transparent-colour values for colours 0..15: 0 opaque .. 63 invisible, whatever the options.</summary>
    public ReadOnlySpan<byte> Transparency { get; }
    /// <summary>The colour the background is drawn with (after <see cref="CdgSurfaceOptions.BackgroundColor"/>), to fill
    /// the area around a scaled picture. All zero while <see cref="BackgroundIndex"/> is -1.</summary>
    public CdgColor Background { get; }
    /// <summary>Goes up whenever an update changes a pixel (by more than one when <see cref="CdgSurface.UpdateInto(nint, int, int, int, ulong, TimeSpan?)"/>
    /// drew changes in between). Each render target compares it with the revision it
    /// last uploaded, so one surface can feed several targets.</summary>
    public ulong Revision { get; }
    /// <summary>Goes up whenever <see cref="Palette"/>, <see cref="BackgroundIndex"/>, <see cref="BorderIndex"/> or
    /// <see cref="Transparency"/> change.</summary>
    public ulong PaletteRevision { get; }
}

/// <summary>
/// Draws a stream's CD+G karaoke graphics in sync with its audio. Create it with
/// <see cref="AudioStream.CreateCdgSurface"/> and call <see cref="Update"/> from your render loop (each vsync or a timer):
/// it draws the picture for the audio heard right now and says whether and where it changed.
/// </summary>
/// <remarks>
/// <para>
/// Pausing, seeking, looping, crossfades and tempo changes need no extra code: the picture always follows the stream's
/// <see cref="AudioStream.Clock"/>. A stream without graphics gives a blank picture until graphics are loaded.
/// </para>
/// <para>
/// One surface is updated from one thread at a time; different surfaces may be used from different threads. An update
/// never waits for the device or decode threads, and one with nothing new copies no pixels. Disposing the stream frees
/// its surfaces: <see cref="Update"/> then throws <see cref="ObjectDisposedException"/>.
/// </para>
/// </remarks>
/// <example>
/// WPF, from <c>CompositionTarget.Rendering</c>:
/// <code>
/// var cdg = song.CreateCdgSurface(new CdgSurfaceOptions { Premultiplied = true });
/// var bitmap = new WriteableBitmap(300, 216, 96, 96, PixelFormats.Pbgra32, null);
/// CompositionTarget.Rendering += (_, _) =>
/// {
///     var frame = cdg.Update();
///     if (!frame.Changed) return;
///     var d = frame.Dirty;
///     bitmap.WritePixels(new Int32Rect(d.X, d.Y, d.Width, d.Height),
///         frame.PixelPointer + d.Y * frame.Stride + d.X * 4, frame.Stride * frame.Height, frame.Stride);
/// };
/// </code>
/// </example>
public sealed unsafe class CdgSurface : IKaraokeSurface
{
    private readonly AudioStream _stream;
    private readonly object _lock = new();
    private readonly CdgColor[] _palette = new CdgColor[16];
    private readonly byte[] _transparency = new byte[16];
    private readonly PixelRect[] _dirty = new PixelRect[8];
    private PixelSize _presentationSize;
    private CdgPresentation? _presentation;
    private bool _renderAhead;
    private nint _native;

    internal CdgSurface(AudioStream stream, CdgSurfaceOptions options)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(options.Width);
        ArgumentOutOfRangeException.ThrowIfNegative(options.Height);
        _stream = stream;
        Options = options;
        FaCdgSurfaceConfig c;
        NativeMethods.fa_cdg_surface_config_init(&c);
        c.PixelFormat = (int)options.PixelFormat;
        c.Width = (uint)options.Width;
        c.Height = (uint)options.Height;
        c.Flags = (options.HideBorder ? 1u : 0) | (options.Transparent ? 2u : 0) |
                  (options.Premultiplied ? 4u : 0) | (options.Smooth ? 8u : 0) |
                  (options.TransparentBackgroundOnly ? 16u : 0) | (options.HideUntilPreset ? 32u : 0);
        if (options.BackgroundColor is { } bg)
        {
            c.Flags |= 64u;
            c.BackgroundColor = ((uint)bg.A << 24) | ((uint)bg.R << 16) | ((uint)bg.G << 8) | bg.B;
        }
        if (options.BackgroundOpacity is { } opacity)
        {
            ArgumentOutOfRangeException.ThrowIfNegative(opacity, nameof(options));
            ArgumentOutOfRangeException.ThrowIfGreaterThan(opacity, 100, nameof(options));
            c.Flags |= 128u;
            c.BackgroundOpacity = (uint)opacity;
        }
        var handle = stream.Handle;
        int rc = NativeMethods.fa_cdg_surface_create(handle, &c, out _native);
        if (rc == (int)ErrorCode.InvalidArgument)
            throw new ArgumentException("Unsupported CD+G surface options (pixel format or size above 8192).", nameof(options));
        FatzAudioException.ThrowIfFailed(rc);
        handle.TrackSurface(this);
    }

    /// <summary>The options the surface was created with.</summary>
    public CdgSurfaceOptions Options { get; }

    /// <summary>In <see cref="CdgPixelFormat.Indexed8"/> pixels: transparent (padding, and the border with
    /// <see cref="CdgPresentation.TransparentBorder"/>).</summary>
    public const byte TransparentIndex = 16;

    /// <summary>
    /// How the picture is presented: safety zone, padding, presentation scaling, smoothness, edge blend. Null (the default)
    /// is the picture of <see cref="Options"/>, as without presentation options. Takes effect at the next update or
    /// <see cref="GetLayout"/>; a change of size gives a new <see cref="SurfaceLayout.LayoutRevision"/>.
    /// </summary>
    /// <example>
    /// <code>
    /// cdg.Presentation = new CdgPresentation { Scale = true, SafetyZone = SafetyZone.Auto, Smoothness = 2, EdgeBlend = true };
    /// cdg.PresentationSize = new PixelSize(1920, 1080);
    /// var layout = cdg.GetLayout();   // allocate layout.Width x layout.Height, 4 bytes per pixel
    /// </code>
    /// </example>
    /// <exception cref="ArgumentOutOfRangeException">A value out of its range.</exception>
    public CdgPresentation? Presentation
    {
        get => _presentation;
        set
        {
            FaCdgPresentation p;
            NativeMethods.fa_cdg_presentation_init(&p);
            if (value is not null)
            {
                ArgumentOutOfRangeException.ThrowIfNegative(value.Padding, nameof(value));
                ArgumentOutOfRangeException.ThrowIfGreaterThan(value.Padding, 32, nameof(value));
                ArgumentOutOfRangeException.ThrowIfNegative(value.Smoothness, nameof(value));
                ArgumentOutOfRangeException.ThrowIfGreaterThan(value.Smoothness, 10, nameof(value));
                if (value.AutoIgnore < TimeSpan.Zero || value.AutoIgnore > TimeSpan.FromHours(24))
                    throw new ArgumentOutOfRangeException(nameof(value), "AutoIgnore must be 0 .. 24 hours.");
                if (!Enum.IsDefined(value.SafetyZone))
                    throw new ArgumentOutOfRangeException(nameof(value), "Unknown safety zone.");
                p.Scale = value.Scale ? 1 : 0;
                p.SafetyZone = (int)value.SafetyZone;
                p.AutoIgnoreSeconds = (float)value.AutoIgnore.TotalSeconds;
                p.SuperCentered = value.SuperCentered ? 1 : 0;
                p.Padding = (uint)value.Padding;
                p.Smoothness = value.Smoothness;
                p.EdgeBlend = value.EdgeBlend ? 1 : 0;
                p.Flags = value.TransparentBorder ? 1u : 0;
            }
            var h = Enter(out bool added);
            try
            {
                lock (_lock)
                {
                    ObjectDisposedException.ThrowIf(_native == 0, this);
                    FatzAudioException.ThrowIfFailed(
                        NativeMethods.fa_cdg_surface_set_presentation(_native, value is null ? null : &p));
                    _presentation = value;
                }
            }
            finally
            {
                if (added) h.DangerousRelease();
            }
        }
    }

    /// <summary>The stream whose graphics this surface draws.</summary>
    public AudioStream Stream => _stream;

    /// <summary>Called by the stream's handle right before the native stream is closed.</summary>
    internal void Orphan()
    {
        lock (_lock)
        {
            if (_native == 0) return;
            NativeMethods.fa_cdg_surface_destroy(_native);
            _native = 0;
        }
    }

    /// <summary>Draws the picture for the audio heard right now. Cheap when nothing changed: call it every frame and
    /// upload only when <see cref="CdgFrame.Changed"/>.</summary>
    /// <exception cref="ObjectDisposedException">The surface or its stream was disposed.</exception>
    public CdgFrame Update() => Render(null);

    /// <summary>Draws the picture at a position you choose on the stream's timeline (your own clock, a preview, or
    /// rendering to a video file). <see cref="AudioStream.CdgOffset"/> still applies. The picture shows packs
    /// [0, ceil(t × 300)) for t = <paramref name="time"/> minus the offset, in seconds: pack n counts once its slot starts.</summary>
    /// <param name="time">Position from the start of the stream.</param>
    /// <exception cref="ObjectDisposedException">The surface or its stream was disposed.</exception>
    public CdgFrame UpdateAt(TimeSpan time) => Render(TimeConvert.ToNanoseconds(time));

    private CdgFrame Render(long? time)
    {
        var handle = _stream.Handle;
        bool added = false;
        // Holding a reference keeps the native stream (and so this surface) alive during the call.
        handle.DangerousAddRef(ref added);
        try
        {
            FaCdgFrame f;
            lock (_lock)
            {
                ObjectDisposedException.ThrowIf(_native == 0, this);
                int rc = time is { } t
                    ? NativeMethods.fa_cdg_surface_update_at(_native, t, &f)
                    : NativeMethods.fa_cdg_surface_update(_native, &f);
                FatzAudioException.ThrowIfFailed(rc);
            }
            for (int i = 0; i < 16; i++)
            {
                _palette[i] = new CdgColor(f.Palette[i * 4], f.Palette[i * 4 + 1], f.Palette[i * 4 + 2], f.Palette[i * 4 + 3]);
                _transparency[i] = f.Transparency[i];
            }
            return new CdgFrame(f, _palette, _transparency);
        }
        finally
        {
            if (added) handle.DangerousRelease();
        }
    }

    /// <inheritdoc/>
    public PixelSize PresentationSize
    {
        get => _presentationSize;
        set
        {
            ArgumentOutOfRangeException.ThrowIfNegative(value.Width, nameof(value));
            ArgumentOutOfRangeException.ThrowIfNegative(value.Height, nameof(value));
            if ((value.Width == 0) != (value.Height == 0))
                throw new ArgumentException("Width and height must both be 0 (unset) or both positive.", nameof(value));
            var h = Enter(out bool added);
            try
            {
                lock (_lock)
                {
                    ObjectDisposedException.ThrowIf(_native == 0, this);
                    FatzAudioException.ThrowIfFailed(
                        NativeMethods.fa_surface_set_presentation_size(_native, (uint)value.Width, (uint)value.Height));
                }
            }
            finally
            {
                if (added) h.DangerousRelease();
            }
            _presentationSize = ClampPresentation(value);
        }
    }

    /// <inheritdoc/>
    public bool RenderAhead
    {
        get => _renderAhead;
        set
        {
            var h = Enter(out bool added);
            try
            {
                lock (_lock)
                {
                    ObjectDisposedException.ThrowIf(_native == 0, this);
                    FatzAudioException.ThrowIfFailed(NativeMethods.fa_surface_set_render_ahead(_native, value ? 1 : 0));
                }
            }
            finally
            {
                if (added) h.DangerousRelease();
            }
            _renderAhead = value;
        }
    }

    /// <inheritdoc/>
    public SurfaceLayout GetLayout()
    {
        FaSurfaceLayout l = default;
        l.StructSize = (uint)sizeof(FaSurfaceLayout);
        var h = Enter(out bool added);
        try
        {
            lock (_lock)
            {
                ObjectDisposedException.ThrowIf(_native == 0, this);
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_surface_get_layout(_native, &l));
            }
        }
        finally
        {
            if (added) h.DangerousRelease();
        }
        string font = Marshal.PtrToStringUTF8((nint)l.FontUsed) ?? "";
        return new SurfaceLayout((int)l.Width, (int)l.Height, l.BitmapX, l.BitmapY,
            new PixelRect((int)l.ContentX, (int)l.ContentY, (int)l.ContentWidth, (int)l.ContentHeight), l.Scale,
            new PixelRect((int)l.CropX, (int)l.CropY, (int)l.CropWidth, (int)l.CropHeight), (SafetyZone)l.SafetyZone,
            l.SafetyZoneResolved != 0, font, (int)l.Margin, (int)l.Padding, l.LayoutRevision);
    }

    /// <inheritdoc/>
    public SurfaceUpdate UpdateInto(nint destination, int stride, int width, int height, ulong destinationRevision,
        TimeSpan? time = null)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(width);
        ArgumentOutOfRangeException.ThrowIfNegative(height);
        if (destination == 0) throw new ArgumentNullException(nameof(destination));
        FaSurfaceFrame f = default;
        f.StructSize = (uint)sizeof(FaSurfaceFrame);
        long t = time is { } at ? TimeConvert.ToNanoseconds(at) : long.MinValue;
        var h = Enter(out bool added);
        try
        {
            lock (_lock)
            {
                ObjectDisposedException.ThrowIf(_native == 0, this);
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_surface_update_into(_native, t, (void*)destination, stride,
                    (uint)width, (uint)height, destinationRevision, &f));
            }
        }
        finally
        {
            if (added) h.DangerousRelease();
        }
        int count = (int)Math.Min(f.DirtyCount, 8u);
        for (int i = 0; i < count; i++)
            _dirty[i] = new PixelRect(f.Dirty[i * 4], f.Dirty[i * 4 + 1], f.Dirty[i * 4 + 2], f.Dirty[i * 4 + 3]);
        for (int i = 0; i < 16; i++)
        {
            _palette[i] = new CdgColor(f.Palette[i * 4], f.Palette[i * 4 + 1], f.Palette[i * 4 + 2], f.Palette[i * 4 + 3]);
            _transparency[i] = f.Transparency[i];
        }
        var background = new CdgColor(f.Background[0], f.Background[1], f.Background[2], f.Background[3]);
        return new SurfaceUpdate(f, _dirty.AsSpan(0, count), _palette, _transparency, background);
    }

    /// <inheritdoc/>
    public SurfaceUpdate UpdateInto(Span<byte> destination, int stride, int width, int height, ulong destinationRevision,
        TimeSpan? time = null)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(height);
        // Under the lock (taken again inside, which Monitor allows), so another thread can't switch scaling on
        // between this check and the draw.
        lock (_lock)
        {
            int bpp = Options.PixelFormat == CdgPixelFormat.Indexed8 && _presentation?.Scale != true ? 1 : 4;
            if (stride < width * bpp || destination.Length < (long)stride * (height - 1) + (long)width * bpp)
                throw new ArgumentException("The span is smaller than height rows of stride bytes.", nameof(destination));
            fixed (byte* p = destination)
                return UpdateInto((nint)p, stride, width, height, destinationRevision, time);
        }
    }

    /// <summary>The native rule: the shape kept, inside 300x216 .. 3840x2160.</summary>
    private static PixelSize ClampPresentation(PixelSize size)
    {
        if (size.IsUnset) return size;
        double w = size.Width, h = size.Height;
        double k = Math.Min(3840 / w, 2160 / h);
        if (k < 1) { w *= k; h *= k; }
        k = Math.Max(300 / w, 216 / h);
        if (k > 1) { w *= k; h *= k; }
        return new PixelSize(Math.Clamp((int)(w + 0.5), 300, 3840), Math.Clamp((int)(h + 0.5), 216, 2160));
    }

    /// <summary>Holds the stream for a native call, which keeps the native stream (and so this surface) alive.</summary>
    private StreamHandle Enter(out bool added)
    {
        var handle = _stream.Handle;
        added = false;
        handle.DangerousAddRef(ref added);
        return handle;
    }

    /// <summary>Frees the surface. Safe to call more than once, and after the stream was disposed.</summary>
    public void Dispose()
    {
        StreamHandle handle;
        bool added = false;
        try
        {
            handle = _stream.Handle;
            handle.DangerousAddRef(ref added);
        }
        catch (ObjectDisposedException)
        {
            // The stream is closing: its handle destroys the surfaces it still tracks (Orphan).
            return;
        }
        try
        {
            lock (_lock)
            {
                if (_native == 0) return;
                NativeMethods.fa_cdg_surface_destroy(_native);
                _native = 0;
            }
            handle.UntrackSurface(this);
        }
        finally
        {
            if (added) handle.DangerousRelease();
        }
    }
}
