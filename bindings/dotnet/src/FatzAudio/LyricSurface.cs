using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>A starting point for <see cref="LyricStyle"/>.</summary>
public enum LyricPreset
{
    /// <summary>MediaControl's look: white, sung yellow, a black outline and shadow, a cross-fading wipe.</summary>
    Classic = 0,
    /// <summary>A sweeping wipe and a soft shadow.</summary>
    Modern,
    /// <summary>Glowing outlines and a sweeping wipe.</summary>
    Neon,
    /// <summary>One line above and below, a light shadow, no countdown.</summary>
    Minimal,
    /// <summary>Two lines in a band at the bottom on a panel, no countdown.</summary>
    Subtitle,
}

/// <summary>How a syllable turns to the sung colour.</summary>
public enum LyricWipe
{
    /// <summary>The syllable changes colour as a whole.</summary>
    Crossfade = 0,
    /// <summary>An edge sweeps across the syllable.</summary>
    Sweep,
}

/// <summary>The countdown before a line that follows a long gap.</summary>
public enum LyricCountdown
{
    /// <summary>None.</summary>
    Off = 0,
    /// <summary>An arrow each <see cref="LyricStyle.CountdownDuration"/> / (count + 1).</summary>
    Clock,
    /// <summary>An arrow a beat, from the song's tempo.</summary>
    Beat,
}

/// <summary>
/// How a <see cref="LyricSurface"/> lays out and draws a song's lyrics. "em" is <see cref="FontSize"/>; sizes are for a
/// presentation area 1080 pixels high and scale with its height. Times are real time: the animations keep their length
/// whatever <see cref="AudioStream.Tempo"/>. Start from <see cref="FromPreset"/> (a new <see cref="LyricStyle"/> is
/// <see cref="LyricPreset.Classic"/>) and change what you need with <c>with</c>.
/// </summary>
/// <example>
/// <code>
/// var style = LyricStyle.FromPreset(LyricPreset.Modern) with { FontFamily = "Segoe UI", SungColor = new(255, 80, 200, 255) };
/// </code>
/// </example>
public sealed record LyricStyle
{
    /// <summary>The <see cref="LyricPreset.Classic"/> style.</summary>
    public LyricStyle() : this(Preset(LyricPreset.Classic)) { }

    /// <summary>The style of a preset (the system's UI font).</summary>
    public static LyricStyle FromPreset(LyricPreset preset) =>
        Enum.IsDefined(preset) ? new LyricStyle(Preset(preset)) : throw new ArgumentOutOfRangeException(nameof(preset));

    private static unsafe FaLyricStyle Preset(LyricPreset preset)
    {
        FaLyricStyle s;
        NativeMethods.fa_lyric_style_init(&s, (int)preset);
        return s;
    }

    // ---- the font ----

    /// <summary>An installed family (as <c>fa_font_find</c>); null with no <see cref="FontPath"/> or
    /// <see cref="FontData"/> = the system's UI font, which is also used when the family is not installed.</summary>
    public string? FontFamily { get; init; }
    /// <summary>A font file (TrueType, OpenType, a collection); wins over <see cref="FontFamily"/>.</summary>
    public string? FontPath { get; init; }
    /// <summary>A font file in memory (copied); wins over the other two. Null or empty: not used.</summary>
    public ReadOnlyMemory<byte>? FontData { get; init; }
    /// <summary>The face of a collection (.ttc).</summary>
    public int FontFace { get; init; }
    /// <summary>False: a song whose text the font mostly cannot show (Japanese, Korean, Chinese) is drawn with the
    /// system's font for its script. True: always the font given.</summary>
    public bool FontExplicit { get; init; }
    /// <summary>Pixels at 1080, 6..240.</summary>
    public float FontSize { get; init; }
    /// <summary>100..900. A family without a face that heavy gets a synthetic bold.</summary>
    public int FontWeight { get; init; }
    /// <summary>Unsung text.</summary>
    public CdgColor TextColor { get; init; }
    /// <summary>Sung text.</summary>
    public CdgColor SungColor { get; init; }

