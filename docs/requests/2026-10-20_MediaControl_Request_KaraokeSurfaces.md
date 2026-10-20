# FatzBomb.MediaControl: requests for karaoke surfaces (MIDI lyrics and CD+G)

To: FatzAudio. From: FatzBomb.MediaControl, 2026-10-20 (final, after review with the owner).
Companion notes: `2026-10-20_MediaControl_KaraokeDesignReference.md` (our karaoke design, every
number with its source line) and `2026-10-20_MediaControl_Review_MidiAndCdg.md` (our review of
your MIDI and CD+G code).

## Roles

FatzBomb.MediaControl is a video player. FatzAudio is the audio engine, and the karaoke formats
that travel with audio (CD+G graphics and MIDI/`.kar` lyrics) are yours: you decode them and draw
them. We pass you sizes and settings, take the pixels you return, and own everything after that:
placing them in our windows, scaling to each window, the background behind them, compositing
over video, and multiple windows. You never need to know how many windows we have or how big
they really are.

The goal: if we do this, it should be the best-looking karaoke there is, not a generic renderer.

## Summary of what we ask

1. A **MIDI lyric surface**, drawn in our design (or better), working like your CD+G surface.
2. **CD+G presentation options**: render at a presentation size, smoothness 0-10, edge blend,
   safety zone, margin and padding.
3. **Effects shared by both**: outline, shadow, bevel, each with size and fade.
4. **One surface model for both**: presentation size, layout report, drawing into our buffer,
   transparent background, dirty rectangles, rendering ahead, timings.
5. **Shift-JIS lyric text**, a new feature (neither of us has it).

Everything below is an option on a surface, can change while playing, and takes effect on the
next update.

---

# 1. The surface model (both kinds)

## Presentation size

`presentation_size` (width, height in physical pixels) is a property of each surface: the CD+G
surface and the lyric surface each have their own. It means "draw for this many pixels". We
usually pass our largest window's size, in its exact shape (aspect), and may pass a smaller size
in the same shape (half, a quarter) when we want to save work; we scale your result to our
windows ourselves. Clamped to at least the native CD+G size (300x216) and at most 3840x2160.
Unset means your defaults.

You fit your content inside it without distortion (uniform fit). Stretching or cropping to fill
is not wanted: it distorts the lyrics or hides them.

## Layout report and changes

- `get_layout(surface, &layout)`: the size of the bitmap you will return, where the picture (or
  lyric block) sits inside the presentation area, the CD+G crop box, the chosen safety-zone mode,
  and a `layout_revision`. For CD+G files it is valid once your pre-pass is done (before the
  first update).
- Changing any option that changes the size (presentation size, scale switch, safety zone,
  margin, padding, effects that grow the margin, font size, lines shown, layout mode) bumps
  `layout_revision` and makes the next update a full redraw. Every frame reports
  `layout_revision`, so our render thread notices on the next frame and reallocates. No extra
  callbacks are needed.
- We only pass a new presentation size when a window resize has finished, not during the drag.

## Drawing into our buffer

Today `pixels` belongs to the surface and is overwritten by the next update, so we would have to
copy every changed frame. We ask for:

- `update_into(surface, time, dst, dst_stride, dst_width, dst_height, &frame)`: draw into our
  buffer. If it is not the current layout size, return a size-mismatch result without drawing,
  and we reallocate.
- Dirty rectangles as today, in output pixels, so a lyric wipe uploads a few tiles.
- A frame drawn into our buffer is ours; when the buffer is a mapped GPU upload texture there is
  no copy at all.

## Background: transparent, we fill it

