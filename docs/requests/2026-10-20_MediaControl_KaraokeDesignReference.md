<!-- Written 2026-10-20 from the code at commit 561a4ba; file:line references are to that commit. -->

# MIDI/.kar Karaoke Lyric Display: Design Specification

Source of truth (all paths relative to /home/user/FatzBombMedia):

- STR = FatzBomb.MediaControl.Rendering.Direct3D11/Subtitle/SubtitleTextRenderer.cs
- SC  = FatzBomb.MediaControl/Core/Subtitle/SubtitleCue.cs (KaraokeCueTiming, KaraokeLineTiming, KaraokeTextSegment, KaraokeCountdownState)
- MLE = FatzBomb.MediaControl/Core/Audio/MidiLyricExtractor.cs
- MME = FatzBomb.MediaControl/Core/Audio/MidiMetadataExtractor.cs
- KTS = FatzBomb.MediaControl/Core/Configuration/KaraokeTextStyle.cs
- MPC = FatzBomb.MediaControl/Controllers/MediaPresentationCoordinator.cs

Architecture: MLE (BASS MIDI markers) produces a list of cues. Each cue carries a pure-data `KaraokeCueTiming`. The renderer is a pure function `RenderKaraoke(timing, position, 1920, 1080)` that returns a premultiplied BGRA bitmap plus its offset. It keeps no state between frames. Any time-dependent value is derived from `position`, so seeking in either direction needs no special handling. A reimplementation only needs three things: the timing data model (sections 2 and 3), the per-frame draw rules (sections 4 to 6), and the style options (section 7).

"Position" means media position in seconds plus the user subtitle offset, clamped to >= 0 (MPC:1366-1378).

---

## 1. Canvas and coordinate system

- **Canvas size.** The canvas is a fixed logical 1920x1080 (MPC:25-26, passed at MPC:1395). The renderer is resolution-independent only through this fixed canvas. The resulting bitmap is composited over the video elsewhere.
- **Font size.** `FontSize` is in canvas pixels, so the default 72 means 72 px on the 1920x1080 canvas (STR:119, KTS:28). It is not a fraction of the canvas height. To reproduce this at other sizes, scale the whole canvas, or scale FontSize by H/1080.
- **Layout box.** `layoutWidth = 0.85 * canvasWidth` = 1632 px (STR:122). The box is centred horizontally, so the left and right margins are 7.5% (STR:152, 380). The maximum box height is `0.2 * canvasHeight` (STR:123, 362).
- **Alignment.** Text is `TextAlignment.Center` inside the box (STR:130, 376). Paragraph alignment is top and word wrap is the DirectWrite default (on). The text block's left edge is `boxLeft + max(0, (boxWidth - measuredWidth)/2)` (STR:382). This left edge is the origin for wipe clip rectangles and countdown arrows.
- **Vertical anchor.** The centre line is `middleY = 0.5 * H` = 540 (STR:141). A line's top is `y = centerY - lineHeight/2`, where lineHeight is the measured layout height (STR:378-379).
- **Slot pitch.** `slotDistance = 1.55 * FontSize` = 111.6 px at the default size (STR:142). It is constant and does not depend on the scale of the lines or on their wrapped height.
- **Padding.** `padding = ceil(max(OutlineWidth, WipeOutlineWidth, |ShadowOffsetX|, |ShadowOffsetY|))` (STR:143-145). It is used for layer and clip rectangles. The descender allowance below each line is `0.35 * effectiveFontSize` (STR:383).
- **Output bitmap and dirty rectangle** (STR:168-179, 860-877).
  - If the intro card/bands, the end card, or a page fade is active, the bitmap is the full canvas (0,0,W,H).
  - Otherwise it is a tight rectangle:
    - Left is the minimum text left over all layouts.
    - Right is the maximum text right. When a countdown is visible, the maximum is raised to at least `layoutCentre + 4*arrowWidth + 3*0.12*FontSize`.
    - Top is `middleY - 2*slot - maxLineHeight/2 - countdownTopExtension`. The extension is `arrowHeight + 0.1*FontSize` when any line shows a countdown, else 0.
    - Bottom is `middleY + 3*slot + maxLineHeight/2`.
  - CreateBounds then adds `max(2, ceil(pad)+2)` pixels on every side plus the shadow offset on the offset side, and clamps to the canvas.
- **Re-render trigger** (SC:42-70, MPC:1377-1389). A new bitmap is produced only when the cue text or `GetProgressRevision` changes. The revision is a hash of:
  - per line: highlighted length, `(int)(wipeFraction*100)`, and countdown revision (`(int)(secondsSinceCountdownStart*60)`, or -1 when hidden);
  - the same values for outgoing lines, and `ClearsPage`;
  - transition frame index `(int)(elapsed*60)` (page fade-out frames at 0.., fade-in frames at 10000+, -1 otherwise);
  - intro opacity*100, end-card opacity*100, end-lyric opacity*100;
  - three booleans: position >= intro fade-out end, position >= MediaDuration, position >= EndLyricFadeEnd.
  - Effect: animation is quantised to 60 Hz and the wipe to 1% steps. A GPU renderer can simply draw every frame.

---

## 2. Text preparation (MIDI to lines, pages, segments)

### 2.1 Event source and filtering (MLE:42-52, 84-120, 504-521)