    // ---- placement ----

    /// <summary>The active line's centre, 0 top .. 1 bottom.</summary>
    public float VerticalPosition { get; init; }
    /// <summary>The width lines fit in, of the area's width, 0.2..1; a wider line is split between words.</summary>
    public float BoxWidth { get; init; }
    /// <summary>Em between line centres, 0.8..4.</summary>
    public float LinePitch { get; init; }
    /// <summary>Lines shown above the active one, 0..2.</summary>
    public int LinesAbove { get; init; }
    /// <summary>Lines shown below it, 0..2.</summary>
    public int LinesBelow { get; init; }
    /// <summary>The opacity of the line just above.</summary>
    public float AboveOpacity1 { get; init; }
    /// <summary>The opacity of the line two above.</summary>
    public float AboveOpacity2 { get; init; }
    /// <summary>The opacity of the line below.</summary>
    public float NextLineOpacity { get; init; }
    /// <summary>The line two below: <see cref="NextLineOpacity"/> times this.</summary>
    public float AfterNextFactor { get; init; }
    /// <summary>The size of the lines one away, 0.2..1.</summary>
    public float NearScale { get; init; }
    /// <summary>The size of the lines two away, 0.2..1.</summary>
    public float FarScale { get; init; }

    // ---- the wipe ----

    /// <summary>How a syllable is coloured in.</summary>
    public LyricWipe Wipe { get; init; }
    /// <summary>A syllable's wipe takes <see cref="WipeShare"/> of the time to the next, at most this (0..2 s; 0 =
    /// instant).</summary>
    public TimeSpan WipeCap { get; init; }
    /// <summary>0.1..1.</summary>
    public float WipeShare { get; init; }
    /// <summary>Shorter wipes are instant (0..1 s).</summary>
    public TimeSpan InstantCut { get; init; }
    /// <summary>Syllables are coloured this early (0..2 s).</summary>
    public TimeSpan Lead { get; init; }

    // ---- lines and pages ----

    /// <summary>A pause this long starts a new page (0..60 s).</summary>
    public TimeSpan ParagraphGap { get; init; }
    /// <summary>A pause this long after ". ! ? : ;" starts a new line (0..60 s).</summary>
    public TimeSpan SentenceGap { get; init; }
    /// <summary>A line this many characters long breaks at the next space; 0 = no rule (0..1000).</summary>
    public int BreakLength { get; init; }
    /// <summary>A line scrolling up (0..4 s).</summary>
    public TimeSpan Scroll { get; init; }
    /// <summary>A page fading out or in (0..4 s).</summary>
    public TimeSpan PageFade { get; init; }
    /// <summary>A page stays this long after its last syllable when there is time (0..10 s).</summary>
    public TimeSpan PageHold { get; init; }
    /// <summary>A new page is in place this early (0..10 s).</summary>
    public TimeSpan PagePreview { get; init; }
    /// <summary>True: the fades may overlap, so new lyrics are never late. False: the new page waits for the old to
    /// go.</summary>
    public bool PageOverlap { get; init; }
    /// <summary>A new page grows in from this size (1..2).</summary>
    public float PageInScale { get; init; }
    /// <summary>An old page shrinks to this size (0.25..1).</summary>
    public float PageOutScale { get; init; }
    /// <summary>The lyrics fade out this long after the last syllable (0..60 s).</summary>
    public TimeSpan EndHold { get; init; }
    /// <summary>Over this (0..60 s).</summary>
    public TimeSpan EndFade { get; init; }

    // ---- the countdown ----

    /// <summary>The countdown before a line after a long gap.</summary>
    public LyricCountdown Countdown { get; init; }
    /// <summary>Arrows, 1..8.</summary>
    public int CountdownCount { get; init; }
    /// <summary><see cref="LyricCountdown.Clock"/>: the whole countdown, the arrows and a last fade (1..20 s).</summary>
    public TimeSpan CountdownDuration { get; init; }
    /// <summary>After a gap this long (0..60 s).</summary>
    public TimeSpan CountdownGap { get; init; }
    /// <summary>An arrow's colour before.</summary>
    public CdgColor CountdownFrom { get; init; }
    /// <summary>And after.</summary>
    public CdgColor CountdownTo { get; init; }
    /// <summary>Em, 0.2..1.5.</summary>
    public float CountdownSize { get; init; }

