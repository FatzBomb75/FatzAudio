/* The insertion effect types: the SC-88 Pro's 64 EFX types (40 03 00, MSB and LSB)
 * and the MU100's XG types beyond the MU80 variation list, each turned into a chain
 * of midi_efx.c's stages from its parameters.
 *
 * GS parameters are numbered by their SysEx address (40 03 03-16 are parameters 1-20)
 * and converted with the SC-88 Pro's published value tables (pre-delay, delay times,
 * rates, the HF damp, cutoff, EQ and manual frequencies). XG parameters are 1-16 as
 * Yamaha's effect parameter list numbers them, with the XG tables (LFO frequency, EQ
 * frequency, 0.1 ms delay steps). The defaults where the manuals give none, the drive
 * curves, the amp and cabinet tones, the formants and the lo-fi settings are our own
 * reading of what each type does; nothing here was measured on a module. */
#include "midi_efx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    /* GS */
    G_THRU, G_STEREO_EQ, G_SPECTRUM, G_ENHANCER, G_HUMANIZER, G_OVERDRIVE, G_DISTORTION, G_PHASER, G_AUTO_WAH,
    G_ROTARY, G_ST_FLANGER, G_STEP_FLANGER, G_TREMOLO, G_AUTO_PAN, G_COMPRESSOR, G_LIMITER, G_HEXA_CHORUS,
    G_TREM_CHORUS, G_ST_CHORUS, G_SPACE_D, G_3D_CHORUS, G_ST_DELAY, G_MOD_DELAY, G_3TAP_DELAY, G_4TAP_DELAY,
    G_TM_DELAY, G_REVERB, G_GATE_REVERB, G_3D_DELAY, G_2PITCH, G_FB_PITCH, G_3D_AUTO, G_3D_MANUAL, G_LOFI1,
    G_LOFI2, G_OD_CHO, G_OD_FL, G_OD_DLY, G_DS_CHO, G_DS_FL, G_DS_DLY, G_EH_CHO, G_EH_FL, G_EH_DLY, G_CHO_DLY,
    G_FL_DLY, G_CHO_FL, G_ROT_MULTI, G_GTR1, G_GTR2, G_GTR3, G_CLEAN1, G_CLEAN2, G_BASS, G_RHODES, G_KEYS,
    G_P_CHO_DLY, G_P_FL_DLY, G_P_CHO_FL, G_P_OD_OD, G_P_OD_ROT, G_P_OD_PH, G_P_OD_AW, G_P_PH_ROT, G_P_PH_AW,
    /* XG */
    X_COMP_DIST, X_WAH_DIST, X_PITCH, X_EXCITER, X_TOUCH_WAH, X_TOUCH_WAH_DIST, X_COMPRESSOR, X_NOISE_GATE,
    X_VOICE_CANCEL, X_ROTARY2, X_ENSEMBLE, X_AMBIENCE, X_TALK, X_LOFI, X_DIST_DELAY, X_COMP_DIST_DELAY,
    X_WAH_DIST_DELAY
};

struct efx_type {
    uint8_t gs, msb, lsb, build;
    int8_t dw;                  /* XG: the dry/wet parameter (0-15), -1 none */
    int8_t ctl[2];              /* GS: the parameters (address) controls 1 and 2 move; XG: [0] the one the controllers move (0-15) */
    uint8_t od;                 /* drive types: 1 distortion */
    int16_t def[20];
};

#define A(x) [(x) - 3]
#define GAINS A(0x13) = 64, A(0x14) = 64
#define TAIL(bal, lev) A(0x12) = (bal), GAINS, A(0x15) = 64, A(0x16) = (lev)
#define OD1 A(0x03) = 48, A(0x04) = 64, A(0x05) = 1, A(0x06) = 1
#define DS1 A(0x03) = 76, A(0x04) = 64, A(0x05) = 3, A(0x06) = 1
#define EH1 A(0x03) = 64, A(0x04) = 64
#define CHO2 A(0x08) = 0x10, A(0x09) = 8, A(0x0A) = 64
#define FL2 A(0x08) = 0x10, A(0x09) = 0x0B, A(0x0A) = 24, A(0x0B) = 0x68
#define DLY2 A(0x08) = 0x74, A(0x09) = 0x50, A(0x0A) = 0x78
#define ROT2 A(0x08) = 6, A(0x09) = 0x71, A(0x0A) = 24, A(0x0B) = 127, A(0x0C) = 0x11, A(0x0D) = 0x78, A(0x0E) = 88, A(0x0F) = 64, A(0x10) = 96, A(0x11) = 0
#define AW2 A(0x08) = 1, A(0x09) = 0, A(0x0A) = 68, A(0x0B) = 62, A(0x0C) = 0x28, A(0x0D) = 72, A(0x0E) = 1
#define PH1 A(0x03) = 0x24, A(0x04) = 0x10, A(0x05) = 64, A(0x06) = 16, A(0x07) = 127
#define N -1