1. Read BASS MIDI markers of type Lyric (meta 5) and Text (meta 1).
2. Lyric markers are used if any survive filtering. Otherwise the Text markers are used (MLE:91-92).
3. Filtering drops a marker when its text is empty or starts with `@` (Soft Karaoke headers) or `%` (Tune1000/Solton chord lines) (MLE:513-514).
   - Quirk: a real lyric that begins with `@` or `%` is also dropped.
4. Markers with non-finite or negative time are skipped (MLE:102-103).
5. **Lead time.** Every syllable start is shifted 400 ms earlier, clamped to >= 0 (`LyricLeadOffset`, MLE:21, 105-109). The purpose is that the highlight starts ahead of the note, as CDG does.
6. If the media duration is unknown, it is taken as `lastFragment.Start + 3 s` (MLE:117-118).
7. Fragments are sorted by start time before line building (MLE:368).

### 2.2 Control characters and line building (`BuildLines`, MLE:359-422)

Constants (MLE:15, 22-23): `PreferredLineLength = 42`, `ParagraphGap = 2 s`, `SentenceGap = 0.9 s`.

Inside a fragment, these characters are control characters:

| Char | Meaning | Action (MLE) |
|---|---|---|
| `\` or LF | Clear the screen (new page) | flush the current line, `paragraph++` (392-398) |
| `/` or CR | Next line | flush the current line (399-404) |
| `<` | Solton: a full line follows | flush the current line (405-409) |

Text before the control character in the same fragment is added to the old line first. All parts of a fragment share the fragment's start time. After the control character, `lineStart = fragment.Start`.

Before processing a fragment (MLE:373-385), `gap = fragment.Start - previousFragment.Start`. This is the start-to-start gap, not note length. The check applies only when the fragment does not start with `\ / CR LF <` and the current line is non-empty:

- If `gap >= 2 s`: flush the line and start a new page (`paragraph++`).
- Else if `gap >= 0.9 s` and the line, ignoring trailing whitespace, ends in `. ! ? : ;`: flush the line only. The page stays the same.
- Otherwise, if the raw line length is >= 42 and the last character of the line so far is whitespace, flush the line (the "42-character rule", MLE:382). This check also runs for fragments that start with a control character.
  - Quirk: it needs the trailing-space convention ("Hel" "lo " "world "). Files that put the space at the start of the next syllable (" world") never reach a break, so lines grow unbounded and the renderer's word wrap takes over.

### 2.3 Normalisation (`NormalizeParts`, MLE:457-489)

- All whitespace runs collapse to a single space. Leading and trailing whitespace of a line is removed.
- Each non-empty part becomes a `KaraokeTextSegment(TextStart, TextLength, Start)`.
  - The segment covers the first to last non-space character appended by that part.
  - The separating space is not part of any segment, so a space belongs to the gap between segments.
  - Parts that contain only whitespace produce no segment.
- Empty-text lines are discarded.

### 2.4 Pages and countdown flags

- A **page** is a run of lines with the same `Paragraph` index. `Paragraph` increments on a gap >= 2 s, `\`, or LF.
- `MarkCountdownLines` (MLE:562-581): a line gets `HasCountdown = true` when `firstSyllableStart - previousLine.lastSyllableStart > 6 s`.
  - `CountdownGap = 6 s` (MLE:562).
  - For the first line, previous end = 0.
  - The check is per line, so it also applies mid-page.
  - "Last syllable start" is the start of the last segment, not an end time (MLE:308).

### 2.5 Cue and line-window construction (MLE:122-213, 319-357)

- One cue is produced per line, so cue i means "line i is the active line".
  - Cue 0 starts at 0.
  - Cue i starts at `schedule[i].Start`.
  - A cue ends where the next cue starts. The last cue ends at media duration, or at least `start + 100 ms`.
- Each cue includes a window of up to 2 lines before and 2 lines after the active line, only lines of the same page (`CreateLineWindow`, MLE:326-331). This gives slots -2..+2.
  - On a page-clearing cue it also includes `OutgoingPageLines`: the previous page's active line and up to 2 before it, in the same page, and no lines after it (MLE:341-357).
- The coordinator renders karaoke only when exactly one cue is active (MPC:1377).
- End fade (MLE:156-164): `endFadeStart = lastLine.lastSyllableStart + 5 s`, `endFadeEnd = +1000 ms`.
  - If that exceeds the duration, `endFadeEnd = duration` and `endFadeStart = max(lastSyllableStart, duration - 1 s)`.

### 2.6 Metadata fields (MME)

Source markers are the merged, sorted Lyric+Text events, including the `@` lines that the lyric filter removes (MLE:50-52).

- `@T<text>` (case-insensitive): collected in order. Title = 1st, Artist = 2nd. A 3rd becomes Copyright if it contains "copyright", "(c)" or the copyright sign, and Copyright is empty. Otherwise it goes to `AdditionalInfo` (MME:104-135).
- `@I<text>`: labelled information. Labels match case-insensitively by prefix; the remainder is trimmed of `space : - =` (MME:178-234).

| Field | Labels |
|---|---|
| title | "title", "song" |
| artist | "artist", "performer", "performed by" |
| writer | "written by", "writer", "composer", "music by" |
| lyricist | "lyricist", "lyrics by", "lyrics" |
| year | "year", "date" |
| copyright | "copyright", "copyright information" |
| key | "key" |
| tempo | "tempo", "bpm" (gets " BPM" appended if missing) |
| timing | "timing", "time signature", "meter" |

- An unlabelled `@I` value is handled in this order:
  - copyright if it looks like one;
  - year if it is 4 digits in 1800..2200;
  - otherwise it goes to `AdditionalInfo`, with distinct case-insensitive de-duplication.
- Standard MIDI fallbacks (MME:27-54):
  - The title falls back to the lowest-track Track Name meta event with non-blank text, then to the file name without extension, then to "MIDI Track" (MME:27-31, 67-71).
  - A MIDI Copyright meta event (joined with " • " when several) overrides the copyright.
  - Key signature meta (two numbers: accidentals -7..7, mode 0 or 1) is formatted as a key name. Major keys are `Cb Gb Db Ab Eb Bb F C G D A E B F# C#`, minor keys are `Abm Ebm Bbm Fm Cm Gm Dm Am Em Bm F#m C#m G#m D#m A#m`, indexed by `accidentals+7` (MME:173-175). Unparseable input is shown as is.
  - The time signature uses the first token of the time-signature meta event.
  - The tempo comes from the first positive Tempo event: `round(60e6 / usPerQuarter)` shown as "N BPM". It overrides any text tempo.
