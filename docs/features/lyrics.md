# MIDI lyric surface

A lyric surface draws the lyrics of a MIDI or `.kar` song as a karaoke screen, so an app does not need its own text renderer, timer and colouring code. It is a **surface**, the same kind of object as a [CD+G surface](cdg.md#surfaces): you ask it for pixels from your render loop and copy them to whatever you draw with. The picture is a function of the stream's position, so pausing, seeking, looping and tempo changes need no sync code.

## What it draws

- The active line in the middle, the lines before and after it smaller and fainter (two above and two below by default).
- Each syllable coloured in as it is sung, from the unsung to the sung colour: the syllable changes colour as a whole (cross-fade) or an edge sweeps across it.
- Lines scrolling up as the song moves on, and pages fading out and in at long pauses.
- A countdown of arrows before a line that follows a long gap.
- The lyrics fading out after the last syllable.
- An optional rounded panel behind each line, and the [surface effects](#effects) (outline, shadow, bevel).

Text is drawn with FatzAudio's own font reader and rasterizer, not the system's text engine, so the picture is the same on every platform. Pixels are premultiplied BGRA against a transparent background: draw the surface over your video or background.

Not drawn yet: the title and credits page before the song and the ending page of the [design reference](../requests/2026-10-20_MediaControl_KaraokeDesignReference.md). They come in the next step. A song without lyrics draws nothing.

## Quick start

### C

```c
#include <fatzaudio/fatzaudio.h>

fa_stream* song;
fa_stream_open_path(engine, "Artist - Song.kar", NULL, &song);

fa_lyric_style style;
fa_lyric_style_init(&style, FA_LYRIC_CLASSIC);
style.font_family = "Segoe UI";                      /* optional: else the system's UI font */

fa_surface* lyrics;
fa_lyric_surface_create(song, &style, &lyrics);      /* style NULL = Classic */
fa_surface_set_presentation_size(lyrics, 1920, 1080); /* the area the lyrics are shown in, in pixels */

fa_surface_layout lay = { sizeof(lay) };
fa_surface_get_layout(lyrics, &lay);                 /* the buffer size it wants: lay.width x lay.height */
/* The buffer is the block of lyric lines, not the whole area. Draw it at
 * (lay.bitmap_x, lay.bitmap_y) inside the presentation area. */

uint8_t* mem[3];                                     /* lay.height rows of `stride` bytes (BGRA) */
uint64_t rev[3] = { 0, 0, 0 };                       /* 0 = the buffer holds nothing yet */

fa_stream_attach(song, device, NULL);
fa_stream_play(song);

/* Each frame, with the next buffer b of the rotation: */
fa_surface_frame fr = { sizeof(fr) };
fa_surface_update_into(lyrics, FA_SURFACE_NOW, mem[b], stride, lay.width, lay.height, rev[b], &fr);
rev[b] = fr.revision;
for (uint32_t i = 0; i < fr.dirty_count; i++) upload_rect(b, fr.dirty[i]);   /* 0 rects: up to date */
/* then draw the texture at lay.bitmap_x, lay.bitmap_y (it is already at size) */

/* Call fa_surface_get_layout again when fa_surface_layout.layout_revision changes (a new
 * presentation size or style): the buffer size can change. */

fa_surface_destroy(lyrics);                          /* before or after closing the stream */
fa_stream_close(song);
```

`fa_surface_update_into`, the dirty rectangles, `revision` and the buffer rotation work exactly as for CD+G: see [drawing into your own buffers](cdg.md#drawing-into-your-own-buffers-fa_surface_update_into). A buffer of another size than the layout's gets `FA_ERROR_SIZE_MISMATCH`.

The block keeps one size for a presentation size and style: every line slot, the countdown row and the page fade-in are inside it, so the texture does not change size while the song plays. `bitmap_x` and `bitmap_y` give its top-left corner in the presentation area. `fa_surface_layout.scale` is the presentation height / 1080, and `font_used` names the font family that was drawn with.

### C#

```csharp
using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();
using var song = engine.OpenFile("Artist - Song.kar");

var style = LyricStyle.FromPreset(LyricPreset.Modern) with { FontFamily = "Segoe UI" };
using var lyrics = song.CreateLyricSurface(style);          // no argument = Classic
lyrics.PresentationSize = new PixelSize(1920, 1080);
song.PlayOn(output);

var layout = lyrics.GetLayout();                            // Width x Height, BitmapX, BitmapY, FontUsed
int stride = layout.Width * 4;
var buffers = new[] { new byte[stride * layout.Height], new byte[stride * layout.Height] };
var revisions = new ulong[2];

// each frame, buffer b:
var update = lyrics.UpdateInto(buffers[b], stride, layout.Width, layout.Height, revisions[b]);
revisions[b] = update.Revision;
foreach (var r in update.Dirty) Upload(buffers[b], r);      // then draw at (layout.BitmapX, layout.BitmapY)
```

`LyricSurface` implements `IKaraokeSurface`, like `CdgSurface`, so code that draws either kind works with both. Set `lyrics.Style = ...` to change the style while it plays: the lines are built again and the next frame is drawn in full (get the layout again when `LayoutRevision` changes).

## Presets

`fa_lyric_style_init(&style, preset)` (.NET `LyricStyle.FromPreset(preset)`; `new LyricStyle()` is Classic) fills a style with the preset's values. Start from one and change what you need.

| Preset | What it changes from the defaults |
|---|---|
| `FA_LYRIC_CLASSIC` | The defaults: MediaControl's look. White text, sung yellow (`0xFFFFFF00`), a black outline and a down-right shadow, a cross-fading wipe, four lines around the active one, a countdown |
| `FA_LYRIC_MODERN` | Sweeping wipe, sung colour light blue (`0xFF4FD8FF`), a thinner outline (0.12 em) and a soft, larger shadow |
| `FA_LYRIC_NEON` | Sweeping wipe, pale pink text and white-cyan sung text, a wide glowing outline (magenta before, cyan after), no shadow |
| `FA_LYRIC_MINIMAL` | One line above and one below, no outline, a light soft shadow, no countdown |
| `FA_LYRIC_SUBTITLE` | Two lines in a band near the bottom (the active line and the next, at 48 px, weight 600) on a dark rounded panel, a sweeping wipe, no lines above, no page scaling, short scrolls and fades (400 ms, 300 ms), no countdown |

## Style reference

`fa_lyric_style` (.NET `LyricStyle`, a record: change it with `with`). "em" is `font_size`. Sizes are for a presentation area 1080 pixels high and scale with its height. Times are milliseconds of real time (.NET: `TimeSpan`). Values outside the range give `FA_ERROR_INVALID_ARG` (.NET `ArgumentException`). Defaults are those of Classic; Classic is also what you get with a NULL style.

Always set `struct_size` (`fa_lyric_style_init` does).

### Font

| Field (.NET) | Default | Range | Meaning |
|---|---|---|---|
| `font_family` (`FontFamily`) | NULL | | An installed family, found as `fa_font_find`. NULL, or a family that is not installed: the system's UI font |
| `font_path` (`FontPath`) | NULL | | A font file (TrueType, OpenType, collection). Wins over `font_family` |
| `font_data`, `font_data_size` (`FontData`) | NULL | | A font file in memory, copied. Wins over the other two |
| `font_face` (`FontFace`) | 0 | | The face of a collection (`.ttc`) |
| `font_explicit` (`FontExplicit`) | 0 | 0/1 | 0: a song the font mostly cannot show is drawn in the system's font for its script. 1: always your font |
| `font_size` (`FontSize`) | 72 | 6..240 | Pixels at 1080 |
| `font_weight` (`FontWeight`) | 700 | 100..900 | A family without a face that heavy gets a synthetic bold |
| `text_color`, `sung_color` (`TextColor`, `SungColor`) | white, yellow | | `0xAARRGGBB`, straight alpha |

### Placement

| Field (.NET) | Default | Range | Meaning |
|---|---|---|---|
| `vertical_position` (`VerticalPosition`) | 0.5 | 0..1 | The active line's centre, from the top (0) to the bottom (1) |
| `box_width` (`BoxWidth`) | 0.85 | 0.2..1 | The width lines fit in, of the area's width |
| `line_pitch` (`LinePitch`) | 1.55 | 0.8..4 | Em between line centres |
| `lines_above`, `lines_below` (`LinesAbove`, `LinesBelow`) | 2, 2 | 0..2 | Lines shown around the active one |
| `above_opacity[2]` (`AboveOpacity1`, `AboveOpacity2`) | 0.5, 0.25 | 0..1 | The lines 1 and 2 above |
| `next_line_opacity` (`NextLineOpacity`) | 0.65 | 0..1 | The line below |
| `after_next_factor` (`AfterNextFactor`) | 0.55 | 0..1 | The line two below: `next_line_opacity` times this |
| `near_scale`, `far_scale` (`NearScale`, `FarScale`) | 0.8, 0.6 | 0.2..1 | The size of the lines 1 and 2 away |

### Wipe

| Field (.NET) | Default | Range | Meaning |
|---|---|---|---|
| `wipe` (`Wipe`) | `FA_WIPE_CROSSFADE` (Classic, Minimal); `FA_WIPE_SWEEP` (others) | | Cross-fade: the syllable changes colour as a whole. Sweep: an edge moves across it |
| `wipe_share` (`WipeShare`) | 0.8 | 0.1..1 | A syllable's wipe takes this part of the time to the next syllable |
| `wipe_cap_ms` (`WipeCap`) | 600 | 0..2000 | ...at most this long. 0 = every wipe is instant |
| `instant_cut_ms` (`InstantCut`) | 150 | 0..1000 | Wipes shorter than this are instant |
| `lead_ms` (`Lead`) | 400 | 0..2000 | Syllables are coloured this early, so the colour is on screen when the singer reaches the word |

### Lines and pages

| Field (.NET) | Default | Range | Meaning |
|---|---|---|---|
| `paragraph_gap_ms` (`ParagraphGap`) | 2000 | 0..60000 | A pause this long starts a new page |
| `sentence_gap_ms` (`SentenceGap`) | 900 | 0..60000 | A pause this long after `. ! ? : ;` starts a new line |
| `break_length` (`BreakLength`) | 42 | 0..1000 | Characters. A line this long breaks at the next space. 0 = no rule |
| `scroll_ms` (`Scroll`) | 1500 | 0..4000 | A line scrolling up |
| `page_ms` (`PageFade`) | 1000 | 0..4000 | A page fading out, or in |
| `page_hold_ms` (`PageHold`) | 2000 | 0..10000 | A page stays this long after its last syllable, when there is time |
| `page_preview_ms` (`PagePreview`) | 1000 | 0..10000 | A new page is in place this early |
| `page_overlap` (`PageOverlap`) | 1 | 0/1 | 1: the fades may overlap, so new lyrics are never late. 0: the new page waits for the old one to go |
| `page_in_scale` (`PageInScale`) | 1.5 | 1..2 | A new page grows in from this size |
| `page_out_scale` (`PageOutScale`) | 0.75 | 0.25..1 | An old page shrinks to this size |
| `end_hold_ms`, `end_fade_ms` (`EndHold`, `EndFade`) | 5000, 1000 | 0..60000 | The lyrics fade out this long after the last syllable, over this long |

### Countdown

Shown before a line that follows a long gap.

| Field (.NET) | Default | Range | Meaning |
|---|---|---|---|
| `countdown` (`Countdown`) | `FA_COUNTDOWN_CLOCK` | | `OFF`; `CLOCK`: an arrow every `countdown_ms / (count + 1)`; `BEAT`: an arrow a beat, from the song's tempo |
| `countdown_count` (`CountdownCount`) | 4 | 1..8 | Arrows |
| `countdown_ms` (`CountdownDuration`) | 5000 | 1000..20000 | CLOCK: the whole countdown, the arrows and a last fade |
| `countdown_gap_ms` (`CountdownGap`) | 6000 | 0..60000 | Shown after a gap this long |
| `countdown_from`, `countdown_to` (`CountdownFrom`, `CountdownTo`) | red, green | | An arrow's colour before and after its turn |
| `countdown_size` (`CountdownSize`) | 0.5 | 0.2..1.5 | Em |

### Panel

| Field (.NET) | Default | Range | Meaning |
|---|---|---|---|
| `panel` (`Panel`) | 0 | 0/1 | A rounded panel behind each line |
| `panel_color` (`PanelColor`) | black at 50 % | | `0xAARRGGBB`; the alpha is the opacity |
| `panel_padding`, `panel_radius` (`PanelPadding`, `PanelRadius`) | 0.25, 0.2 | 0..1 | Em |
| `subtitle_mode` (`SubtitleMode`) | 0 | 0/1 | 1: no countdown (the Subtitle preset sets it) |

### Effects

`fa_lyric_style.effects` (.NET `LyricStyle.Effects`) is a `fa_surface_effects`: outline, shadow and bevel, the same effects as on [CD+G surfaces](cdg.md#presentation-options-fa_cdg_surface_set_presentation), with `sung_color` values so the outline and shadow can change colour as a syllable is sung. `fa_surface_set_effects(surface, &effects)` (.NET `lyrics.Effects`) sets the effects alone and also updates the style's.

For lyrics the sizes are in em, so the look is the same at every font size: at 1, the outline is 0.12 em wide, the shadow's `size` is 0.20 em, its `distance` 0.15 em and the bevel's `size` 0.08 em. Ranges and the other fields are those of `fa_surface_effects`.

## How lines and pages are built

The text comes from `fa_stream_get_midi_text` (see [MIDI lyrics](midi.md#lyrics-and-text-events)). The surface builds its lines once, from these rules, in this order of strength:

- **The song's own marks.** A syllable flagged `FA_MIDI_NEW_LINE` starts a new line, and `FA_MIDI_NEW_PARAGRAPH` starts a new page. A song's own new-line mark does not look at gaps or lengths, as in MediaControl: the line breaks where the song says.
- **Paragraph gap.** A pause of `paragraph_gap_ms` (2 s) between two syllables starts a new page.
- **Sentence gap.** A pause of `sentence_gap_ms` (0.9 s) after a syllable ending in `.`, `!`, `?`, `:` or `;` starts a new line.
- **The 42-character rule.** A line that reaches `break_length` characters breaks at the next space, also before a syllable that starts with a space.
- **Lines too wide.** A line wider than the box (`box_width` of the area's width) is split between words when the lines are built. Each row is a line of its own, with its own wipe, on the same page. A single word wider than the box is drawn smaller to fit.

A page is the group of lines shown together; the lines of the page before and after the active line are what you see above and below it.

## Timing

Lyric times follow the stream's clock (`FA_SURFACE_NOW` is `fa_stream_get_clock`), so they follow seeks, pauses and `FA_PARAM_TEMPO`. The animations (wipes, scrolls, page fades, countdown, end fade) run in real time, with the lengths in the style, whatever the tempo: a song played at half speed does not scroll twice as slowly.

| What | Default |
|---|---|
| Colouring starts early | 400 ms before the syllable (`lead_ms`) |
| A syllable's wipe | 0.8 of the time to the next syllable, at most 600 ms; shorter than 150 ms: instant |
| A line scrolling up | 1.5 s |
| A page fading out, fading in | 1 s each, overlapping so new lyrics are not late; a new page grows in from 1.5x, an old one shrinks to 0.75x |
| A page stays after its last syllable | up to 2 s when there is time |
| A new page is in place | 1 s before its first syllable |
| Countdown | after a gap of 6 s or more: 4 arrows over 5 s (one every 1 s), then a last fade |
| End of the song | the lyrics fade out 5 s after the last syllable, over 1 s |

## Fonts

The font is chosen in this order: `font_data`, `font_path`, `font_family` (found like `fa_font_find`), and the system's UI font when none is given or the family is not installed. `fa_surface_layout.font_used` (.NET `SurfaceLayout.FontUsed`) names the family that was drawn with.

A song whose text the chosen font mostly cannot show, such as a Japanese, Korean or Chinese `.kar` with a Latin font, is drawn with the system's font for its script instead, unless `font_explicit` is 1. A family without a face as heavy as `font_weight` gets a synthetic bold.

Creating a surface fails with `FA_ERROR_NOT_FOUND` when no font could be opened (a `font_path` that is missing, or no system font at all) and with `FA_ERROR_UNSUPPORTED_FORMAT` when `font_data` or `font_path` is not a font. `fa_lyric_surface_set_style` returns the same errors and keeps the old style.

## Threads and lifetime

These are the rules of [CD+G surfaces](cdg.md#threading):

- A surface can be created, updated and destroyed from any thread; one surface is updated from one thread at a time.
- Destroy a surface with `fa_surface_destroy`. It can outlive its stream: after the stream is closed it draws nothing more. In .NET, disposing the stream also disposes its surfaces.
- An update never waits for decoding or the device.
- `fa_cdg_surface_update`, `fa_cdg_surface_set_presentation` and the other CD+G-only calls return `FA_ERROR_INVALID_ARG` for a lyric surface. Use the `fa_surface_*` functions and the three `fa_lyric_surface_*` ones.
- `fa_lyric_surface_create` needs a MIDI stream: any other gives `FA_ERROR_INVALID_ARG`.
- `fa_lyric_surface_get_style` copies the style, but its strings and `font_data` point into the surface; they stay valid until the next `fa_lyric_surface_set_style` or until the surface is destroyed.

## Cost

Measured at 1080p on one core of a desktop x64, Release build:

| Frame | Time |
|---|---|
| A still frame (nothing moves; a syllable being wiped, a countdown arrow) | about 0.3 ms |
| A frame while lines scroll or a page fades | about 3-5 ms |
| The first frame of a page | more, while its lines are drawn |

Lines are laid out once; a frame colours and places them, so only what changed is redrawn. A lyric surface accepts `fa_surface_set_render_ahead` (`RenderAhead`) and keeps the setting, but for now the frames are drawn on the caller's thread. Drawing on the engine's workers, with the intro and ending pages, comes in the next step (5b of the [karaoke surfaces plan](../requests/2026-10-20_FatzAudio_Plan_KaraokeSurfaces.md)). A cost table for 1080p and 2160p will be measured then.

## Pitfalls

- The bitmap is the block of lyrics, not the whole area. Draw it at `bitmap_x`, `bitmap_y` in the presentation area; do not stretch it to the screen.
- Set `fa_surface_set_presentation_size` to the real size of the area, in physical pixels, before the first layout. Without it the surface draws for 1920x1080.
- Use a `dst_revision` of 0 for a buffer you have cleared or drawn into yourself.
- The pixels are premultiplied BGRA. Blend with "one, one minus source alpha".
- A scaled-up line during a page fade-in is slightly soft until it settles: see the [engineering notes](../engineering-notes.md).
