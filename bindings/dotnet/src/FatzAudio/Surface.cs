using System.Runtime.InteropServices;
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
    /// <summary><see cref="Remove"/>, unless the song draws in the border area (tiles or scrolls; a border preset's
    /// solid colour does not count): then <see cref="Show"/>. Worked out from the whole song before the first
    /// picture; on a disc it can't be, and <see cref="Show"/> is used (<see cref="SurfaceLayout.SafetyZoneResolved"/>
    /// false).</summary>
    Auto,
    /// <summary>The box around everything the song ever draws other than the background, on the 6x12 tile grid, fixed
    /// for the whole song. Like <see cref="Auto"/>, <see cref="Show"/> on a disc.</summary>
    Super,
}

/// <summary>What a surface draws: the buffer <see cref="IKaraokeSurface.UpdateInto(nint, int, int, int, ulong, TimeSpan?)"/>
/// wants and where it goes (<see cref="IKaraokeSurface.GetLayout"/>).</summary>
/// <param name="Width">Width of the buffer to draw into, in pixels.</param>
/// <param name="Height">Height of the buffer, in pixels.</param>
/// <param name="BitmapX">Left edge of the buffer inside the presentation area (0 without a presentation size).</param>
/// <param name="BitmapY">Top edge of the buffer inside the presentation area.</param>
/// <param name="Content">The picture inside the buffer, without padding and margin.</param>
/// <param name="Scale">CD+G: the whole-number multiple drawn with <see cref="CdgPresentation.Scale"/>; 1 without it,
/// whatever the output size.</param>
/// <param name="Crop">CD+G: the part of the 300x216 screen shown, in CD+G pixels.</param>
/// <param name="SafetyZone">CD+G: the zone in use (<see cref="FatzAudio.SafetyZone.Auto"/> becomes
/// <see cref="FatzAudio.SafetyZone.Show"/> or <see cref="FatzAudio.SafetyZone.Remove"/>).</param>
/// <param name="SafetyZoneResolved">False when <see cref="FatzAudio.SafetyZone.Auto"/> or
/// <see cref="FatzAudio.SafetyZone.Super"/> could not look at the song (a disc, or no graphics yet) and
/// <see cref="FatzAudio.SafetyZone.Show"/> is used.</param>
/// <param name="FontUsed">Lyrics: the font family drawn with; empty for CD+G.</param>
/// <param name="Margin">In buffer pixels, on each side: the transparent margin of presentation scaling (one CD+G
/// pixel); 0 without it.</param>
/// <param name="Padding">In buffer pixels, on each side: <see cref="CdgPresentation.Padding"/>.</param>
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
    /// <summary>Render-ahead: true when the frame shown is more than 1/60 s older than the time asked for, of a later
    /// position (after a seek back) or of graphics since replaced; <see cref="Time"/> is the position it shows. A newer
    /// frame follows. Always false without render-ahead.</summary>
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
/// An outline around the shape: a true stroke, round and even at any width. See <see cref="SurfaceEffects"/>.
/// </summary>
public sealed record OutlineEffect
{
    /// <summary>The outline's colour (straight alpha).</summary>
    public CdgColor Color { get; init; } = new(0, 0, 0, 255);
    /// <summary>Lyrics: the colour once sung. CD+G ignores it.</summary>
    public CdgColor SungColor { get; init; } = new(0, 0, 0, 255);
    /// <summary>0..1, multiplies the colour's alpha.</summary>
    public float Opacity { get; init; } = 1.0f;
    /// <summary>0..1: the width, 1 = 1.5 CD+G pixels (lyrics: 0.12 em).</summary>
    public float Size { get; init; } = 0.5f;
    /// <summary>0..1: the part of the width that eases out, 0 a hard edge, 1 a glow.</summary>
    public float Fade { get; init; }
}