- `IsFallback = true` when no embedded metadata field is non-blank (title is not counted when it is merely the file name) (MME:56-65, 84). The `Duration` field is the media duration.

---

## 3. Layout: slots, scale, opacity, wrapping

Let `rel = lineIndexInWindow - ActiveLineIndex`, an integer from -2 to +2. A line's vertical centre is `centerY = middleY + rel * slotDistance` (STR:212, 236).

| rel | Stable opacity (STR:825-833) | Stable scale (STR:835-841) | centerY (FontSize 72, H 1080) |
|---|---|---|---|
| -2 | 0.25 | 0.60 | 316.8 |
| -1 | 0.50 | 0.80 | 428.4 |
| 0 (active) | 1.00 | 1.00 | 540 |
| +1 | `NextLineOpacity` (0.65) | 0.80 | 651.6 |
| +2 | `NextLineOpacity * 0.55` (0.3575) | 0.60 | 763.2 |
| other | 0 | 0.40 | not drawn |

- **Scale.** Scaling is applied to the font size and to the layout width. `effectiveFontSize = FontSize * scale * presentationScale`; `scaledLayoutWidth = 0.85W * scale * presentationScale`; `scaledLayoutHeight = 0.2H * scale * presentationScale` (STR:358-362). Each line is still centred on the canvas centre. Positions are not scaled, only the glyphs. Outline width, shadow offset, countdown arrow size and the countdown gaps are not scaled with the line (see section 8).
- **Wrapping.** There is no explicit wrap policy beyond the DirectWrite default inside `scaledLayoutWidth`. Because the width scales with the font, a line wraps at the same character position at every scale. The 42-character rule normally keeps lines on one row. A wrapped line has a larger height, and the line is vertically centred on its slot. Slots are not pushed apart, so multi-row lines overlap neighbours. The wipe math (section 5) assumes a single row.
- **Stacking order.** Lines are drawn in window order, index 0 upward, each as one group layer.
- **Group opacity.** The line's opacity is applied once to the whole composed line (text, outline, shadow, countdown arrows, wipe passes) via `PushLayer` with `Opacity = opacity`, ContentBounds `[left-padding, top-arrowHeight-0.1*FontSize-padding, left+width+padding, bottom+0.35*effFont+padding]` (STR:389-400). This prevents overlapping outline and shadow draws from showing through each other at partial opacity.
- Lines with opacity <= 0 are skipped (STR:355).

### 3.1 Normal line transition (cue not page-clearing, STR:252-284, 798-823, 843-849)

- `p = GetTransitionProgress(position)` = linear clamp of `(position - TransitionStart) / (TransitionComplete - TransitionStart)`; 1 if the duration <= 0 (SC:72-83).
- `s = SmoothStep(p) = p*p*(3-2p)` (STR:856).
- `hasPreviousLine = ActiveLineIndex > 0` within the window. This is false only for cue 0 and for a page's first line, which is always a page-clearing cue (page-clearing cues are handled in section 4.2).
- If `hasPreviousLine`, for each line:
  - `centerY = lerp(middle + (rel+1)*slot, middle + rel*slot, s)`: the line moves up by one slot.
  - `opacity = lerp(stableOpacity(rel+1), stableOpacity(rel), s)`.
  - `scale = lerp(stableScale(rel+1), stableScale(rel), s)`.
- If not (cue 0): there is no movement. `opacity = lerp(0, stableOpacity(rel), s)` and scale is the stable scale. This is the initial fade-in of the first page.
- All opacities are multiplied by `endLyricOpacity` (section 4.4).

### 3.2 When the transition runs (scheduling, MLE:215-259)