static const efx_type types[] = {
    { 1, 0x00, 0x00, G_THRU, N, { N, N }, 0, { 0 } },
    { 1, 0x01, 0x00, G_STEREO_EQ, N, { 0x16, N }, 0, { A(0x03) = 1, A(0x04) = 0x45, A(0x05) = 1, A(0x06) = 0x3C, A(0x07) = 56,
      A(0x08) = 0, A(0x09) = 0x38, A(0x0A) = 80, A(0x0B) = 0, A(0x0C) = 64, A(0x16) = 127 } },
    { 1, 0x01, 0x01, G_SPECTRUM, N, { 0x15, 0x16 }, 0, { A(0x03) = 64, A(0x04) = 64, A(0x05) = 64, A(0x06) = 64, A(0x07) = 64,
      A(0x08) = 64, A(0x09) = 64, A(0x0A) = 64, A(0x0B) = 1, A(0x15) = 64, A(0x16) = 127 } },
    { 1, 0x01, 0x02, G_ENHANCER, N, { 0x03, 0x04 }, 0, { EH1, GAINS, A(0x16) = 127 } },
    { 1, 0x01, 0x03, G_HUMANIZER, N, { 0x05, 0x16 }, 0, { A(0x03) = 127, A(0x04) = 1, A(0x05) = 0, A(0x06) = 15 * 8, GAINS,
      A(0x15) = 64, A(0x16) = 127 } },
    { 1, 0x01, 0x10, G_OVERDRIVE, N, { 0x03, 0x15 }, 0, { A(0x03) = 48, A(0x04) = 1, A(0x05) = 1, GAINS, A(0x15) = 64, A(0x16) = 96 } },
    { 1, 0x01, 0x11, G_DISTORTION, N, { 0x03, 0x15 }, 1, { A(0x03) = 76, A(0x04) = 3, A(0x05) = 1, GAINS, A(0x15) = 64, A(0x16) = 84 } },
    { 1, 0x01, 0x20, G_PHASER, N, { 0x03, 0x04 }, 0, { PH1, GAINS, A(0x16) = 104 } },
    { 1, 0x01, 0x21, G_AUTO_WAH, N, { 0x05, 0x07 }, 0, { A(0x03) = 1, A(0x04) = 0, A(0x05) = 68, A(0x06) = 62, A(0x07) = 0x28,
      A(0x08) = 72, A(0x09) = 1, GAINS, A(0x15) = 64, A(0x16) = 96 } },
    { 1, 0x01, 0x22, G_ROTARY, N, { 0x0D, 0x16 }, 0, { A(0x03) = 6, A(0x04) = 0x71, A(0x05) = 24, A(0x06) = 127, A(0x07) = 0x11,
      A(0x08) = 0x78, A(0x09) = 88, A(0x0A) = 64, A(0x0B) = 96, A(0x0D) = 0, GAINS, A(0x16) = 127 } },
    { 1, 0x01, 0x23, G_ST_FLANGER, N, { 0x06, 0x08 }, 0, { A(0x03) = 0, A(0x04) = 64, A(0x05) = 0x10, A(0x06) = 0x0B, A(0x07) = 24,
      A(0x08) = 0x68, A(0x09) = 0x5A, TAIL(64, 127) } },
    { 1, 0x01, 0x24, G_STEP_FLANGER, N, { 0x06, 0x08 }, 0, { A(0x03) = 0x10, A(0x04) = 0x0B, A(0x05) = 24, A(0x06) = 0x68,
      A(0x07) = 0x5A, A(0x08) = 0x28, TAIL(64, 127) } },
    { 1, 0x01, 0x25, G_TREMOLO, N, { 0x04, 0x05 }, 0, { A(0x03) = 1, A(0x04) = 0x3C, A(0x05) = 96, GAINS, A(0x16) = 127 } },
    { 1, 0x01, 0x26, G_AUTO_PAN, N, { 0x04, 0x05 }, 0, { A(0x03) = 1, A(0x04) = 0x3C, A(0x05) = 96, GAINS, A(0x16) = 127 } },
    { 1, 0x01, 0x30, G_COMPRESSOR, N, { 0x15, 0x16 }, 0, { A(0x03) = 72, A(0x04) = 100, A(0x05) = 1, GAINS, A(0x15) = 64, A(0x16) = 104 } },
    { 1, 0x01, 0x31, G_LIMITER, N, { 0x15, 0x16 }, 0, { A(0x03) = 85, A(0x04) = 1, A(0x05) = 16, A(0x06) = 1, GAINS, A(0x15) = 64,
      A(0x16) = 127 } },
    { 1, 0x01, 0x40, G_HEXA_CHORUS, N, { 0x04, 0x12 }, 0, { A(0x03) = 0x18, A(0x04) = 8, A(0x05) = 64, A(0x06) = 5, A(0x07) = 64,
      A(0x08) = 16, TAIL(64, 112) } },
    { 1, 0x01, 0x41, G_TREM_CHORUS, N, { 0x07, 0x12 }, 0, { A(0x03) = 0x10, A(0x04) = 8, A(0x05) = 40, A(0x06) = 40, A(0x07) = 0x3C,
      A(0x08) = 96, TAIL(64, 127) } },
    { 1, 0x01, 0x42, G_ST_CHORUS, N, { 0x06, 0x12 }, 0, { A(0x03) = 0, A(0x04) = 64, A(0x05) = 0x0A, A(0x06) = 8, A(0x07) = 111,
      A(0x09) = 0x5A, TAIL(64, 104) } },
    { 1, 0x01, 0x43, G_SPACE_D, N, { 0x04, 0x12 }, 0, { A(0x03) = 0x20, A(0x04) = 8, A(0x05) = 64, A(0x06) = 0x5A, TAIL(64, 96) } },
    { 1, 0x01, 0x44, G_3D_CHORUS, N, { 0x04, 0x12 }, 0, { A(0x03) = 0x0A, A(0x04) = 8, A(0x05) = 72, TAIL(64, 80) } },
    { 1, 0x01, 0x50, G_ST_DELAY, N, { 0x05, 0x12 }, 0, { A(0x03) = 0x65, A(0x04) = 0x74, A(0x05) = 0x50, A(0x0A) = 0x78, TAIL(64, 127) } },
    { 1, 0x01, 0x51, G_MOD_DELAY, N, { 0x07, 0x12 }, 0, { A(0x03) = 0x5A, A(0x04) = 0x6C, A(0x05) = 0x50, A(0x07) = 0x0C, A(0x08) = 21,
      A(0x09) = 0x5A, A(0x0A) = 0x78, TAIL(64, 127) } },
    { 1, 0x01, 0x52, G_3TAP_DELAY, N, { 0x06, 0x12 }, 0, { A(0x03) = 0x14, A(0x04) = 0, A(0x05) = 7, A(0x06) = 0x50, A(0x07) = 127,
      A(0x08) = 127, A(0x09) = 127, A(0x0A) = 0x78, TAIL(64, 127) } },
    { 1, 0x01, 0x53, G_4TAP_DELAY, N, { 0x0B, 0x12 }, 0, { A(0x03) = 0x3C, A(0x04) = 0x14, A(0x05) = 0x28, A(0x06) = 0, A(0x07) = 127,
      A(0x08) = 127, A(0x09) = 127, A(0x0A) = 127, A(0x0B) = 0x50, A(0x0C) = 0x78, TAIL(64, 127) } },
    { 1, 0x01, 0x54, G_TM_DELAY, N, { 0x03, 0x05 }, 0, { A(0x03) = 0x4B, A(0x04) = 80, A(0x05) = 0x50, A(0x06) = 0x78, A(0x07) = 64,
      TAIL(64, 127) } },
    { 1, 0x01, 0x55, G_REVERB, N, { 0x05, 0x12 }, 0, { A(0x03) = 4, A(0x04) = 0x40, A(0x05) = 96, A(0x06) = 0x58, TAIL(64, 127) } },
    { 1, 0x01, 0x56, G_GATE_REVERB, N, { 0x12, 0x16 }, 0, { A(0x03) = 0, A(0x04) = 0, A(0x05) = 64, TAIL(64, 127) } },
    { 1, 0x01, 0x57, G_3D_DELAY, N, { 0x06, 0x12 }, 0, { A(0x03) = 0x14, A(0x04) = 0, A(0x05) = 7, A(0x06) = 0x50, A(0x07) = 127,
      A(0x08) = 127, A(0x09) = 127, A(0x0A) = 0x78, TAIL(64, 127) } },
    { 1, 0x01, 0x60, G_2PITCH, N, { 0x03, 0x07 }, 0, { A(0x03) = 0x47, A(0x04) = 64, A(0x05) = 0, A(0x06) = 0, A(0x07) = 0x34, A(0x08) = 64,
      A(0x09) = 0, A(0x0A) = 127, A(0x0B) = 2, A(0x0C) = 64, TAIL(64, 127) } },
    { 1, 0x01, 0x61, G_FB_PITCH, N, { 0x03, 0x05 }, 0, { A(0x03) = 0x4C, A(0x04) = 64, A(0x05) = 0x50, A(0x06) = 0, A(0x07) = 2,
      A(0x08) = 64, TAIL(64, 127) } },
    { 1, 0x01, 0x70, G_3D_AUTO, N, { 0x04, 0x06 }, 0, { A(0x03) = 64, A(0x04) = 0x14, A(0x05) = 1, A(0x06) = 1, A(0x16) = 127 } },
    { 1, 0x01, 0x71, G_3D_MANUAL, N, { 0x03, 0x16 }, 0, { A(0x03) = 64, A(0x16) = 127 } },
    { 1, 0x01, 0x72, G_LOFI1, N, { 0x12, 0x15 }, 0, { A(0x03) = 1, A(0x04) = 3, A(0x05) = 1, TAIL(127, 127) } },
    { 1, 0x01, 0x73, G_LOFI2, N, { 0x06, 0x12 }, 0, { A(0x03) = 1, A(0x04) = 1, A(0x05) = 0x48, A(0x09) = 0x78, A(0x0C) = 0x78,
      A(0x0F) = 0x78, A(0x11) = 1, TAIL(127, 127) } },
    { 1, 0x02, 0x00, G_OD_CHO, N, { 0x04, 0x0C }, 0, { OD1, CHO2, A(0x0C) = 64, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x01, G_OD_FL, N, { 0x04, 0x0C }, 0, { OD1, FL2, A(0x0C) = 64, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x02, G_OD_DLY, N, { 0x04, 0x0C }, 0, { OD1, DLY2, A(0x0C) = 32, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x03, G_DS_CHO, N, { 0x04, 0x0C }, 1, { DS1, CHO2, A(0x0C) = 64, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x04, G_DS_FL, N, { 0x04, 0x0C }, 1, { DS1, FL2, A(0x0C) = 64, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x05, G_DS_DLY, N, { 0x04, 0x0C }, 1, { DS1, DLY2, A(0x0C) = 32, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x06, G_EH_CHO, N, { 0x03, 0x0C }, 0, { EH1, CHO2, A(0x0C) = 64, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x07, G_EH_FL, N, { 0x03, 0x0C }, 0, { EH1, FL2, A(0x0C) = 64, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x08, G_EH_DLY, N, { 0x03, 0x0C }, 0, { EH1, DLY2, A(0x0C) = 32, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x09, G_CHO_DLY, N, { 0x07, 0x0C }, 0, { A(0x03) = 0x10, A(0x04) = 8, A(0x05) = 64, A(0x07) = 64, DLY2, A(0x0C) = 32,
      GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x0A, G_FL_DLY, N, { 0x06, 0x0C }, 0, { A(0x03) = 0x10, A(0x04) = 0x0B, A(0x05) = 24, A(0x06) = 0x68, A(0x07) = 64, DLY2,
      A(0x0C) = 32, GAINS, A(0x16) = 127 } },
    { 1, 0x02, 0x0B, G_CHO_FL, N, { 0x07, 0x0C }, 0, { A(0x03) = 0x0A, A(0x04) = 8, A(0x05) = 120, A(0x07) = 64, FL2, A(0x0C) = 64,
      GAINS, A(0x16) = 112 } },
    { 1, 0x02, 0x0C, G_ROT_MULTI, N, { 0x03, 0x13 }, 0, { A(0x03) = 13, A(0x04) = 1, A(0x05) = 64, A(0x06) = 72, A(0x07) = 0, A(0x08) = 64,
      A(0x09) = 64, A(0x0A) = 6, A(0x0B) = 0x71, A(0x0C) = 24, A(0x0D) = 127, A(0x0E) = 0x11, A(0x0F) = 0x78, A(0x10) = 88,
      A(0x11) = 64, A(0x12) = 96, A(0x13) = 0, A(0x16) = 96 } },
    { 1, 0x04, 0x00, G_GTR1, N, { 0x08, 0x15 }, 0, { A(0x03) = 100, A(0x04) = 80, A(0x05) = 100, A(0x06) = 1, A(0x07) = 0, A(0x08) = 80,
      A(0x09) = 0, A(0x0A) = 1, A(0x0B) = 0x45, A(0x0C) = 0x4A, A(0x0D) = 1, A(0x0E) = 0, A(0x0F) = 8, A(0x10) = 30, A(0x11) = 0x66,
      A(0x12) = 40, A(0x13) = 60, A(0x14) = 34, A(0x15) = 15, A(0x16) = 110 } },
    { 1, 0x04, 0x01, G_GTR2, N, { 0x08, 0x15 }, 0, { A(0x03) = 70, A(0x04) = 127, A(0x05) = 90, A(0x06) = 1, A(0x07) = 0, A(0x08) = 80,
      A(0x09) = 2, A(0x0A) = 1, A(0x0B) = 1, A(0x0C) = 0x4C, A(0x0D) = 56, A(0x0E) = 2, A(0x0F) = 0x45, A(0x10) = 0x36, A(0x11) = 0,
      A(0x12) = 8, A(0x13) = 96, A(0x14) = 0x66, A(0x15) = 0, A(0x16) = 80 } },
    { 1, 0x04, 0x02, G_GTR3, N, { 0x04, 0x08 }, 0, { A(0x03) = 1, A(0x04) = 60, A(0x05) = 10, A(0x06) = 1, A(0x07) = 1, A(0x08) = 80,
      A(0x09) = 2, A(0x0A) = 1, A(0x0B) = 64, A(0x0C) = 64, A(0x0D) = 1, A(0x0E) = 0, A(0x0F) = 8, A(0x10) = 0, A(0x11) = 0x59,
      A(0x12) = 50, A(0x13) = 32, A(0x14) = 64, A(0x15) = 30, A(0x16) = 88 } },
    { 1, 0x04, 0x03, G_CLEAN1, N, { 0x10, 0x14 }, 0, { A(0x03) = 50, A(0x04) = 127, A(0x05) = 75, A(0x06) = 1, A(0x07) = 64, A(0x08) = 0,
      A(0x09) = 2, A(0x0A) = 0x45, A(0x0B) = 0x34, A(0x0C) = 0, A(0x0D) = 8, A(0x0E) = 40, A(0x0F) = 0x4F, A(0x10) = 100, A(0x11) = 24,
      A(0x12) = 40, A(0x13) = 0x78, A(0x14) = 30, A(0x16) = 95 } },
    { 1, 0x04, 0x04, G_CLEAN2, N, { 0x04, 0x12 }, 0, { A(0x03) = 1, A(0x04) = 68, A(0x05) = 62, A(0x06) = 0x28, A(0x07) = 72, A(0x08) = 1,
      A(0x09) = 64, A(0x0A) = 56, A(0x0B) = 1, A(0x0C) = 64, A(0x0D) = 64, A(0x0E) = 0, A(0x0F) = 8, A(0x10) = 40, A(0x11) = 0x4F,
      A(0x12) = 100, A(0x13) = 24, A(0x14) = 40, A(0x15) = 30, A(0x16) = 95 } },
    { 1, 0x04, 0x05, G_BASS, N, { 0x08, 0x15 }, 0, { A(0x03) = 50, A(0x04) = 100, A(0x05) = 90, A(0x06) = 1, A(0x07) = 0, A(0x08) = 40,
      A(0x09) = 1, A(0x0A) = 1, A(0x0B) = 1, A(0x0C) = 64, A(0x0D) = 56, A(0x0E) = 1, A(0x0F) = 64, A(0x10) = 64, A(0x11) = 0,
      A(0x12) = 8, A(0x13) = 40, A(0x14) = 64, A(0x15) = 40, A(0x16) = 100 } },
    { 1, 0x04, 0x06, G_RHODES, N, { 0x03, 0x14 }, 0, { A(0x03) = 64, A(0x04) = 64, A(0x05) = 0x24, A(0x06) = 0x10, A(0x07) = 64,
      A(0x08) = 16, A(0x09) = 64, A(0x0A) = 0, A(0x0B) = 0x78, A(0x0C) = 0x10, A(0x0D) = 8, A(0x0E) = 40, A(0x0F) = 64,
      A(0x10) = 64, A(0x11) = 0, A(0x12) = 2, A(0x13) = 0x3C, A(0x14) = 64, A(0x15) = 1, A(0x16) = 100 } },
    { 1, 0x05, 0x00, G_KEYS, N, { 0x03, 0x0F }, 0, { A(0x03) = 64, A(0x04) = 32, A(0x05) = 64, A(0x06) = 56, A(0x07) = 1, A(0x08) = 64,
      A(0x09) = 64, A(0x0A) = 64, A(0x0B) = 72, A(0x0C) = 2, A(0x0D) = 32, A(0x0E) = 0x24, A(0x0F) = 0x10, A(0x10) = 64,
      A(0x11) = 16, A(0x12) = 32, A(0x13) = 60, A(0x14) = 40, A(0x15) = 30, A(0x16) = 100 } },
    { 1, 0x11, 0x00, G_P_CHO_DLY, N, { 0x07, 0x0C }, 0, { A(0x03) = 0x10, A(0x04) = 8, A(0x05) = 64, A(0x07) = 64, A(0x12) = 0,
      A(0x13) = 127, DLY2, A(0x0C) = 64, A(0x14) = 127, A(0x15) = 127, A(0x16) = 127 } },
    { 1, 0x11, 0x01, G_P_FL_DLY, N, { 0x06, 0x0C }, 0, { A(0x03) = 0x10, A(0x04) = 0x0B, A(0x05) = 24, A(0x06) = 0x68, A(0x07) = 64,
      A(0x12) = 0, A(0x13) = 127, DLY2, A(0x0C) = 64, A(0x14) = 127, A(0x15) = 127, A(0x16) = 127 } },
    { 1, 0x11, 0x02, G_P_CHO_FL, N, { 0x07, 0x0C }, 0, { A(0x03) = 0x10, A(0x04) = 8, A(0x05) = 64, A(0x07) = 64, A(0x12) = 0,
      A(0x13) = 127, FL2, A(0x0C) = 64, A(0x14) = 127, A(0x15) = 127, A(0x16) = 127 } },
    { 1, 0x11, 0x03, G_P_OD_OD, N, { 0x04, 0x09 }, 0, { A(0x03) = 0, A(0x04) = 48, A(0x05) = 1, A(0x06) = 1, A(0x12) = 0, A(0x13) = 96,
      A(0x08) = 1, A(0x09) = 76, A(0x0A) = 3, A(0x0B) = 1, A(0x14) = 127, A(0x15) = 84, A(0x16) = 127 } },
    { 1, 0x11, 0x04, G_P_OD_ROT, N, { 0x04, 0x11 }, 0, { A(0x03) = 0, A(0x04) = 48, A(0x05) = 1, A(0x06) = 1, A(0x12) = 0, A(0x13) = 96,
      ROT2, A(0x14) = 127, A(0x15) = 127, A(0x16) = 127 } },
    { 1, 0x11, 0x05, G_P_OD_PH, N, { 0x04, 0x09 }, 0, { A(0x03) = 0, A(0x04) = 48, A(0x05) = 1, A(0x06) = 1, A(0x12) = 0, A(0x13) = 96,
      A(0x08) = 0x24, A(0x09) = 0x10, A(0x0A) = 64, A(0x0B) = 16, A(0x0C) = 127, A(0x14) = 127, A(0x15) = 127, A(0x16) = 127 } },
    { 1, 0x11, 0x06, G_P_OD_AW, N, { 0x04, 0x0A }, 0, { A(0x03) = 0, A(0x04) = 48, A(0x05) = 1, A(0x06) = 1, A(0x12) = 0, A(0x13) = 96,
      AW2, A(0x14) = 127, A(0x15) = 127, A(0x16) = 127 } },
    { 1, 0x11, 0x07, G_P_PH_ROT, N, { 0x04, 0x11 }, 0, { PH1, A(0x12) = 0, A(0x13) = 127, ROT2, A(0x14) = 127, A(0x15) = 127,
      A(0x16) = 127 } },
    { 1, 0x11, 0x08, G_P_PH_AW, N, { 0x04, 0x0A }, 0, { PH1, A(0x12) = 0, A(0x13) = 127, AW2, A(0x14) = 127, A(0x15) = 127,
      A(0x16) = 127 } },

    /* XG (MU100): parameters 1-16 as [0]-[15]; the dry/wet is parameter 10 unless noted. */
    { 0, 0x49, 0x01, X_COMP_DIST, 9, { 0, N }, 1, { 40, 20, 72, 53, 48, 0, 43, 74, 10, 127, 120, 5, 10, 100, 3 } },
    { 0, 0x4E, 0x01, X_WAH_DIST, 9, { 2, N }, 1, { 70, 56, 39, 25, 0, 16, 64, 46, 64, 127, 40, 64, 64, 46, 100 } },
    { 0, 0x4E, 0x02, X_WAH_DIST, 9, { 2, N }, 0, { 70, 56, 39, 25, 0, 16, 64, 46, 64, 127, 30, 64, 64, 46, 100 } },
    { 0, 0x50, 0x00, X_PITCH, 9, { 0, N }, 0, { 64, 0, 72, 56, 64, 0, 0, 0, 0, 64, 32, 100, 96, 100 } },
    { 0, 0x50, 0x01, X_PITCH, 9, { 0, N }, 0, { 76, 0, 64, 64, 64, 0, 0, 0, 0, 64, 64, 100, 64, 0 } },
    { 0, 0x51, 0x00, X_EXCITER, N, { 1, N }, 0, { 40, 64, 64 } },
    { 0, 0x52, 0x00, X_TOUCH_WAH, 9, { 1, N }, 0, { 72, 40, 40, 0, 0, 16, 64, 46, 64, 127, 0 } },
    { 0, 0x52, 0x01, X_TOUCH_WAH_DIST, 9, { 1, N }, 1, { 72, 40, 40, 0, 0, 16, 64, 46, 64, 127, 40, 64, 64, 46, 100, 30 } },
    { 0, 0x52, 0x02, X_TOUCH_WAH_DIST, 9, { 1, N }, 0, { 72, 40, 40, 0, 0, 16, 64, 46, 64, 127, 30, 64, 64, 46, 100, 30 } },
    { 0, 0x52, 0x08, X_TOUCH_WAH, 9, { 1, N }, 0, { 80, 30, 70, 0, 0, 16, 64, 46, 64, 127, 0 } },
    { 0, 0x53, 0x00, X_COMPRESSOR, N, { N, N }, 0, { 5, 10, 100, 3, 100 } },
    { 0, 0x54, 0x00, X_NOISE_GATE, N, { N, N }, 0, { 0, 8, 70, 100 } },
    { 0, 0x55, 0x00, X_VOICE_CANCEL, N, { N, N }, 0, { 10, 10 } },
    { 0, 0x56, 0x00, X_ROTARY2, 9, { 0, N }, 0, { 40, 0, 0, 64, 0, 16, 64, 46, 64, 127, 30, 90 } },
    { 0, 0x57, 0x00, X_ENSEMBLE, 9, { 9, N }, 0, { 74, 10, 30, 0, 0, 16, 64, 46, 64, 64 } },
    { 0, 0x58, 0x00, X_AMBIENCE, 9, { 9, N }, 0, { 18, 10, 8, 0, 60, 30, 30, 30, 0, 64 } },
    { 0, 0x5D, 0x00, X_TALK, N, { 0, N }, 0, { 0, 20, 64, 100 } },
    { 0, 0x5E, 0x00, X_LOFI, 9, { 0, N }, 0, { 4, 60, 64, 54, 0, 10, 0, 0, 0, 127, 1 } },
    { 0, 0x5F, 0x00, X_DIST_DELAY, 9, { 5, N }, 1, { 2500, 3750, 2500, 80, 64, 40, 100, 64, 64, 127 } },
    { 0, 0x5F, 0x01, X_DIST_DELAY, 9, { 5, N }, 0, { 2500, 3750, 2500, 80, 64, 30, 100, 64, 64, 127 } },
    { 0, 0x60, 0x00, X_COMP_DIST_DELAY, 9, { 3, N }, 1, { 3000, 80, 50, 40, 100, 64, 64, 0, 0, 127, 5, 10, 100, 3 } },
    { 0, 0x60, 0x01, X_COMP_DIST_DELAY, 9, { 3, N }, 0, { 3000, 80, 50, 30, 100, 64, 64, 0, 0, 127, 5, 10, 100, 3 } },
    { 0, 0x61, 0x00, X_WAH_DIST_DELAY, 9, { 11, N }, 1, { 3000, 80, 50, 40, 100, 64, 64, 0, 0, 127, 72, 40, 40, 30 } },
    { 0, 0x61, 0x01, X_WAH_DIST_DELAY, 9, { 11, N }, 0, { 3000, 80, 50, 30, 100, 64, 64, 0, 0, 127, 72, 40, 40, 30 } },
};
#undef N

static const efx_type* find(int gs, int msb, int lsb, int any_lsb)
{
    const efx_type* low = NULL;
    size_t i;
    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (types[i].gs != gs || types[i].msb != msb) continue;
        if (types[i].lsb == lsb) return &types[i];
        if (types[i].lsb == 0 && !low) low = &types[i];
    }
    return any_lsb ? low : NULL;
}

const efx_type* efx_find_gs(int msb, int lsb) { return find(1, msb, lsb, 0); }
const efx_type* efx_find_xg(int msb, int lsb) { return find(0, msb, lsb, 1); }
int efx_type_is_gs(const efx_type* t) { return t && t->gs; }
int efx_type_dry_wet(const efx_type* t) { return t ? t->dw : -1; }
int efx_type_ctl(const efx_type* t, int which) { return t && which >= 0 && which < 2 ? t->ctl[which] : -1; }
int efx_type_lsb(const efx_type* t) { return t ? t->lsb : -1; }

int fa_midi_efx_gs_count(void)
{
    int n = 0;
    size_t i;
    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) n += types[i].gs;
    return n;
}

int fa_midi_efx_xg_count(void)
{
    return (int)(sizeof(types) / sizeof(types[0])) - fa_midi_efx_gs_count();
}

void efx_load_defaults(fa_midi_efx* e)
{
    int i;
    for (i = 0; i < 20; i++) e->p[i] = e->t->def[i];
}

/* ---- tables --------------------------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* GS (SC-88 Pro effect data table) */
static double gs_rate1(int v)
{
    v = clampi(v, 0, 127);
    if (v <= 0x63) return 0.05 + 0.05 * v;
    if (v <= 0x77) return 5.1 + 0.1 * (v - 0x64);
    if (v <= 0x7D) return 7.5 + 0.5 * (v - 0x78);
    return 10.0;
}
static double gs_rate2(int v) { return 0.05 + 0.05 * clampi(v, 0, 127); }
static double gs_pre_ms(int v)
{
    v = clampi(v, 0, 127);
    if (v <= 0x32) return v * 0.1;
    if (v <= 0x3B) return 5.5 + 0.5 * (v - 0x33);
    if (v <= 0x64) return 10.0 + (v - 0x3C);
    if (v <= 0x7D) return 52.0 + 2.0 * (v - 0x65);
    return 100.0;
}
static double gs_delay1_ms(int v)   /* 200 ms .. 1 s */
{
    v = clampi(v, 0, 127);
    if (v <= 0x46) return 200.0 + 5.0 * v;
    return fmin(560.0 + 10.0 * (v - 0x47), 1000.0);
}
static double gs_delay3_ms(int v)   /* 0 .. 500 ms */
{
    v = clampi(v, 0, 127);
    if (v <= 0x3F) return gs_pre_ms(v);
    if (v <= 0x5A) return 14.0 + (v - 0x40);
    if (v <= 0x6F) return 50.0 + 10.0 * (v - 0x5B);
    if (v <= 0x73) return 260.0 + 10.0 * (v - 0x70);
    if (v <= 0x7D) return 300.0 + 20.0 * (v - 0x74);
    return 500.0;
}
static double gs_delay4_ms(int v) { return 5.0 * clampi(v, 0, 127); }   /* 0 .. 635 ms */
static const double gs_third_oct[18] = { 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 0 };
static double gs_hf_damp(int v) { return gs_third_oct[2 + (clampi(v, 0, 127) >> 3)]; }    /* 315 .. 8k, bypass (0) */
static double gs_cutoff(int v) { return gs_third_oct[1 + (clampi(v, 0, 127) >> 3)]; }     /* 250 .. 8k */
static double gs_eq_freq(int v) { return gs_third_oct[clampi(v, 0, 127) >> 3]; }          /* 200 .. 6.3k */
static double gs_lpf(int v) { int k = clampi(v, 0, 127) >> 3; return k == 15 ? 0.0 : gs_third_oct[1 + k]; }   /* 250 .. 6.3k, bypass */
static double gs_manual(int v)
{
    v = clampi(v, 0, 127);
    if (v <= 0x14) return 100.0 + 10.0 * v;
    if (v <= 0x37) return 320.0 + 20.0 * (v - 0x15);
    if (v <= 0x3F) return 1100.0 + 100.0 * (v - 0x38);
    if (v <= 0x7C) return 1900.0 + 100.0 * (v - 0x40);
    return 8000.0;
}
static float gs_azimuth(int v) { float d = (float)(((clampi(v, 0, 127) - 64 + 2) >> 2) * 12); return d > 180.0f ? 180.0f : d < -180.0f ? -180.0f : d; }
static double gs_gain(int v) { return (double)clampi(v - 64, -12, 12); }
static float gs_fb(int v) { float f = (float)(clampi(v, 0, 127) - 64) * 0.02f; return f > 0.98f ? 0.98f : f < -0.98f ? -0.98f : f; }
static float unit(int v) { return (float)clampi(v, 0, 127) / 127.0f; }
static double gs_q(int v) { static const double q[5] = { 0.5, 1.0, 2.0, 4.0, 9.0 }; return q[clampi(v, 0, 4)]; }
/* Acceleration 0-15 as a glide coefficient per frame: 15 about 60 ms, 0 about 4 s. */
static float accel(uint32_t rate, int raw) { double t = 4.0 * pow(2.0, -(clampi(raw, 0, 127) >> 3) / 2.5); return (float)(1.0 - exp(-1.0 / (t * rate))); }

/* XG */
static double xg_attack_ms(int v) { static const double t[20] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 23, 26, 30, 35, 40 }; return t[clampi(v, 0, 19)]; }
static double xg_release_ms(int v) { static const double t[16] = { 10, 15, 25, 35, 45, 55, 65, 75, 85, 100, 115, 140, 170, 230, 340, 680 }; return t[clampi(v, 0, 15)]; }
static double xg_ratio(int v) { static const double t[8] = { 1.0, 1.5, 2.0, 3.0, 5.0, 7.0, 10.0, 20.0 }; return t[clampi(v, 0, 7)]; }
static float xg_level(int v) { return (float)clampi(v, 0, 127) / 100.0f; }

static float coef_ms(uint32_t rate, double ms) { return (float)(1.0 - exp(-1.0 / (ms * 0.001 * rate))); }
static float frames(uint32_t rate, double ms) { return (float)(ms * rate / 1000.0); }

/* ---- building blocks ----------------------------------------------------------- */

/* The next stage, its settings back at their neutral values (its state is kept, so a
 * parameter change does not restart the sound). */
static efx_stage* add(fa_midi_efx* e, int kind)
{
    efx_stage* s = &e->st[e->nst++];
    if (s->kind != kind) {
        efx_stage_free(s);
        s->kind = kind;
    }
    s->dry = 0.0f; s->wet = 1.0f; s->pan = -1.0f; s->mono_in = 0; s->nsec = 0;
    s->drive = 1.0f; s->edge = 0.0f; s->level = 1.0f; s->att = s->rel = 0.0f; s->sens = 0.0f;
    s->hz = 0.0; s->depth = 0.0f; s->phase = 0.0f; s->wave = WAVE_SIN; s->step = 0;
    s->base = s->range = s->reso = s->fb = 0.0f; s->npole = 0; s->ftype = FILT_BP; s->down = 0;
    s->glide = 0.0f; s->need = 0; s->ntaps = 0; s->fb_t[0] = s->fb_t[1] = 0.0f; s->fb_gain = 0.0f; s->fb_cross = 0;
    s->damp_a = s->pre_a = 0.0f; s->pre_hp = 0; s->voices = 0; s->window = 0.0f;
    s->trem_hz = 0.0; s->trem_depth = s->trem_phase = 0.0f;
    s->sep = 0.0f; s->xover_a = 0.0f; s->fast_on = 0;
    s->dyn = DYN_COMP; s->thresh = 0.0f; s->ratio = 1.0f; s->makeup = 1.0f;
    s->hold_n = 1; s->levels = 0.0f; s->hum_hz = 50.0f; s->crackle = 0; s->mono_out = 0;
    memset(s->noise, 0, sizeof(s->noise));
    memset(s->noise_lp, 0, sizeof(s->noise_lp));
    s->ring_hz = 0.0; s->az = s->az_speed = s->az_from = 0.0f; s->az_turn = 0;
    return s;
}

static void sec(fa_midi_efx* e, efx_stage* s, int type, double hz, double q, double db)
{
    if (s->nsec < EFX_SECS) s->sec[s->nsec++] = efx_biquad(type, hz, q, db, e->rate);
}

static void balance(efx_stage* s, int v) { s->wet = unit(v); s->dry = 1.0f - s->wet; }
static void mix(efx_stage* s, int v) { s->dry = 1.0f; s->wet = unit(v); }

static void pans(float p, float g, float* gl, float* gr)
{
    *gl = g * (p <= 0.5f ? 1.0f : (1.0f - p) * 2.0f);
    *gr = g * (p >= 0.5f ? 1.0f : p * 2.0f);
}

/* The GS ending: low (200 Hz) and high (4 kHz) shelves, pan (mono types, else -1), level. */
static void post(fa_midi_efx* e, double lo_db, double hi_db, int pan, int level)
{
    e->post = 1;
    e->lo = efx_biquad(BQ_LOSHELF, 200.0, 0.7071, lo_db, e->rate);
    e->hi = efx_biquad(BQ_HISHELF, 4000.0, 0.7071, hi_db, e->rate);
    e->out_mono = pan >= 0;
    pans(pan >= 0 ? unit(pan) : 0.5f, unit(level), &e->out_gl, &e->out_gr);
}

/* Drive: soft (overdrive) or harder (distortion) clipping, then an amp's tone. */
static void drive_gs(fa_midi_efx* e, efx_stage* s, int dist, int drive, int amp, int amp_on)
{
    s->mono_in = 1;
    s->drive = (float)pow(10.0, unit(drive) * (dist ? 3.0 : 2.0));
    s->edge = dist ? 0.6f : 0.0f;
    s->level = dist ? 0.35f : 0.5f;
    if (!amp_on) { sec(e, s, BQ_LP, 9000.0, 0.7071, 0.0); return; }
    switch (clampi(amp, 0, 3)) {
    case 0: sec(e, s, BQ_LP, 3200.0, 0.9, 0.0); sec(e, s, BQ_HP, 150.0, 0.7071, 0.0); break;    /* small */
    case 1: sec(e, s, BQ_LP, 4000.0, 0.8, 0.0); sec(e, s, BQ_HP, 100.0, 0.7071, 0.0); break;    /* built-in */
    case 2: sec(e, s, BQ_LP, 5000.0, 0.7071, 0.0); sec(e, s, BQ_PEAK, 150.0, 1.0, 3.0); break;  /* 2 stack */
    default: sec(e, s, BQ_LP, 6000.0, 0.7071, 0.0); sec(e, s, BQ_LOSHELF, 120.0, 0.7071, 5.0); break;   /* 3 stack */
    }
}

static void enhancer(fa_midi_efx* e, efx_stage* s, int sens, int amount, double hpf)
{
    s->pre_a = efx_pole(e->rate, hpf);
    s->drive = 1.0f + unit(sens) * 12.0f;
    s->level = unit(amount);
}

/* Modulated voices: a stereo chorus (or flanger, shorter and with feedback). */
static void chorus(fa_midi_efx* e, efx_stage* s, double pre_ms, double hz, int depth, float fb, double phase_deg, int flanger)
{
    float pre = frames(e->rate, pre_ms) + 1.0f, dep = frames(e->rate, unit(depth) * (flanger ? 4.0 : 8.0));
    int v;
    s->voices = 2;
    s->hz = hz;
    for (v = 0; v < 2; v++) {
        s->v_ch[v] = (uint8_t)v;
        s->v_pre[v] = pre;
        s->v_depth[v] = dep;
        s->v_gl[v] = v ? 0.0f : 1.0f;
        s->v_gr[v] = v ? 1.0f : 0.0f;
        s->v_phase[v] = v ? (float)(phase_deg / 360.0) : 0.0f;
    }
    s->fb_gain = fb;
    s->need = (int)(pre + dep) + 4;
}

/* Echo: one line, the delay heard on both sides, repeating. */
static void echo(fa_midi_efx* e, efx_stage* s, double ms, float fb, double hf)
{
    float t = frames(e->rate, ms) + 1.0f;
    s->mono_in = 1;
    s->ntaps = 1;
    s->tap_t[0] = t; s->tap_ch[0] = 0; s->tap_gl[0] = s->tap_gr[0] = 1.0f;
    s->fb_t[0] = t;
    s->fb_t[1] = 0.0f;
    s->fb_gain = fb;
    s->damp_a = efx_pole(e->rate, hf);
    s->need = (int)t + 4;
}

static void phaser(fa_midi_efx* e, efx_stage* s, double man_hz, double hz, int depth, int reso, int amount)
{
    float m = unit(amount);
    (void)e;
    s->base = (float)man_hz;
    s->hz = hz;
    s->depth = unit(depth);
    s->range = 4.0f;
    s->fb = unit(reso) * 0.9f;
    s->npole = 6;
    s->dry = 1.0f / (1.0f + m);
    s->wet = m / (1.0f + m);
}

/* Wah: a resonant filter the input's level (sens) and an LFO sweep up or down from the manual setting. */
static void wah(fa_midi_efx* e, efx_stage* s, int lpf, int sens, int man, int peak, double hz, int depth, int up)
{
    s->mono_in = 1;
    s->ftype = lpf ? FILT_LP : FILT_BP;
    s->sens = unit(sens);
    s->base = (float)(200.0 * pow(2.0, unit(man) * 4.0));
    s->reso = 0.7f + unit(peak) * 9.0f;
    s->hz = hz;
    s->depth = unit(depth);
    s->range = 3.0f;
    s->down = !up;
    s->att = coef_ms(e->rate, 2.0);
    s->rel = coef_ms(e->rate, 60.0);
}

static void rotary(fa_midi_efx* e, efx_stage* s, const int* r)   /* r: low slow, fast, accel, level, high slow, fast, accel, level, separation, speed */
{
    s->slow[0] = gs_rate1(r[0]); s->fast[0] = gs_rate1(r[1]); s->rot_acc[0] = accel(e->rate, r[2]); s->rot_lev[0] = unit(r[3]);
    s->slow[1] = gs_rate1(r[4]); s->fast[1] = gs_rate1(r[5]); s->rot_acc[1] = accel(e->rate, r[6]); s->rot_lev[1] = unit(r[7]);
    s->sep = unit(r[8]);
    s->fast_on = r[9] >= 64;
    s->xover_a = efx_pole(e->rate, 800.0);
    s->need = (int)frames(e->rate, 2.0) + 4;
}

static void comp_gs(fa_midi_efx* e, efx_stage* s, int attack, int sustain, int post_gain)
{
    s->dyn = DYN_COMP;
    s->att = coef_ms(e->rate, 0.5 + unit(attack) * 50.0);
    s->rel = coef_ms(e->rate, 150.0);
    s->thresh = -unit(sustain) * 40.0f;
    s->ratio = 4.0f;
    s->makeup = (float)pow(10.0, clampi(post_gain, 0, 3) * 6.0 / 20.0);
}

/* The multis' EQ: low shelf 200 Hz, a middle peak, high shelf 4 kHz. */
static void eq3_gs(fa_midi_efx* e, efx_stage* s, int lo, int mid_fq, int mid_q, int mid, int hi)
{
    sec(e, s, BQ_LOSHELF, 200.0, 0.7071, gs_gain(lo));
    sec(e, s, BQ_PEAK, gs_eq_freq(mid_fq), gs_q(mid_q), gs_gain(mid));
    sec(e, s, BQ_HISHELF, 4000.0, 0.7071, gs_gain(hi));
}

/* The multis' chorus or flanger: rate (table 2), depth, feedback, mixed in. */
static void cf_gs(fa_midi_efx* e, efx_stage* s, int sel, int rate, int depth, int fb, int amount)
{
    chorus(e, s, sel ? 1.0 : 6.0, gs_rate2(rate), depth, sel ? gs_fb(fb) : gs_fb(fb) * 0.3f, 90.0, sel);
    mix(s, amount);
}

static void delay_multi(fa_midi_efx* e, efx_stage* s, double ms, int fb, int amount, double hf)
{
    echo(e, s, ms, unit(fb) * 0.9f, hf);
    s->mono_in = 0;
    mix(s, amount);
}

static void drive_xg(fa_midi_efx* e, efx_stage* s, int dist, int drive, int level, int lo, int mid, int lpf)
{
    s->drive = (float)pow(10.0, unit(drive) * 2.5);
    s->edge = dist ? 0.5f : 0.0f;
    s->level = unit(level) * 0.5f;
    sec(e, s, BQ_LOSHELF, 250.0, 0.7071, fa_xg_eq_db(lo));
    sec(e, s, BQ_PEAK, 1200.0, 1.0, fa_xg_eq_db(mid));
    if (lpf >= 0) sec(e, s, BQ_LP, fa_xg_eq_hz(lpf), 0.7071, 0.0);
}

static void eq_xg(fa_midi_efx* e, efx_stage* s, const int* p)   /* low freq, gain, high freq, gain */
{
    sec(e, s, BQ_LOSHELF, fa_xg_eq_hz(p[0]), 0.7071, fa_xg_eq_db(p[1]));
    sec(e, s, BQ_HISHELF, fa_xg_eq_hz(p[2]), 0.7071, fa_xg_eq_db(p[3]));
}

static void comp_xg(fa_midi_efx* e, efx_stage* s, int attack, int release, int threshold, int ratio, int level)
{
    s->dyn = DYN_COMP;
    s->att = coef_ms(e->rate, xg_attack_ms(attack));
    s->rel = coef_ms(e->rate, xg_release_ms(release));
    s->thresh = (float)clampi(threshold - 127, -48, -6);
    s->ratio = (float)xg_ratio(ratio);
    s->makeup = level >= 0 ? xg_level(level) : 1.0f;
}

static void echo_xg(fa_midi_efx* e, efx_stage* s, double l_ms, double r_ms, double fb_ms, int fb, int amount)
{
    float tl = frames(e->rate, l_ms) + 1.0f, tr = frames(e->rate, r_ms) + 1.0f;
    s->ntaps = 2;
    s->tap_t[0] = tl; s->tap_ch[0] = 0; s->tap_gl[0] = 1.0f; s->tap_gr[0] = 0.0f;
    s->tap_t[1] = tr; s->tap_ch[1] = 1; s->tap_gl[1] = 0.0f; s->tap_gr[1] = 1.0f;
    s->fb_t[0] = s->fb_t[1] = frames(e->rate, fb_ms) + 1.0f;
    s->fb_gain = fa_xg_feedback(fb);
    s->need = (int)fmax(fmax(tl, tr), s->fb_t[0]) + 4;
    mix(s, amount);
}

static const float vowels[5][2] = { { 730, 1090 }, { 270, 2290 }, { 300, 870 }, { 530, 1840 }, { 570, 840 } };   /* a i u e o */

/* ---- the types ------------------------------------------------------------------ */

/* GS parameter at a SysEx address (03-16), with the controls on the type's + and # parameters. */
static int P(const fa_midi_efx* e, int addr)
{
    int i = addr - 3, v = e->p[i], k;
    for (k = 0; k < 2; k++)
        if (e->t->ctl[k] == addr && e->ctl[k] != 0.0f) v = clampi((int)lrintf((float)v + e->ctl[k] * 127.0f), 0, 127);
    return v;
}

/* XG parameter n (1-16), with the controllers on the type's controlled parameter. */
static int X(const fa_midi_efx* e, int n)
{
    int v = e->p[n - 1];
    if (e->t->ctl[0] == n - 1 && e->xg_ctl != 0.0f) v = clampi((int)lrintf((float)v + e->xg_ctl * 127.0f), 0, v > 127 ? 16383 : 127);
    return v;
}

static void build_gs_series(fa_midi_efx* e, int first, int second)
{
    efx_stage* s;
    int b = e->t->build;
    if (first == 0) {
        s = add(e, ST_DRIVE);
        drive_gs(e, s, e->t->od, P(e, 0x03), P(e, 0x05), P(e, 0x06));
        s->pan = unit(P(e, 0x04));
        s->mono_in = 1;
    } else if (first == 1) {
        enhancer(e, add(e, ST_ENHANCE), P(e, 0x03), P(e, 0x04), 2500.0);
    } else if (first == 2) {
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x03)), gs_rate1(P(e, 0x04)), P(e, 0x05), 0.0f, 90.0, 0);
        balance(s, P(e, 0x07));
    } else {
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x03)), gs_rate1(P(e, 0x04)), P(e, 0x05), gs_fb(P(e, 0x06)), 90.0, 1);
        balance(s, P(e, 0x07));
    }
    if (second == 0) {
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x08)), gs_rate1(P(e, 0x09)), P(e, 0x0A), 0.0f, 90.0, 0);
    } else if (second == 1) {
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x08)), gs_rate1(P(e, 0x09)), P(e, 0x0A), gs_fb(P(e, 0x0B)), 90.0, 1);
    } else {
        s = add(e, ST_DELAY);
        echo(e, s, gs_delay3_ms(P(e, 0x08)), gs_fb(P(e, 0x09)), gs_hf_damp(P(e, 0x0A)));
        s->mono_in = 0;
    }
    balance(s, P(e, 0x0C));
    (void)b;
    post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
}

