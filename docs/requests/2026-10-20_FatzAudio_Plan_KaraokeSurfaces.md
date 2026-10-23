# FatzAudio: plan for the karaoke surfaces request

To: FatzBomb.MediaControl and Keith. From: FatzAudio, 2026-10-20. Status: **approved by Keith on 2026-10-20 with three changes, which are worked in below (section 12 lists them). Step 0 is under way.**
Nothing in sections 2 to 9 is built yet. Answers to the three requests in this folder:
[review](2026-10-20_MediaControl_Review_MidiAndCdg.md),
[request](2026-10-20_MediaControl_Request_KaraokeSurfaces.md),
[design reference](2026-10-20_MediaControl_KaraokeDesignReference.md).

Ground rules this plan keeps: one binary per platform with no new runtime libraries, every capability
implemented once (no per-OS renderers), clean-room code, what the OS already has where it helps, and
"not the everything player". **Every default stays as it is: a surface that sets none of the new
options draws exactly what it draws today**, and the tests compare it byte for byte with today's output.

---

## 1. The review: what was fixed

All three CD+G findings hold. They are fixed in the same pull request as this plan, each with a test
that fails on the old code.

| Finding | Verdict | Fix |
|---|---|---|
| Memory-preset repeats after drawing clear the screen again | Holds. A repeat was skipped only while the screen was still clean. | A repeat (count above 0) of the same colour with a higher count than the preset just applied, within 32 packs (about 0.1 s) of it, is a copy of that clear and is skipped, whatever was drawn between the copies. Another colour, or a count that goes back, is a new clear. The old "clean screen" rule stays too. |
| Disc packs written without a parity check | Holds. Parity only chose the subcode layout. | When the layout was chosen by parity, every pack is checked: one damaged symbol is corrected from the P parity (four syndromes: one error is located and checked against the other two), and a pack with more damage is dropped (an all-zero pack does nothing). A drive that returns no parity gets the packs as read, as before, and so does a read that stops returning parity part-way (300 bad packs in a row). |
| Use-after-free on `f->stream` | Holds. ThreadSanitizer reports the race on the old code. An update or destroy racing `fa_stream_close` on another thread could lock a freed stream. | The surface's stream pointer is read under a small lock and the call holds the stream while it runs; `fa_stream_close` first cuts its surfaces loose and waits for running surface calls, before it tears anything down. Update, update-at and destroy are now safe against a close on another thread (the docs used to forbid the destroy case). |

Other points in the review:

- **Precise dirty rectangles** (a rect only where a tile changed a pixel): taken, in phase 1.
- **Frames other threads can read safely**: taken, in a different form. `update_into` draws into your
  buffer, and the frame says which revision its dirty rectangles are relative to (section 2.4).
- **Instruction 28 (transparent colour)**: we have no authoritative spec text either. Our reading comes
  from the public CD+G descriptions, which name the instruction and its 16 six-bit values. It is always
  reported in `fa_cdg_frame.transparency`, but it changes alpha only with `FA_CDG_TRANSPARENT`, so
  default output does not depend on it. We have not seen a disc that uses it.
- **MIDI and DLS suggestions** (program-change fallback, velocity curve, `lart` merge, the 128-region
  cap, gain clamp, SF3 stereo): checked one by one in a separate small pull request after this one,
  each against FluidSynth output where it changes the sound. The text suggestions (encoding, karaoke
  detection, chord lines, line marks, `@I`) are phase 0 below.

---

## 2. The surface model (shared by CD+G and lyrics)

### 2.1 One handle type

C gets one surface type, `fa_surface`, with two kinds. `fa_cdg_surface` becomes the same type
(`typedef struct fa_surface fa_cdg_surface;`), so existing code compiles and runs unchanged. The
existing `fa_cdg_surface_*` functions stay as they are. New functions take `fa_surface*` and work on
both kinds.

```c
typedef struct fa_surface fa_surface;
typedef fa_surface fa_cdg_surface;                 /* unchanged for existing callers */

FA_API fa_result FA_CALL fa_lyric_surface_create(fa_stream* stream, const fa_lyric_style* style,
                                                 fa_surface** out);   /* MIDI/.kar streams */
/* fa_cdg_surface_create / _update / _update_at / _destroy: unchanged */
FA_API void FA_CALL fa_surface_destroy(fa_surface* surface);           /* either kind */

/* Draw for this many physical pixels (uniform fit inside it). 0 x 0 = unset (today's behaviour).
 * Clamped to 300x216 .. 3840x2160. Takes effect at the next update. */
FA_API fa_result FA_CALL fa_surface_set_presentation_size(fa_surface* s, uint32_t width, uint32_t height);
FA_API fa_result FA_CALL fa_surface_set_effects(fa_surface* s, const fa_surface_effects* effects); /* NULL = none */
FA_API fa_result FA_CALL fa_surface_set_render_ahead(fa_surface* s, int32_t enabled);  /* default 0 */
FA_API fa_result FA_CALL fa_surface_get_layout(fa_surface* s, fa_surface_layout* out_layout);
```