- **Scroll duration.** `TargetScrollDuration = 1500 ms` is hard-coded (MLE:20). It is not driven by `LineTransitionMilliseconds`.
- **Scroll within a page** (`CalculateScrollTransition`):
  - `nextPresentationStart = nextLine.firstSyllable - (HasCountdown ? 5 s : 0)`, floored at 0.
  - `currentEnd = currentLine.lastSyllableStart`.
  - `gap = nextPresentationStart - currentEnd`.
  - If `gap >= 1.5 s`: scroll ends exactly at `nextPresentationStart` and starts 1.5 s earlier.
  - Else: the scroll is centred in the gap (start = gapCentre - 0.75 s), then shifted later if needed so it never starts before the current line's first syllable.
  - `Start` is also forced to be >= previous schedule start + 1 tick (MLE:145-148).
- **Initial fade-in of the first page** (`CalculateInitialTransition`):
  - `Complete = max(0, firstSyllable - preview)`, where `preview = 5 s` if line 0 has a countdown, else `TargetPagePreview` = 1 s (MLE:18, 255).
  - `Start = max(0, Complete - transitionDuration)`.
  - `transitionDuration` is the configured `LineTransitionMilliseconds`, default 1 s (MLE:127-129, PlaybackCoordinator.cs:27, 326).

---

## 4. Animation

### 4.1 Easing definitions (exact)

- SmoothStep(t) = `t*t*(3-2t)` (STR:856, SC:214).
- EaseInCubic(t) = `t^3` (SC:215).
- EaseInOutCubic(t) = `t<0.5 ? 4t^3 : 1 - (-2t+2)^3 / 2` (SC:216-218).
- EaseInOutQuint(t) = `t<0.5 ? 16 t^5 : 1 - (-2t+2)^5 / 2` (STR:851-854).
- Lerp(a,b,t) = `a + (b-a)t`.
- LerpColor is a per-channel lerp of straight (non-premultiplied) ARGB bytes including alpha, truncated to byte (STR:891-907).

### 4.2 Page clear (page-clearing cue, `ClearsPage`)

Applies when line i-1 is in a different page from line i. The old page is the "outgoing" set; the new page is the "incoming" set. Timing (`CalculatePageTransition`, MLE:261-306), where `T = transitionDuration` (1 s default):

- `currentPageEnd` = the last syllable start of the old page's last line. `nextPageStart` = the first syllable start of the new page. `totalGap = nextPageStart - currentPageEnd`.
- If `totalGap <= 0`: everything is instantaneous. FadeOut start = end = `currentPageEnd`, FadeIn start = end = `nextPageStart`.
- `presentationLead`:
  - with a countdown on the new first line, `min(5 s, totalGap)`;
  - otherwise `min(1 s, max(0, totalGap - 2T))`.
- `incomingReady = nextPageStart - presentationLead`. `transitionWindow = max(0, incomingReady - currentPageEnd)`. `fadeDuration = min(T, transitionWindow)`.
- `outHold = 0`, or `min(2 s, transitionWindow - 2*fadeDuration)` when `transitionWindow >= 2T` (`TargetPageHold = 2000 ms`, MLE:17).
- `outStart = currentPageEnd + outHold`; `outEnd = outStart + fadeDuration`; `inEnd = incomingReady`; `inStart = inEnd - fadeDuration`.
- If the window is shorter than 2T, the fades can overlap (outEnd > inStart).
- The cue's `TransitionStart = outStart` and `TransitionEnd`/cue end = next cue start.

Rendering (STR:201-251):

- **Outgoing lines.** Drawn with `relLine = i - OutgoingActiveLineIndex`. The outgoing set contains only the active line and up to 2 before it, and each line keeps its slot.
  - `fadeOut = EaseInCubic(clamp((pos - outStart)/(outEnd - outStart)))`, and 1 if the duration <= 0 (SC:85-93).
  - opacity = `stableOpacity(rel) * (1 - fadeOut) * endLyricOpacity`.
  - presentationScale = `lerp(1, 0.75, fadeOut)`.
  - `scrollProgress = 1`, so the stable per-slot scale applies.
  - The highlighted (sung) state keeps being drawn at the current position.
- **Incoming lines** (all lines of the window of the new page, slots 0..+2 and so on).
  - `fadeIn = EaseInOutCubic(clamp((pos - inStart)/(inEnd - inStart)))` (SC:95-105).
  - Its duration is `inEnd - inStart`. If that is zero, it falls back to the configured transition duration, shortened to `TransitionEnd - IncomingStart` when that is positive and shorter (SC:180-186). If the resulting duration is <= 0, fadeIn = 1 when `position >= PageFadeInEnd`, else 0.
  - opacity = `stableOpacity(rel) * fadeIn * endLyricOpacity`.
  - presentationScale = `lerp(1.5, 1, fadeIn)`, which shrinks the page from 150% to 100% as it fades in.
  - Quirk: at 150% the layout box is 2448 px wide, so long lines overflow the canvas and get clipped.
- Between outEnd and inStart the page is empty (blank screen).
- Both sets are drawn centred on their slot centres. The scale pivots at each line's own centre, not at the page centre.
- A page effect makes the dirty rectangle the full canvas while `[outStart,outEnd)` or `[inStart,inEnd)` is active (STR:134-139).

### 4.3 Intro metadata (SC:17-22, 107-135, 188-190; STR:186-199, 286-298)