/* One chain of a parallel type: OD, chorus, flanger, delay, phaser, rotary or auto wah
 * from `at`; its pan and level at `pan`, `pan` + 1. */
enum { PC_OD, PC_CHO, PC_FL, PC_DLY, PC_PH, PC_ROT, PC_AW };
static void parallel_chain(fa_midi_efx* e, int kind, int at, int pan, int which)
{
    efx_stage* s;
    int r[10], i;
    switch (kind) {
    case PC_OD:
        drive_gs(e, add(e, ST_DRIVE), P(e, at) != 0, P(e, at + 1), P(e, at + 2), P(e, at + 3));
        break;
    case PC_CHO:
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, at)), gs_rate1(P(e, at + 1)), P(e, at + 2), 0.0f, 90.0, 0);
        balance(s, P(e, at + 4));
        break;
    case PC_FL:
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, at)), gs_rate1(P(e, at + 1)), P(e, at + 2), gs_fb(P(e, at + 3)), 90.0, 1);
        balance(s, P(e, at + 4));
        break;
    case PC_DLY:
        s = add(e, ST_DELAY);
        echo(e, s, gs_delay3_ms(P(e, at)), gs_fb(P(e, at + 1)), gs_hf_damp(P(e, at + 2)));
        balance(s, P(e, at + 4));
        break;
    case PC_PH:
        phaser(e, add(e, ST_PHASER), gs_manual(P(e, at)), gs_rate1(P(e, at + 1)), P(e, at + 2), P(e, at + 3), P(e, at + 4));
        break;
    case PC_ROT:
        for (i = 0; i < 10; i++) r[i] = P(e, at + i);
        rotary(e, add(e, ST_ROTARY), r);
        break;
    default:
        wah(e, add(e, ST_WAH), P(e, at) == 0, P(e, at + 1), P(e, at + 2), P(e, at + 3), gs_rate1(P(e, at + 4)), P(e, at + 5), P(e, at + 6));
        break;
    }
    pans(unit(P(e, pan)), unit(P(e, pan + 1)), &e->par_g[which][0], &e->par_g[which][1]);
}