    // ---- a panel behind each line ----

    /// <summary>A rounded panel behind each line.</summary>
    public bool Panel { get; init; }
    /// <summary>Its colour; the alpha is its opacity.</summary>
    public CdgColor PanelColor { get; init; }
    /// <summary>Em, 0..1.</summary>
    public float PanelPadding { get; init; }
    /// <summary>Em, 0..1.</summary>
    public float PanelRadius { get; init; }
    /// <summary>No countdown.</summary>
    public bool SubtitleMode { get; init; }
    /// <summary>Outline, shadow and bevel, with sung colours; null for none. Sizes are in em: outline 0.12, shadow 0.20,
    /// distance 0.15, bevel 0.08 em at 1.</summary>
    public SurfaceEffects? Effects { get; init; }

    private unsafe LyricStyle(FaLyricStyle style)
    {
        var s = &style;
        FontFamily = s->FontFamily == 0 ? null : Marshal.PtrToStringUTF8(s->FontFamily);
        FontPath = s->FontPath == 0 ? null : Marshal.PtrToStringUTF8(s->FontPath);
        FontData = s->FontData == 0
            ? null
            : (ReadOnlyMemory<byte>?)new ReadOnlySpan<byte>((void*)s->FontData, checked((int)s->FontDataSize)).ToArray();
        FontFace = (int)s->FontFace;
        FontExplicit = s->FontExplicit != 0;
        FontSize = s->FontSize;
        FontWeight = s->FontWeight;
        TextColor = SurfaceInterop.FromArgb(s->TextColor);
        SungColor = SurfaceInterop.FromArgb(s->SungColor);
        VerticalPosition = s->VerticalPosition;
        BoxWidth = s->BoxWidth;
        LinePitch = s->LinePitch;
        LinesAbove = s->LinesAbove;
        LinesBelow = s->LinesBelow;
        AboveOpacity1 = s->AboveOpacity0;
        AboveOpacity2 = s->AboveOpacity1;
        NextLineOpacity = s->NextLineOpacity;
        AfterNextFactor = s->AfterNextFactor;
        NearScale = s->NearScale;
        FarScale = s->FarScale;
        Wipe = (LyricWipe)s->Wipe;
        WipeCap = TimeSpan.FromMilliseconds(s->WipeCapMs);
        WipeShare = s->WipeShare;
        InstantCut = TimeSpan.FromMilliseconds(s->InstantCutMs);
        Lead = TimeSpan.FromMilliseconds(s->LeadMs);
        ParagraphGap = TimeSpan.FromMilliseconds(s->ParagraphGapMs);
        SentenceGap = TimeSpan.FromMilliseconds(s->SentenceGapMs);
        BreakLength = s->BreakLength;
        Scroll = TimeSpan.FromMilliseconds(s->ScrollMs);
        PageFade = TimeSpan.FromMilliseconds(s->PageMs);
        PageHold = TimeSpan.FromMilliseconds(s->PageHoldMs);
        PagePreview = TimeSpan.FromMilliseconds(s->PagePreviewMs);
        PageOverlap = s->PageOverlap != 0;
        PageInScale = s->PageInScale;
        PageOutScale = s->PageOutScale;
        EndHold = TimeSpan.FromMilliseconds(s->EndHoldMs);
        EndFade = TimeSpan.FromMilliseconds(s->EndFadeMs);
        Countdown = (LyricCountdown)s->Countdown;
        CountdownCount = s->CountdownCount;
        CountdownDuration = TimeSpan.FromMilliseconds(s->CountdownMs);
        CountdownGap = TimeSpan.FromMilliseconds(s->CountdownGapMs);
        CountdownFrom = SurfaceInterop.FromArgb(s->CountdownFrom);
        CountdownTo = SurfaceInterop.FromArgb(s->CountdownTo);
        CountdownSize = s->CountdownSize;
        Panel = s->Panel != 0;
        PanelColor = SurfaceInterop.FromArgb(s->PanelColor);
        PanelPadding = s->PanelPadding;
        PanelRadius = s->PanelRadius;
        SubtitleMode = s->SubtitleMode != 0;
        Effects = SurfaceInterop.FromNative(s->Effects);
    }