- Constants: fade starts at 1 s (`IntroFadeInStart`), fade duration 1 s, `IntroClearLead = 3 s`, `SplitBandEnd = 8 s`.
- Opacity:
  - 0 before 1 s;
  - `SmoothStep((pos - 1 s)/1 s)` over 1 s to 2 s;
  - 1 until `fadeOutEnd - 1 s`;
  - `1 - SmoothStep(...)` over the last 1 s to `fadeOutEnd`;
  - 0 after `fadeOutEnd`.
  - It is 0 whenever `Metadata == null`.
- `FirstPresentationStart` = `max(0, firstSyllableOfLine0 - 5 s)` if line 0 has a countdown, else the first syllable start (MLE:151-154).
- **Mode selection.** `UseSplitMetadataBands = FirstPresentationStart < 8 s`.
  - Split bands: `fadeOutEnd = min(8 s, MediaDuration)`.
  - Centre card (`FirstPresentationStart >= 8 s`): `fadeOutEnd = max(2 s, FirstPresentationStart - 3 s)`.
- Draw order: split bands are drawn first, behind the lyric lines. The centre card is drawn after all lyrics, on top. The end card is drawn last.
- The intro is shown also for fallback metadata (title = file name). Fallback only changes font scale and omits some fields (see below).

**Centre card layout** (STR:520-602, `DrawMetadataCard`; F = FontSize):

- Font sizes:
  - title `F*0.92` (`F*0.65` if fallback), Bold;
  - artist `F*0.55`, SemiBold;
  - info `F*0.31`, Normal;
  - copyright `F*0.23`, Normal Italic.
- Box: `boxWidth = W*0.72` (`W*0.5` if fallback). `textWidth = boxWidth - 0.9F`. Padding `0.45F`. Corner radius `0.22F`.
- Spacing: gap title to artist `0.13F`; section gap before info and before copyright `0.2F`.
- Max layout heights: title `0.15H`, artist `0.1H`, info `0.24H`, copyright `0.08H`. All are centre-aligned, wrapping within `textWidth`.
- Content height = sum of the measured heights plus the gaps. `boxHeight = content + 2*padding`. The box is centred on the canvas both ways.
- Sections are omitted when empty (artist, info, copyright).
- Smoked glass panel (`DrawSmokedGlassPanel`, STR:703-714; all alphas multiplied by opacity):
  - drop shadow: a rounded rectangle offset (+8, +10), black alpha 0.42;
  - glass fill: RGBA (0.055, 0.075, 0.11, 0.88);
  - sheen: a rounded rectangle inset 3 px, height `0.42*boxHeight`, radius `max(1, r-3)`, RGBA (0.2, 0.34, 0.44, 0.12);
  - border: 1.5 px stroke, colour 0xFF62D6E8 at alpha 0.5.
- Text colours (ARGB, with opacity applied to alpha):
  - title 0xFFFFF4DC;
  - artist 0xFF78DCE8;
  - info 0xFFD5DCE6;
  - copyright 0xFFB8A87A.
- Text shadow: a copy in 0xD0000000 offset (+d, +d) with d = 2.2 (title), 1.5 (artist), 1.0 (info), 1.0 (copyright).
- Info lines (`BuildMetadataDetails`, STR:644-665), one per line, joined with LF:
  1. (intro and non-fallback only) credits:
     - "Written By / Lyricist:  X" if both exist and are equal ignoring case;
     - "Written By:  X  •  Lyricist:  Y" if both exist and differ;
     - "Written By:  X" or "Lyricist:  Y" if only one exists.
  2. year, if present (also on the end card);
  3. (intro only) "Length  m:ss" (or "h:mm:ss" if >= 1 h);
  4. (intro only) music details: "Key  X", "Tempo  X", "Timing  X", joined with "  •  " (STR:687-697);
  5. (intro only, non-fallback) AdditionalInfo joined with "  •  ".
- Separators are two spaces, U+2022, two spaces.

**Split bands** (STR:604-642, `DrawSplitMetadataBands`):

- The top band is a full-width rectangle of height `0.17H`; the bottom band has height `0.16H` and is aligned to the bottom edge.
- Band fill RGBA (0.025, 0.04, 0.065, 0.78*opacity). Sheen over the top 45% of each band: RGBA (0.12, 0.2, 0.27, 0.16*opacity). Border line 1.5 px RGBA (0.3, 0.8, 0.9, 0.45*opacity): at the top edge of the bottom band, and at `bottom-1` of the top band.
- Text width is `0.88W`, centred; x is `(W - 0.88W)/2`.
  - Title: `F*0.68` (`F*0.52` fallback), Bold, at y = `0.12*topHeight`, layout height `0.55*topHeight`.
  - Secondary line: `F*0.36`, SemiBold, accent colour 0xFF78DCE8. It sits directly below the title (y = titleY + titleHeight). Content is "Artist  •  Year" with blanks removed, and it is omitted if empty.
  - Bottom text: `F*0.27`, Normal, colour 0xFFD5DCE6. It starts at `H - bottomHeight + 0.14*bottomHeight`. Lines: "Length  m:ss", the music details line if non-empty, and the copyright if non-blank (STR:667-676).
  - Title colour 0xFFFFF4DC. Shadow 0xD0000000 offset 1.8 / 1.2 / 1.0 (title / secondary / bottom).

### 4.4 Ending sequence (SC:137-178; STR:200-213, 300-310)