static void build_gs_parallel(fa_midi_efx* e, int a, int b, int a_at, int b_at)
{
    parallel_chain(e, a, a_at, 0x12, 0);
    e->par = e->nst;
    parallel_chain(e, b, b_at, 0x14, 1);
    post(e, 0.0, 0.0, -1, P(e, 0x16));
}

static void build_gs(fa_midi_efx* e)
{
    efx_stage* s;
    int i, r[10];
    switch (e->t->build) {
    case G_THRU:
        return;
    case G_STEREO_EQ:
        s = add(e, ST_EQ);
        sec(e, s, BQ_LOSHELF, P(e, 0x03) ? 400.0 : 200.0, 0.7071, gs_gain(P(e, 0x04)));
        sec(e, s, BQ_HISHELF, P(e, 0x05) ? 8000.0 : 4000.0, 0.7071, gs_gain(P(e, 0x06)));
        sec(e, s, BQ_PEAK, gs_eq_freq(P(e, 0x07)), gs_q(P(e, 0x08)), gs_gain(P(e, 0x09)));
        sec(e, s, BQ_PEAK, gs_eq_freq(P(e, 0x0A)), gs_q(P(e, 0x0B)), gs_gain(P(e, 0x0C)));
        post(e, 0.0, 0.0, -1, P(e, 0x16));
        return;
    case G_SPECTRUM: {
        static const double band[8] = { 250, 500, 1000, 1250, 2000, 3150, 4000, 8000 };
        static const double width[5] = { 4.0, 2.8, 2.0, 1.4, 1.0 };
        s = add(e, ST_EQ);
        for (i = 0; i < 8; i++) sec(e, s, BQ_PEAK, band[i], width[clampi(P(e, 0x0B), 0, 4)], gs_gain(P(e, 0x03 + i)));
        post(e, 0.0, 0.0, P(e, 0x15), P(e, 0x16));
        return;
    }
    case G_ENHANCER:
        enhancer(e, add(e, ST_ENHANCE), P(e, 0x03), P(e, 0x04), 2500.0);
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_HUMANIZER: {
        int v = clampi(P(e, 0x05), 0, 4);
        s = add(e, ST_FORMANT);
        s->mono_in = 1;
        s->drive = P(e, 0x04) ? 1.0f + unit(P(e, 0x03)) * 8.0f : 1.0f;
        s->f1_to = vowels[v][0];
        s->f2_to = vowels[v][1];
        s->glide = 0.02f + (float)(clampi(P(e, 0x06), 0, 127) >> 3) / 15.0f * 0.5f;
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), P(e, 0x15), P(e, 0x16));
        return;
    }
    case G_OVERDRIVE: case G_DISTORTION:
        drive_gs(e, add(e, ST_DRIVE), e->t->od, P(e, 0x03), P(e, 0x04), P(e, 0x05));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), P(e, 0x15), P(e, 0x16));
        return;
    case G_PHASER:
        phaser(e, add(e, ST_PHASER), gs_manual(P(e, 0x03)), gs_rate1(P(e, 0x04)), P(e, 0x05), P(e, 0x06), P(e, 0x07));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_AUTO_WAH:
        wah(e, add(e, ST_WAH), P(e, 0x03) == 0, P(e, 0x04), P(e, 0x05), P(e, 0x06), gs_rate1(P(e, 0x07)), P(e, 0x08), P(e, 0x09));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), P(e, 0x15), P(e, 0x16));
        return;
    case G_ROTARY:
        for (i = 0; i < 9; i++) r[i] = P(e, 0x03 + i);
        r[9] = P(e, 0x0D);
        rotary(e, add(e, ST_ROTARY), r);
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_ST_FLANGER:
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x05)), gs_rate1(P(e, 0x06)), P(e, 0x07), gs_fb(P(e, 0x08)), P(e, 0x09) * 2.0, 1);
        if (P(e, 0x03)) { s->pre_a = efx_pole(e->rate, gs_cutoff(P(e, 0x04))); s->pre_hp = P(e, 0x03) == 2; }
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_STEP_FLANGER:
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x03)), gs_rate1(P(e, 0x04)), P(e, 0x05), gs_fb(P(e, 0x06)), P(e, 0x07) * 2.0, 1);
        s->step = (int)(e->rate / gs_rate1(P(e, 0x08)));
        s->wave = WAVE_TRI;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_TREMOLO: case G_AUTO_PAN:
        s = add(e, ST_TREMOLO);
        s->wave = clampi(P(e, 0x03), 0, 4);
        s->hz = gs_rate1(P(e, 0x04));
        s->depth = unit(P(e, 0x05));
        s->phase = e->t->build == G_AUTO_PAN ? 0.5f : 0.0f;
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_COMPRESSOR:
        comp_gs(e, add(e, ST_DYNAMICS), P(e, 0x03), P(e, 0x04), P(e, 0x05));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), P(e, 0x15), P(e, 0x16));
        return;
    case G_LIMITER: {
        static const float ratio[4] = { 1.5f, 2.0f, 4.0f, 100.0f };
        s = add(e, ST_DYNAMICS);
        s->dyn = DYN_LIMIT;
        s->thresh = (float)(clampi(P(e, 0x03), 0, 127) - 127) * 0.375f;
        s->ratio = ratio[clampi(P(e, 0x04), 0, 3)];
        s->att = coef_ms(e->rate, 0.5);
        s->rel = coef_ms(e->rate, 5.0 + clampi(P(e, 0x05), 0, 127) * 4.0);
        s->makeup = (float)pow(10.0, clampi(P(e, 0x06), 0, 3) * 6.0 / 20.0);
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), P(e, 0x15), P(e, 0x16));
        return;
    }
    case G_HEXA_CHORUS: {
        float pre = frames(e->rate, gs_pre_ms(P(e, 0x03))) + 1.0f, dep = frames(e->rate, unit(P(e, 0x05)) * 8.0);
        float pre_dev = frames(e->rate, clampi(P(e, 0x06), 0, 20) * 0.5), depth_dev = (float)clampi(P(e, 0x07) - 64, -20, 20) / 40.0f;
        float pan_dev = (float)clampi(P(e, 0x08), 0, 20) / 20.0f;
        s = add(e, ST_MOD);
        s->mono_in = 1;
        s->voices = 6;
        s->hz = gs_rate1(P(e, 0x04));
        for (i = 0; i < 6; i++) {
            float p = 0.5f + (i % 2 ? 0.5f : -0.5f) * pan_dev * (1.0f - (float)(i / 2) / 3.0f);
            s->v_ch[i] = 0;
            s->v_pre[i] = pre + pre_dev * (float)i / 5.0f;
            s->v_depth[i] = dep * (1.0f + ((float)i - 2.5f) / 2.5f * depth_dev);
            s->v_phase[i] = (float)i / 6.0f;
            pans(p, 0.4f, &s->v_gl[i], &s->v_gr[i]);
        }
        s->need = (int)(pre + pre_dev + dep * 1.5f) + 4;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    }
    case G_TREM_CHORUS:
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x03)), gs_rate1(P(e, 0x04)), P(e, 0x05), 0.0f, 90.0, 0);
        s->trem_hz = gs_rate1(P(e, 0x07));
        s->trem_depth = unit(P(e, 0x08));
        s->trem_phase = (float)(P(e, 0x06) * 2) / 360.0f;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_ST_CHORUS:
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x05)), gs_rate1(P(e, 0x06)), P(e, 0x07), 0.0f, P(e, 0x09) * 2.0, 0);
        if (P(e, 0x03)) { s->pre_a = efx_pole(e->rate, gs_cutoff(P(e, 0x04))); s->pre_hp = P(e, 0x03) == 2; }
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_SPACE_D: case G_3D_CHORUS:
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x03)), gs_rate1(P(e, 0x04)), P(e, 0x05), 0.0f,
               e->t->build == G_SPACE_D ? P(e, 0x06) * 2.0 : 180.0, 0);
        s->mono_in = 1;
        /* Each voice also on the other side, inverted: wide, and the mono sum keeps the dry sound clean. */
        s->v_gr[0] = s->v_gl[1] = e->t->build == G_SPACE_D ? -0.5f : -0.3f;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    case G_ST_DELAY: case G_MOD_DELAY: {
        float tl = frames(e->rate, gs_delay3_ms(P(e, 0x03))) + 1.0f, tr = frames(e->rate, gs_delay3_ms(P(e, 0x04))) + 1.0f;
        int mod = e->t->build == G_MOD_DELAY;
        s = add(e, ST_DELAY);
        s->ntaps = 2;
        s->tap_t[0] = tl; s->tap_ch[0] = 0; s->tap_gl[0] = 1.0f; s->tap_gr[0] = 0.0f;
        s->tap_t[1] = tr; s->tap_ch[1] = 1; s->tap_gl[1] = 0.0f; s->tap_gr[1] = 1.0f;
        s->fb_t[0] = tl;
        s->fb_t[1] = tr;
        s->fb_gain = gs_fb(P(e, 0x05));
        s->fb_cross = P(e, 0x06) != 0;
        if (!mod) {
            if (P(e, 0x07)) s->tap_gl[0] = -1.0f;
            if (P(e, 0x08)) s->tap_gr[1] = -1.0f;
        } else {
            s->hz = gs_rate1(P(e, 0x07));
            s->depth = frames(e->rate, unit(P(e, 0x08)) * 10.0);
            s->phase = (float)(P(e, 0x09) * 2) / 360.0f;
        }
        s->damp_a = efx_pole(e->rate, gs_hf_damp(P(e, 0x0A)));
        s->need = (int)(fmax(tl, tr) + s->depth) + 4;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    }
    case G_3TAP_DELAY: case G_3D_DELAY: {
        float tc = frames(e->rate, gs_delay1_ms(P(e, 0x03))) + 1.0f, tl = frames(e->rate, gs_delay1_ms(P(e, 0x04))) + 1.0f;
        float tr = frames(e->rate, gs_delay1_ms(P(e, 0x05))) + 1.0f, cross = e->t->build == G_3D_DELAY ? -0.3f : 0.0f;
        s = add(e, ST_DELAY);
        s->mono_in = 1;
        s->ntaps = 3;
        s->tap_t[0] = tc; s->tap_ch[0] = 0; s->tap_gl[0] = s->tap_gr[0] = unit(P(e, 0x07));
        s->tap_t[1] = tl; s->tap_ch[1] = 0; s->tap_gl[1] = unit(P(e, 0x08)); s->tap_gr[1] = cross * unit(P(e, 0x08));
        s->tap_t[2] = tr; s->tap_ch[2] = 0; s->tap_gl[2] = cross * unit(P(e, 0x09)); s->tap_gr[2] = unit(P(e, 0x09));
        s->fb_t[0] = tc;
        s->fb_gain = gs_fb(P(e, 0x06));
        s->damp_a = efx_pole(e->rate, gs_hf_damp(P(e, 0x0A)));
        s->need = (int)fmax(tc, fmax(tl, tr)) + 4;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    }
    case G_4TAP_DELAY: {
        static const float gl[4] = { 1.0f, 0.0f, 0.8f, 0.4f }, gr[4] = { 0.0f, 1.0f, 0.4f, 0.8f };
        float longest = 0.0f;
        s = add(e, ST_DELAY);
        s->mono_in = 1;
        s->ntaps = 4;
        for (i = 0; i < 4; i++) {
            s->tap_t[i] = frames(e->rate, gs_delay1_ms(P(e, 0x03 + i))) + 1.0f;
            s->tap_ch[i] = 0;
            s->tap_gl[i] = gl[i] * unit(P(e, 0x07 + i));
            s->tap_gr[i] = gr[i] * unit(P(e, 0x07 + i));
            if (s->tap_t[i] > longest) longest = s->tap_t[i];
        }
        s->fb_t[0] = s->tap_t[0];
        s->fb_gain = gs_fb(P(e, 0x0B));
        s->damp_a = efx_pole(e->rate, gs_hf_damp(P(e, 0x0C)));
        s->need = (int)longest + 4;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    }
    case G_TM_DELAY: {
        float p = unit(P(e, 0x07)), g[2];
        s = add(e, ST_DELAY);
        echo(e, s, gs_delay1_ms(P(e, 0x03)), gs_fb(P(e, 0x05)), gs_hf_damp(P(e, 0x06)));
        s->glide = accel(e->rate, P(e, 0x04)) * 4.0f;
        pans(p, 1.0f, &g[0], &g[1]);
        s->tap_gl[0] = g[0];
        s->tap_gr[0] = g[1];
        s->need = (int)frames(e->rate, 1000.0) + 4;   /* the time glides: room for any */
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    }
    case G_REVERB: {
        static const uint8_t xg[6][2] = { { 0x02, 0 }, { 0x02, 1 }, { 0x03, 0 }, { 0x03, 1 }, { 0x01, 0 }, { 0x01, 1 } };
        int t = clampi(P(e, 0x03), 0, 5);
        s = add(e, ST_REVERB);
        if (!s->sub && !(s->sub = fa_midi_fx_new(e->rate))) { e->nst--; s->kind = ST_NONE; return; }
        fa_midi_fx_xg(s->sub, 0x20, 0);
        fa_midi_fx_xg(s->sub, 0x00, xg[t][0]);
        fa_midi_fx_xg(s->sub, 0x01, xg[t][1]);
        fa_midi_fx_xg(s->sub, 0x02, 4 + clampi(P(e, 0x05), 0, 127) * 40 / 127);
        fa_midi_fx_gs(s->sub, 0x37, (int)gs_pre_ms(P(e, 0x04)));
        if (gs_hf_damp(P(e, 0x06)) > 0.0) sec(e, s, BQ_LP, gs_hf_damp(P(e, 0x06)), 0.7071, 0.0);
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    }
    case G_GATE_REVERB: {
        int type = clampi(P(e, 0x03), 0, 3);
        float pre = frames(e->rate, gs_pre_ms(P(e, 0x04))) + 1.0f, span = frames(e->rate, 5.0 + clampi(P(e, 0x05), 0, 127) * 3.9);
        float norm = 0.0f, g[EFX_TAPS];
        s = add(e, ST_DELAY);
        s->mono_in = 1;
        s->ntaps = EFX_TAPS;
        for (i = 0; i < EFX_TAPS; i++) { g[i] = type == 1 ? 0.2f + 0.8f * (float)i / 15.0f : 1.0f; norm += g[i] * g[i]; }
        norm = 0.8f / sqrtf(norm);
        for (i = 0; i < EFX_TAPS; i++) {
            float p = type == 2 ? (float)i / 15.0f : type == 3 ? 1.0f - (float)i / 15.0f : (i % 2 ? 0.7f : 0.3f);
            s->tap_t[i] = pre + span * (float)i / 15.0f;
            s->tap_ch[i] = 0;
            pans(p, g[i] * norm, &s->tap_gl[i], &s->tap_gr[i]);
        }
        s->need = (int)(pre + span) + 4;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    }
    case G_2PITCH: case G_FB_PITCH: {
        static const double window_ms[5] = { 15.0, 25.0, 35.0, 50.0, 70.0 };
        int two = e->t->build == G_2PITCH, nv = two ? 2 : 1;
        float longest = 0.0f;
        s = add(e, ST_PITCH);
        s->voices = nv;
        s->window = frames(e->rate, window_ms[clampi(P(e, two ? 0x0B : 0x07), 0, 4)]);
        for (i = 0; i < nv; i++) {
            int at = 0x03 + i * 4;
            double semis = clampi(P(e, at) - 64, -24, 12) + clampi(P(e, at + 1) - 64, -50, 50) * 0.02;
            float lv = two ? (i ? unit(P(e, 0x0C)) : 1.0f - unit(P(e, 0x0C))) * 2.0f : 1.0f;
            if (lv > 1.0f) lv = 1.0f;
            s->v_ratio[i] = (float)pow(2.0, semis / 12.0);
            s->v_pre[i] = frames(e->rate, gs_delay3_ms(P(e, two ? at + 2 : 0x06)));
            pans(unit(P(e, two ? at + 3 : 0x08)), lv, &s->v_gl[i], &s->v_gr[i]);
            if (s->v_pre[i] > longest) longest = s->v_pre[i];
        }
        if (!two) s->fb_gain = gs_fb(P(e, 0x05));
        s->need = (int)(longest + s->window) + 4;
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), -1, P(e, 0x16));
        return;
    }
    case G_3D_AUTO: case G_3D_MANUAL:
        s = add(e, ST_PAN3D);
        s->az = s->az_from = gs_azimuth(P(e, 0x03));
        if (e->t->build == G_3D_AUTO) {
            s->az_speed = (float)gs_rate1(P(e, 0x04));
            s->hz = P(e, 0x05) ? 1.0 : -1.0;
            s->az_turn = P(e, 0x06) != 0;
            if (!s->az_turn && s->az_from == 0.0f) s->az_from = 90.0f;
        }
        s->need = (int)frames(e->rate, 1.0) + 4;
        post(e, 0.0, 0.0, -1, P(e, 0x16));
        return;
    case G_LOFI1: case G_LOFI2: {
        static const double filt[6] = { 0.0, 8000.0, 6000.0, 4000.0, 2500.0, 1500.0 };
        static const uint8_t kind[9][2] = { { 1, 12 }, { 2, 12 }, { 2, 10 }, { 3, 10 }, { 4, 8 }, { 6, 8 }, { 8, 6 }, { 12, 5 }, { 16, 4 } };
        int one = e->t->build == G_LOFI1, k = clampi(P(e, 0x03 + one), 0, one ? 8 : 5);
        s = add(e, ST_LOFI);
        s->hold_n = (int)lrint(kind[k][0] * e->rate / 44100.0);
        if (s->hold_n < 1) s->hold_n = 1;
        s->levels = (float)(1 << (kind[k][1] - 1));
        if (one) {
            s->pre_a = efx_pole(e->rate, filt[clampi(P(e, 0x03), 0, 5)]);
            if (filt[clampi(P(e, 0x05), 0, 5)] > 0.0) sec(e, s, BQ_LP, filt[clampi(P(e, 0x05), 0, 5)], 0.7071, 0.0);
        } else {
            static const int density[4] = { 2, 5, 9, 4 };
            if (P(e, 0x04) == 1) sec(e, s, BQ_LP, gs_cutoff(P(e, 0x05)), 0.7071, 0.0);
            if (P(e, 0x04) == 2) sec(e, s, BQ_HP, gs_cutoff(P(e, 0x05)), 0.7071, 0.0);
            s->noise[0] = unit(P(e, 0x07)) * 0.1f * (1.0f + unit(P(e, 0x06)));
            s->noise_lp[0] = efx_pole(e->rate, 3000.0);
            s->noise[1] = unit(P(e, 0x0A)) * 0.1f;
            s->noise_lp[1] = efx_pole(e->rate, P(e, 0x08) ? fmin(gs_lpf(P(e, 0x09)) > 0 ? gs_lpf(P(e, 0x09)) : 12000.0, 1500.0) : gs_lpf(P(e, 0x09)));
            s->noise[2] = unit(P(e, 0x0D)) * 0.1f;
            s->noise_lp[2] = efx_pole(e->rate, gs_lpf(P(e, 0x0C)));
            s->crackle = density[clampi(P(e, 0x0B), 0, 3)];
            s->noise[3] = unit(P(e, 0x10)) * 0.1f;
            s->noise_lp[3] = efx_pole(e->rate, gs_lpf(P(e, 0x0F)));
            s->hum_hz = P(e, 0x0E) ? 60.0f : 50.0f;
            s->mono_out = P(e, 0x11) == 0;
        }
        balance(s, P(e, 0x12));
        post(e, gs_gain(P(e, 0x13)), gs_gain(P(e, 0x14)), P(e, 0x15), P(e, 0x16));
        return;
    }
    case G_OD_CHO: case G_DS_CHO: build_gs_series(e, 0, 0); return;
    case G_OD_FL: case G_DS_FL: build_gs_series(e, 0, 1); return;
    case G_OD_DLY: case G_DS_DLY: build_gs_series(e, 0, 2); return;
    case G_EH_CHO: build_gs_series(e, 1, 0); return;
    case G_EH_FL: build_gs_series(e, 1, 1); return;
    case G_EH_DLY: build_gs_series(e, 1, 2); return;
    case G_CHO_DLY: build_gs_series(e, 2, 2); return;
    case G_FL_DLY: build_gs_series(e, 3, 2); return;
    case G_CHO_FL: build_gs_series(e, 2, 1); return;
    case G_ROT_MULTI:
        s = add(e, ST_DRIVE);
        if (P(e, 0x04)) drive_gs(e, s, 0, P(e, 0x03), 1, 1);
        else s->mono_in = 1;
        eq3_gs(e, add(e, ST_EQ), P(e, 0x05), P(e, 0x06), P(e, 0x07), P(e, 0x08), P(e, 0x09));
        for (i = 0; i < 10; i++) r[i] = P(e, 0x0A + i);
        rotary(e, add(e, ST_ROTARY), r);
        post(e, 0.0, 0.0, -1, P(e, 0x16));
        return;
    case G_GTR1: case G_GTR2: case G_BASS: {
        int gtr1 = e->t->build == G_GTR1;
        s = add(e, ST_DYNAMICS);
        comp_gs(e, s, P(e, 0x03), P(e, 0x04), 0);
        s->makeup = unit(P(e, 0x05)) * 1.6f;
        if (!P(e, 0x06)) s->kind = ST_NONE;
        s = add(e, ST_DRIVE);
        drive_gs(e, s, P(e, 0x07) != 0, P(e, 0x08), P(e, 0x09), P(e, 0x0A));
        if (gtr1) { sec(e, s, BQ_LOSHELF, 200.0, 0.7071, gs_gain(P(e, 0x0B))); sec(e, s, BQ_HISHELF, 4000.0, 0.7071, gs_gain(P(e, 0x0C))); }
        if (!P(e, gtr1 ? 0x0D : 0x0B)) s->kind = ST_NONE;
        if (!gtr1) eq3_gs(e, add(e, ST_EQ), P(e, 0x0C), P(e, 0x0D), P(e, 0x0E), P(e, 0x0F), P(e, 0x10));
        if (gtr1) {
            cf_gs(e, add(e, ST_MOD), P(e, 0x0E), P(e, 0x0F), P(e, 0x10), P(e, 0x11), P(e, 0x12));
            delay_multi(e, add(e, ST_DELAY), gs_delay4_ms(P(e, 0x13)), P(e, 0x14), P(e, 0x15), 0.0);
        } else {
            cf_gs(e, add(e, ST_MOD), P(e, 0x11), P(e, 0x12), P(e, 0x13), P(e, 0x14), P(e, 0x15));
        }
        post(e, 0.0, 0.0, -1, P(e, 0x16));
        return;
    }
    case G_GTR3:
        s = add(e, ST_WAH);
        wah(e, s, P(e, 0x03) == 0, 0, P(e, 0x04), P(e, 0x05), 0.0, 0, 1);
        if (!P(e, 0x06)) s->kind = ST_NONE;
        s = add(e, ST_DRIVE);
        drive_gs(e, s, P(e, 0x07) != 0, P(e, 0x08), P(e, 0x09), P(e, 0x0A));
        sec(e, s, BQ_LOSHELF, 200.0, 0.7071, gs_gain(P(e, 0x0B)));
        sec(e, s, BQ_HISHELF, 4000.0, 0.7071, gs_gain(P(e, 0x0C)));
        if (!P(e, 0x0D)) s->kind = ST_NONE;
        cf_gs(e, add(e, ST_MOD), P(e, 0x0E), P(e, 0x0F), P(e, 0x10), P(e, 0x11), P(e, 0x12));
        delay_multi(e, add(e, ST_DELAY), gs_delay4_ms(P(e, 0x13)), P(e, 0x14), P(e, 0x15), 0.0);
        post(e, 0.0, 0.0, -1, P(e, 0x16));
        return;
    case G_CLEAN1: case G_CLEAN2: {
        int one = e->t->build == G_CLEAN1;
        if (one) {
            s = add(e, ST_DYNAMICS);
            comp_gs(e, s, P(e, 0x03), P(e, 0x04), 0);
            s->makeup = unit(P(e, 0x05)) * 1.6f;
            if (!P(e, 0x06)) s->kind = ST_NONE;
        } else {
            s = add(e, ST_WAH);
            wah(e, s, P(e, 0x03) == 0, 0, P(e, 0x04), P(e, 0x05), gs_rate1(P(e, 0x06)), P(e, 0x07), 1);
            if (!P(e, 0x08)) s->kind = ST_NONE;
        }
        i = one ? 0x07 : 0x09;
        eq3_gs(e, add(e, ST_EQ), P(e, i), P(e, i + 1), P(e, i + 2), P(e, i + 3), P(e, i + 4));
        i += 5;
        cf_gs(e, add(e, ST_MOD), P(e, i), P(e, i + 1), P(e, i + 2), P(e, i + 3), P(e, i + 4));
        i += 5;
        delay_multi(e, add(e, ST_DELAY), gs_delay4_ms(P(e, i)), P(e, i + 1), P(e, one ? 0x14 : 0x15),
                    one ? gs_hf_damp(P(e, 0x13)) : 0.0);
        post(e, 0.0, 0.0, -1, P(e, 0x16));
        return;
    }
    case G_RHODES:
        enhancer(e, add(e, ST_ENHANCE), P(e, 0x03), P(e, 0x04), 2500.0);
        phaser(e, add(e, ST_PHASER), gs_manual(P(e, 0x05)), gs_rate1(P(e, 0x06)), P(e, 0x07), P(e, 0x08), P(e, 0x09));
        s = add(e, ST_MOD);
        chorus(e, s, gs_pre_ms(P(e, 0x0C)), gs_rate1(P(e, 0x0D)), P(e, 0x0E), P(e, 0x0A) ? gs_fb(P(e, 0x0F)) : 0.0f, 90.0, P(e, 0x0A) != 0);
        if (gs_lpf(P(e, 0x0B)) > 0.0) s->pre_a = efx_pole(e->rate, gs_lpf(P(e, 0x0B)));
        mix(s, P(e, 0x10));
        s = add(e, ST_TREMOLO);
        s->wave = clampi(P(e, 0x12), 0, 4);
        s->hz = gs_rate1(P(e, 0x13));
        s->depth = unit(P(e, 0x14));
        s->phase = P(e, 0x11) ? 0.5f : 0.0f;
        if (!P(e, 0x15)) s->kind = ST_NONE;
        post(e, 0.0, 0.0, -1, P(e, 0x16));
        return;
    case G_KEYS:
        s = add(e, ST_RING);
        s->ring_hz = 20.0 * pow(2.0, unit(P(e, 0x03)) * 8.0);
        balance(s, P(e, 0x04));
        eq3_gs(e, add(e, ST_EQ), P(e, 0x05), P(e, 0x06), P(e, 0x07), P(e, 0x08), P(e, 0x09));
        s = add(e, ST_PITCH);
        s->voices = 1;
        s->window = frames(e->rate, 15.0 + clampi(P(e, 0x0C), 0, 4) * 12.0);
        s->v_ratio[0] = (float)pow(2.0, (clampi(P(e, 0x0A) - 64, -24, 12) + clampi(P(e, 0x0B) - 64, -50, 50) * 0.02) / 12.0);
        s->v_gl[0] = s->v_gr[0] = 1.0f;
        s->need = (int)s->window + 4;
        balance(s, P(e, 0x0D));
        phaser(e, add(e, ST_PHASER), gs_manual(P(e, 0x0E)), gs_rate1(P(e, 0x0F)), P(e, 0x10), P(e, 0x11), P(e, 0x12));
        delay_multi(e, add(e, ST_DELAY), gs_delay4_ms(P(e, 0x13)), P(e, 0x14), P(e, 0x15), 0.0);
        post(e, 0.0, 0.0, -1, P(e, 0x16));
        return;
    case G_P_CHO_DLY: build_gs_parallel(e, PC_CHO, PC_DLY, 0x03, 0x08); return;
    case G_P_FL_DLY: build_gs_parallel(e, PC_FL, PC_DLY, 0x03, 0x08); return;
    case G_P_CHO_FL: build_gs_parallel(e, PC_CHO, PC_FL, 0x03, 0x08); return;
    case G_P_OD_OD: build_gs_parallel(e, PC_OD, PC_OD, 0x03, 0x08); return;
    case G_P_OD_ROT: build_gs_parallel(e, PC_OD, PC_ROT, 0x03, 0x08); return;
    case G_P_OD_PH: build_gs_parallel(e, PC_OD, PC_PH, 0x03, 0x08); return;
    case G_P_OD_AW: build_gs_parallel(e, PC_OD, PC_AW, 0x03, 0x08); return;
    case G_P_PH_ROT: build_gs_parallel(e, PC_PH, PC_ROT, 0x03, 0x08); return;
    case G_P_PH_AW: build_gs_parallel(e, PC_PH, PC_AW, 0x03, 0x08); return;
    default:
        return;
    }
}

