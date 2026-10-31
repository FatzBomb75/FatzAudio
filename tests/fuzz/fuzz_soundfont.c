/* Fuzz target: soundfonts. fa_soundfont_open_memory (SF2, SF3 with its Vorbis samples,
 * DLS through the converter), then the synthesizer plays a few notes on every preset
 * (up to 256), with the controllers that reach the SoundFont modulators moved.
 * Fails (abort) on a sanitizer report, a hang, or rendered samples that are not finite. */
#include "fa_internal.h"
#include "tsf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 44100
#define BLOCK 128

static void render(tsf* t, float* buf)
{
    int i;
    tsf_render_float(t, buf, BLOCK, 0);
    for (i = 0; i < BLOCK * 2; i++)
        if (!isfinite(buf[i])) { fprintf(stderr, "non-finite sample %d\n", i); abort(); }
}

static void play(tsf* bank)
{
    static float buf[BLOCK * 2];
    tsf* t = tsf_copy(bank);
    int n, p;
    if (!t) return;
    tsf_set_output(t, TSF_STEREO_INTERLEAVED, RATE, 0.0f);
    if (!tsf_set_max_voices(t, 64)) { tsf_close(t); return; }
    n = tsf_get_presetcount(t);
    for (p = 0; p < n && p < 256; p++) {
        int ch = p % 16;
        (void)tsf_get_presetname(t, p);
        if (!tsf_channel_set_presetindex(t, ch, p)) continue;
        tsf_channel_note_on(t, ch, 60, 0.8f);
        tsf_channel_note_on(t, ch, 36 + p % 64, 0.3f);
        render(t, buf);
        tsf_channel_midi_control(t, ch, 1, 127);     /* mod wheel */
        tsf_channel_midi_control(t, ch, 74, 0);      /* brightness */
        tsf_channel_set_pressure(t, ch, 100);
        tsf_channel_set_pitchwheel(t, ch, 16383);
        tsf_channel_note_on(t, ch, 127, 1.0f);
        render(t, buf);
        tsf_channel_note_off_all(t, ch);
        render(t, buf);
        tsf_channel_sounds_off_all(t, ch);
    }
    tsf_close(t);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fa_soundfont* font = NULL;
    fa_soundfont_info info;
    uint8_t* sf2 = NULL;
    size_t sf2_size = size;
    tsf* bank;
    if (fa_soundfont_open_memory(data, size, &font) != FA_OK) return 0;
    fa_soundfont_get_info(font, &info);
    fa_soundfont_close(font);
    /* The bank the stream would play: the same conversion fa_soundfont_open_memory made. */
    if (fa_dls_probe(data, size)) {
        if (fa_dls_to_sf2(data, size, &sf2, &sf2_size) != FA_OK) abort();
    } else if (fa_sf3_probe(data, size)) {
        if (fa_sf3_to_sf2(data, size, &sf2, &sf2_size) != FA_OK) abort();
    }
    bank = tsf_load_memory(sf2 ? sf2 : data, (int)sf2_size);
    if (bank) {
        play(bank);
        tsf_close(bank);
    }
    free(sf2);
    return 0;
}