/// <summary>A shadow of the shape and its outline. See <see cref="SurfaceEffects"/>.</summary>
public sealed record ShadowEffect
{
    /// <summary>The shadow's colour (straight alpha).</summary>
    public CdgColor Color { get; init; } = new(0, 0, 0, 255);
    /// <summary>Lyrics: the colour once sung. CD+G ignores it.</summary>
    public CdgColor SungColor { get; init; } = new(0, 0, 0, 255);
    /// <summary>0..1, multiplies the colour's alpha.</summary>
    public float Opacity { get; init; } = 0.6f;
    /// <summary>0..1: how far it spreads beyond the shape and outline, 1 = 2 CD+G pixels (lyrics: 0.20 em).</summary>
    public float Size { get; init; }
    /// <summary>0..1: how far away it falls, 1 = 2 CD+G pixels (lyrics: 0.15 em).</summary>
    public float Distance { get; init; } = 0.5f;
    /// <summary>Where it falls, degrees counter-clockwise from the right (315 = down right).</summary>
    public float AngleDegrees { get; init; } = 315.0f;
    /// <summary>0..1: the part of <see cref="Size"/> that eases out (a soft shadow).</summary>
    public float Fade { get; init; }
}

/// <summary>A bevel lit from one side. See <see cref="SurfaceEffects"/>.</summary>
public sealed record BevelEffect
{
    /// <summary>On the sides facing the light.</summary>
    public CdgColor Highlight { get; init; } = new(255, 255, 255, 128);
    /// <summary>On the sides facing away.</summary>
    public CdgColor Shade { get; init; } = new(0, 0, 0, 128);
    /// <summary>0..1.</summary>
    public float Strength { get; init; } = 0.5f;
    /// <summary>0..1: how far in from the edge, 1 = 1 CD+G pixel (lyrics: 0.08 em).</summary>
    public float Size { get; init; } = 0.5f;
    /// <summary>Where the light comes from, degrees counter-clockwise from the right (135 = top left).</summary>
    public float LightAngleDegrees { get; init; } = 135.0f;
    /// <summary>0..1: the part of <see cref="Size"/> that eases out.</summary>
    public float Fade { get; init; } = 0.5f;
}

/// <summary>
/// Outline, shadow and bevel around what a surface draws, from one distance field of its shape; drawn shadow first, then
/// the outline, the shape, and the bevel over it, premultiplied against transparent. A null effect is off. CD+G: the shape
/// is everything but the background, and the effects apply with <see cref="CdgPresentation.Scale"/> on (the classic
/// picture ignores them); they are cut at the edge of the picture.
/// </summary>
public sealed record SurfaceEffects
{
    /// <summary>The outline, or null.</summary>
    public OutlineEffect? Outline { get; init; }
    /// <summary>The shadow, or null.</summary>
    public ShadowEffect? Shadow { get; init; }
    /// <summary>The bevel, or null.</summary>
    public BevelEffect? Bevel { get; init; }
}

/// <summary>
/// What every karaoke surface can do (<see cref="CdgSurface"/>, <see cref="LyricSurface"/>): draw into a
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
    /// <see cref="CdgPresentation.Scale"/>, lyrics); <see cref="PixelSize.Unset"/> by default. The shape is kept and the
    /// size brought into 300x216 .. 3840x2160; the getter returns the size in use. A CD+G picture without presentation
    /// scaling ignores it.</summary>
    PixelSize PresentationSize { get; set; }

    /// <summary>Off by default. On, a surface that renders into a bitmap of its own (CD+G with
    /// <see cref="CdgPresentation.Scale"/>; lyrics) has the engine's workers render its next frame ahead, and
    /// <see cref="UpdateInto(nint, int, int, int, ulong, TimeSpan?)"/> copies the finished one that fits best without
    /// waiting for a worker (<see cref="SurfaceUpdate.Behind"/> says when it is late); with no worker busy and no fitting
    /// frame it draws one itself. Takes two more buffers of the layout's size; the classic
    /// CD+G picture ignores it.</summary>
    bool RenderAhead { get; set; }

    /// <summary>Outline, shadow and bevel; null (the default) for none. The next frame is drawn again in full.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A value out of its range (sizes, opacities, strength and fades are
    /// 0..1; angles must be finite).</exception>
    SurfaceEffects? Effects { get; set; }

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

/// <summary>Conversions every surface shares.</summary>
internal static unsafe class SurfaceInterop
{
    public static uint Argb(CdgColor c) => (uint)c.A << 24 | (uint)c.R << 16 | (uint)c.G << 8 | c.B;

    public static CdgColor FromArgb(uint v) => new((byte)(v >> 16), (byte)(v >> 8), (byte)v, (byte)(v >> 24));

    public static float Unit(float v) =>
        v is >= 0.0f and <= 1.0f ? v : throw new ArgumentOutOfRangeException("value", v, "Must be 0 .. 1.");

    public static float Angle(float v) =>
        float.IsFinite(v) ? v : throw new ArgumentOutOfRangeException("value", v, "Must be finite.");