static void build_xg(fa_midi_efx* e)
{
    efx_stage* s;
    int q[4], i;
    switch (e->t->build) {
    case X_COMP_DIST:
        comp_xg(e, add(e, ST_DYNAMICS), X(e, 12), X(e, 13), X(e, 14), X(e, 15), -1);
        s = add(e, ST_DRIVE);
        s->drive = (float)pow(10.0, unit(X(e, 1)) * 2.5);
        s->edge = unit(X(e, 11));
        s->level = unit(X(e, 5)) * 0.5f;
        sec(e, s, BQ_LP, fa_xg_eq_hz(X(e, 4)), 0.7071, 0.0);
        sec(e, s, BQ_LOSHELF, fa_xg_eq_hz(X(e, 2)), 0.7071, fa_xg_eq_db(X(e, 3)));
        sec(e, s, BQ_PEAK, fa_xg_eq_hz(X(e, 7)), X(e, 9) < 10 ? 1.0 : X(e, 9) / 10.0, fa_xg_eq_db(X(e, 8)));
        return;
    case X_WAH_DIST:
        s = add(e, ST_WAH);
        wah(e, s, 0, 0, X(e, 3), 0, fa_xg_lfo_hz(X(e, 1)), X(e, 2), 1);
        s->reso = (float)clampi(X(e, 4), 10, 120) / 10.0f;
        s->mono_in = 0;
        for (i = 0; i < 4; i++) q[i] = X(e, 6 + i);
        eq_xg(e, s, q);
        drive_xg(e, add(e, ST_DRIVE), e->t->od, X(e, 11), X(e, 15), X(e, 12), X(e, 13), X(e, 14));
        return;
    case X_PITCH:
        s = add(e, ST_PITCH);
        s->mono_in = 1;
        s->voices = 2;
        s->window = frames(e->rate, 40.0);
        for (i = 0; i < 2; i++) {
            double semis = clampi(X(e, 1) - 64, -24, 24) + clampi(X(e, 3 + i) - 64, -50, 50) / 100.0;
            s->v_ratio[i] = (float)pow(2.0, semis / 12.0);
            s->v_pre[i] = frames(e->rate, clampi(X(e, 2), 0, 127));
            pans(unit(X(e, 11 + 2 * i)), unit(X(e, 12 + 2 * i)), &s->v_gl[i], &s->v_gr[i]);
        }
        s->fb_gain = (float)clampi(X(e, 5) - 64, -63, 63) / 64.0f;
        s->need = (int)(s->v_pre[0] + s->window) + 4;
        return;
    case X_EXCITER:
        s = add(e, ST_ENHANCE);
        enhancer(e, s, X(e, 2), X(e, 3), fa_xg_eq_hz(X(e, 1)));
        return;
    case X_TOUCH_WAH: case X_TOUCH_WAH_DIST: {
        int dist = e->t->build == X_TOUCH_WAH_DIST;
        s = add(e, ST_WAH);
        wah(e, s, 0, X(e, 1), X(e, 2), 0, 0.0, 0, 1);
        s->reso = (float)clampi(X(e, 3), 10, 120) / 10.0f;
        s->mono_in = 0;
        if (dist) s->rel = coef_ms(e->rate, 10.0 + clampi(X(e, 16), 0, 127) * 5.0);
        for (i = 0; i < 4; i++) q[i] = X(e, 6 + i);
        eq_xg(e, s, q);
        if (dist) drive_xg(e, add(e, ST_DRIVE), e->t->od, X(e, 11), X(e, 15), X(e, 12), X(e, 13), X(e, 14));
        else if (X(e, 11) > 0) drive_xg(e, add(e, ST_DRIVE), 0, X(e, 11) / 3, 127, 64, 64, -1);
        return;
    }
    case X_COMPRESSOR:
        comp_xg(e, add(e, ST_DYNAMICS), X(e, 1), X(e, 2), X(e, 3), X(e, 4), X(e, 5));
        return;
    case X_NOISE_GATE:
        s = add(e, ST_DYNAMICS);
        s->dyn = DYN_GATE;
        s->att = coef_ms(e->rate, xg_attack_ms(X(e, 1)));
        s->rel = coef_ms(e->rate, xg_release_ms(X(e, 2)));
        s->thresh = (float)clampi(X(e, 3) - 127, -72, -30);
        s->makeup = xg_level(X(e, 4));
        /* The detector: a fast peak follower. */
        return;
    case X_VOICE_CANCEL:
        s = add(e, ST_CANCEL);
        s->pre_a = efx_pole(e->rate, 100.0 * pow(2.0, clampi(X(e, 1), 0, 26) / 26.0 * 3.0));
        s->damp_a = efx_pole(e->rate, 2000.0 * pow(2.0, clampi(X(e, 2), 0, 26) / 26.0 * 3.0));
        return;
    case X_ROTARY2: {
        int r[10];
        double hz = fa_xg_lfo_hz(X(e, 1));
        s = add(e, ST_ROTARY);
        r[0] = r[1] = r[4] = r[5] = 0; r[2] = r[6] = 127; r[3] = r[7] = 127; r[8] = 127; r[9] = 0;
        rotary(e, s, r);
        s->slow[1] = s->fast[1] = hz;
        s->slow[0] = s->fast[0] = hz * 0.85;
        s->rot_lev[0] = X(e, 4) <= 64 ? 1.0f : (127.0f - (float)X(e, 4)) / 63.0f;
        s->rot_lev[1] = X(e, 4) >= 64 ? 1.0f : (float)(X(e, 4) - 1) / 63.0f;
        s->xover_a = efx_pole(e->rate, fa_xg_eq_hz(X(e, 11)));
        s->sep = (float)clampi(X(e, 12), 0, 127) / 127.0f;
        if (X(e, 2) > 0 || X(e, 3) > 0) { s->mono_in = 0; }
        for (i = 0; i < 4; i++) q[i] = X(e, 6 + i);
        eq_xg(e, s, q);
        return;
    }
    case X_ENSEMBLE:
        s = add(e, ST_PITCH);
        s->mono_in = 1;
        s->voices = 2;
        s->window = frames(e->rate, 30.0);
        for (i = 0; i < 2; i++) {
            s->v_ratio[i] = (float)pow(2.0, (i ? -1.0 : 1.0) * clampi(X(e, 1) - 64, -50, 50) / 1200.0);
            s->v_pre[i] = frames(e->rate, clampi(X(e, 2 + i), 0, 127) * 0.1);
            s->v_gl[i] = i ? 0.0f : 1.0f;
            s->v_gr[i] = i ? 1.0f : 0.0f;
        }
        s->need = (int)(fmax(s->v_pre[0], s->v_pre[1]) + s->window) + 4;
        for (i = 0; i < 4; i++) q[i] = X(e, 6 + i);
        eq_xg(e, s, q);
        return;
    case X_AMBIENCE:
        s = add(e, ST_REVERB);
        if (!s->sub && !(s->sub = fa_midi_fx_new(e->rate))) { e->nst--; s->kind = ST_NONE; return; }
        fa_midi_fx_xg(s->sub, 0x20, 0);
        fa_midi_fx_xg(s->sub, 0x00, 0x02);
        fa_midi_fx_xg(s->sub, 0x01, 0);
        fa_midi_fx_xg(s->sub, 0x02, clampi(X(e, 1), 0, 69));
        fa_midi_fx_gs(s->sub, 0x37, clampi(X(e, 3), 0, 63));
        if (X(e, 4) > 0) sec(e, s, BQ_HP, fa_xg_eq_hz(X(e, 4)), 0.7071, 0.0);
        if (X(e, 5) < 60) sec(e, s, BQ_LP, fa_xg_eq_hz(X(e, 5)), 0.7071, 0.0);
        return;
    case X_TALK: {
        int v = clampi(X(e, 1), 0, 4);
        s = add(e, ST_FORMANT);
        s->mono_in = 1;
        s->drive = 1.0f + unit(X(e, 3)) * 8.0f;
        s->f1_to = vowels[v][0];
        s->f2_to = vowels[v][1];
        s->glide = 1.0f / (float)clampi(X(e, 2), 1, 62);
        s->level = xg_level(X(e, 4));
        s->wet = s->level;
        return;
    }
    case X_LOFI:
        s = add(e, ST_LOFI);
        s->mono_in = X(e, 11) == 0;
        s->hold_n = (int)lrint((1 + clampi(X(e, 1), 0, 127)) * e->rate / 44100.0);
        if (s->hold_n < 1) s->hold_n = 1;
        s->levels = (float)clampi(X(e, 2), 1, 127);
        if (X(e, 4) < 60) sec(e, s, X(e, 5) == 1 ? BQ_HP : BQ_LP, fa_xg_eq_hz(X(e, 4)), clampi(X(e, 6), 10, 120) / 10.0 * 0.7071, 0.0);
        s->wet = (float)pow(10.0, clampi(X(e, 3) - 64, -6, 12) / 20.0);
        return;
    case X_DIST_DELAY:
        drive_xg(e, add(e, ST_DRIVE), e->t->od, X(e, 6), X(e, 7), X(e, 8), X(e, 9), -1);
        echo_xg(e, add(e, ST_DELAY), X(e, 1) / 10.0, X(e, 2) / 10.0, X(e, 3) / 10.0, X(e, 4), X(e, 5));
        return;
    case X_COMP_DIST_DELAY: case X_WAH_DIST_DELAY:
        if (e->t->build == X_COMP_DIST_DELAY) {
            comp_xg(e, add(e, ST_DYNAMICS), X(e, 11), X(e, 12), X(e, 13), X(e, 14), -1);
        } else {
            s = add(e, ST_WAH);
            wah(e, s, 0, X(e, 11), X(e, 12), 0, 0.0, 0, 1);
            s->reso = (float)clampi(X(e, 13), 10, 120) / 10.0f;
            s->rel = coef_ms(e->rate, 10.0 + clampi(X(e, 14), 0, 127) * 5.0);
        }
        drive_xg(e, add(e, ST_DRIVE), e->t->od, X(e, 4), X(e, 5), X(e, 6), X(e, 7), -1);
        echo_xg(e, add(e, ST_DELAY), X(e, 1) / 10.0, X(e, 1) / 10.0, X(e, 1) / 10.0, X(e, 2), X(e, 3));
        return;
    default:
        return;
    }
}

void efx_build(fa_midi_efx* e)
{
    e->nst = 0;
    e->par = 0;
    e->post = 0;
    if (!e->t) return;
    if (e->t->gs) build_gs(e);
    else build_xg(e);
}