- **Lyric fade.** Every lyric line (outgoing and incoming) is multiplied by `endLyricOpacity`. It is 1 before `EndLyricFadeStart`, `1 - SmoothStep((pos - start)/(end - start))` between start and end, and 0 from `EndLyricFadeEnd` on. It is 1 when `end <= start`. See section 2.5 for the start/end values.
- **End card.** It appears only if all of these hold:
  - metadata is non-fallback;
  - `MediaDuration - EndLyricFadeEnd >= 3 s`;
  - `EndLyricFadeEnd <= pos < MediaDuration`.
  - Opacity: `SmoothStep` over 750 ms from `EndLyricFadeEnd`; 1 in the middle; `1 - SmoothStep` over the last 750 ms before `MediaDuration`.
  - It uses the centre-card layout with `isEndCard = true`. Fields: title, artist, year, copyright only. No credits, length, music details or additional info.
  - It is drawn last, on top.

### 4.5 Countdown arrows (SC:223-268; STR:733-769, 408-415)

- **When shown.** A line shows its countdown only if `HasCountdown` and `0 < FirstLyricStart - position <= 5 s` (`CountdownDuration = 5 s`, SC:228). `FirstLyricStart` is that line's first segment start (already includes the 400 ms lead). It applies to whichever line carries the flag, not necessarily the active line.
- **Timing model.** It is wall-clock based, not tempo/beat based. `timeRemaining = FirstLyricStart - position`.
  - If `timeRemaining > 1 s`: `elapsed = 5 - timeRemaining` (0..4).
    - `completedArrowCount = floor(elapsed)` (0..3).
    - `activeArrowProgress = frac(elapsed)`.
    - Opacity is 1.
  - If `timeRemaining <= 1 s`:
    - `completedArrowCount = 4`: all arrows are green.
    - `activeArrowProgress = 0`.
    - Opacity is `timeRemaining` in seconds, a linear fade 1 to 0 ending exactly at the first syllable start.
  - At `timeRemaining <= 0` the countdown disappears instantly (the wipe of the first syllable starts at the same moment).
- **Glyph and size.** U+25B6 "BLACK RIGHT-POINTING TRIANGLE" in the lyric font family, Bold, at `0.5 * FontSize` (STR:120-121). Four arrows are always laid out.
- **Spacing.** Slot pitch = `arrowAdvanceWidth + 0.12*FontSize` (STR:748). Arrow i has x = `lyricLeft + i*pitch`, where `lyricLeft` is the left edge of the (scaled, centre-aligned) line text. Arrows are left-aligned to the lyric, not centred.
- **Vertical position.** Arrow top = `lineTop - arrowHeight - 0.1*FontSize` (STR:747, 766), i.e. above the line.
- **Colours.** Fixed values, not style options:
  - red 0xFFFF0000 for arrows after the active one;
  - green 0xFF00C000 for completed arrows (index < completedArrowCount);
  - the active arrow (index == completedArrowCount, when < 4) has colour `LerpColor(red, green, activeArrowProgress)`.
  - Each arrow turns green progressively during its 1 s slot.
- **Layering.** The arrows are drawn inside the line's group layer (so they share the line opacity and the scale of the line's position), after the unsung text and before the wipe passes. They are not affected by the wipe.
- **Page/line appearance.** When a countdown line is on a new page, the page-fade-in completes exactly 5 s before the first syllable (lead = `min(5 s, gap)`), so the countdown starts as the page finishes appearing. For an in-page line, the scroll finishes at `firstSyllable - 5 s`.

---

## 5. Wipe (sung highlighting)

Per line, at `position` (SC:270-379; STR:417-512):

1. `highlightedLength` = the max `TextStart + TextLength` over segments with `Start <= position` (SC:270-281). If it is 0, nothing is highlighted.
2. The current segment is the last segment with `Start <= position`. Its effective wipe duration `D` (`GetSegmentWipeDuration`, SC:349-379) is:
   - `0` if the configured `WipeTransitionMilliseconds <= 0`;
   - the configured duration for the last segment of the line (no next segment);
   - otherwise `c = 0.8 * (nextSegment.Start - segment.Start)`, capped at the configured duration, and snapped to 0 (instant cut) if `c < 150 ms` (`InstantCutThreshold`). If `nextStart <= start`, D = 0.
3. `wipeFraction = clamp(elapsed / D, 0, 1)`; it is 1 if D <= 0 (SC:318-342). `eased = EaseInOutQuint(wipeFraction)`.
4. `fullyHighlightedLength` (SC:287-312): the same as `highlightedLength`, minus the current segment's `TextLength` while `0 <= elapsed < D`. So the current syllable counts as "full" only once its wipe has finished.
5. Widths are measured with DirectWrite by laying out the substring `Text[..n]` with the same scaled format and taking `Metrics.Width`:
   - `fullW` = width of `Text[..fullyHighlightedLength]` (0 if the length is 0).
   - `transW` = width of `Text[..highlightedLength]`, plus `(width(Text[..highlightedLength+1]) - that) * eased`. If `highlightedLength == Text.Length`, `transW` is the full text width.

**Draw order for a line** (STR:403-512), all inside the line's group layer:

