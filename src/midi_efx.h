/* The insertion effect engine shared by midi_efx.c (the processing blocks) and
 * midi_efx_types.c (the GS EFX and XG types built from them). Internal to those two
 * files; the rest of FatzAudio sees fa_midi_efx through fa_internal.h. */
#ifndef FA_MIDI_EFX_H
#define FA_MIDI_EFX_H
#include "fa_internal.h"
#include "mixer.h"

#define EFX_STAGES 6
#define EFX_TAPS 16
#define EFX_VOICES 6
#define EFX_SECS 8
#define EFX_CHUNK 64

/* What a stage does. */
enum {
    ST_NONE, ST_EQ, ST_DRIVE, ST_ENHANCE, ST_FORMANT, ST_PHASER, ST_WAH, ST_ROTARY, ST_MOD, ST_TREMOLO,
    ST_DYNAMICS, ST_DELAY, ST_REVERB, ST_PITCH, ST_PAN3D, ST_LOFI, ST_RING, ST_CANCEL
};
enum { WAVE_TRI, WAVE_SQR, WAVE_SIN, WAVE_SAW_UP, WAVE_SAW_DOWN };
enum { DYN_COMP, DYN_LIMIT, DYN_GATE };
enum { FILT_LP, FILT_BP, FILT_HP };

typedef struct efx_line { float* d; int cap, pos; } efx_line;   /* stereo, interleaved */

/* One processing block of a type's chain. The fields used depend on the kind; the
 * type builder (midi_efx_types.c) sets them, efx_stage_run (midi_efx.c) uses them. */
typedef struct efx_stage {
    int kind;
    float dry, wet;             /* out = in * dry + the stage's sound * wet */
    float pan;                  /* < 0: stereo; 0..1: the stage's sound summed to mono and placed there */
    int mono_in;                /* the input summed to mono first */
    /* filters: EQ sections, the drive's tone, a fixed filter after the effect */
    int nsec;
    biquad sec[EFX_SECS];
    float z[EFX_SECS][2][2];
    /* drive (ST_DRIVE, and the enhancer's saturation) */
    float drive, edge, level;
    /* envelope follower (wah, dynamics) */
    float env, att, rel, sens;
    /* LFO (song-frame phase) */
    double hz;
    float depth, phase;         /* phase: the right side's LFO offset, 0..1 of a cycle */
    int wave, step;             /* step: frames between steps of a stepped LFO (step flanger), 0: smooth */
    /* swept filters (phaser, wah, formant) */
    float base, range, reso, fb;
    int npole, ftype, down;
    float coef[2], svf[2][2], ap[2][8], last[2];
    float f1, f2, f1_to, f2_to, glide;   /* formant: the two formants now and where they go */
    biquad fmt[2];
    float fz[2][2][2];
    /* delay lines (delays, modulated voices, rotary, pitch, 3D) */
    efx_line line;
    int need;                   /* frames the line must hold */
    int ntaps;
    float tap_t[EFX_TAPS];      /* frames */
    float tap_gl[EFX_TAPS], tap_gr[EFX_TAPS];
    uint8_t tap_ch[EFX_TAPS];
    float tap_now;              /* tap 0's time now (frames), gliding towards tap_t[0] when `glide` > 0 */
    float fb_t[2];              /* feedback taps (frames); 0: none */
    float fb_gain;
    int fb_cross;               /* the left line feeds back into the right and the other way round */
    float damp_a, damp_z[2];
    float pre_a, pre_z[2];      /* input filter: lowpass, or highpass when pre_hp */
    int pre_hp;
    /* modulated voices (chorus family, pitch shifter) */
    int voices;
    float v_pre[EFX_VOICES], v_depth[EFX_VOICES], v_phase[EFX_VOICES], v_gl[EFX_VOICES], v_gr[EFX_VOICES];
    float v_ratio[EFX_VOICES], v_ph[EFX_VOICES], held[EFX_VOICES];
    uint8_t v_ch[EFX_VOICES];   /* the line (side) each voice reads */
    float window;               /* pitch shifter window, frames */
    double trem_hz;             /* tremolo chorus: level LFO */
    float trem_depth, trem_phase;
    /* rotary: [0] the low rotor, [1] the horn */
    double slow[2], fast[2];
    float rot_hz[2], rot_acc[2], rot_lev[2], rot_ph[2], sep, xover_a, xz[2];
    int fast_on;
    /* dynamics */
    int dyn;
    float thresh, ratio, makeup, gate_g;
    /* lo-fi */
    int hold_n, hold_i;
    float hold[2], levels, noise[4], noise_lp[4], noise_z[4][2], hum_hz;
    int crackle, mono_out;
    /* ring modulator */
    double ring_hz;
    /* 3D pan */
    float az, az_speed, az_from;
    int az_turn;
    /* reverb */
    fa_midi_fx* sub;
} efx_stage;

typedef struct efx_type efx_type;

struct fa_midi_efx {
    uint32_t rate;
    const efx_type* t;          /* NULL: off */
    int p[20];                  /* parameters as set (GS 1-20, XG 1-16) */
    float ctl[2];               /* GS: control 1 and 2 (-1..1 of the range) on the type's + and # parameters */
    float xg_ctl;               /* XG: the controllers' sum (-1..1) on the type's controlled parameter */
    int nst, par;               /* stages; par > 0: stages [0, par) and [par, nst) run side by side */
    efx_stage st[EFX_STAGES];
    float par_g[2][2];          /* side by side: each chain's left and right gain */
    /* GS output: low and high shelves, pan (mono types), level */
    int post;
    biquad lo, hi;
    float lz[2][2], hz2[2][2];
    float out_gl, out_gr;
    int out_mono;
    float a[EFX_CHUNK * 2], b[EFX_CHUNK * 2], c[EFX_CHUNK * 2];
    fa_frames soft;             /* frames since something at -140 dB or more went in or came out */
    int tail, idle;
};

/* midi_efx.c */
void efx_stage_clear(efx_stage* s);
void efx_stage_free(efx_stage* s);
void efx_stage_run(fa_midi_efx* e, efx_stage* s, float* x, int n, fa_frames frame);
int efx_stage_ready(fa_midi_efx* e, efx_stage* s);
biquad efx_biquad(int type, double hz, double q, double db, uint32_t rate);
float efx_pole(uint32_t rate, double hz);
enum { BQ_LP, BQ_HP, BQ_BP, BQ_PEAK, BQ_LOSHELF, BQ_HISHELF };

/* midi_efx_types.c: the type tables and builders. */
const efx_type* efx_find_gs(int msb, int lsb);
const efx_type* efx_find_xg(int msb, int lsb);
void efx_load_defaults(fa_midi_efx* e);
void efx_build(fa_midi_efx* e);
int efx_type_is_gs(const efx_type* t);
int efx_type_dry_wet(const efx_type* t);    /* XG: the dry/wet parameter (0-15), -1 none */
int efx_type_ctl(const efx_type* t, int which);
int efx_type_lsb(const efx_type* t);

#endif