### 2.2 Layout report

```c
typedef enum fa_safety_zone { FA_SAFETY_SHOW = 0, FA_SAFETY_REMOVE, FA_SAFETY_AUTO, FA_SAFETY_SUPER } fa_safety_zone;

typedef struct fa_surface_layout {
    uint32_t struct_size;
    uint32_t width, height;            /* the bitmap update_into wants, in pixels */
    int32_t  bitmap_x, bitmap_y;       /* its top-left inside the presentation area (may be negative
                                          when a whole-number CD+G scale is larger than the area) */
    uint32_t content_x, content_y;     /* inside the bitmap: the picture or lyric block, without */
    uint32_t content_width, content_height; /* padding and margin */
    float    scale;                    /* CD+G: the whole-number multiple (1 when scaling is off);
                                          lyrics: presentation height / 1080 */
    uint32_t crop_x, crop_y, crop_width, crop_height; /* CD+G: the crop box in CD+G pixels */
    fa_safety_zone safety_zone;        /* CD+G: the mode in use (Auto and Super resolved) */
    int32_t  safety_zone_resolved;     /* 1 = the pre-pass decided (files); 0 = fell back to Show */
    char     font_used[128];           /* lyrics: the family actually drawn with (section 5) */
    uint32_t margin, padding;          /* in bitmap pixels */
    uint64_t layout_revision;          /* goes up whenever any of the above changes */
} fa_surface_layout;
```

Changing anything that moves or resizes the bitmap bumps `layout_revision` and makes the next update a
full redraw. For CD+G files the layout is final from the first `get_layout` (the pre-pass runs with the
checkpoints, when the first surface is created).

Where we differ: **the lyric bitmap keeps one size for a given presentation size and style.** It is
sized for the largest the block can get: every line slot shown, the countdown row, and the page fade-in
scale. It does not follow the text, because that would bump `layout_revision` on every page and make you
reallocate during playback. The block is still tight: about 85% of the width and the height of the
slots, not a full transparent canvas.

### 2.3 Drawing into your buffer

```c
#define FA_SURFACE_NOW INT64_MIN       /* time = the stream's clock (fa_stream_get_clock) */

/* Draws the frame for `time` into dst (premultiplied BGRA32, or the CD+G surface's own format when
 * presentation scaling is off). dst must be exactly layout.width x layout.height, else
 * FA_ERROR_SIZE_MISMATCH (new) and nothing is drawn. `dst_revision` is the frame revision dst holds
 * now (0 = unknown). It may be any of the last FA_SURFACE_HISTORY (8) revisions, so an app that
 * rotates 2-3 buffers passes the revision of the one it is about to reuse: only the area that changed
 * since then is drawn (the union of the dirty areas in between). Older or unknown: a full draw. */
FA_API fa_result FA_CALL fa_surface_update_into(fa_surface* s, fa_time time, void* dst, int32_t dst_stride,
                                                uint32_t dst_width, uint32_t dst_height,
                                                uint64_t dst_revision, fa_surface_frame* out_frame);

typedef struct fa_surface_frame {
    uint32_t struct_size;
    uint64_t revision;                 /* of the picture now in dst */
    uint64_t base_revision;            /* the dirty rectangles are relative to this (= dst_revision,
                                          or 0 after a full draw) */
    uint64_t layout_revision;
    uint32_t dirty_count;              /* 0 = nothing changed */
    fa_rect  dirty[8];                 /* bitmap pixels; more changes are merged into fewer rects */
    fa_rect  dirty_bounds;
    fa_time  time;                     /* position shown */
    int64_t  draw_ns;                  /* time this call took */
    int64_t  render_ns;                /* time the frame took to render (on a worker with render-ahead) */
    int32_t  behind;                   /* render-ahead: 1 = this is an older finished frame */
    /* CD+G only, as in fa_cdg_frame: */
    int32_t  background_index, border_index;
    uint8_t  background[4];
    uint8_t  palette[16][4];           /* R, G, B, A (straight): to colour INDEXED8 output on the GPU */
    uint8_t  transparency[16];         /* from transparent-colour packs, 0 opaque .. 63 invisible */
    uint64_t palette_revision;
} fa_surface_frame;
```

A frame drawn into your buffer is yours: the surface never writes it again unless you pass it to
`update_into` again. Dirty-rectangle drawing needs `dst` to still hold the frame of `dst_revision`. With
2-3 rotating buffers, each one comes back holding a frame 2-3 updates old, and the surface draws the
area that changed since that frame (it keeps the dirty areas of its last 8 revisions). A fresh staging
texture per frame means `dst_revision = 0` and a full draw.

