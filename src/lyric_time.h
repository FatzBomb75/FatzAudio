/* The lyric surface's timeline: a song's syllables made into lines and pages, when
 * each line scrolls in, when pages fade, and, for any position, which lines show
 * where and how much of each is sung. Pure functions of the syllables and the style,
 * so every frame is a function of the position (seeks and loops need nothing). */
#ifndef FA_LYRIC_TIME_H
#define FA_LYRIC_TIME_H

#include "fatzaudio/fatzaudio.h"
#include <stddef.h>
#include <stdint.h>

/* A syllable as the MIDI reader gives it (fa_midi_text of type FA_MIDI_LYRIC). */
typedef struct ly_frag {
    const char* text;             /* UTF-8, NUL-terminated */
    fa_time time;                 /* where it is heard */
    uint32_t flags;               /* FA_MIDI_NEW_LINE, FA_MIDI_NEW_PARAGRAPH */
} ly_frag;

/* A syllable of a line: bytes [start, start + len) of the line's text (no spaces at
 * either end; the space between two words belongs to neither). */
typedef struct ly_seg {
    uint32_t start, len;
    fa_time time;                 /* its start, the lead included */
} ly_seg;

typedef struct ly_line {
    char* text;                   /* UTF-8, single spaces, no space at either end */
    uint32_t text_len;
    ly_seg* seg;
    uint32_t nseg;
    int page;
    float fit;                    /* 1, or less for a single word wider than the box */
    int countdown;                /* shows a countdown before its first syllable */
    fa_time first, last;          /* its first and last syllable's start */
    fa_time cd_period;            /* countdown: one arrow's time */
    /* When it becomes the active line (its cue), and how it gets there. */
    fa_time cue;
    int clears;                   /* the first line of a page after another page */
    fa_time t0, t1;               /* scroll (or the first page's fade-in) */
    fa_time out0, out1, in0, in1; /* clears: the old page's fade-out, this page's fade-in */
} ly_line;

/* The timeline's settings, in nanoseconds; scroll, page and wipe-cap times are real
 * time made stream time (times the tempo) by the caller. */
typedef struct ly_params {
    fa_time lead, paragraph_gap, sentence_gap;
    int break_length;             /* 0 = off */
    fa_time scroll, page, page_hold, page_preview;
    int page_overlap;
    fa_time end_hold, end_fade;
    int countdown;                /* fa_lyric_countdown */
    int countdown_count;
    fa_time countdown_time;       /* clock: the whole countdown */
    fa_time countdown_gap;
    fa_time wipe_cap, instant_cut;
    float wipe_share;
    int lines_above, lines_below;
    float above_opacity[2], next_opacity, after_next_factor;
    float near_scale, far_scale;
    float page_in_scale, page_out_scale;
    fa_time duration;             /* the song's length; 0 = unknown */
    /* The intro and ending pages: on, and their times (real time made stream time). */
    int intro, ending;
    fa_time intro_start, intro_fade, intro_clear, intro_band;
    fa_time ending_min, ending_fade;
} ly_params;

/* Width of `len` bytes of UTF-8 text at the design size, for splitting lines wider
 * than `max_width`; NULL = never split. */
typedef float (*ly_measure)(void* ctx, const char* text, uint32_t len);

/* Beat length (ns) at a time, for the beat countdown; NULL = 120 bpm. */
typedef fa_time (*ly_beat)(void* ctx, fa_time at);

typedef struct ly_timeline {
    ly_line* line;
    uint32_t n;
    fa_time end_fade0, end_fade1;
    fa_time duration;
    /* The intro: on bands (the lyrics start before intro_band) or on a card, gone at
     * intro_end. */
    int intro_bands;
    fa_time intro_end;
} ly_timeline;

fa_result ly_build(ly_timeline* tl, const ly_frag* frags, uint32_t count, const ly_params* p, ly_measure measure,
                   void* measure_ctx, float max_width, ly_beat beat, void* beat_ctx);
void ly_free(ly_timeline* tl);
/* Recomputes only the timing (scroll, pages, countdown, end) for new times. */
void ly_schedule(ly_timeline* tl, const ly_params* p, ly_beat beat, void* beat_ctx);

/* A line as shown at one position. */
typedef struct ly_show {
    uint32_t line;
    int outgoing;                 /* of the page fading out */
    float slot;                   /* its centre, in line pitches from the anchor (+ down) */
    float scale, opacity;
} ly_show;

#define LY_MAX_SHOW 12

/* The lines shown at `t`, in drawing order. */
uint32_t ly_frame(const ly_timeline* tl, const ly_params* p, fa_time t, ly_show out[LY_MAX_SHOW]);

/* How much of a line is sung at `t`: syllables [0, full) are, syllable `current`
 * (= full, or -1) is `mix` of the way (0..1, eased), the rest are not. */
typedef struct ly_wipe {
    int full;
    int current;
    float mix;
} ly_wipe;

void ly_wipe_at(const ly_line* l, const ly_params* p, fa_time t, ly_wipe* out);

/* A line's countdown at `t`: shown, arrows done, the next arrow's progress (0..1),
 * and the whole countdown's opacity. */
typedef struct ly_count {
    int shown, done;
    float progress, opacity;
} ly_count;

void ly_countdown_at(const ly_line* l, const ly_params* p, fa_time t, ly_count* out);

/* The intro page's opacity at `t` (0 when it is off or not shown), and the ending
 * card's: it shows after the lyrics have faded when ending_min is left of the song. */
float ly_intro_at(const ly_timeline* tl, const ly_params* p, fa_time t);
float ly_ending_at(const ly_timeline* tl, const ly_params* p, fa_time t);

/* The easing curves of the design reference. */
float ly_smoothstep(float t);
float ly_ease_in_cubic(float t);
float ly_ease_in_out_cubic(float t);
float ly_ease_in_out_quint(float t);

#endif
