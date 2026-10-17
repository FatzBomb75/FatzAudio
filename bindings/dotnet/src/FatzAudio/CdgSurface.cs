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

    internal CdgFrame(FaCdgFrame f, ReadOnlySpan<CdgColor> palette)
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
/// One surface is updated from one thread at a time; different surfaces may be used from different threads. Disposing
/// the stream frees its surfaces: <see cref="Update"/> then throws <see cref="ObjectDisposedException"/>.
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
public sealed unsafe class CdgSurface : IDisposable
{
    private readonly AudioStream _stream;
    private readonly object _lock = new();
    private readonly CdgColor[] _palette = new CdgColor[16];
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
                  (options.Premultiplied ? 4u : 0) | (options.Smooth ? 8u : 0);
        var handle = stream.Handle;
        int rc = NativeMethods.fa_cdg_surface_create(handle, &c, out _native);
        if (rc == (int)ErrorCode.InvalidArgument)
            throw new ArgumentException("Unsupported CD+G surface options (pixel format or size above 8192).", nameof(options));
        FatzAudioException.ThrowIfFailed(rc);
        handle.TrackSurface(this);
    }

    /// <summary>The options the surface was created with.</summary>
    public CdgSurfaceOptions Options { get; }

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
    /// rendering to a video file). <see cref="AudioStream.CdgOffset"/> still applies.</summary>
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
                _palette[i] = new CdgColor(f.Palette[i * 4], f.Palette[i * 4 + 1], f.Palette[i * 4 + 2], f.Palette[i * 4 + 3]);
            return new CdgFrame(f, _palette);
        }
        finally
        {
            if (added) handle.DangerousRelease();
        }
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