How the surface does it (Keith's change 1 and 3): with presentation scaling on, and for lyrics, the
surface keeps its own rendered bitmap (the "master"), renders changes into it, and `update_into` copies
the changed area since `dst_revision` from the master into `dst`. That copy is the price of accepting
any recent revision; for a lyric wipe or a few CD+G tiles it is a few kilobytes. With scaling off
(classic CD+G) the picture is small enough to draw straight into `dst` as today.

### 2.4 Render-ahead (off by default)

With it on, the surface's own worker renders the next frames into a buffer of its own, ahead of the
clock (lyric lines are known in advance, CD+G packs too), and `update_into` copies the dirty rectangles
of the newest finished frame. It never waits; `behind = 1` says the frame is older than `time`. Copying
dirty rectangles costs a few kilobytes for a lyric wipe. Off, everything happens on the caller's thread
with no copy. We suggest off for CD+G (updates are cheap) and on for 2160p lyrics (section 8). The
workers are the engine's existing worker threads, not new ones per surface.

### 2.5 Background, transparent edge, alpha

With presentation scaling on (CD+G) and always for lyrics: premultiplied BGRA, transparent background,
every smoothing, blend and effect computed in premultiplied alpha against transparent, so there are no
halos over whatever you fill. The bitmap always has at least one fully transparent pixel beyond its
outermost visible pixel. New CD+G flag `FA_CDG_TRANSPARENT_BORDER`: the border is transparent too, and its
colour is still reported in `border_index`.

---

## 3. CD+G presentation options

```c
typedef struct fa_cdg_presentation {
    uint32_t struct_size;
    int32_t  scale;                    /* 0 = off (classic: today's path, exactly), 1 = on */
    fa_safety_zone safety_zone;        /* default SHOW (today's default; REMOVE = FA_CDG_NO_BORDER) */
    float    auto_ignore_seconds;      /* AUTO: border drawing before this is ignored (title cards) */
    int32_t  super_centered;           /* SUPER: the same crop on both sides */
    uint32_t padding;                  /* CD+G pixels of transparent space, default 0 */
    int32_t  smoothness;               /* 0..10, default 0; scale on only */
    int32_t  edge_blend;               /* 0/1, default 0; scale on only */
    uint32_t flags;                    /* FA_CDG_TRANSPARENT_BORDER, ... */
} fa_cdg_presentation;
FA_API fa_result FA_CALL fa_cdg_surface_set_presentation(fa_surface* s, const fa_cdg_presentation* p);
```

- **Scale off:** the native picture, as today (INDEXED8, BGRA or RGBA as configured), with the safety-zone
  crop and padding applied. Presentation size, smoothness, edge blend, effects: ignored.
- **Scale on:** a whole-number multiple N of (crop + margin + padding): the multiple nearest the
  presentation size, rounded up, as you asked. 1920x1080: Show is 5x (1080 high), Remove is 6x (1152).
  Premultiplied BGRA. The **margin** is at least one CD+G pixel and grows to the effects' reach.
- **Safety zone:** Show and Remove are fixed crops. **Auto** and **Super** come from a pre-pass that
  runs inside the checkpoint build we already do (same walk, about 1 ms more per song): Auto watches for
  tiles and scrolls that leave non-background pixels in the border area; Super keeps the union bounding
  box of everything drawn other than the background, snapped to the 6x12 tile grid. One crop for the
  whole song. Discs cannot be pre-passed, so Auto and Super fall back to Show there, and the layout
  says so (`safety_zone_resolved = 0`).

### 3.0 Scrolling (Keith's change 3)

A scroll is a shift, not a repaint. Scroll preset and scroll copy move the master bitmap by one tile
(6N or 12N output pixels) with a memory move, and only the band they expose is rendered, plus the
effects' reach and the smoothing reach inside the old picture next to it (the outline and contour of a
shape that now touches the band change). The smooth-scroll offsets (0-5 and 0-11 CD+G pixels) do not
render anything: the master covers the whole 300x216 screen with its margin, and the offsets only move
the window that is copied out of it, so an offset change is one copy of the bitmap. At 2160p that is
about 3-5 ms for a scroll step (memory moves) instead of a 95 ms repaint with effects.

### 3.1 Smoothness 0 to 10: the methods

The levels share one ladder: each one keeps everything the level below does and adds to it, so higher
is never rougher. None of them invents a colour that is not on the screen, except as a blend between
two neighbours in premultiplied alpha, so there are no colour fringes.

| Level | Method | Look |
|---|---|---|
| 0 | Nearest, whole-number multiple. Nothing invented. | Original pixels. |
| 1 | "Sharp bilinear": nearest, with a one-output-pixel anti-aliased band at each CD+G pixel boundary. | Crisp pixels, no shimmer. |
| 2-4 | Transition band widened to 0.25, 0.5 and 1.0 CD+G pixel (4 = plain bilinear). | Progressively softer. |
| 5 | **Contour mode.** Each colour region's boundary is traced at CD+G pixel corners (marching squares on the 16-colour grid) and drawn as a polygon with exact-area anti-aliasing at the output size. Level 5 straightens single-pixel stair steps into 45-degree edges. | Like the xBR family: strokes crisp, staircases gone. |
| 6 | Also straightens shallow steps (runs of 2-4 pixels) into the slope they approximate (digital straight-segment fitting). | Smooth slants. |
| 7-8 | Also rounds corners, with a growing radius; a right-angle corner whose both legs are at least 2 (7) or 3 (8) pixels long stays square, so serif and block letters keep their corners. | Smooth curves. |
| 9-10 | **Vectorization**: the traced polygons are fitted with curves, after Selinger's published potrace method (our own code from the paper; potrace itself is GPL and not used). 10 uses a lower corner threshold than 9. Drawn with the same exact-area rasterizer. | "True HD" lettering. |

