# CD+G karaoke graphics

CD+G songs pair an audio track with a stream of graphics that draws the lyrics, 300 small "packs" per second. FatzAudio plays the graphics as a side stream of the audio: you open the song as usual, ask a **surface** for the picture from your render loop, and copy its pixels to whatever you draw with. The picture always matches what is heard, so pausing, seeking, looping, crossfades and tempo changes need no sync code in the app.

| Where the graphics come from | What to do |
|---|---|
| MP3+G pairs (`song.mp3` + `song.cdg` in one folder), or any other audio format with a `.cdg` of the same name | Nothing: `fa_stream_open_path` loads the `.cdg` automatically (any case: `.cdg`, `.CDG`) |
| A ZIP archive holding the audio and the `.cdg` | Nothing: `fa_stream_open_path` or `fa_stream_open_memory` of the `.zip` plays the audio inside and loads the graphics |
| Anything else: another file name, a web stream, the FFmpeg hand-off, a `.cdg` your app already has in memory | `fa_stream_load_cdg_path` / `fa_stream_load_cdg_memory` on the opened stream |
| Audio CDs with CD+G (a drive, or a `.cue` image with a `.sub` file) | Nothing: `fa_stream_open_disc` reads the graphics from the disc's subcode while the track plays ([disc playback](disc.md#cdg-karaoke-discs)) |

`fa_stream_info.has_cdg` is 1 when a stream has graphics. On a disc track it turns 1 once the graphics are found, usually in the first second of play, so create the surface anyway: a surface of a stream without graphics stays blank until they arrive.

## C

```c
#include <fatzaudio/fatzaudio.h>

fa_stream* song;
fa_stream_open_path(engine, "Artist - Song.mp3", NULL, &song);   /* finds "Artist - Song.cdg" */

fa_stream_info info;
fa_stream_get_info(song, &info);
if (!info.has_cdg) { /* show a cover image instead */ }

fa_cdg_surface_config cfg;
fa_cdg_surface_config_init(&cfg);
cfg.pixel_format = FA_CDG_PIXELS_BGRA32;   /* Direct3D, WPF, SkiaSharp, GDI */
cfg.width = 1200; cfg.height = 864;        /* 0 = native 300x216 */

fa_cdg_surface* cdg;
fa_cdg_surface_create(song, &cfg, &cdg);

fa_stream_attach(song, device, NULL);
fa_stream_play(song);

/* In your render loop (each vsync, or a 60 Hz timer): */
fa_cdg_frame frame;
fa_cdg_surface_update(cdg, &frame);
if (frame.changed) {
    /* Upload only the part that changed. */
    const uint8_t* first = (const uint8_t*)frame.pixels
                         + frame.dirty_y * frame.stride + frame.dirty_x * 4;
    upload_to_texture(first, frame.stride,
                      frame.dirty_x, frame.dirty_y, frame.dirty_width, frame.dirty_height);
}

/* ... */
fa_cdg_surface_destroy(cdg);   /* before or after closing the stream */
fa_stream_close(song);
```

### Direct3D 11

```c
D3D11_BOX box = { frame.dirty_x, frame.dirty_y, 0,
                  frame.dirty_x + frame.dirty_width, frame.dirty_y + frame.dirty_height, 1 };
const uint8_t* src = (const uint8_t*)frame.pixels + frame.dirty_y * frame.stride + frame.dirty_x * 4;
ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)texture, 0, &box, src, frame.stride, 0);
/* texture: DXGI_FORMAT_B8G8R8A8_UNORM, 300x216 (or the size you configured) */
```

### OpenGL

```c
/* cfg.pixel_format = FA_CDG_PIXELS_RGBA32 */
glBindTexture(GL_TEXTURE_2D, tex);
glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.stride / 4);
glTexSubImage2D(GL_TEXTURE_2D, 0, frame.dirty_x, frame.dirty_y, frame.dirty_width, frame.dirty_height,
                GL_RGBA, GL_UNSIGNED_BYTE,
                (const uint8_t*)frame.pixels + frame.dirty_y * frame.stride + frame.dirty_x * 4);
glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
```

### Graphics from elsewhere

```c
fa_stream* radio;
fa_stream_open_url(engine, "https://example.com/karaoke/song.mp3", NULL, &radio);
fa_stream_load_cdg_memory(radio, cdg_bytes, cdg_size);   /* copied */

fa_stream_load_cdg_path(song, "other-name.cdg");          /* replaces what it had */
fa_stream_load_cdg_path(song, NULL);                      /* removes the graphics */
```

## C#