    private static int Ms(TimeSpan t, string name) =>
        t >= TimeSpan.Zero && t <= TimeSpan.FromMinutes(1)
            ? (int)Math.Round(t.TotalMilliseconds)
            : throw new ArgumentOutOfRangeException(name, t, "Out of range.");

    /// <summary>Calls <paramref name="use"/> with the native style; its strings and font data live for the call.</summary>
    internal unsafe T WithNative<T>(Func<nint, T> use)
    {
        FaLyricStyle s;
        NativeMethods.fa_lyric_style_init(&s, 0);
        s.FontFace = (uint)Math.Max(0, FontFace);
        s.FontExplicit = FontExplicit ? 1 : 0;
        s.FontSize = FontSize;
        s.FontWeight = FontWeight;
        s.TextColor = SurfaceInterop.Argb(TextColor);
        s.SungColor = SurfaceInterop.Argb(SungColor);
        s.VerticalPosition = VerticalPosition;
        s.BoxWidth = BoxWidth;
        s.LinePitch = LinePitch;
        s.LinesAbove = LinesAbove;
        s.LinesBelow = LinesBelow;
        s.AboveOpacity0 = AboveOpacity1;
        s.AboveOpacity1 = AboveOpacity2;
        s.NextLineOpacity = NextLineOpacity;
        s.AfterNextFactor = AfterNextFactor;
        s.NearScale = NearScale;
        s.FarScale = FarScale;
        s.Wipe = (int)Wipe;
        s.WipeCapMs = Ms(WipeCap, nameof(WipeCap));
        s.WipeShare = WipeShare;
        s.InstantCutMs = Ms(InstantCut, nameof(InstantCut));
        s.LeadMs = Ms(Lead, nameof(Lead));
        s.ParagraphGapMs = Ms(ParagraphGap, nameof(ParagraphGap));
        s.SentenceGapMs = Ms(SentenceGap, nameof(SentenceGap));
        s.BreakLength = BreakLength;
        s.ScrollMs = Ms(Scroll, nameof(Scroll));
        s.PageMs = Ms(PageFade, nameof(PageFade));
        s.PageHoldMs = Ms(PageHold, nameof(PageHold));
        s.PagePreviewMs = Ms(PagePreview, nameof(PagePreview));
        s.PageOverlap = PageOverlap ? 1 : 0;
        s.PageInScale = PageInScale;
        s.PageOutScale = PageOutScale;
        s.EndHoldMs = Ms(EndHold, nameof(EndHold));
        s.EndFadeMs = Ms(EndFade, nameof(EndFade));
        s.Countdown = (int)Countdown;
        s.CountdownCount = CountdownCount;
        s.CountdownMs = Ms(CountdownDuration, nameof(CountdownDuration));
        s.CountdownGapMs = Ms(CountdownGap, nameof(CountdownGap));
        s.CountdownFrom = SurfaceInterop.Argb(CountdownFrom);
        s.CountdownTo = SurfaceInterop.Argb(CountdownTo);
        s.CountdownSize = CountdownSize;
        s.Panel = Panel ? 1 : 0;
        s.PanelColor = SurfaceInterop.Argb(PanelColor);
        s.PanelPadding = PanelPadding;
        s.PanelRadius = PanelRadius;
        s.SubtitleMode = SubtitleMode ? 1 : 0;
        SurfaceInterop.ToNative(Effects, &s.Effects);
        nint family = FontFamily is null ? 0 : Marshal.StringToCoTaskMemUTF8(FontFamily);
        nint path = FontPath is null ? 0 : Marshal.StringToCoTaskMemUTF8(FontPath);
        try
        {
            s.FontFamily = family;
            s.FontPath = path;
            if (FontData is { IsEmpty: false } data)
            {
                fixed (byte* d = data.Span)
                {
                    s.FontData = (nint)d;
                    s.FontDataSize = (nuint)data.Length;
                    return use((nint)(&s));
                }
            }
            return use((nint)(&s));
        }
        finally
        {
            Marshal.FreeCoTaskMem(family);
            Marshal.FreeCoTaskMem(path);
        }
    }
}