Why contour mode rather than xBR, xBRZ or ScaleFX: xBRZ is GPL, so it cannot be in the binary, and the
xBR-family filters are built for photos of pixel art at 2-6x. CD+G lyrics are 16 flat colours on a grid,
and a traced outline gives the exact shape at any whole-number scale (10x at 2160p) with one rasterizer,
the same one that draws text. Regions are drawn as one partition of the picture (each edge pixel is
sampled 4x4 against the polygons), so two colours that touch never leave a background seam between them.

### 3.2 Edge blend: the method

Morphological anti-aliasing (MLAA, after Reshetov's published method) on the **CD+G grid**, not on
output pixels: edges between CD+G pixels are found, each run along an edge is classified as an L, Z or U
step, and the area of each output pixel on the far side of the line through the step is computed
exactly. Every edge output pixel becomes a premultiplied blend of the two colours by that area. Because
the grid is exact, there is no edge detection threshold to tune and nothing else is blurred.

At smoothness 5 and above the contours are already anti-aliased, so edge blend has nothing left to do
there: it is accepted at every level and documented as "no extra effect at levels 5-10", so the app can
leave it on while the user moves the smoothness slider (agreed with MediaControl), rather than letting it
blur the contours.

### 3.3 Internal quality and thin strokes

Effects and contours are computed at the output size. At small presentation sizes (N of 1 to 3) they run
at 2x internally and are box-filtered down, so a one-pixel "I" still gets a full outline and a visible
bevel. The bevel's width is limited to half the stroke's own width, so a thin stroke is never filled in.

---

## 4. Effects (both surfaces)

```c
typedef struct fa_fx_outline { int32_t on; uint32_t color, sung_color; float opacity, size, fade; } fa_fx_outline;
typedef struct fa_fx_shadow  { int32_t on; uint32_t color, sung_color; float opacity, size, distance,
                               angle_degrees, fade; } fa_fx_shadow;
typedef struct fa_fx_bevel   { int32_t on; uint32_t highlight, shade; float strength, size,
                               light_angle_degrees, fade; } fa_fx_bevel;
typedef struct fa_surface_effects {
    uint32_t struct_size;
    fa_fx_shadow shadow; fa_fx_outline outline; fa_fx_bevel bevel;   /* drawn in that order, under/over */
} fa_surface_effects;
```

Colours are 0xAARRGGBB, and `opacity` multiplies their alpha. `sung_color` is for lyrics; CD+G ignores it.

**How they are drawn.** All three come from one distance field: the shape's coverage at the output size
(text, or every CD+G pixel that is not background or transparent) goes through an exact Euclidean
distance transform (the linear-time Felzenszwalb-Huttenlocher method), corrected to sub-pixel accuracy
from the coverage. Then:

- **Outline** alpha = ease(distance / width): solid to `width * (1 - fade)`, then an eased (smoothstep)
  fall to 0 at `width`. With fade 1.0 it is a glow.
- **Shadow** = the shape plus outline, offset by distance and angle, with the same eased fade over its
  size (a soft shadow without a blur pass).
- **Bevel** = the distance field's gradient inside the shape, within `size` of the edge: facing the light
  gives the highlight, facing away the shade, scaled by strength and faded.

A true stroke, not 8 offset copies, so outlines are round and even and scale with the line (your quirks
Q5 and the heavier diagonal corners are gone). The field is computed only in the dirty area plus the
effects' reach.

**What 0.0-1.0 means** (1.0 is the maximum):

| | Unit | Outline 1.0 | Shadow size 1.0 | Shadow distance 1.0 | Bevel 1.0 |
|---|---|---|---|---|---|
| Lyrics | font size (em) | 0.12 em | 0.20 em | 0.15 em | 0.08 em |
| CD+G | one CD+G pixel | 1.5 px | 2 px | 2 px | 1 px |

Your current look in these units: outline 1.75 px at 72 px is about size 0.2, shadow (2, 2) px at 72 px is
about distance 0.26 at 315 degrees, size 0, fade 0.

---

## 5. Text rendering

**Recommendation: our own font reader and rasterizer, no third-party library.**

- **Reader:** TrueType (`glyf`) and CFF outlines, TrueType collections (`.ttc`, needed for the Japanese
  Windows fonts), `cmap` formats 4 and 12, `hmtx`, and kerning from both the `kern` table and `GPOS`
  pair adjustment. Every table read is bounds-checked and fuzzed, like the DLS and SF2 readers, because
  the app can hand us a user's font file. About 2,000 lines.
- **Rasterizer:** exact-area coverage (signed-area accumulation), grayscale anti-aliasing, sub-pixel
  positioning, no hinting (lyrics are 30 to 150 px high, where hinting does nothing useful, and
  DirectWrite does not hint at those sizes in grayscale either). The same rasterizer draws the CD+G
  contours of section 3.1.
- **Shaping:** cmap lookup, advances and pair kerning, which is what Latin, Greek, Cyrillic, kana, CJK
  and precomposed Hangul need. **No ligatures, on purpose:** a ligature would join two syllables into one
  glyph and break the wipe. Arabic, Hebrew and Indic shaping are out of scope, as you said for
  right-to-left text.
- **Finding fonts** (the OS's own lists): DirectWrite's system font collection on Windows (family and
  weight to font file), CoreText on macOS and iOS (CoreText gives the font's tables, so it works inside
  the iOS sandbox), the system font matcher on Android 10 and later (`/system/fonts` before that), and on
  Linux fontconfig loaded at runtime if present (as libcurl is), else a scan of the usual font folders.
  Bold comes from the family's bold face; a family without one gets a synthetic bold from the distance
  field. Variable fonts are read at their default instance. Or the app passes a font file or memory
  block.
- **Size:** about 60-80 KB of code, no tables. Licence: ours.

Why not the alternatives:

| Option | Size | Licence | Against it |
|---|---|---|---|
| FreeType + HarfBuzz, static | about +2 MB (FreeType alone 0.5-1 MB) | FreeType FTL (credit in docs) or GPLv2; HarfBuzz MIT | Most of the size buys complex-script shaping and hinting we do not use. The `.so` is 4 MB today. |
| stb_truetype | about +40 KB | public domain / MIT | Its author documents it as unsafe on untrusted fonts (no bounds checks), and its CFF support is basic. |
| Each OS's own text stack (DirectWrite, CoreText, ...) | small | OS | Four renderers that look different (the "implemented once" rule), and none on Linux or Android without Java. |

The font code sits behind its own internal interface (load face, map characters, advances and kerning,
outline of a glyph), so if CFF or GPOS kerning turns out to be a struggle, FreeType alone (without
HarfBuzz) can replace our reader without touching the API. The reader must cover the four things
MediaControl named: `.ttc` collections, CFF outlines (Hiragino, Noto Sans CJK), GPOS kerning, and good
anti-aliasing at small sizes (exact-area coverage, for subtitle mode). Each has its own test fonts.

**Japanese (or other) text the chosen font cannot show** (agreed with MediaControl): one font for the
whole song, never mixed within a line. If the style's font lacks most of a song's characters, the song
uses the OS's font for that script (Yu Gothic or Meiryo on Windows, Hiragino on Apple, Noto Sans CJK on
Android and Linux). A font the user set explicitly for that song or player always wins (a style flag,
`font_explicit`). The layout reports the family actually used in `font_used`.

If complex scripts are ever wanted, HarfBuzz can be added behind the same layout code later; nothing in
the API changes.

---

## 6. MIDI lyric surface

```c
typedef enum fa_lyric_preset { FA_LYRIC_CLASSIC = 0, FA_LYRIC_MODERN, FA_LYRIC_NEON, FA_LYRIC_MINIMAL,
                               FA_LYRIC_SUBTITLE } fa_lyric_preset;
typedef enum fa_lyric_wipe { FA_WIPE_CROSSFADE = 0, FA_WIPE_SWEEP } fa_lyric_wipe;

typedef struct fa_lyric_style {
    uint32_t struct_size;
    const char* font_family;           /* NULL = the system's default UI font */
    const void* font_data; size_t font_size_bytes;   /* or a font file in memory (copied) */
    const char* font_path;             /* or a font file */
    float    font_size;                /* at the 1080-high design canvas; default 72 */
    int32_t  font_weight;              /* 100..900, default 700 */
    uint32_t text_color, sung_color;   /* default 0xFFFFFFFF, 0xFFFFFF00 */
    fa_lyric_wipe wipe;                /* Classic: crossfade (yours); Modern, Neon: sweep */
    int32_t  wipe_cap_ms;              /* 600 */
    int32_t  lines_above, lines_below; /* 2, 1 */
    int32_t  subtitle_mode;            /* bottom band, 1-2 lines, no intro pages or countdown */
    float    vertical_position;        /* 0 top .. 0.5 centre .. 1 bottom */
    float    next_line_opacity;        /* 0.65 */
    int32_t  scroll_ms, page_ms;       /* 1500, 1000 (two options, as asked) */
    int32_t  countdown;                /* 0 off, 1 clock (default, as yours), 2 beat */
    int32_t  countdown_count;          /* 4 */
    uint32_t countdown_from, countdown_to; /* red, green */
    int32_t  intro_pages, ending_page; /* 1, 1 */
    int32_t  lead_ms;                  /* 400 */
    int32_t  panel; uint32_t panel_color; float panel_opacity, panel_radius, panel_padding;
    fa_surface_effects effects;        /* with sung colours */
    /* every hard-coded value of your quirk Q1 (slot pitch, per-slot scale and opacity, gaps,
     * timings) is a field here too, defaulting to your numbers */
} fa_lyric_style;
FA_API void FA_CALL fa_lyric_style_init(fa_lyric_style* style, fa_lyric_preset preset);
FA_API fa_result FA_CALL fa_lyric_surface_set_style(fa_surface* s, const fa_lyric_style* style);
```

It follows your design reference, timing for timing, with the twelve quirks treated as follows:

| Quirk | Ours |
|---|---|
| Q1 hard-coded values | All are style fields with your values as defaults. |
| Q2 scroll ignores its option | `scroll_ms` drives the scroll; `page_ms` the page fades. |
| Q3 wipe clip creeps into the next letter | Crossfade is clipped exactly to the syllable; sweep moves an anti-aliased edge across the syllable's own glyphs over its duration (eased, capped). |
| Q4 wrapped lines | No renderer wrapping. A line too wide for the box is split at a word boundary when the lines are built: each row becomes its own line with its own wipe and slot, and the rows of one split line stay on the same page. A single word too wide is scaled down to fit. |
| Q5 outline and shadow do not scale | Effects are in em units and scale with each line. The sung layer replaces the unsung one in the wiped region instead of stacking a second shadow. |
| Q6 layouts rebuilt every frame | Each line is laid out once; its unsung and sung layers (with effects) are cached at the largest scale it shows at and resampled during scrolls and page fades. A wipe frame only composites the syllable's rectangle. |
| Q7 countdown | Clock-based by default (yours), beat-based as an option (from the tempo map we already have); arrows are drawn as vector triangles, not a font glyph, and scale with the line. |
| Q8 extraction | Phase 0 below. |
| Q9 page fade-in overflow | The fade-in scale is clamped so the widest line still fits. |
| Q10 bounds | The block is sized from the slots actually shown. |
| Q11 overlapping page fades | A style option, `page_overlap`: 1 = overlap (yours, the default: new lyrics never arrive late), 0 = wait for the fade-out. |
| Q12 one active cue | Not applicable: the surface draws from the whole song's timeline, not cues. |

The surface stays stateless in the way yours is: every frame is a function of the position, so seeks
and loops need nothing special. Lyric times follow the stream's clock (tempo, pitch, seeks), and the
animations run in real time. The intro and ending pages use the `@T`/`@I` metadata (phase 0).

---

## 7. Text cleanup and Shift-JIS (phase 0)

```c
/* fa_open_options gains (at the end; struct_size keeps older callers working): */
int32_t text_code_page;   /* 0 = detect (default), 65001 UTF-8, 1252 Windows-1252, 932 Shift-JIS */
```

- **Detection, per file, not per event:** all text events of the file are scored together. Valid UTF-8
  (with or without a BOM) is UTF-8; otherwise the bytes are scored as Shift-JIS (valid lead/trail pairs,
  kana and common kanji ranges) against Windows-1252, and Shift-JIS wins only with a clear margin and no
  invalid pairs. The Shift-JIS table (JIS X 0208 plus the Windows 932 extensions) is generated from
  Unicode's published mapping files: about 15 KB in the binary.
- GBK, Big5 and EUC-KR are not detected (their byte ranges overlap Shift-JIS and each other, so guessing
  is unreliable, and each table is 20-40 KB). An explicit code page for them can follow if a user needs
  it.
- Chord lines (Tune1000 `%...`, Solton `<...>`) become `FA_MIDI_TEXT`, not lyrics.
- `/` and `\` are line and page marks anywhere in a fragment.
- Text-event fallback for `.kar` files without `@` headers or a "Words" track: the track with the most
  syllable-like text events (short, timed within notes) becomes the lyrics, and stray lyric events no
  longer turn the fallback off.
- `@I` lines are parsed (writer, lyricist, year, key, tempo, and so on). New `fa_stream_get_midi_credits`,
  so `fa_midi_info` does not grow again.

---

## 8. CPU cost: estimates now, measured tables with each phase

One core of a current desktop CPU (about 4 GHz), before render-ahead and before splitting a full
repaint across workers. Every phase publishes the measured table, on Keith's PC and on CI, in
`docs/features/cdg.md` and `docs/features/lyrics.md`; these are estimates from per-pixel costs.

**CD+G** ("typical" = one update during a lyric wipe, about 4 tiles; "full" = a seek or a new layout; a
screen clear to a transparent background is a fill and costs almost nothing; a scroll step is a shift
of the master plus the exposed band, about 1 ms at 1080p and 3-5 ms at 2160p with effects, section 3.0):

| | 1080p (5x) typical | 1080p full | 2160p (10x) typical | 2160p full |
|---|---|---|---|---|
| Scale off (today) | 0.01 ms | 0.3 ms | same | same |
| Smoothness 0 | 0.02 ms | 2 ms | 0.05 ms | 7 ms |
| 1-4 | 0.05 ms | 6 ms | 0.2 ms | 25 ms |
| + edge blend | +0.02 ms | +2 ms | +0.05 ms | +6 ms |
| 5-8 (contours) | 0.15 ms | 6 ms | 0.4 ms | 18 ms |
| 9-10 (curves) | 0.3 ms | 12 ms | 0.6 ms | 25 ms |
| + effects at maximum | +0.2 ms | +18 ms | +0.6 ms | +70 ms |

**Lyrics** (1920x1080 and 3840x2160 presentation size, font 72 at 1080):

| | 1080p effects off | 1080p effects max | 2160p effects off | 2160p effects max |
|---|---|---|---|---|
| Lay out and render a new line (both layers, ahead of time) | 1.5 ms | 5 ms | 6 ms | 20 ms |
| Wipe frame (one syllable's rectangle) | 0.03 ms | 0.05 ms | 0.1 ms | 0.2 ms |
| Scroll or page-fade frame (all lines resampled) | 3 ms | 3 ms | 12 ms | 12 ms |
| Intro or ending page (once) | 3 ms | 8 ms | 12 ms | 30 ms |

The numbers measured with the finished lyric surface, with and without render-ahead, are in [docs/features/lyrics.md](../features/lyrics.md#cost).

So: CD+G is cheap enough on the caller's thread except full repaints with effects at 2160p, which
render-ahead or a split across workers takes care of. Lyrics at 2160p want render-ahead during the
1-1.5 s scroll and page animations. The GPU path is not needed on these numbers; we would revisit it
only if the measured tables say otherwise.

---

## 9. .NET binding

```csharp
public interface IKaraokeSurface : IDisposable
{
    PixelSize PresentationSize { get; set; }     // default: unset
    SurfaceEffects? Effects { get; set; }
    bool RenderAhead { get; set; }
    SurfaceLayout GetLayout();
    SurfaceUpdate UpdateInto(IntPtr dst, int stride, int width, int height, ulong dstRevision,
                             TimeSpan? time = null);          // null = the stream's clock
}
public sealed class CdgSurface : IKaraokeSurface       // existing members unchanged
{
    public CdgPresentation Presentation { get; set; }  // record: Scale, SafetyZone, Padding,
}                                                      //   Smoothness, EdgeBlend, TransparentBorder, ...
public sealed class LyricSurface : IKaraokeSurface
{
    public static LyricSurface Create(AudioStream stream, LyricStyle? style = null);
    public LyricStyle Style { get; set; }              // LyricStyle.Preset(LyricPreset.Classic) with { ... }
}
public sealed record SurfaceEffects(OutlineEffect? Outline, ShadowEffect? Shadow, BevelEffect? Bevel);
public enum SafetyZone { Show, Remove, Auto, Super }
// StreamOptions.TextCodePage: Auto, Utf8, Windows1252, ShiftJis. AudioStream.MidiCredits.
```

`SurfaceUpdate` carries the revision, base revision, layout revision, dirty rectangles and timings.
`UpdateInto` also has a `Span<byte>` overload. Records with `init` properties keep the presets readable:
`LyricStyle.Preset(LyricPreset.Neon) with { FontSize = 64 }`.

---

## 10. Build order

Each step is one pull request with its docs, migration-guide rows, .NET binding and tests, merged when
clean. Nothing from step 1 on starts before this plan is reviewed.

| Step | What | Why here |
|---|---|---|
| (done) | The three CD+G fixes; these notes in the repo. | |
| 0 | Text cleanup and Shift-JIS (section 7); MIDI/DLS review items in their own PR. | Small, independent, useful at once, and the lyric surface builds on it. |
| 1 | Surface model (section 2): `fa_surface`, presentation size, layout report, `update_into`, precise dirty rectangles, timings, render-ahead. CD+G classic on it, byte-identical. | Everything else hangs off it. |
| 2 | CD+G presentation: safety zones and pre-pass, padding, scale on with margin, smoothness 0-4, edge blend, transparent border. First measured table. | Quick visible win on the format you use most. |
| 3 | Effects engine (section 4) on CD+G. | Shared; proven on CD+G before text. |
| 4 | Font reader, rasterizer and OS font lookup (section 5), with fuzz tests. | The lyric surface needs it. |
| 5 | Lyric surface (section 6) with the presets. | The main request. |
| 6 | Smoothness 5-10 (contours and curves). | The most research-like part; it does not block anything else. |

You test each step in your player and send screenshots, as section 5 of your request says.

---

**Progress:** step 0 done (PRs 80 and 81). Step 1 done (PR 82): `fa_surface`, `update_into` with the
8-revision history and up to 8 dirty rectangles, timings, the layout report, the presentation size and
render-ahead settings. Step 2 in two parts. 2a: the CD+G presentation options (safety zones with the
pre-pass, padding, presentation scaling with the margin, the master bitmap with scrolls as moves and
offsets as copies, smoothness 0-4, edge blend, the transparent border) and the first measured table
(docs/features/cdg.md). 2b: render-ahead on the engine's workers: two output-sized slots, a worker renders
the frame for the time the next call is expected at, `update_into` copies from the newest finished frame not
ahead of its time and never waits (when no worker is busy and no finished frame fits, such as the first frame
after a layout or option change, a seek, or a frame more than 1/60 s late, it draws one on the caller's
thread); `behind` allows 1/60 s, since frames rendered for a predicted time are usually a pack or two older
than the time asked for. Step 3: the effects (section 4) on CD+G, as specified, with the effects redrawn
incrementally around changed tiles and moved with scrolls, and their cost added to the measured table
(a full redraw with outline and shadow is about 28 ms at 1080p and 115 ms at 2160p, so render-ahead is
recommended with effects at 2160p). Not in step 3: section 3.3 (the 2x internal pass at N 1-3 and the bevel
limited to half a stroke's width), moved to step 6, which needs the same 2x path for contours. Step 4: the font reader
(TrueType and CFF outlines, CID-keyed CFF, collections, cmap 4/6/12, kern and GPOS pair kerning) and
the exact-area rasterizer, about 40 KB of code, with test fonts of our own, damage tests, and finding
fonts through each OS (registry list, CoreText, Android's system font list (not its font matcher, which crashes outside an app process), fontconfig, else the font
folders). Checked against FreeType on DejaVu, FreeFont, Inter (CFF), WenQuanYi (a collection) and Noto
Sans JP (CID-keyed CFF, 18 font dicts): a mean difference under 0.06 of 255 per pixel at 100 px; and
against HarfBuzz kerning: 8,400 pairs, none different. No public API yet: the lyric surface (step 5)
uses it. Step 5a done, merged as PR 87: the lyric surface (timeline, layout and drawing, C API, .NET, docs, tests; docs/features/lyrics.md). Step 5b done, merged as PR 88: the intro and ending pages. Step 5c done, merged as PR 89: render-ahead for lyrics (which also takes the pages' first-show cost off the caller's thread) and the measured cost table. Step 6 in two parts. 6a, this PR: smoothness 5-10 as in section 3.1, built so that it stays local: the outlines are traced along the pixel edges and smoothed by rules that look only a few pixels along each outline (no global optimal polygon, which would let a change move curves far away), and drawn with exact-area coverage summed in fixed point, so drawing only around a change and moving the picture for a scroll give exactly what a full redraw gives. Choices made: a one-pixel diagonal (two pixels of one colour touching at a corner) joins the colour that is scarcer in the 4x4 pixels around the corner; at 9-10 the potrace smoothing runs on the level-6 outline with legs taken up to 4 pixels and curves up to 2 pixels from a vertex, and right-angle corners with both sides at least 4 (9) or 3 (10) pixels long stay corners ("10 uses a lower corner threshold"); edge blend has no effect at 5-10. Measured: a full redraw at 2160p is 18-20 ms, a typical update 0.9-1.7 ms, most of it tracing the whole screen again (docs/features/cdg.md). 6b is next: section 3.3.

## 11. Questions, answered by MediaControl (2026-10-20)

Their answers are in [2026-10-20_MediaControl_Answers_Plan.md](2026-10-20_MediaControl_Answers_Plan.md)
and are worked into the sections above:

1. **Text engine:** our own reader, provided it covers `.ttc`, CFF, GPOS kerning and small-size
   anti-aliasing (it does, section 5); FreeType alone is the fallback, not HarfBuzz. Keith decides.
2. **Script font per song:** agreed, explicit user font wins, layout reports the font used (section 5).
3. **Edge blend at 5-10:** agreed, documented as "no extra effect", setting still accepted (3.2).
4. **Fixed lyric bitmap size:** agreed (2.2).
5. **Wipe per preset:** agreed; Classic cross-fades, Modern and Neon sweep. They may ask for Classic to
   sweep later.
6. **Page fades:** an option, overlap by default (section 6, Q11).
7. **Wrapped lines:** split at word boundaries when lines are built, rows of one line on one page (Q4).

Still open: lyrics for other formats (LRC, subtitle files) are out of scope for now; the surface takes
MIDI and `.kar` streams only.

---

## 12. Changes from Keith's review (2026-10-20)

1. **Rotating buffers:** `update_into` accepts any of the last 8 revisions and draws the area changed
   since then; anything older is a full draw (section 2.3).
2. **Palette in the frame:** `fa_surface_frame` carries the 16-colour palette and the transparency
   values for classic INDEXED8 output, so the app colours the indices on the GPU (section 2.3).
3. **Scrolling as a shift:** scroll steps shift the rendered bitmap and render only the exposed band
   plus the effects' reach; smooth-scroll offsets only move the copied window (section 3.0).
