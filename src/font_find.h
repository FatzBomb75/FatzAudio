/* Finding the system's fonts, through each OS's own lists: the registry's font
 * list on Windows, CoreText on macOS and iOS, the system's font list on Android
 * 10 and later in an app (the system font folders otherwise), fontconfig on Linux when
 * the system has it (loaded at runtime), else the usual font folders. The faces
 * found are read with our own reader (font.h), so a family is matched the same
 * way everywhere. */
#ifndef FA_FONT_FIND_H
#define FA_FONT_FIND_H

#include "font.h"

/* Opens the installed face of `family` (case, spaces and hyphens ignored; the
 * typographic or the legacy family name) nearest to `weight` (100..900) and
 * `italic`: an italic face when one exists, then the nearest weight. NULL `family`
 * is the system's UI font (Segoe UI, Helvetica Neue, the Linux and Android
 * sans-serif). FA_ERROR_NOT_FOUND when the system has no such family. The face's
 * own weight is in its info: draw a synthetic bold when it is too light. */
fa_result fa_font_find(const char* family, int weight, int italic, fa_font** out);

/* Opens the system's usual font for text in a language (BCP 47: "ja", "ko",
 * "zh-Hans", "zh-Hant", "zh"; others give the UI font): Yu Gothic or Meiryo on
 * Windows, Hiragino on Apple, Noto Sans CJK on Android and Linux. */
fa_result fa_font_find_for_language(const char* language, int weight, int italic, fa_font** out);

/* Tests: 1 = ignore the OS's own lists and scan the font folders. */
void fa_font_find_set_scan_only(int scan_only);

#endif