    /// <summary>fa_surface_effects for <paramref name="value"/> (null: the defaults, all off).</summary>
    public static void ToNative(SurfaceEffects? value, FaSurfaceEffects* e)
    {
        NativeMethods.fa_surface_effects_init(e);
        if (value is null) return;
        if (value.Outline is { } o)
        {
            e->Outline.On = 1;
            e->Outline.Color = Argb(o.Color);
            e->Outline.SungColor = Argb(o.SungColor);
            e->Outline.Opacity = Unit(o.Opacity);
            e->Outline.Size = Unit(o.Size);
            e->Outline.Fade = Unit(o.Fade);
        }
        if (value.Shadow is { } s)
        {
            e->Shadow.On = 1;
            e->Shadow.Color = Argb(s.Color);
            e->Shadow.SungColor = Argb(s.SungColor);
            e->Shadow.Opacity = Unit(s.Opacity);
            e->Shadow.Size = Unit(s.Size);
            e->Shadow.Distance = Unit(s.Distance);
            e->Shadow.AngleDegrees = Angle(s.AngleDegrees);
            e->Shadow.Fade = Unit(s.Fade);
        }
        if (value.Bevel is { } b)
        {
            e->Bevel.On = 1;
            e->Bevel.Highlight = Argb(b.Highlight);
            e->Bevel.Shade = Argb(b.Shade);
            e->Bevel.Strength = Unit(b.Strength);
            e->Bevel.Size = Unit(b.Size);
            e->Bevel.LightAngleDegrees = Angle(b.LightAngleDegrees);
            e->Bevel.Fade = Unit(b.Fade);
        }
    }

    /// <summary>The effects that are on; null when none is.</summary>
    public static SurfaceEffects? FromNative(in FaSurfaceEffects e)
    {
        if (e.Outline.On == 0 && e.Shadow.On == 0 && e.Bevel.On == 0) return null;
        return new SurfaceEffects
        {
            Outline = e.Outline.On == 0 ? null : new OutlineEffect
            {
                Color = FromArgb(e.Outline.Color), SungColor = FromArgb(e.Outline.SungColor), Opacity = e.Outline.Opacity,
                Size = e.Outline.Size, Fade = e.Outline.Fade,
            },
            Shadow = e.Shadow.On == 0 ? null : new ShadowEffect
            {
                Color = FromArgb(e.Shadow.Color), SungColor = FromArgb(e.Shadow.SungColor), Opacity = e.Shadow.Opacity,
                Size = e.Shadow.Size, Distance = e.Shadow.Distance, AngleDegrees = e.Shadow.AngleDegrees,
                Fade = e.Shadow.Fade,
            },
            Bevel = e.Bevel.On == 0 ? null : new BevelEffect
            {
                Highlight = FromArgb(e.Bevel.Highlight), Shade = FromArgb(e.Bevel.Shade), Strength = e.Bevel.Strength,
                Size = e.Bevel.Size, LightAngleDegrees = e.Bevel.LightAngleDegrees, Fade = e.Bevel.Fade,
            },
        };
    }

    public static SurfaceLayout FromNative(FaSurfaceLayout* l)
    {
        string font = NativeText.Read(l->FontUsed);
        return new SurfaceLayout((int)l->Width, (int)l->Height, l->BitmapX, l->BitmapY,
            new PixelRect((int)l->ContentX, (int)l->ContentY, (int)l->ContentWidth, (int)l->ContentHeight), l->Scale,
            new PixelRect((int)l->CropX, (int)l->CropY, (int)l->CropWidth, (int)l->CropHeight), (SafetyZone)l->SafetyZone,
            l->SafetyZoneResolved != 0, font, (int)l->Margin, (int)l->Padding, l->LayoutRevision);
    }

    /// <summary>The native rule: the shape kept, inside 300x216 .. 3840x2160.</summary>
    public static PixelSize ClampPresentation(PixelSize size)
    {
        if (size.IsUnset) return size;
        double w = size.Width, h = size.Height;
        double k = Math.Min(3840 / w, 2160 / h);
        if (k < 1) { w *= k; h *= k; }
        k = Math.Max(300 / w, 216 / h);
        if (k > 1) { w *= k; h *= k; }
        return new PixelSize(Math.Clamp((int)(w + 0.5), 300, 3840), Math.Clamp((int)(h + 0.5), 216, 2160));
    }
}
