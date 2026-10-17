/* TinySoundFont (SoundFont 2 synthesizer) and TinyMidiLoader (Standard MIDI
 * File parser), vendored in third_party/tinysoundfont with FatzAudio's hardening
 * patches. Their implementation lives in this file alone, compiled like the other
 * vendored code (src/miniaudio_impl.c); dec_midi.c uses them. The symbols stay
 * hidden: the library exports fa_* only. */
#define TSF_NO_STDIO          /* files are read through fa_source (UTF-8 paths everywhere) */
#define TML_NO_STDIO
#define TSF_IMPLEMENTATION
#define TML_IMPLEMENTATION
#include "tsf.h"
#include "tml.h"