```csharp
using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();
using var song = engine.OpenFile("Artist - Song.zip");     // audio + .cdg inside
using var cdg = song.CreateCdgSurface(new CdgSurfaceOptions { Premultiplied = true });

var bitmap = new WriteableBitmap(300, 216, 96, 96, PixelFormats.Pbgra32, null);
lyrics.Source = bitmap;                                    // an <Image> in your window
song.PlayOn(output);

CompositionTarget.Rendering += (_, _) =>                   // WPF, once per frame
{
    var frame = cdg.Update();
    if (!frame.Changed) return;
    var d = frame.Dirty;
    bitmap.WritePixels(new Int32Rect(d.X, d.Y, d.Width, d.Height),
        frame.PixelPointer + d.Y * frame.Stride + d.X * 4,
        frame.Stride * frame.Height, frame.Stride);
};
```

Other members: `song.HasCdg`, `song.LoadCdg(path)`, `song.LoadCdg(bytes)`, `song.RemoveCdg()`, `song.CdgOffset`, `cdg.UpdateAt(time)`. `StreamOptions.NoCdg` skips the automatic lookup. `CdgFrame` is a `ref struct`: read its pixels before the next `Update` call.

SkiaSharp:

```csharp
var info = new SKImageInfo(frame.Width, frame.Height, SKColorType.Bgra8888, SKAlphaType.Premul);
bitmap.InstallPixels(info, frame.PixelPointer, frame.Stride);   // or copy with frame.Pixels
```

## Surfaces

A surface turns the graphics into pixels for one output. Each has its own pixel format, size and options, so a small preview window and a full-screen TV output can show the same stream.