Both surfaces return a transparent background, and we draw the background colour behind them
across the whole window (around the picture, under video, with the user's opacity). For CD+G we
need, as today: the background colour (`background`, `background_index`) and its changes
(`palette_revision`, `FA_CDG_CHANGE_PALETTE` on the push event). New: the **border** made
transparent too (a flag), with its colour still reported (`border_index`), so we decide how the
border is drawn. All smoothing, blending and effects are computed in premultiplied alpha against
transparent, so edges blend correctly over whatever we fill (no dark or coloured halos).

## Transparent edge

Whoever scales a bitmap with interpolation needs transparent neighbours around its outermost
pixels. So every bitmap rendered at a presentation size has a transparent margin of at least one
pixel beyond its outermost visible pixel (text, outline, shadow, glow, CD+G shape). For CD+G this
is the built-in margin (section 2); for lyrics it is part of the lyric block.

## Performance

- **CPU first.** Implement everything on the CPU and publish the cost of an update for a full
  repaint and for a typical lyric wipe: at 1080p and 2160p, at each smoothness level, with
  effects off and at maximum.
- **Redraw only what changed**, plus the effects' reach around it.
- **Render ahead on your own threads** (or processes, your choice) for heavy redraws; an update
  returns the latest finished frame and never makes our render thread wait.
- **Report the time each update took** in the frame, so we can lower the presentation size when
  a machine cannot keep up.
- **GPU later, only if the numbers call for it**, and then on a device the app passes in
  (Direct3D11, Vulkan, Metal), drawing into a texture the app owns, with the same look as the
  CPU path. We do not want cross-device texture sharing.

---

# 2. CD+G presentation options

Your decoder is exact: it matched ours pixel for pixel on 9,500 fuzzed packs (see the review).
These options are about how it is presented.

## PresentationScale (off by default)

- **Off (classic):** you return the native picture as 16-colour indices and palette (INDEXED8),
  as today, with the safety-zone crop and padding applied (no margin: nothing is interpolated).
  Presentation size is ignored. This is exactly our current path, so the default changes nothing.
- **On:** you render at a whole-number multiple of the cropped picture (crop + margin +
  padding): the multiple nearest the presentation size, rounded up. You return that bitmap (full
  colour, premultiplied, transparent background) at that size; we do the final fit to our window
  on the GPU, which is free for us. Smoothness, edge blend and effects apply. At a 1920x1080
  presentation size: Show (216 high) renders at 5x, 1080 high; Remove (192 high) at 6x, 1152
  high, which we shrink slightly. Whole-number multiples keep pixels even at smoothness 0 and give
  the smoothing methods an exact grid.

## Safety zone

The CD+G screen is 300x216 with a 6-pixel border left and right and 12 at the top and bottom;
most discs keep their graphics inside the 288x192 picture area, some do not. Four modes, in both
scale modes, each changing the returned size:

- **Show:** the whole 300x216 screen, border included (today's default).
- **Remove:** only the 288x192 picture area (today's `FA_CDG_NO_BORDER`).
- **Auto:** a pre-pass over the whole file decides: if nothing other than the background is ever
  drawn in the border area, Remove; otherwise Show. Border presets (a solid border colour) do not
  count, only tiles and scrolls. Option: ignore border drawing in the first few seconds (title
  cards).
- **Super:** a pre-pass finds the bounding box of everything ever drawn other than the background,
  over the whole song, and returns only that box. Fixed for the whole song so the picture never
  jumps; snapped to the 6x12 tile grid so shapes are never cut; option to keep it centred (the
  same crop on both sides).

The pre-pass is the walk you already do to build checkpoints. Report the mode and box in the
layout and in `fa_cdg_info`. Discs cannot be pre-passed: there Auto and Super fall back to Show.

## Margin and padding

From the outside in, the returned bitmap is: padding, margin, the cropped picture.

- **Padding** (`padding`, default 0): transparent space around everything, so the bitmap is
  exactly the size we lay out and the padding scales with the picture. Arithmetic, not drawing.
- **Built-in margin** (PresentationScale on only): always at least one CD+G pixel, never zero,
  separate from padding. Room for smoothing to anti-alias the outermost edge, for effects to
  spread (it grows to their reach), and for our interpolation when we fit the bitmap.

## Smoothness 0 to 10

One quality number; you choose the method behind each level and may change it between versions.
It must be monotonic (higher is never rougher) and free of colour fringes.

- **0: original.** Sharp pixels, nothing invented.
- **1-4: soft.** From light smoothing up to bilinear-like softness.
- **5-8: sharp and smooth.** Edge-directed pixel-art upscaling (xBR, xBRZ, ScaleFX family):
  strokes stay crisp while their stair-steps round off.
- **9-10: true HD, as far as there is a method.** CD+G lyrics are flat-colour shapes on a tile
  grid, which suits vectorization: trace each colour region to curves (as in "Depixelizing Pixel
  Art", or potrace) and render the curves with anti-aliasing. If that is too slow per change, the
  best edge-directed upscaler with a final anti-alias pass.

## Edge blend (on or off, separate from smoothness)

Each edge pixel takes a colour and alpha computed from its neighbours, so stair-step edges are
anti-aliased without resizing. For example, a pixel whose top, left and top-left neighbours are
orange and whose others are background becomes part-orange with matching coverage.
Morphological anti-aliasing (MLAA, SMAA) does exactly this. A plain blur is not it: it softens
strokes instead of shaping their edges. Applies at any smoothness and size when PresentationScale
is on.

## Internal quality

How you get there is yours: supersampling, drawing effects at a higher internal resolution and
averaging down, whatever gives the best result per millisecond. The thin-stroke case matters: a
letter "I" is one or two CD+G pixels wide, and a bevel or outline on it must still look right at
small presentation sizes.

---

# 3. MIDI lyric surface

## The ask

Today you draw CD+G for us but MIDI and `.kar` lyrics only come as timed text
(`fa_stream_get_midi_text`, `FA_EVENT_MIDI_TEXT`). We ask for a lyric surface that works like the
CD+G surface (section 1), drawn in our design: one path for both kinds of karaoke. It is a plus
for both sides: every FatzAudio app gets lyrics that look finished, and our renderers get them on
every platform (our current lyric renderer is Windows/Direct3D11-only).

## Our design, and what not to copy

The design reference gives every number with its source line, and the sources, in the
FatzBomb.MediaControl repository: `FatzBomb.MediaControl.Rendering.Direct3D11/Subtitle/SubtitleTextRenderer.cs`
(drawing), `FatzBomb.MediaControl/Core/Subtitle/SubtitleCue.cs` (timing model),
`FatzBomb.MediaControl/Core/Audio/MidiLyricExtractor.cs` and `MidiMetadataExtractor.cs` (text to
lines, pages and metadata), `FatzBomb.MediaControl/Core/Configuration/KaraokeTextStyle.cs`
(options). In short:

- **Layout:** lines centred in a box 85% of the width, in slots 1.55 x the font size apart: the
  line being sung at full size, the next line below at reduced opacity and scale, the lines just
  sung above, fading and shrinking (scales 1.0, 0.8, 0.6; opacities 1, 0.65, 0.5, 0.25). Lines
  scroll up a slot as each is finished, eased.
- **Wipe:** each syllable changes from the unsung to the sung colours as it is reached, eased
  over the shorter of 600 ms and 80% of the gap to the next syllable (under 150 ms: instant).
  Ours is a colour cross-fade per syllable. Offer both: a true left-to-right **sweep** across the
  syllable over its duration, and the cross-fade.
- **Pages:** a paragraph mark or a 2-second pause clears the page: the old page fades out
  shrinking to 75%, the new one fades in from 150%.
- **Countdown:** before a line that follows a gap of more than 6 s, four arrows above it count
  down the last 5 seconds, one turning green per second, then fade.
- **Intro and ending:** before the first lyric and after the last, the song's metadata (title,
  artist, credits from `@I` lines, length, key, tempo, copyright) on bands or a card.

Section 8 of the reference lists twelve quirks of ours not to copy, among them: many hard-coded
values that should be options; the line scroll ignores its option (1500 ms fixed); the wipe clip
creeps into the next letter and flickers mid-word; wrapped lines are wiped wrongly; outlines and
shadows do not scale on smaller lines; the countdown follows the clock, not the beat (beat-based
would be better: the MIDI tempo map is known).

## Sizing

Text has no native size, so the lyric surface draws exactly at its presentation size (no
whole-number steps). The design is defined against a 1080-pixel-high canvas (font size 72, slot
spacing, everything), multiplied by presentation height / 1080 when drawn, so proportions never
change and text is as sharp as the presentation size allows. The width follows the presentation
area's shape: the text box is 85% of the actual width, so wide or narrow areas use their width
instead of letterboxing the lyrics. You return only the lyric block (or the bottom band in
subtitle mode), with its position inside the presentation area in the layout report, not a full
transparent canvas.

## Layout modes and lines shown

- **Lines shown:** how many sung lines stay above the current one and how many upcoming lines
  show below (ours: up to two above, one below).
- **Subtitle mode:** one or two lines at the bottom of the area, smaller, without intro pages and
  countdown, for lyrics over video.

## Design presets and options

A few complete looks the app picks by name, each setting every option below; individual options
override the preset. For example **Classic** (ours: white, yellow wipe, black outline and
shadow), **Modern** (slimmer font, soft panel, gradient wipe), **Neon** (glow instead of
outline), **Minimal** (no outline, light shadow) and **Subtitle**. All must look designed, not
like defaults.

| Option | Our default |
|---|---|
| Font family, or a font file / memory blob; default picked per system | Segoe UI on Windows |
| Font size (at the 1080-high design canvas) and weight | 72, bold |
| Unsung text colour; sung (wipe) text colour | white `FFFFFFFF`; yellow `FFFFFF00` |
| Effects (section 4) with unsung and sung colours | outline `E0000000`, shadow `A0000000` |
| Wipe style: sweep or cross-fade; time cap per syllable | (new); 600 ms |
| Lines shown above and below | 2 above, 1 below |
| Layout mode: karaoke (centred) or subtitle (bottom) | karaoke |
| Vertical position of the lyric block | centre |
| Next-line opacity | 0.65 |
| Line scroll time; page transition time (separate options) | 1500 ms; 1000 ms |
| Countdown: on/off, count, colours, clock- or beat-based | on, 4, red to green, clock |
| Intro and ending metadata pages | on |
| Lyric lead (show a line before it is sung) | 400 ms |
| Background panel behind the lyric block: on/off, colour, opacity, corner radius, padding | off |
| Design preset | Classic |

Fonts: the user picks the font and you pick a sensible default per system; if a system lacks
glyphs for a song's text, that is the user's to fix (font fallback and right-to-left text are out
of scope). Text quality: anti-aliased glyphs with subpixel positioning, proper shaping and
kerning, comparable to DirectWrite.

## Text: cleanup and Shift-JIS (new feature)

- **Shift-JIS** (common in Japanese karaoke `.kar` files) comes out garbled today in both our code
  and yours (`dec_midi.c:340-358`: UTF-8 or else Windows-1252). Please detect it (score the bytes
  as Shift-JIS before the Windows-1252 fallback) and let the app force a code page per file.
- Chord lines (Tune1000 `%...`, Solton `<...>`) are not lyrics.
- `/` and `\` are line and page marks wherever they appear in a fragment, not only at the start.
- Fall back to text events for lyrics in `.kar` files without `@` headers or a "Words" track
  (details in the review).

## Tempo

CD+G surfaces follow `fa_stream_get_clock`, and `fa_midi_text` times are on the same timeline,
so graphics and lyrics follow tempo, pitch, seeks and loops. The lyric surface keeps that.
Animations (line scroll, page transitions, the wipe cap) stay in real time: a faster tempo must
not speed them up.

---

# 4. Effects (shared by CD+G and MIDI lyrics)

One effects block for both surfaces. For CD+G it applies to the lyric shapes (everything that is
not background or transparent) when PresentationScale is on; for MIDI lyrics to the text, with
separate unsung and sung colours. Drawn in this order: shadow, outline, shape or text, bevel.

| Effect | Options |
|---|---|
| Outline (stroke around the shapes; with fade 1.0 it is a glow) | on/off, colour, opacity, size 0.0-1.0, fade 0.0-1.0 |
| Shadow (offset copy under the shapes) | on/off, colour, opacity, size 0.0-1.0, distance 0.0-1.0, angle, fade 0.0-1.0 |
| Bevel (light upper-left edge, dark lower-right edge) | on/off, highlight and shade colours, strength, size 0.0-1.0, light angle, fade 0.0-1.0 |

**Size and distance, 0.0 to 1.0.** You decide what they mean, relative to the shapes and the
resolution you render at (stroke width or letter height for CD+G, font size for lyrics), so one
setting looks right on CD+G and on text at every presentation size. 1.0 is your maximum; there is
no bigger.

**Fade, 0.0 to 1.0**, for each effect with a width:
- **0.0:** no fade: solid out to the effect's full width.
- **0.5:** solid for the inner half, then fades to transparent by the outer edge.
- **1.0:** fades over the whole width, from the shape's edge outwards (a soft glow or shadow).

The fade is eased (smooth, not linear), so its end is invisible. Effects need room outside the
shapes: the margin grows to their reach.

---

# 5. Review

You build; we test visually in our player and report back what we see, with screenshots.