1. Base layer: the whole line in the unsung style (`DrawTextLayer`: shadow, outline, then fill).
2. Countdown arrows (if any).
3. Pass 1 (sung, finished part): clip to `x in [left - padding, left + fullW + padding]` and `y in [top - padding, top + lineHeight + 0.35*effFont + padding]` (`PushAxisAlignedClip`, `AntialiasMode.PerPrimitive`). Draw the whole line again with the sung style (`WipeTextArgb`, `WipeOutlineArgb`/`WipeOutlineWidth`, `WipeShadowArgb`).
4. Pass 2 (transition region): if `transW > fullW`, clip to `[left + fullW - padding, left + transW + padding]`. Draw the line a third time with all three colours interpolated base to sung by `eased` (`LerpColor` on text, outline and shadow). The outline width and shadow style used are the sung ones.

Behaviour to reproduce:

- The current syllable is cross-faded from the unsung to the sung colour over `D` with the quint ease-in-out. Its clip rectangle is the whole syllable, so it changes colour uniformly. It is not a left-to-right sweep.
- Everything before it is the sung colour.
- A line that is fully sung stays sung until it leaves.
- Previous syllables are left in the sung colour; a line is stateless, so it can be recomputed from `position` alone.
- The 8-offset outline and shadow follow the same colour state because they are drawn inside the clipped passes.
- As written, the clip's right edge is not the syllable's end. It extends by `eased * widthOfNextCharacter`. See section 8 (quirk Q3).

---

## 6. Rendering details