| Option | Values |
|---|---|
| `pixel_format` | `FA_CDG_PIXELS_BGRA32` (default: Direct3D/DXGI B8G8R8A8, WPF Bgra32/Pbgra32, SkiaSharp Bgra8888, GDI DIBs), `FA_CDG_PIXELS_RGBA32` (OpenGL, Vulkan, Metal RGBA8, Android bitmaps), `FA_CDG_PIXELS_INDEXED8` (one byte per pixel, index into `frame.palette`) |
| `width`, `height` | Output size, up to 8192. 0 = native: 300x216 with the border, 288x192 without |
| `FA_CDG_NO_BORDER` | Only the 288x192 picture area. The border is usually a plain colour; leave it out when you letterbox yourself |
| `FA_CDG_TRANSPARENT` | The background colour (the colour of the last screen clear) is fully transparent, so the lyrics can be drawn over video. Colours a disc marks with the "transparent colour" instruction get that transparency (0 opaque .. 63 invisible) |
| `FA_CDG_PREMULTIPLIED` | Colour multiplied by alpha, for WPF `Pbgra32`, Direct2D and most compositors. Without it alpha is straight |
| `FA_CDG_SMOOTH` | Bilinear scaling. Without it, scaling keeps sharp square pixels (the classic look). Ignored for INDEXED8 |
| `FA_CDG_TRANSPARENT_BACKGROUND_ONLY` | Like `FA_CDG_TRANSPARENT`, but only the background colour is transparent: transparent-colour packs are ignored. Some discs set them in ways that punch holes in the lyrics. Wins over `FA_CDG_TRANSPARENT` |
| `FA_CDG_HIDE_UNTIL_PRESET` | Every colour is fully transparent until the first screen clear (memory preset), so the picture or video underneath shows instead of a black screen at the start of a song |
| `FA_CDG_BACKGROUND_COLOR` + `background_color` | Replaces the background colour with your own, `0xAARRGGBB`, alpha included |
| `FA_CDG_BACKGROUND_OPACITY` + `background_opacity` | How opaque the background is, 0..100 % of 255 (or of `background_color`'s alpha), whatever the transparency flags make it. A host's "background opacity" slider |

None of these change what a surface draws without them. Apps built against the older, shorter `fa_cdg_surface_config` keep working: pass its `struct_size` as before.

`fa_cdg_surface_update` returns:

| Field | Meaning |
|---|---|
| `pixels`, `stride`, `width`, `height` | The picture. Owned by the surface, unchanged until its next update or destroy |
| `changed` | 1 when the pixels or the palette differ from the previous update. Always 1 the first time |
| `dirty_x`, `dirty_y`, `dirty_width`, `dirty_height` | The pixels that changed, in output pixels. Usually one 6x12 tile per pack while lyrics are being coloured in, so uploads stay tiny |
| `palette_changed` | 1 when `palette` changed. With INDEXED8 a colour-table change recolours the screen without touching a pixel, so the dirty rectangle is empty: update your palette (or shader lookup) instead |
| `palette[16][4]` | R, G, B, A of the 16 colours (straight alpha). A is 255 unless a transparency or background option is set, and RGB never has alpha folded in |
| `time` | The position on the graphics timeline that is shown |
| `background_index` | Colour of the last memory preset (screen clear); -1 before the first one and for streams without graphics |
| `border_index` | Colour of the last border preset; -1 before one |
| `transparency[16]` | The values of the last transparent-colour pack, 0 opaque .. 63 invisible, whatever the surface's flags |
| `background[4]` | R, G, B, A the background is drawn with, options applied: fill the area around a scaled picture with it. All 0 while `background_index` is -1 |
| `revision` | Goes up whenever an update changes a pixel (1 after the first update; by more than one when `fa_surface_update_into` drew changes in between). Never goes down, also not on seeks |
| `palette_revision` | Goes up whenever `palette`, `background_index`, `border_index` or `transparency` change |

### Your own compositor (INDEXED8)

Apps that scale, colour and blend the picture themselves take INDEXED8 pixels and the raw state, and apply their own policy. A typical setup: one INDEXED8 surface per stream at native size, flags 0, updated with your own presentation clock.

```c
cfg.pixel_format = FA_CDG_PIXELS_INDEXED8;   /* width = height = 0, flags = 0 */
fa_cdg_surface_create(song, &cfg, &cdg);

/* Presentation thread, once per displayed frame: */
fa_cdg_surface_update_at(cdg, my_presentation_time, &frame);
for (each render target t) {
    if (t->revision != frame.revision) { upload_indices(t, &frame); t->revision = frame.revision; }
    if (t->palette_revision != frame.palette_revision) {
        upload_palette(t, frame.palette);    /* RGB; alpha is 255 with flags 0 */
        t->palette_revision = frame.palette_revision;
    }
}
/* In the shader: skip pixels equal to frame.background_index (draw nothing at all
 * while it is -1), draw every other index opaque. transparency[] is there if you
 * want to honour the disc's transparent-colour packs. */
```

The revisions let one surface feed several render targets (a main window and a projector window, each with its own textures): each target compares them with what it last uploaded. `changed` and the dirty rectangle are relative to the previous update and suit a single target.

### Drawing into your own buffers (`fa_surface_update_into`)

`fa_cdg_surface_update` draws into the surface's own pixels, and you copy them. `fa_surface_update_into` draws straight into a buffer of yours instead: a mapped upload buffer, a texture's staging memory, a `WriteableBitmap`'s back buffer. The pixels are exactly those `fa_cdg_surface_update` gives (same format, size and options). `fa_surface` is the same type as `fa_cdg_surface`, so the `fa_surface_*` functions take a CD+G surface as it is.

With two or three buffers in rotation, each buffer comes back holding an older frame. Pass the revision it holds as `dst_revision`, and the surface draws only the pixels that changed since that frame. It remembers what changed in its last 8 revisions (`FA_SURFACE_HISTORY`); a buffer older than that, or `dst_revision` 0, gets a full draw. The rest of the buffer is not touched, so it must still hold the frame you said it holds.

```c
fa_surface_layout lay = { sizeof(lay) };
fa_surface_get_layout(cdg, &lay);                       /* 300 x 216 here */
uint8_t* mem[3];                                          /* lay.height rows of `stride` bytes each */
uint64_t rev[3] = { 0, 0, 0 };

/* Each frame, with the next buffer b: */
fa_surface_frame fr = { sizeof(fr) };
fa_surface_update_into(cdg, FA_SURFACE_NOW, mem[b], stride, lay.width, lay.height, rev[b], &fr);
rev[b] = fr.revision;
for (uint32_t i = 0; i < fr.dirty_count; i++) upload_rect(b, fr.dirty[i]);   /* 0 rects: up to date */
```

| `fa_surface_frame` field | Meaning |
|---|---|
| `revision` | The frame now in the buffer: keep it with the buffer |
| `base_revision` | The dirty rectangles are the change from this one (your `dst_revision`); 0 after a full draw |
| `dirty[0 .. dirty_count - 1]`, `dirty_bounds` | The pixels drawn, up to 8 separate rectangles (tiles far apart stay separate) and the box around them |
| `time` | The position on the graphics timeline shown |
| `behind` | Render-ahead only: the frame shown is late (see below) |
| `draw_ns`, `render_ns` | How long the call took, and how much of that was drawing pixels |
| `background_index`, `border_index`, `background`, `palette`, `transparency`, `palette_revision` | As in `fa_cdg_frame`: the palette for colouring INDEXED8 output on the GPU |

`dst` must be exactly the layout's width x height (`FA_ERROR_SIZE_MISMATCH` otherwise, and nothing is drawn: get the layout again and reallocate); `dst_stride` is bytes per row and may include padding. `fa_surface_get_layout` describes the buffer: its size, where the picture sits in it (`content_*`), the part of the CD+G screen shown (`crop_*`: all 300x216, or the 288x192 picture area with `FA_CDG_NO_BORDER`, reported as `FA_SAFETY_REMOVE`), and a `layout_revision` that goes up when any of it changes. Without presentation options it never changes. A surface used only through `update_into` never allocates pixels of its own.

**Render-ahead** (`fa_surface_set_render_ahead`, off by default; .NET `RenderAhead`). With presentation scaling on, the engine's worker threads render the surface's next frame while you show the current one, and `update_into` only copies the changed area of the finished frame that fits your time best into your buffer: it never waits for a worker that is rendering. The next frame is rendered for the time your next call is expected at, judged from the times of your calls, so pass the time of the frame you are about to show (or `FA_SURFACE_NOW`) and call at the display's pace. `time` in the frame is the position the copied frame shows, and `behind = 1` says it is more than 1/60 s older than the time you asked for (the workers are not keeping up, or just after a seek), newer (just after a seek back) or of graphics since replaced; keep calling and the right frame follows within a call or two. When no worker is rendering and no finished frame fits (the first frame, the first after a new layout or new presentation options, after a seek or new graphics, or one more than 1/60 s late), `update_into` draws the frame on your thread instead, so you never get an empty or wrong one for long. Calls that change the surface (options, sizes, `fa_surface_get_layout`, `fa_cdg_surface_update`) wait for a render in progress, at most one frame. It costs two more buffers of the layout's size (about 7 MB each at 1080p), freed when you turn it off. Workers decode audio first and render only when no stream needs decoding. Without effects it is rarely worth it for CD+G: even a full 2160p frame with every presentation option is under 20 ms and a typical frame well under a millisecond (see the table below), so leave it off unless the drawing thread is busy with other work. With effects, a full redraw takes 30-60 ms at 1080p and 100-220 ms at 2160p, so turn it on there. The classic picture ignores the setting.

In .NET, `CdgSurface` implements `IKaraokeSurface`:

```csharp
IKaraokeSurface surface = song.CreateCdgSurface(new CdgSurfaceOptions { Premultiplied = true });
var layout = surface.GetLayout();
int stride = layout.Width * 4;
var buffers = new[] { new byte[stride * layout.Height], new byte[stride * layout.Height] };
var revisions = new ulong[2];
// each frame, buffer b:
var update = surface.UpdateInto(buffers[b], stride, layout.Width, layout.Height, revisions[b]);
revisions[b] = update.Revision;
foreach (var r in update.Dirty) Upload(buffers[b], r);   // PixelRect
```

`UpdateInto` also takes a pointer (`nint`) for mapped memory; a buffer of the wrong size throws `FatzAudioException` with `ErrorCode.SizeMismatch`.

### Presentation options (`fa_cdg_surface_set_presentation`)

On top of its config, a surface can crop the screen to a safety zone, add transparent padding, and draw the picture in full colour at a whole-number multiple of its size, smoothed and anti-aliased, for a big screen. A surface without them (the default) draws exactly as before.

```c
fa_cdg_presentation p;
fa_cdg_presentation_init(&p);
p.scale = 1;                          /* full colour at a whole multiple */
p.safety_zone = FA_SAFETY_AUTO;       /* the border only when the song draws in it */
p.smoothness = 2;
p.edge_blend = 1;
fa_cdg_surface_set_presentation(cdg, &p);
fa_surface_set_presentation_size(cdg, 1920, 1080);   /* the window's size, in physical pixels */

fa_surface_layout lay = { sizeof(lay) };
fa_surface_get_layout(cdg, &lay);     /* lay.width x lay.height, 4 bytes per pixel */
/* ... fa_surface_update_into as above; reallocate when fr.layout_revision changes */
```

| `fa_cdg_presentation` field | Meaning |
|---|---|
| `scale` | 0: the surface's own pixel format, size and flags, with the crop and padding below. 1: presentation scaling (below) |
| `safety_zone` | `FA_SAFETY_SHOW` (default): all 300x216. `FA_SAFETY_REMOVE`: the 288x192 picture area. `FA_SAFETY_AUTO`: REMOVE, unless the song draws in the border area (tiles or scrolls; a border preset's solid colour does not count), then SHOW. `FA_SAFETY_SUPER`: only the box around everything the song ever draws other than the background, on the 6x12 tile grid. Replaces `FA_CDG_NO_BORDER` |
| `auto_ignore_seconds` | AUTO: drawing in the border before this time does not count (title cards) |
| `super_centered` | SUPER: the same crop on both sides, so the picture stays centred |
| `padding` | CD+G pixels of transparent space on each side, 0..32. It scales with the picture, so the buffer is exactly the size you lay out |
| `smoothness` | 0..10, scale on only: see below |
| `edge_blend` | 0/1, scale on only: anti-aliases the stair steps of edges (below) |
| `flags` | `FA_CDG_TRANSPARENT_BORDER`: the border area is transparent too; `border_index` still reports its colour so you can draw it your way |

AUTO and SUPER are worked out once per song, by the same walk over the packs that builds the seek checkpoints (a few milliseconds when the first surface is created), so the crop is fixed for the whole song and the picture never jumps. A disc's packs arrive while it plays, so there they use SHOW, and the layout says so (`safety_zone_resolved` 0). A surface created before graphics are loaded also reports SHOW unresolved, and gets its zone (and a new `layout_revision`) once they are.

**Scale off.** The buffer is the configured picture with the crop and the padding around it: transparent pixels (alpha 0, or `FA_CDG_INDEX_TRANSPARENT`, index 16, in INDEXED8). The transparent border is index 16 too in INDEXED8.

**Scale on.** The cropped picture is drawn at the whole-number multiple N nearest the presentation size, rounded up: at 1920x1080, SHOW is 5x (1500x1080) and REMOVE is 6x (1728x1152, which you shrink slightly). Your renderer does the final fit; whole multiples keep pixels even at smoothness 0 and give the smoothing an exact grid. Without a presentation size N is 1. The buffer is premultiplied 32-bit colour, RGBA for an RGBA32 surface and BGRA otherwise, from the outside in: padding, a transparent margin of one CD+G pixel (room for smoothing at the edge, and for your own scaling), then the picture. The background colour is drawn transparent, so you fill behind it: `background` in the frame is the colour to fill with (the song's, or `background_color` and `background_opacity` when those flags are set, which then only change what is reported), and every blend is computed against transparent, so there are no dark or coloured halos over whatever you draw underneath. The config's output size, `FA_CDG_SMOOTH` and `FA_CDG_PREMULTIPLIED` do not apply; the transparency flags and `FA_CDG_HIDE_UNTIL_PRESET` do. `bitmap_x`, `bitmap_y` in the layout say where the buffer goes inside the presentation area, centred (negative when it is a little larger).

| Smoothness | Look |
|---|---|
| 0 | Sharp pixels: each CD+G pixel an N x N block |
| 1 | Sharp, with the output pixel either side of each pixel boundary blended |
| 2, 3 | A blend band a quarter, then half a CD+G pixel wide |
| 4 | Plain bilinear |
| 5-10 | Planned (traced outlines, then curves); in this version they draw as 4 |

**Edge blend** is morphological anti-aliasing on the CD+G grid. An edge is where two neighbouring pixels have different colours. Along each straight run of an edge, where one colour steps over the edge at an end (an L, Z or U shape), the pixels between the edge and a line through the step take the other colour by the area they cover. Straight edges and flat areas are left alone, nothing is blurred, and no colour appears that is not one of the two meeting there. It can be combined with any smoothness.

**Scrolls are moves, not redraws.** The surface keeps the picture it drew (its master) and moves it when the screen scrolls, drawing only the band that came in and what touches it. Smooth-scroll offsets draw nothing: they move the window copied out of the master. A colour-table change draws the master again.

**Effects** (`fa_surface_set_effects`; .NET `Effects`) put an outline, a shadow and a bevel around everything that is not the background, with presentation scaling on (the classic picture ignores them). All three come from one exact distance field of the picture's shape at the output size, corrected to sub-pixel accuracy from the edge pixels' coverage, so the outline is a true stroke: round at corners and the same width on diagonals as on straight edges.

```c
fa_surface_effects fx;
fa_surface_effects_init(&fx);        /* everything off, sensible values in each */
fx.outline.on = 1;                    /* black, size 0.5 (0.75 CD+G pixel) */
fx.shadow.on = 1;                     /* black at 60 %, 0.5 (1 CD+G pixel) down and right */
fa_surface_set_effects(cdg, &fx);     /* NULL turns them off */
```

| Field | Meaning |
|---|---|
| `outline` | `color`, `opacity` (multiplies the colour's alpha), `size` 0..1 (1 = 1.5 CD+G pixels), `fade` 0..1 (the part of the width that eases out: 0 a hard edge, 1 a glow) |
| `shadow` | The shape and its outline, `distance` 0..1 (1 = 2 CD+G pixels) away towards `angle_degrees` (counter-clockwise from the right, 315 = down right), spread by `size` 0..1 (1 = 2 CD+G pixels) with `fade` easing it out: a soft shadow without a blur |
| `bevel` | Lit from `light_angle_degrees` (135 = top left): `highlight` on the edges facing the light and `shade` on those facing away, `size` 0..1 in from the edge (1 = 1 CD+G pixel), `strength` 0..1, `fade` |

Colours are 0xAARRGGBB with straight alpha. `sung_color` is for the lyric surface; CD+G ignores it. Sizes are in CD+G pixels, so the look is the same at every presentation size. The drawing order is shadow, outline, the picture, then the bevel over it, all premultiplied against transparent like the rest of the picture. Effects see only the cropped picture and stop at its edge (at the margin), so nothing outside the crop, such as a coloured border with `FA_SAFETY_REMOVE`, casts an outline or shadow into it; while a song uses the smooth-scroll offsets they also see the strip the offsets can bring into view. Changing the effects draws the next frame again in full. With effects on, a scroll still moves the picture, and a lyric being coloured in redraws only around it (the changed tiles plus the effects' reach).

Measured on one core of the Linux CI-class sandbox, Release build, a screen of six lines of lyrics; "full" is a seek or a new layout, "typical" one update while lyrics are coloured in (four tiles) into buffers in rotation:

| | 1080p full | 1080p typical | 2160p full | 2160p typical |
|---|---|---|---|---|
| Scale off | 0.3 ms | 0.01 ms | same | same |
| Smoothness 0 | 1.7 ms | 0.02 ms | 7.9 ms | 0.09 ms |
| Smoothness 2 | 3.3 ms | 0.05 ms | 11 ms | 0.15 ms |
| Smoothness 4 | 3.4 ms | 0.05 ms | 13 ms | 0.18 ms |
| Smoothness 4 + edge blend | 4.5 ms | 0.16 ms | 17 ms | 0.55 ms |
| + outline 0.5, shadow 0.5 | 28 ms | 1.2 ms | 115 ms | 4.3 ms |
| + every effect at 1.0, fades on | 55 ms | 3.3 ms | 220 ms | 14 ms |

`fa_test_cdg` prints this table (`test_presentation_costs`), so it can be measured on any machine. Effects cost most when the whole picture is drawn (a seek, a new layout, a colour change); a typical frame stays a few milliseconds even at 2160p. With effects at 2160p, turn on render-ahead so a full redraw happens on a worker instead of your drawing thread.

In .NET, `CdgSurface.Presentation` takes a `CdgPresentation` record (null = none):

```csharp
cdg.Presentation = new CdgPresentation { Scale = true, SafetyZone = SafetyZone.Auto, Smoothness = 2, EdgeBlend = true };
cdg.PresentationSize = new PixelSize(1920, 1080);
var layout = cdg.GetLayout();   // layout.Width x layout.Height, 4 bytes per pixel
```

### Change events (push model)

Apps that would rather be told than call update every frame turn on `fa_stream_set_cdg_events(song, 1)`. The engine's event thread then follows the stream's clock over the packs and raises `FA_EVENT_CDG_CHANGED` on the stream's event callback whenever the picture heard changes: lyrics being coloured in, a screen clear, a colour change, a seek, new graphics (a disc's included). Packs that change nothing raise nothing, a paused stream raises nothing, and one event covers all packs since the last (at most one every few milliseconds). The first event after turning it on covers the whole screen.

On the event, update your surfaces (in the callback, or by waking your render thread) and upload what they report. Surfaces stay yours: the engine never draws into them, so their threading rules above don't change.

| Event field | Meaning |
|---|---|
| `frame` | Packs now shown |
| `FA_CDG_CHANGE_X/Y/WIDTH/HEIGHT(value)` | The changed area in native 300x216 screen pixels (a surface's dirty rectangle is in its own pixels). Width 0 when only colours changed |
| `FA_CDG_CHANGE_PALETTE(value)` | 1 when colours or transparency changed |

```c
static void FA_CALL on_event(const fa_event* ev, void* user)
{
    if (ev->type == FA_EVENT_CDG_CHANGED) wake_render_thread(user);   /* it calls fa_cdg_surface_update */
}
fa_stream_set_event_callback(song, on_event, app);
fa_stream_set_cdg_events(song, 1);
```

In .NET, subscribing to `song.CdgChanged` turns the events on (`CdgChangedEventArgs.Area`, `PaletteChanged`, `Packs`), and removing the last handler turns them off.

### Graphics info

`fa_stream_get_cdg_info` says where a stream's graphics come from (`FA_CDG_SOURCE_FILE` for a `.cdg`, ZIP entry or memory block, `FA_CDG_SOURCE_DISC` for a disc's subcode), how many packs there are, and how far apart the seek checkpoints are. For a disc track, `disc_parity` is 1 when the subcode layout was chosen by the packs' P/Q parity and 0 when it was chosen by counting instructions (drives that return the parity zeroed). When the layout was chosen by parity, every pack is checked as it arrives: a pack with one damaged symbol is corrected from its P parity, and a pack with more damage is dropped (it would draw garbage tiles) rather than drawn. If the parity stops arriving part-way (300 failing packs in a row, as when a drive changes read mode), the rest of the track is applied as read. With `disc_parity` 0 there is nothing to check against, and the packs are applied as read. In .NET this is `song.CdgInfo`.

## Sync

Pack *n* of a `.cdg` belongs at *n* / 300 seconds of the audio. `fa_cdg_surface_update` reads the stream's clock (`fa_stream_get_clock`: the audio leaving the speaker right now, device latency included) and draws every pack up to that moment. There is nothing to keep in step:

- **Pause, seek, loop, crossfade, queue:** the clock follows the audio, and so do the graphics.
- **Tempo and pitch:** the clock is a position in the song, so slower or faster playback moves the graphics with it.
- **Out-of-sync files:** `fa_stream_set_cdg_offset(song, 150000000)` shows the graphics 150 ms later (negative: earlier).
- **Your own clock** (rendering a karaoke video, a scrub preview): `fa_cdg_surface_update_at(cdg, time, &frame)` draws the picture at any time you choose. The CD+G offset still applies.

The exact rule, for apps that compare their own decoder frame for frame: at time *t* (in seconds, after subtracting the offset) the picture shows packs [0, ceil(*t* × 300)) applied. Pack *n* occupies [*n*/300, (*n*+1)/300) and counts as soon as its slot starts, so at *t* = 1 s exactly packs 0..299 are applied, and at 1 s + 1 ns packs 0..300. *t* ≤ 0 shows no pack.

Drawing runs on the thread that calls update, not on the audio thread, and costs microseconds per frame during playback. Only the packs are read under the stream's lock; each surface draws on its own, so a large TV surface never holds up a small preview.

### Seeking

The graphics carry checkpoints, the decoder state every 10 s of packs, shared by all surfaces of the stream. A `.cdg` file, ZIP entry or memory block gets all of them when the stream's first surface is created (about 5 ms for a dense 5-minute song), so opening songs you never draw costs nothing. A disc track gets them as its packs arrive, and redoes them if a part is read again with different packs. Any seek, back or forward, the first one too and into parts never played (on a disc: parts read so far), restores the checkpoint before the target and replays at most 10 s of packs: well under a millisecond, so a seek-bar drag that sends many seeks per second stays cheap. A seek also uploads only the area that differs from what was shown. Graphics longer than 30 minutes space the checkpoints further apart (20 s up to an hour, and so on), which keeps their memory under about 6 MB (two pixels a byte).

## ZIP archives

`fa_stream_open_path`, `fa_stream_open_memory` and `fa_stream_open_io` recognise a ZIP by its content. Inside, the first `.cdg` is the graphics, and the audio is the entry with the same name (`Song.mp3` next to `Song.cdg`), else the first other entry FatzAudio can decode. Stored and deflate-compressed entries are read (the decompressor is built in). A damaged `.cdg` entry leaves a playable song without graphics. ZIP64 archives (larger than 4 GB, entries or offsets past 4 GB, or more than 65,535 entries) open the same way; the directory may be up to 16 MB. Encrypted entries are skipped and multi-part archives are not supported (`FA_ERROR_UNSUPPORTED_FORMAT`).

Only what is needed is read: the archive's directory at the end of the file, then the entries used. The whole file is never loaded.

| Audio entry | How it plays | Memory |
|---|---|---|
| Stored (uncompressed, the usual case) | Straight from the archive, like a plain file | None beyond a plain file's |
| Compressed, up to the memory limit (256 MB unless `fa_engine_config.zip_memory_limit_mb` says otherwise) | Unpacked into memory at open, CRC checked | The song's unpacked size |
| Compressed, bigger than the limit (or memory ran out), with `fa_engine_config.temp_directory` set | Unpacked into a temp file at open, CRC checked, then played from it | About 1 MB while unpacking |
| Compressed, bigger than the limit, no temp directory | Not opened: `FA_ERROR_TOO_LARGE` (.NET `ErrorCode.TooLarge`). Tell the user to extract the archive, or set a temp directory | None |

The `.cdg` is always unpacked into memory (a song's graphics are a few MB).

A stored entry played in place is not CRC checked at open, since that would read the whole song first. A damaged byte then plays like a damaged byte in an unzipped MP3: a short glitch, never a crash. Compressed entries are always checked while they are unpacked.

The temp directory is off by default, so FatzAudio never writes to disk unless the app asks. When it is set, FatzAudio works in a `fatzaudio-tmp` folder inside it and never touches anything else there. The directory and that folder are created if missing (not the directory's parents); if they cannot be written when a song needs them, the open fails with `FA_ERROR_IO`. Each unpacked song is deleted when its stream closes, and also if the app crashes: Windows opens the file with delete-on-close, and Linux, macOS, iOS and Android remove its name as soon as it exists. Files left by a power cut are deleted the next time an engine is created with that directory. On Android and iOS, pass the app's cache directory.

```c
fa_engine_config cfg;
fa_engine_config_init(&cfg);
cfg.temp_directory = "C:\\Users\\me\\AppData\\Local\\MyPlayer";   /* opt in */
fa_engine_create(&cfg, &engine);
```

```csharp
using var engine = AudioEngine.Create(new AudioEngineOptions { TempDirectory = Path.GetTempPath() });
```

`fa_stream_open_io` needs `seek` and `length` callbacks for a ZIP, because its directory is at the end (`FA_ERROR_NOT_SEEKABLE` without them). When the audio was unpacked, the archive is no longer read, but `close` is still called once, when the stream closes. Loudness scans and `fa_stream_get_tags` read a stored entry again from the archive file or block; a song unpacked into a temp file gets the running loudness estimate instead of a scan (as with app callbacks).

## The CD+G format

Every instruction of the CD+G "TV graphics" mode is implemented: memory preset (clear the screen), border preset, tile block in normal and XOR mode, scroll preset and scroll copy (horizontal and vertical, with the smooth-scroll offsets), define transparent colour, and both halves of the colour table. Repeated memory-preset packs (repeat count above 0) are copies of one clear, sent several times in case one is damaged on a disc: a copy with a higher count than the preset just applied, of the same colour and within 32 packs (about 0.1 s) of it, is skipped even if a border or tiles were drawn between the copies, and so is any repeat while the screen is still clean in that colour. A preset in another colour, or with a count that goes back, is a new clear. The palette starts black until the first colour-table load. CD+EG (extended graphics) packs are ignored, and so are packs of other subcode modes.

## Threading

- Surfaces can be created, updated and destroyed from any thread; one surface is updated from one thread at a time. Different surfaces, even of the same stream, may be updated from different threads.
- Loading or removing graphics while surfaces are being updated is safe: each surface starts over on the new graphics at its next update.
- An update never waits for the device or decode threads. It shares one short lock per stream with graphics loads, a disc's pack writes and the stream's other surfaces. The one longer hold is building a file's checkpoints, once, in the first `fa_cdg_surface_create` (or the first update after a load): about 5 ms for a song. `fa_cdg_surface_update` also reads `fa_stream_get_clock`; `fa_cdg_surface_update_at` does not.
- An update with nothing new compares the palette and returns: no pixels are copied, so calling it at 240 Hz costs next to nothing.
- Destroy every surface you create with `fa_cdg_surface_destroy`, before or after closing its stream, from any thread: an update or destroy that runs while `fa_stream_close` closes the stream on another thread is safe (the close waits for it to finish). After the stream is closed, updates return `FA_ERROR_INVALID_ARG`. In .NET, disposing the stream also disposes its surfaces, and `Update` then throws `ObjectDisposedException`.

## Pitfalls

- Read or copy `pixels` before the next update of the same surface; it is redrawn in place.
- With `fa_surface_update_into`, pass the revision the buffer really holds. A buffer you cleared or drew into yourself needs `dst_revision` 0, or the pixels outside the dirty rectangles stay wrong.
- Upload in the surface's pixel format. A BGRA surface into an RGBA texture swaps red and blue. With presentation scaling on the buffer is premultiplied colour even for an INDEXED8 surface.
- With presentation options, INDEXED8 buffers can hold index 16 (`FA_CDG_INDEX_TRANSPARENT`) in the padding and a transparent border: give your palette lookup a 17th, transparent entry.
- Leave `FA_CDG_TRANSPARENT` off unless something is drawn behind the lyrics: on its own, the background turns black or invisible depending on the renderer.

## Not provided yet

- CD+EG extended graphics.
- Ready-made helpers for specific renderers (a D3D11 texture updater, a CVPixelBuffer on Apple). The pixel buffer above covers them all today.
