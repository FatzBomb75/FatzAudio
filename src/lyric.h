/* The MIDI lyric surface's drawing (lyric.c): the song's lines from lyric_time.c,
 * laid out with our font reader, drawn with the surface effects into a bitmap of
 * its own, and copied into the app's buffer by changed area. cdg.c holds it in a
 * fa_surface and calls these under the surface's lock. */
#ifndef FA_LYRIC_H
#define FA_LYRIC_H

#include "fatzaudio/fatzaudio.h"

typedef struct fa_lyric fa_lyric;

/* 1 when the style is in range (fa_lyric_surface_create's rules). */
int fa_lyric_style_valid(const fa_lyric_style* style);
/* Reads the stream's lyrics, tempo map and length and opens the style's font. */
fa_result fa_lyric_new(fa_stream* stream, const fa_lyric_style* style, fa_lyric** out);
void fa_lyric_free(fa_lyric* lyric);
fa_result fa_lyric_set_style(fa_lyric* lyric, const fa_lyric_style* style);
/* The whole style; its strings point into `lyric`. */
void fa_lyric_get_style(const fa_lyric* lyric, fa_lyric_style* out);
/* NULL = none. Already checked with fa_fx_valid. */
void fa_lyric_set_effects(fa_lyric* lyric, const fa_surface_effects* effects);
/* The layout for a presentation area (0 x 0 = 1920x1080). */
fa_result fa_lyric_get_layout(fa_lyric* lyric, uint32_t area_w, uint32_t area_h, fa_surface_layout* out);
/* update_into for the lyric surface: `tempo` is the stream's FA_PARAM_TEMPO. Fills
 * every field of `fr` but struct_size, draw_ns and layout_revision. */
fa_result fa_lyric_update(fa_lyric* lyric, uint32_t area_w, uint32_t area_h, double tempo, fa_time time, uint8_t* dst,
                          int32_t dst_stride, uint32_t dst_w, uint32_t dst_h, uint64_t dst_revision, fa_surface_frame* fr);
uint64_t fa_lyric_layout_revision(const fa_lyric* lyric);

#endif