- **API.** Direct2D 1.x `ID2D1RenderTarget` on a WIC bitmap, pixel format `32bppPBGRA`, render target `B8G8R8A8_UNorm` with `AlphaMode.Premultiplied` (STR:181-184). The bitmap is cleared to transparent (0,0,0,0) and copied out as premultiplied BGRA.
- **Fonts.**
  - Lyric format: family from the style ("Segoe UI"), `FontWeight.Bold`, `FontStyle.Normal`, `FontStretch.Normal`, locale "" (STR:119, 363).
  - Metadata formats: title Bold; artist and secondary SemiBold; info and detail Normal; copyright Normal+Italic (STR:534-537, 620-622).
  - No font fallback or fallback-family list is configured (the arrow glyph U+25B6 relies on DirectWrite's system fallback).
- **Antialiasing.** The text antialias mode is not set. On a premultiplied-alpha WIC target Direct2D uses grayscale antialiasing. Axis-aligned clips use `AntialiasMode.PerPrimitive`, so clip edges are soft. The layer mask antialias mode is Aliased, but there is no geometric mask (STR:390-399).
- **Outline technique.** It is not a geometry stroke. It is 8 offset copies of the text, at `(dx, dy)` with dx, dy in {-1,0,1} except (0,0), each offset multiplied by `OutlineWidth` px (STR:783-793).
  - The outline is drawn only if `OutlineWidth > 0` and the outline alpha is not 0.
  - Offsets of 1.75 px at the diagonal give a square-ish outline, so it is slightly heavier at corners.
  - Outline width does not scale with the line scale.
- **Shadow technique.** It is a single offset copy, drawn first (under the outline), at `(ShadowOffsetX, ShadowOffsetY)` in the shadow colour. It is skipped if the shadow alpha is 0 (STR:780-781). The same offsets are used for the unsung and sung styles (only the colours differ). It is not blurred, and the offset does not scale with the line scale.
- **Per-line draw order** (within `DrawTextLayer`, STR:771-796): shadow, then outline (8 copies), then fill.
- **Layer opacity.** One `PushLayer` per line with the line opacity, so overlapping passes composite as one group (section 3).
- **Alpha.** All colours are straight ARGB in the style, converted to float RGBA for Direct2D, which premultiplies when drawing. `ToColor(argb, opacity)` multiplies the alpha channel by clamp(opacity, 0, 1) (STR:885-889).
- **Layouts per frame.** A layout object is created for every line every frame, plus several extra prefix layouts per frame for the wipe widths (STR:123-124, 371, 435-454). This is CPU-heavy (see Q6).
- **Dirty area.** See section 1 (output bitmap and dirty rectangle). The bitmap offset (`bounds.Left, bounds.Top`) is returned with the pixels, along with the canvas size (STR:317-325).

---

## 7. Style options (`KaraokeTextStyle`, KTS:7-70)

Colours are ARGB `uint`. Normalisation (`Normalize`, KTS:44-69): if all of FontFamily/FontSize/colours are zero or blank (a default-initialised struct) it is replaced with the defaults. Otherwise the clamps below apply, and non-finite floats fall back to the stated fallback.

| Field | Default | Clamp / fallback | Meaning |
|---|---|---|---|
| FontFamily | "Segoe UI" | blank gives "Segoe UI" | Lyric and metadata font family. Weight is hard-coded. |
| FontSize | 72 | 6..240 (non-finite gives 72) | Font size in px on the 1920x1080 canvas. Drives slot pitch, arrow size and metadata sizes. |
| TextArgb | 0xFFFFFFFF | none | Unsung fill colour. |
| WipeTextArgb | 0xFFFFFF00 | none | Sung fill colour. |
| OutlineArgb | 0xE0000000 | none | Unsung outline colour. |
| WipeOutlineArgb | 0xE0000000 | none | Sung outline colour. |
| OutlineWidth | 1.75 | 0..32 (non-finite gives 0) | Unsung outline offset in px. |
| WipeOutlineWidth | 1.75 | 0..32 (non-finite gives 0) | Sung outline offset in px. |
| ShadowArgb | 0xA0000000 | none | Unsung shadow colour. |
| WipeShadowArgb | 0xA0000000 | none | Sung shadow colour. |
| ShadowOffsetX | 2 | -128..128 (non-finite gives 2) | Shadow x offset (shared by unsung and sung). |
| ShadowOffsetY | 2 | -128..128 (non-finite gives 2) | Shadow y offset (shared). |
| LineTransitionMilliseconds | 1000 | 0..4000 (non-finite gives 1000) | Page fade-out/in duration `T`, and the initial fade-in duration. Not the in-page scroll duration (that is a hard-coded 1500 ms). |
| WipeTransitionMilliseconds | 600 | 0..2000 (non-finite gives 600) | Upper cap on a syllable's colour cross-fade (section 5). |
| NextLineOpacity | 0.65 | 0..1 (non-finite gives 0.65) | Opacity of the slot +1 line. Slot +2 is this times 0.55. |

---

## 8. Gaps and quirks a reimplementation should treat deliberately

Q1. **Hard-coded values that should be options.**
- Slot pitch 1.55F (STR:142), opacities -1 = 0.5 and -2 = 0.25 (STR:828-829), next-line factor 0.55 (STR:831).
- Per-slot scales 1.0, 0.8, 0.6, 0.4 (STR:835-841).
- Layout width fraction 0.85 and max height 0.2H (STR:122-123).
- Scroll duration 1500 ms (MLE:20). Page hold 2000 ms, preview 1000 ms (MLE:17-18). Lead 400 ms (MLE:21).
- Paragraph gap 2 s, sentence gap 0.9 s, break length 42 (MLE:15, 22-23). Countdown gap 6 s (MLE:562).
- Countdown: 5 s duration, 4 arrows, red/green colours, 0.5F glyph size, 0.12F/0.1F gaps (SC:228; STR:120, 750-751, 757).
- Page scale-in 1.5 and scale-out 0.75 (STR:216, 240).
- Intro timings (1 s start, 1 s fade, 3 s clear lead, 8 s band limit) and end timings (5 s after the last syllable, 1 s lyric fade, 3 s minimum, 750 ms card fade) (SC:17-22; MLE:158-159).
- All metadata colours, sizes and strings (STR:529-641). The labels "Length", "Key", "Tempo", "Timing", "Written By" and "Lyricist" are not localisable.

Q2. **Misnamed option.** `LineTransitionMilliseconds` does not control the line scroll. It controls page fades and the first fade-in (MLE:127-129). The scroll uses a hard-coded 1500 ms, and the per-slot movement uses SmoothStep.

Q3. **The wipe is a cross-fade, not a sweep.**
- Within a syllable, the colour changes uniformly. The clip region's right edge also extends into the next character by `eased * width(nextChar)`, and that extension persists after the wipe completes, until the next segment starts (STR:443-462, 489-512).
- For mid-word syllables the next character is the first letter of the next syllable. It briefly appears blended or sung before its own wipe starts, then resets to the unsung colour at the next segment start (visible flicker). When the next character is a space, nothing is visible.
- A reimplementation should either clip pass 2 exactly to `[fullW, widthOfHighlightedPrefix]` for a crossfade look, or sweep the clip edge across the syllable if a true left-to-right wipe is wanted. Decide this explicitly.

Q4. **Single-row assumption.** Wipe widths come from prefix layouts and the line's left edge, so wrapped (multi-row) lines are wiped incorrectly. Slots do not grow for wrapped lines. Kerning across the clip boundary is ignored and prefix widths exclude trailing whitespace.

Q5. **Outline/shadow scaling.** Outline width and shadow offset are not scaled with the line (so a 0.6-scale line gets a relatively heavy outline). The sung layer stacks a second semi-transparent shadow over the unsung shadow in the sung region (alpha 0xA0 drawn twice is darker). The unsung outline (drawn first) can show around a thinner sung outline.

Q6. **Performance.** Direct2D layouts are rebuilt and the whole canvas is rasterised on the CPU whenever the 1%/60 Hz revision changes. A new renderer can cache glyph layouts per line and animate with GPU uniforms.

Q7. **Countdown is wall-clock, not beat-based.** It has no relation to tempo or time signature. Its size and gaps are not scaled with the line scale, it is always left-aligned, and the glyph relies on font fallback.

Q8. **Extraction.**
- Lyrics starting with `@` or `%` are dropped.
- The 42-character rule needs trailing-space syllables.
- Page breaks are driven by start-to-start gaps, so a long final note followed by a gap can trigger a page break.
- Countdown flags use the previous line's last syllable start, not its end.

Q9. **Page fade-in scale 1.5** can overflow the canvas on long lines, and the layout box also scales to 2448 px.

Q10. **Bounds.** The dirty rectangle reserves 3 slots below the centre but only slots up to +2 are drawn (STR:174). Slot -2 and +2 appear in the active window only if the page has that many lines.

Q11. **Overlap of fades.** If `transitionWindow < 2T`, page fade-out and fade-in overlap and both pages are visible simultaneously.

Q12. **Karaoke only with a single active cue.** If two cues are active at once, the plain-text renderer is used instead (MPC:1377).