/// <summary>
/// Draws a MIDI or .kar song's lyrics as a karaoke screen: the active line in the middle, the lines around it smaller
/// and fainter, each syllable coloured in as it is sung, lines scrolling up and pages fading out and in, a countdown
/// before a line after a long gap. Create it with <see cref="AudioStream.CreateLyricSurface"/>, set
/// <see cref="PresentationSize"/> to the size of the area it is shown in, and draw with
/// <see cref="UpdateInto(nint, int, int, int, ulong, TimeSpan?)"/> as any <see cref="IKaraokeSurface"/>.
/// </summary>
/// <remarks>
/// The buffer is the block of lyric lines, not the whole area: <see cref="SurfaceLayout.BitmapX"/> and
/// <see cref="SurfaceLayout.BitmapY"/> place it. Pixels are premultiplied BGRA against transparent. Every frame is a
/// function of the position, so seeks, loops and tempo changes need nothing. Text is drawn by FatzAudio's own font
/// reader; <see cref="SurfaceLayout.FontUsed"/> names the family drawn with.
/// </remarks>
/// <example>
/// <code>
/// using var lyrics = song.CreateLyricSurface(LyricStyle.FromPreset(LyricPreset.Classic) with { FontFamily = "Segoe UI" });
/// lyrics.PresentationSize = new PixelSize(1920, 1080);
/// var layout = lyrics.GetLayout();   // a layout.Width x layout.Height buffer at (layout.BitmapX, layout.BitmapY)
/// </code>
/// </example>
public sealed unsafe class LyricSurface : IKaraokeSurface, IStreamSurface
{
    private readonly AudioStream _stream;
    private readonly object _lock = new();
    private readonly PixelRect[] _dirty = new PixelRect[8];
    private PixelSize _presentationSize;
    private bool _renderAhead;
    private LyricStyle _style;
    private nint _native;

    internal LyricSurface(AudioStream stream, LyricStyle style)
    {
        _stream = stream;
        var handle = stream.Handle;
        _native = style.WithNative(s =>
        {
            int rc = NativeMethods.fa_lyric_surface_create(handle, (FaLyricStyle*)s, out nint native);
            if (rc == (int)ErrorCode.InvalidArgument)
                throw new ArgumentException("Not a MIDI stream, or a style value out of its range.", nameof(style));
            FatzAudioException.ThrowIfFailed(rc);
            return native;
        });
        _style = style;
        handle.TrackSurface(this);
    }

    /// <summary>The stream whose lyrics this surface draws.</summary>
    public AudioStream Stream => _stream;

    /// <summary>
    /// The style. Setting it builds the lines again and draws the next frame in full (a new
    /// <see cref="SurfaceLayout.LayoutRevision"/> when sizes or the font change).
    /// </summary>
    /// <exception cref="ArgumentException">A value out of its range (the old style stays).</exception>
    /// <exception cref="FatzAudioException">The font could not be opened (the old style stays).</exception>
    public LyricStyle Style
    {
        get => _style;
        set
        {
            ArgumentNullException.ThrowIfNull(value);
            using (var held = Hold())
            {
                nint native = held.Native;
                int rc = value.WithNative(s => NativeMethods.fa_lyric_surface_set_style(native, (FaLyricStyle*)s));
                if (rc == (int)ErrorCode.InvalidArgument)
                    throw new ArgumentException("A style value is out of its range.", nameof(value));
                FatzAudioException.ThrowIfFailed(rc);
                _style = value;
            }
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
            using (var held = Hold())
                FatzAudioException.ThrowIfFailed(
                    NativeMethods.fa_surface_set_presentation_size(held.Native, (uint)value.Width, (uint)value.Height));
            _presentationSize = SurfaceInterop.ClampPresentation(value);
        }
    }

