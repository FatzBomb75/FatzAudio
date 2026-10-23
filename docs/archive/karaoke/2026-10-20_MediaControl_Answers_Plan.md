# FatzBomb.MediaControl: answers to FatzAudio's questions on the karaoke plan

From: FatzBomb.MediaControl, 2026-10-20, relayed by Keith. Verbatim.

---

Answers for FatzAudio:

Japanese font fallback per song: agreed, and better than what we asked for. One font for the whole song avoids mixed-font lines. Use the system Japanese font when the chosen font lacks most of the song's characters: Yu Gothic or Meiryo on Windows, Hiragino on macOS, Noto Sans CJK on Linux. If the user explicitly sets a font for that song or that player, theirs wins. Please report in the layout which font was actually used, so we can show it.

Fixed lyric block size: agreed. A fixed size per presentation size and style, changing only when the layout changes, is what we want. No reallocation during playback; dirty rectangles keep each upload small. Our current renderer works the same way, reserving the slot area.

Edge blend only at smoothness 0–4: agreed. Please document it as "no extra effect at levels 5–10" rather than rejecting the setting, so the app can leave it on while the user moves the smoothness slider.

Default wipe per preset: agreed. Classic keeps our cross-fade so it looks like what our users know today; Modern and Neon use the sweep. Once we've seen the sweep, we may ask for Classic to switch too.

Overlapping page fades: agreed: an option, with overlap as the default. For a singer, waiting for the old page to finish fading means the new lyrics arrive late, which is worse than a brief overlap.

Wrapped lines: yes, split at a word boundary when the lines are built. Each row becomes its own line with its own correct wipe and its own slot. Keep the rows of one split line on the same page.

Your question: our own font reader (80 KB) or FreeType plus HarfBuzz (2 MB):

My answer is your own reader, but only if it covers four things. If it can't, use FreeType without HarfBuzz.

HarfBuzz isn't needed. It exists for Arabic, Hindi and other complex scripts, which we put out of scope. Japanese and Latin lyrics don't need it. So the real choice is your own reader or FreeType alone, which is roughly 0.5–1 MB, not 2 MB.

The four things the reader needs for the fonts lyrics will actually use:

- Font collections (.ttc): Yu Gothic and Meiryo on Windows ship as .ttc files.
- CFF outlines (OpenType .otf): Hiragino on macOS and Noto Sans CJK on Linux use them. Without CFF, question 1's fallback fails on those systems.
- Kerning from GPOS tables: most modern fonts keep their kerning only there. Without it, Latin text looks uneven next to DirectWrite.
- Good anti-aliasing at small sizes: oversampling or analytic coverage, because subtitle mode draws small text.

If their 80 KB reader covers all four, it's the better choice: small, and no outside dependency. If CFF or GPOS kerning would be a struggle, FreeType alone gets them all for under 1 MB. In either case, they should keep the font code behind their own internal interface, so the choice can change later without touching the API.