    /// <inheritdoc/>
    /// <remarks>Lyric frames are drawn on the calling thread for now; the setting is kept.</remarks>
    public bool RenderAhead
    {
        get => _renderAhead;
        set
        {
            using (var held = Hold())
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_surface_set_render_ahead(held.Native, value ? 1 : 0));
            _renderAhead = value;
        }
    }

    /// <inheritdoc/>
    /// <remarks>The style's <see cref="LyricStyle.Effects"/>: setting it changes <see cref="Style"/> too.</remarks>
    public SurfaceEffects? Effects
    {
        get => _style.Effects;
        set
        {
            FaSurfaceEffects e;
            SurfaceInterop.ToNative(value, &e);
            using var held = Hold();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_surface_set_effects(held.Native, &e));
            _style = _style with { Effects = value };
        }
    }

    /// <inheritdoc/>
    public SurfaceLayout GetLayout()
    {
        FaSurfaceLayout l = default;
        l.StructSize = (uint)sizeof(FaSurfaceLayout);
        using (var held = Hold())
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_surface_get_layout(held.Native, &l));
        return SurfaceInterop.FromNative(&l);
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
        using (var held = Hold())
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_surface_update_into(held.Native, t, (void*)destination,
                stride, (uint)width, (uint)height, destinationRevision, &f));
        int count = (int)Math.Min(f.DirtyCount, 8u);
        for (int i = 0; i < count; i++)
            _dirty[i] = new PixelRect(f.Dirty[i * 4], f.Dirty[i * 4 + 1], f.Dirty[i * 4 + 2], f.Dirty[i * 4 + 3]);
        return new SurfaceUpdate(f, _dirty.AsSpan(0, count), default, default, default);
    }

    /// <inheritdoc/>
    public SurfaceUpdate UpdateInto(Span<byte> destination, int stride, int width, int height, ulong destinationRevision,
        TimeSpan? time = null)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(height);
        if (stride < width * 4 || destination.Length < (long)stride * (height - 1) + (long)width * 4)
            throw new ArgumentException("The span is smaller than height rows of stride bytes.", nameof(destination));
        fixed (byte* p = destination)
            return UpdateInto((nint)p, stride, width, height, destinationRevision, time);
    }

    /// <summary>The stream held (which keeps the native surface alive) and the lock taken, for a native call.</summary>
    private Held Hold()
    {
        var handle = _stream.Handle;
        bool added = false;
        handle.DangerousAddRef(ref added);
        Monitor.Enter(_lock);
        if (_native == 0)
        {
            Monitor.Exit(_lock);
            if (added) handle.DangerousRelease();
            throw new ObjectDisposedException(GetType().FullName);
        }
        return new Held(this, added ? handle : null);
    }

    private readonly ref struct Held
    {
        private readonly LyricSurface _owner;
        private readonly StreamHandle? _handle;

        public Held(LyricSurface owner, StreamHandle? handle)
        {
            _owner = owner;
            _handle = handle;
        }

        public nint Native => _owner._native;

        public void Dispose()
        {
            Monitor.Exit(_owner._lock);
            _handle?.DangerousRelease();
        }
    }

    void IStreamSurface.Orphan()
    {
        lock (_lock)
        {
            if (_native == 0) return;
            NativeMethods.fa_surface_destroy(_native);
            _native = 0;
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
            // The stream is closing: its handle destroys the surfaces it still tracks.
            return;
        }
        try
        {
            lock (_lock)
            {
                if (_native == 0) return;
                NativeMethods.fa_surface_destroy(_native);
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
