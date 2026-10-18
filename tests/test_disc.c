/* Audio CD tests. CI machines have no optical drive, so two stand-ins are used:
 *   - disc images (.cue + .bin/.wav written here), through the public API, and
 *   - a simulated MMC drive: a fake `exec` that answers the SCSI commands a real
 *     drive gets from disc_mmc.c (Linux and Windows send exactly these), with
 *     damaged sectors, a drive without C2 support, CD-Text packs and disc removal. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "disc.h"
#include "cdg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_STR(a, b) do { const char* sa_ = (a); const char* sb_ = (b); if (strcmp(sa_, sb_) != 0) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, sa_, sb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;

/* Sample value of disc sample `i` (absolute, from LBA 0), channel `c`: encodes the
 * position so every read can be checked exactly. */
static int16_t pattern(int64_t i, int c) { int v = (int)(i % 20000) + 1; return (int16_t)(c ? -v : v); }

static void put_sector(uint8_t* out, int64_t lba)
{
    int k;
    for (k = 0; k < FA_DISC_SECTOR_FRAMES; k++) {
        int64_t i = lba * FA_DISC_SECTOR_FRAMES + k;
        int16_t l = pattern(i, 0), r = pattern(i, 1);
        out[k * 4 + 0] = (uint8_t)(l & 0xFF); out[k * 4 + 1] = (uint8_t)((uint16_t)l >> 8);
        out[k * 4 + 2] = (uint8_t)(r & 0xFF); out[k * 4 + 3] = (uint8_t)((uint16_t)r >> 8);
    }
}

static float pf(int64_t i, int c) { return (float)pattern(i, c) / 32768.0f; }

/* ---- disc image ------------------------------------------------------------------ */

/* Layout (sectors):  track 1 audio at 0; track 2 audio, INDEX 00 at 300, INDEX 01 at
 * 375; track 3 audio with PREGAP 00:00:50 (silence, not in the file) and FLAGS PRE,
 * file offset 600 -> LBA 650; file ends at 900 -> session 1 ends at 950.
 * Session 2: data track at 950 + 11400 = 12350, 100 sectors -> lead-out 12450.
 * Track 3 holds a Nyquist-rate square wave for the de-emphasis check. */
static const char* k_cue = "disc_test.cue";

static int write_image(void)
{
    FILE* f = fopen("disc_test_audio.bin", "wb");
    uint8_t sec[FA_CD_SECTOR_BYTES];
    int64_t s;
    if (!f) return 0;
    for (s = 0; s < 900; s++) {
        if (s >= 600) {
            int k;
            for (k = 0; k < FA_DISC_SECTOR_FRAMES * 2; k++) {
                int16_t v = (k / 2) % 2 ? -8192 : 8192;
                sec[k * 2] = (uint8_t)(v & 0xFF);
                sec[k * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
            }
        } else {
            put_sector(sec, s);   /* file sector == LBA for tracks 1 and 2 */
        }
        fwrite(sec, 1, sizeof(sec), f);
    }
    fclose(f);
    f = fopen("disc_test_data.bin", "wb");
    if (!f) return 0;
    memset(sec, 0x5A, sizeof(sec));
    for (s = 0; s < 100; s++) fwrite(sec, 1, sizeof(sec), f);
    fclose(f);
    f = fopen(k_cue, "wb");
    if (!f) return 0;
    fputs("\xEF\xBB\xBF" "REM GENRE \"Karaoke\"\r\n"
          "CATALOG 0123456789012\r\n"
          "PERFORMER \"The Testers\"\r\n"
          "TITLE \"Greatest Hits of the Caf\xC3\xA9\"\r\n"
          "FILE \"disc_test_audio.bin\" BINARY\r\n"
          "  TRACK 01 AUDIO\r\n"
          "    TITLE \"Opening\"\r\n"
          "    PERFORMER \"The Testers\"\r\n"
          "    ISRC USABC2600001\r\n"
          "    INDEX 01 00:00:00\r\n"
          "  TRACK 02 AUDIO\r\n"
          "    TITLE \"A rather long second title that spans several packs\"\r\n"
          "    SONGWRITER \"Somebody\"\r\n"
          "    INDEX 00 00:04:00\r\n"
          "    INDEX 01 00:05:00\r\n"
          "  TRACK 03 AUDIO\r\n"
          "    FLAGS DCP PRE\r\n"
          "    PREGAP 00:00:50\r\n"
          "    INDEX 01 00:08:00\r\n"
          "REM SESSION 02\r\n"
          "FILE \"disc_test_data.bin\" BINARY\r\n"
          "  TRACK 04 MODE1/2352\r\n"
          "    INDEX 01 00:00:00\r\n", f);
    fclose(f);
    return 1;
}

static void remove_image(void)
{
    remove(k_cue);
    remove("disc_test_audio.bin");
    remove("disc_test_data.bin");
}

static void test_image_toc(void)
{
    fa_disc* d = NULL;
    fa_disc_toc toc;
    fa_disc_status st;
    CHECK_EQ(fa_disc_open(k_cue, NULL, &d), FA_OK);
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.state, FA_DISC_READY);
    CHECK_EQ(fa_disc_get_toc(d, &toc), FA_OK);
    CHECK_EQ(toc.first_track, 1);
    CHECK_EQ(toc.last_track, 4);
    CHECK_EQ(toc.track_count, 4);
    CHECK_EQ(toc.audio_track_count, 3);
    CHECK_EQ(toc.tracks[0].start_sector, 0);
    CHECK_EQ(toc.tracks[0].sectors, 375);
    CHECK_EQ(toc.tracks[1].start_sector, 375);
    CHECK_EQ(toc.tracks[1].sectors, 275);
    CHECK_EQ(toc.tracks[2].start_sector, 650);
    CHECK_EQ(toc.tracks[2].sectors, 300);       /* the session gap is not part of track 3 */
    CHECK_EQ(toc.tracks[2].preemphasis, 1);
    CHECK_EQ(toc.tracks[2].copy_permitted, 1);
    CHECK_EQ(toc.tracks[0].preemphasis, 0);
    CHECK_EQ(toc.tracks[3].is_audio, 0);
    CHECK_EQ(toc.tracks[3].start_sector, 12350);
    CHECK_EQ(toc.leadout_sector, 12450);
    CHECK_EQ(toc.tracks[1].frames, 275 * 588);
    CHECK_EQ(toc.duration, (375 + 275 + 300) * 1000000000LL / 75);
    CHECK_EQ(toc.freedb_id, 0x1700A604u);
    CHECK_STR(toc.freedb_query, "1700a604 4 150 525 800 12500 168");
    /* Enhanced CD: the id covers the audio session only. Reference computed with
     * the published algorithm (Python hashlib). */
    CHECK_STR(toc.musicbrainz_id, "gctLGqsIOXx_tsL.MRCd2MIadI4-");
    CHECK_EQ(toc.has_cd_text, 1);
    fa_disc_close(d);
}

static void test_image_text(void)
{
    fa_disc* d = NULL;
    char t[256];
    CHECK_EQ(fa_disc_open(k_cue, NULL, &d), FA_OK);
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Greatest Hits of the Caf\xC3\xA9");   /* ISO 8859-1 on disc, UTF-8 out */
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_PERFORMER, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "The Testers");
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_GENRE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Karaoke");
    CHECK_EQ(fa_disc_get_text(d, 1, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Opening");
    CHECK_EQ(fa_disc_get_text(d, 2, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "A rather long second title that spans several packs");
    CHECK_EQ(fa_disc_get_text(d, 2, FA_DISC_TEXT_SONGWRITER, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Somebody");
    CHECK_EQ(fa_disc_get_text(d, 3, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_disc_get_text(d, 2, FA_DISC_TEXT_PERFORMER, t, sizeof(t)), FA_ERROR_NOT_FOUND);
    /* Codes come from the subcode when CD-Text has none. */
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_CODE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "0123456789012");
    CHECK_EQ(fa_disc_get_text(d, 1, FA_DISC_TEXT_CODE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "USABC2600001");
    CHECK_EQ(fa_disc_get_text(d, 2, FA_DISC_TEXT_CODE, t, sizeof(t)), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_disc_get_text(d, 7, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_ERROR_NOT_FOUND);
    /* Truncation keeps whole UTF-8 characters: "Greatest Hits of the Caf" + 2-byte é. */
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_TITLE, t, 26), FA_OK);
    CHECK_STR(t, "Greatest Hits of the Caf");
    /* Images have no tray, lock or speed. */
    CHECK_EQ(fa_disc_eject(d), FA_ERROR_NOT_IMPLEMENTED);
    CHECK_EQ(fa_disc_set_speed(d, 4.0f), FA_ERROR_NOT_IMPLEMENTED);
    fa_disc_close(d);
}

static fa_stream* open_track(fa_disc* d, int track)
{
    fa_open_options o;
    fa_stream* s = NULL;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    if (fa_stream_open_disc(g_engine, d, track, &o, &s) != FA_OK) return NULL;
    return s;
}

/* Reads `n` frames, all of them (short reads are fine). */
static fa_result read_all(fa_stream* s, float* buf, int64_t n, int64_t* got)
{
    fa_result r = FA_OK;
    *got = 0;
    while (*got < n) {
        fa_frames k = 0;
        r = fa_stream_read(s, buf + *got * 2, n - *got, &k);
        if (r != FA_OK) break;
        *got += k;
    }
    return r;
}

static void test_image_stream(void)
{
    fa_disc* d = NULL;
    fa_stream* s;
    fa_stream_info info;
    float* buf = (float*)malloc(sizeof(float) * 2 * 400 * 588);
    int64_t got, i;
    CHECK(buf);
    CHECK_EQ(fa_disc_open(k_cue, NULL, &d), FA_OK);
    s = open_track(d, 2);
    CHECK(s);
    CHECK_EQ(fa_stream_get_info(s, &info), FA_OK);
    CHECK_STR(info.codec, "cdda");
    CHECK_STR(info.container, "cd");
    CHECK_EQ(info.format.sample_rate, 44100);
    CHECK_EQ(info.format.channels, 2);
    CHECK_EQ(info.length_frames, 275 * 588);
    CHECK_EQ(info.seekable, 1);

    /* The whole track: the file's audio up to the pregap of track 3, then silence. */
    CHECK_EQ(read_all(s, buf, 275 * 588, &got), FA_OK);
    CHECK_EQ(got, 275 * 588);
    for (i = 0; i < 225 * 588; i++) {
        int64_t abs = 375 * 588 + i;
        if (buf[i * 2] != pf(abs, 0) || buf[i * 2 + 1] != pf(abs, 1)) break;
    }
    CHECK_EQ(i, 225 * 588);
    for (i = 225 * 588; i < 275 * 588; i++) if (buf[i * 2] != 0.0f || buf[i * 2 + 1] != 0.0f) break;
    CHECK_EQ(i, 275 * 588);
    {
        fa_frames k = 1;
        CHECK_EQ(fa_stream_read(s, buf, 10, &k), FA_ERROR_END_OF_STREAM);
        CHECK_EQ(k, 0);
    }

    /* Seeking is exact, also into the middle of a sector. */
    CHECK_EQ(fa_stream_seek(s, 12345), FA_OK);
    CHECK_EQ(read_all(s, buf, 1000, &got), FA_OK);
    for (i = 0; i < 1000; i++) if (buf[i * 2] != pf(375 * 588 + 12345 + i, 0)) break;
    CHECK_EQ(i, 1000);
    fa_stream_close(s);

    /* The disc handle can go first: open streams keep it alive. */
    s = open_track(d, 1);
    CHECK(s);
    fa_disc_close(d);
    d = NULL;
    CHECK_EQ(read_all(s, buf, 588 * 3, &got), FA_OK);
    for (i = 0; i < 588 * 3; i++) if (buf[i * 2 + 1] != pf(i, 1)) break;
    CHECK_EQ(i, 588 * 3);
    fa_stream_close(s);
    free(buf);
}

static void test_image_offset(void)
{
    fa_disc_options o;
    fa_disc* d = NULL;
    fa_stream* s;
    float buf[2 * 2000];
    int64_t got, i;
    fa_disc_options_init(&o);
    o.read_offset = 30;
    CHECK_EQ(fa_disc_open(k_cue, &o, &d), FA_OK);
    s = open_track(d, 2);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 2000, &got), FA_OK);
    for (i = 0; i < 2000; i++) if (buf[i * 2] != pf(375 * 588 + 30 + i, 0)) break;
    CHECK_EQ(i, 2000);
    fa_stream_close(s);
    fa_disc_close(d);

    /* Negative offset at the very start of the disc: what lies before LBA 0 is silence. */
    o.read_offset = -100;
    CHECK_EQ(fa_disc_open(k_cue, &o, &d), FA_OK);
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 2000, &got), FA_OK);
    for (i = 0; i < 100; i++) CHECK(buf[i * 2] == 0.0f);
    for (i = 100; i < 2000; i++) if (buf[i * 2] != pf(i - 100, 0)) break;
    CHECK_EQ(i, 2000);
    fa_stream_close(s);
    fa_disc_close(d);
}

static void test_image_deemphasis(void)
{
    fa_disc* d = NULL;
    fa_disc_options o;
    fa_stream* s;
    float buf[2 * 4000];
    int64_t got, i;
    double raw = 0, filt = 0;
    /* Track 3 is a square wave at half the sample rate, +-0.25. As recorded: */
    fa_disc_options_init(&o);
    o.flags = FA_DISC_NO_DEEMPHASIS;
    CHECK_EQ(fa_disc_open(k_cue, &o, &d), FA_OK);
    s = open_track(d, 3);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 4000, &got), FA_OK);
    for (i = 2000; i < 4000; i++) raw += fabs(buf[i * 2]);
    CHECK(fabs(buf[0]) == 0.25f);
    fa_stream_close(s);
    fa_disc_close(d);
    /* De-emphasized: about -9.5 dB at 22 kHz (the 50/15 us shelf). */
    CHECK_EQ(fa_disc_open(k_cue, NULL, &d), FA_OK);
    s = open_track(d, 3);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 4000, &got), FA_OK);
    for (i = 2000; i < 4000; i++) filt += fabs(buf[i * 2]);
    {
        double db = 20.0 * log10(filt / raw);
        if (db > -9.0 || db < -10.5) fprintf(stderr, "  de-emphasis at Nyquist: %.2f dB\n", db);
        CHECK(db < -9.0 && db > -10.5);
    }
    fa_stream_close(s);
    fa_disc_close(d);
}

static void test_image_errors(void)
{
    fa_disc* d = NULL;
    fa_stream* s = NULL;
    CHECK_EQ(fa_disc_open("missing_disc.cue", NULL, &d), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_disc_open(k_cue, NULL, &d), FA_OK);
    CHECK_EQ(fa_stream_open_disc(g_engine, d, 4, NULL, &s), FA_ERROR_UNSUPPORTED_FORMAT);   /* data */
    CHECK_EQ(fa_stream_open_disc(g_engine, d, 5, NULL, &s), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_stream_open_disc(g_engine, d, 0, NULL, &s), FA_ERROR_NOT_FOUND);
    CHECK(s == NULL);
    fa_disc_close(d);
}

/* A WAVE image (EAC's "single WAV + cue") and a MOTOROLA one. */
static void test_image_formats(void)
{
    FILE* f;
    uint8_t sec[FA_CD_SECTOR_BYTES], hdr[44];
    uint32_t data = 20 * FA_CD_SECTOR_BYTES, v;
    fa_disc* d = NULL;
    fa_stream* s;
    float buf[2 * 588 * 4];
    int64_t got, i, sct;
    f = fopen("disc_test.wav", "wb");
    CHECK(f);
    memcpy(hdr, "RIFF", 4); v = data + 36; memcpy(hdr + 4, &v, 4); memcpy(hdr + 8, "WAVEfmt ", 8);
    v = 16; memcpy(hdr + 16, &v, 4);
    hdr[20] = 1; hdr[21] = 0; hdr[22] = 2; hdr[23] = 0;
    v = 44100; memcpy(hdr + 24, &v, 4); v = 44100 * 4; memcpy(hdr + 28, &v, 4);
    hdr[32] = 4; hdr[33] = 0; hdr[34] = 16; hdr[35] = 0;
    memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &data, 4);
    fwrite(hdr, 1, 44, f);
    for (sct = 0; sct < 20; sct++) { put_sector(sec, sct); fwrite(sec, 1, sizeof(sec), f); }
    fclose(f);
    f = fopen("disc_test_wav.cue", "wb");
    CHECK(f);
    fputs("FILE disc_test.wav WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:00:10\n", f);
    fclose(f);
    CHECK_EQ(fa_disc_open("disc_test_wav.cue", NULL, &d), FA_OK);
    s = open_track(d, 2);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 588 * 4, &got), FA_OK);
    for (i = 0; i < 588 * 4; i++) if (buf[i * 2] != pf(10 * 588 + i, 0)) break;
    CHECK_EQ(i, 588 * 4);
    fa_stream_close(s);
    fa_disc_close(d);

    /* MOTOROLA: big-endian samples. */
    f = fopen("disc_test_be.bin", "wb");
    CHECK(f);
    for (sct = 0; sct < 5; sct++) {
        int k;
        put_sector(sec, sct);
        for (k = 0; k < FA_CD_SECTOR_BYTES; k += 2) { uint8_t t = sec[k]; sec[k] = sec[k + 1]; sec[k + 1] = t; }
        fwrite(sec, 1, sizeof(sec), f);
    }
    fclose(f);
    f = fopen("disc_test_be.cue", "wb");
    CHECK(f);
    fputs("FILE \"disc_test_be.bin\" MOTOROLA\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", f);
    fclose(f);
    CHECK_EQ(fa_disc_open("disc_test_be.cue", NULL, &d), FA_OK);
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 588 * 4, &got), FA_OK);
    for (i = 0; i < 588 * 4; i++) if (buf[i * 2 + 1] != pf(i, 1)) break;
    CHECK_EQ(i, 588 * 4);
    fa_stream_close(s);
    fa_disc_close(d);
    remove("disc_test.wav"); remove("disc_test_wav.cue"); remove("disc_test_be.bin"); remove("disc_test_be.cue");
}

/* A disc track plays through a (silent) device like any stream. */
/* A later disc of a set (tracks 14 and 15) with a 2048-byte data session in its
 * own file: CD-Text goes to the right track numbers, the data file's length counts
 * in 2048-byte sectors. */
static void test_image_later_disc(void)
{
    FILE* f;
    uint8_t sec[FA_CD_SECTOR_BYTES];
    int64_t i;
    fa_disc* d;
    fa_disc_toc toc;
    char text[64];
    memset(sec, 0, sizeof(sec));
    f = fopen("disc_set_a.bin", "wb");
    CHECK(f);
    for (i = 0; i < 20; i++) fwrite(sec, 1, sizeof(sec), f);
    fclose(f);
    f = fopen("disc_set_d.bin", "wb");
    CHECK(f);
    for (i = 0; i < 100; i++) fwrite(sec, 1, 2048, f);
    fclose(f);
    f = fopen("disc_set.cue", "wb");
    CHECK(f);
    fputs("TITLE \"Box Set, Disc 2\"\n"
          "FILE \"disc_set_a.bin\" BINARY\n"
          "  TRACK 14 AUDIO\n    TITLE \"Fourteen\"\n    INDEX 01 00:00:00\n"
          "  TRACK 15 AUDIO\n    TITLE \"Fifteen\"\n    INDEX 01 00:00:10\n"
          "REM SESSION 02\n"
          "FILE \"disc_set_d.bin\" BINARY\n"
          "  TRACK 16 MODE1/2048\n    INDEX 01 00:00:00\n", f);
    fclose(f);
    CHECK_EQ(fa_disc_open("disc_set.cue", NULL, &d), FA_OK);
    CHECK_EQ(fa_disc_get_toc(d, &toc), FA_OK);
    CHECK_EQ(toc.first_track, 14);
    CHECK_EQ(toc.tracks[2].start_sector, 20 + FA_CD_SESSION_GAP);
    CHECK_EQ(toc.leadout_sector, 20 + FA_CD_SESSION_GAP + 100);
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_TITLE, text, sizeof(text)), FA_OK);
    CHECK_STR(text, "Box Set, Disc 2");
    CHECK_EQ(fa_disc_get_text(d, 14, FA_DISC_TEXT_TITLE, text, sizeof(text)), FA_OK);
    CHECK_STR(text, "Fourteen");
    CHECK_EQ(fa_disc_get_text(d, 15, FA_DISC_TEXT_TITLE, text, sizeof(text)), FA_OK);
    CHECK_STR(text, "Fifteen");
    fa_disc_close(d);
    remove("disc_set.cue");
    remove("disc_set_a.bin");
    remove("disc_set_d.bin");
}

static void test_image_playback(void)
{
    fa_disc* d = NULL;
    fa_stream* s = NULL;
    fa_device* dev = NULL;
    fa_frames pos = 0;
    int64_t t0;
    CHECK_EQ(fa_disc_open(k_cue, NULL, &d), FA_OK);
    CHECK_EQ(fa_stream_open_disc(g_engine, d, 1, NULL, &s), FA_OK);
    CHECK_EQ(fa_device_open(g_engine, NULL, &dev), FA_OK);
    CHECK_EQ(fa_stream_attach(s, dev, NULL), FA_OK);
    CHECK_EQ(fa_device_start(dev), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    t0 = fa_monotonic_ns();
    while (pos < 4410 && fa_monotonic_ns() - t0 < 5000000000LL) {
        fa_sleep_ms(10);
        fa_stream_get_position(s, &pos);
    }
    CHECK(pos >= 4410);
    fa_stream_close(s);
    fa_device_close(dev);
    fa_disc_close(d);
}

/* ---- simulated drive ------------------------------------------------------------ */

typedef struct fake_drive {
    int state;                 /* FA_CD_* */
    int unit_attention;        /* answer the next command with "medium changed" */
    int no_c2;                 /* ILLEGAL REQUEST when C2 pointers are asked for */
    int64_t flaky_lba;         /* C2 errors on the first `flaky_reads` reads */
    int flaky_reads;
    int64_t dead_lba;          /* always a MEDIUM ERROR */
    int64_t data_from;         /* sectors from here are data: ILLEGAL REQUEST as CD-DA (0 = none) */
    int os_no_medium;          /* the OS call itself fails for want of a disc (Linux ENOMEDIUM) */
    int no_sub;                /* ILLEGAL REQUEST when subcode is asked for */
    int no_c2_with_sub;        /* ... only when C2 and subcode are asked for together */
    const uint8_t* sub;        /* 96 subcode bytes per sector from LBA 0; NULL = zeros */
    int64_t sub_sectors;
    int locked, tray_open;
    uint32_t speed;
    uint8_t last_read_cdb[12];
    int reads;                 /* READ CD commands */
    const uint8_t* toc0;       /* READ TOC responses */
    uint32_t toc0_len;
    const uint8_t* cdtext;
    uint32_t cdtext_len;
    int closed;
} fake_drive;

static void set_sense(fa_mmc_sense* s, int key, int asc, int ascq)
{
    s->key = (uint8_t)key; s->asc = (uint8_t)asc; s->ascq = (uint8_t)ascq;
}

static fa_result fake_exec(void* h, const uint8_t* cdb, int cdb_len, void* buf, uint32_t len, int dir,
                           fa_mmc_sense* sense, uint32_t timeout_ms)
{
    fake_drive* d = (fake_drive*)h;
    uint8_t* out = (uint8_t*)buf;
    (void)cdb_len; (void)dir; (void)timeout_ms;
    if (d->os_no_medium && cdb[0] != 0x12) return FA_ERROR_NO_DISC;
    if (d->unit_attention) { d->unit_attention = 0; set_sense(sense, 6, 0x28, 0); return FA_ERROR_IO; }
    if (cdb[0] == 0x12) {   /* INQUIRY */
        memset(out, ' ', len);
        out[0] = 5;
        memcpy(out + 8, "FAKE    ", 8);
        memcpy(out + 16, "Drive 3000      ", 16);
        memcpy(out + 32, "1.2 ", 4);
        return FA_OK;
    }
    if (d->state != FA_CD_READY && cdb[0] != 0x1B && cdb[0] != 0x1E) {
        if (d->state == FA_CD_SPINNING_UP) set_sense(sense, 2, 0x04, 0x01);
        else set_sense(sense, 2, 0x3A, d->state == FA_CD_TRAY_OPEN ? 2 : 1);
        return FA_ERROR_IO;
    }
    switch (cdb[0]) {
    case 0x00: return FA_OK;   /* TEST UNIT READY */
    case 0x43: {               /* READ TOC */
        int format = cdb[2] & 0x0F;
        uint32_t alloc = ((uint32_t)cdb[7] << 8) | cdb[8], n;
        const uint8_t* src = NULL;
        if (format == 0) { src = d->toc0; n = d->toc0_len; }
        else if (format == 5 && d->cdtext) { src = d->cdtext; n = d->cdtext_len; }
        else { set_sense(sense, 5, 0x24, 0); return FA_ERROR_IO; }
        if (n > alloc) n = alloc;
        memcpy(out, src, n);
        return FA_OK;
    }
    case 0xBE: {               /* READ CD */
        int64_t lba = ((int64_t)cdb[2] << 24) | (cdb[3] << 16) | (cdb[4] << 8) | cdb[5];
        uint32_t count = ((uint32_t)cdb[6] << 16) | (cdb[7] << 8) | cdb[8], i;
        int c2 = (cdb[9] & 0x02) != 0;
        uint32_t stride = FA_CD_SECTOR_BYTES + (c2 ? FA_CD_C2_BYTES : 0) + ((cdb[10] & 1) ? FA_CD_SUB_BYTES : 0);
        memcpy(d->last_read_cdb, cdb, 12);
        d->reads++;
        if (c2 && d->no_c2) { set_sense(sense, 5, 0x24, 0); return FA_ERROR_IO; }
        if ((cdb[10] & 1) && (d->no_sub || (c2 && d->no_c2_with_sub))) { set_sense(sense, 5, 0x24, 0); return FA_ERROR_IO; }
        if (len != count * stride) { set_sense(sense, 5, 0x24, 0); return FA_ERROR_IO; }
        for (i = 0; i < count; i++) {
            uint8_t* sec = out + (size_t)i * stride;
            if (lba + i == d->dead_lba) { set_sense(sense, 3, 0x11, 0); return FA_ERROR_IO; }
            if (d->data_from > 0 && lba + i >= d->data_from) { set_sense(sense, 5, 0x64, 0); return FA_ERROR_IO; }
            put_sector(sec, lba + i);
            if (c2) memset(sec + FA_CD_SECTOR_BYTES, 0, FA_CD_C2_BYTES);
            if (cdb[10] & 1) {   /* subcode after the C2 bytes */
                uint8_t* sb = sec + FA_CD_SECTOR_BYTES + (c2 ? FA_CD_C2_BYTES : 0);
                if (d->sub && lba + i < d->sub_sectors) memcpy(sb, d->sub + (lba + i) * FA_CD_SUB_BYTES, FA_CD_SUB_BYTES);
                else memset(sb, 0, FA_CD_SUB_BYTES);
            }
            if (lba + i == d->flaky_lba && d->flaky_reads > 0) {
                d->flaky_reads--;
                sec[100] ^= 0x7F;                       /* a wrong byte ... */
                if (c2) sec[FA_CD_SECTOR_BYTES + 12] = 0x08;  /* ... that the drive flags */
            }
        }
        return FA_OK;
    }
    case 0x42: {               /* READ SUB-CHANNEL */
        memset(out, 0, len);
        if (cdb[3] == 3 && cdb[6] == 2) { out[8] = 0x80; memcpy(out + 9, "GBXYZ2600042", 12); }
        if (cdb[3] == 2) { out[8] = 0x80; memcpy(out + 9, "0000000000000", 13); }   /* "none" */
        return FA_OK;
    }
    case 0xBB: d->speed = ((uint32_t)cdb[2] << 8) | cdb[3]; return FA_OK;
    case 0x5A: {               /* MODE SENSE(10) page 2A */
        memset(out, 0, len);
        out[8] = 0x2A;
        out[8 + 14] = (uint8_t)(706 >> 8);
        out[8 + 15] = (uint8_t)(706 & 0xFF);   /* 4x */
        return FA_OK;
    }
    case 0x1B: d->tray_open = (cdb[4] & 3) == 2; if (d->tray_open) d->state = FA_CD_TRAY_OPEN; return FA_OK;
    case 0x1E: d->locked = cdb[4] & 1; return FA_OK;
    }
    set_sense(sense, 5, 0x20, 0);
    return FA_ERROR_IO;
}

static void fake_close(void* h) { ((fake_drive*)h)->closed = 1; }

/* A TOC response: tracks at the given LBAs (all audio), then the lead-out. */
static uint32_t make_toc(uint8_t* out, int first, int count, const int64_t* lbas, int64_t leadout)
{
    uint32_t len = 4;
    int i;
    out[2] = (uint8_t)first;
    out[3] = (uint8_t)(first + count - 1);
    for (i = 0; i <= count; i++) {
        uint8_t* e = out + len;
        int64_t lba = i < count ? lbas[i] : leadout;
        memset(e, 0, 8);
        e[1] = 0x10;
        e[2] = (uint8_t)(i < count ? first + i : 0xAA);
        e[4] = (uint8_t)(lba >> 24); e[5] = (uint8_t)(lba >> 16); e[6] = (uint8_t)(lba >> 8); e[7] = (uint8_t)lba;
        len += 8;
    }
    out[0] = (uint8_t)((len - 2) >> 8);
    out[1] = (uint8_t)(len - 2);
    return len;
}

static fa_disc* open_fake(fake_drive* fd, const fa_disc_options* o)
{
    const fa_disc_ops* ops;
    void* h;
    fa_disc* d = NULL;
    if (fa_mmc_open(fake_exec, fake_close, fd, 7, &ops, &h) != FA_OK) return NULL;
    if (fa_disc_open_ops(ops, h, o, &d) != FA_OK) return NULL;
    return d;
}

static void test_mmc_ids(void)
{
    /* The worked example of the MusicBrainz disc id documentation. */
    static const int64_t lbas[6] = { 0, 15213, 32164, 46442, 63264, 80339 };
    uint8_t toc[4 + 8 * 7];
    fake_drive fd;
    fa_disc* d;
    fa_disc_toc t;
    fa_disc_status st;
    char text[64];
    float speed = 0;
    memset(&fd, 0, sizeof(fd));
    fd.toc0 = toc;
    fd.toc0_len = make_toc(toc, 1, 6, lbas, 95312);
    d = open_fake(&fd, NULL);
    CHECK(d);
    CHECK_EQ(fa_disc_get_toc(d, &t), FA_OK);
    CHECK_STR(t.musicbrainz_id, "49HHV7Eb8UKF3aQiNmu1GR8vKTY-");
    CHECK_EQ(t.track_count, 6);
    CHECK_EQ(t.tracks[5].sectors, 95312 - 80339);
    CHECK_EQ(t.has_cd_text, 0);
    CHECK_EQ(fa_disc_get_text(d, 1, FA_DISC_TEXT_TITLE, text, sizeof(text)), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_disc_get_text(d, 2, FA_DISC_TEXT_CODE, text, sizeof(text)), FA_OK);
    CHECK_STR(text, "GBXYZ2600042");
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_CODE, text, sizeof(text)), FA_ERROR_NOT_FOUND);   /* all zeros */

    /* Drive control as MMC commands. */
    CHECK_EQ(fa_disc_set_speed(d, 4.0f), FA_OK);
    CHECK_EQ(fd.speed, 706);
    CHECK_EQ(fa_disc_set_speed(d, 0.0f), FA_OK);
    CHECK_EQ(fd.speed, 0xFFFF);
    CHECK_EQ(fa_disc_get_speed(d, &speed), FA_OK);
    CHECK(fabs(speed - 4.0f) < 0.01f);
    CHECK_EQ(fa_disc_set_locked(d, 1), FA_OK);
    CHECK_EQ(fd.locked, 1);
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.state, FA_DISC_READY);
    CHECK_EQ(st.locked, 1);
    CHECK_EQ(st.change_count, 0);
    CHECK_EQ(fa_disc_eject(d), FA_OK);      /* unlocks first */
    CHECK_EQ(fd.locked, 0);
    CHECK_EQ(fd.tray_open, 1);
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.state, FA_DISC_TRAY_OPEN);
    CHECK_EQ(st.change_count, 1);
    CHECK_EQ(fa_disc_get_toc(d, &t), FA_ERROR_NO_DISC);
    fd.state = FA_CD_NO_DISC;
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.state, FA_DISC_NO_DISC);
    fd.state = FA_CD_SPINNING_UP;
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.state, FA_DISC_SPINNING_UP);
    CHECK_EQ(st.change_count, 1);
    fd.state = FA_CD_READY;
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.change_count, 2);
    /* A disc swapped between two polls shows up as a unit attention. */
    fd.unit_attention = 1;
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.state, FA_DISC_READY);
    CHECK_EQ(st.change_count, 3);
    CHECK_EQ(fa_disc_set_locked(d, 1), FA_OK);
    fa_disc_close(d);                        /* unlocks and closes the drive */
    CHECK_EQ(fd.locked, 0);
    CHECK_EQ(fd.closed, 1);
}

static void test_mmc_reads(void)
{
    static const int64_t lbas[2] = { 0, 100 };
    uint8_t toc[4 + 8 * 3];
    fake_drive fd;
    fa_disc* d;
    fa_stream* s;
    float* buf = (float*)malloc(sizeof(float) * 2 * 100 * 588);
    int64_t got, i;
    CHECK(buf);
    memset(&fd, 0, sizeof(fd));
    fd.toc0 = toc;
    fd.toc0_len = make_toc(toc, 1, 2, lbas, 200);
    fd.flaky_lba = 10;
    fd.flaky_reads = 3;       /* bad on the first read and two retries; the third retry is clean */
    fd.dead_lba = -1;
    d = open_fake(&fd, NULL);
    CHECK(d);
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 100 * 588, &got), FA_OK);
    CHECK_EQ(fd.last_read_cdb[1], 0x04);           /* expected sector type: CD-DA */
    CHECK_EQ(fd.last_read_cdb[9], 0x12);           /* user data + C2 error pointers */
    CHECK_EQ(fd.last_read_cdb[10], 0x01);          /* raw P-W subcode, for CD+G */
    CHECK_EQ(fd.flaky_reads, 0);
    for (i = 0; i < 100 * 588; i++) if (buf[i * 2] != pf(i, 0) || buf[i * 2 + 1] != pf(i, 1)) break;
    CHECK_EQ(i, 100 * 588);                         /* the re-read copy is the clean one */
    fa_stream_close(s);
    fa_disc_close(d);

    /* A drive without C2 support: the first read is refused, then reads go on without. */
    memset(&fd, 0, sizeof(fd));
    fd.toc0 = toc;
    fd.toc0_len = make_toc(toc, 1, 2, lbas, 200);
    fd.no_c2 = 1;
    fd.flaky_lba = fd.dead_lba = -1;
    d = open_fake(&fd, NULL);
    CHECK(d);
    s = open_track(d, 2);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 100 * 588, &got), FA_OK);
    CHECK_EQ(fd.last_read_cdb[9], 0x10);
    for (i = 0; i < 100 * 588; i++) if (buf[i * 2] != pf(100 * 588 + i, 0)) break;
    CHECK_EQ(i, 100 * 588);
    fa_stream_close(s);
    fa_disc_close(d);
    free(buf);
}

static fa_atomic32 g_decode_errors;
static void FA_CALL on_event(const fa_event* ev, void* user)
{
    (void)user;
    if (ev->type == FA_EVENT_DECODE_ERROR && ev->value == FA_ERROR_IO) fa_fetch_add32(&g_decode_errors, 1);
}

static void test_mmc_damage_and_removal(void)
{
    static const int64_t lbas[1] = { 0 };
    uint8_t toc[4 + 8 * 2];
    fake_drive fd;
    fa_disc* d;
    fa_disc_options o;
    fa_stream* s;
    float* buf = (float*)malloc(sizeof(float) * 2 * 64 * 588);
    int64_t got, i, t0;
    fa_frames k;
    CHECK(buf);
    memset(&fd, 0, sizeof(fd));
    fd.toc0 = toc;
    fd.toc0_len = make_toc(toc, 1, 1, lbas, 1000);
    fd.flaky_lba = 5;
    fd.flaky_reads = 1000;    /* never clean */
    fd.dead_lba = 20;         /* never readable */
    fa_disc_options_init(&o);
    o.retries = 2;
    d = open_fake(&fd, &o);
    CHECK(d);
    s = open_track(d, 1);
    CHECK(s);
    fa_store32(&g_decode_errors, 0);
    CHECK_EQ(fa_stream_set_event_callback(s, on_event, NULL), FA_OK);
    CHECK_EQ(read_all(s, buf, 32 * 588, &got), FA_OK);
    CHECK_EQ(got, 32 * 588);
    /* Sector 5 plays as read (damaged), sector 20 as silence, the rest exactly. */
    CHECK(buf[(5 * 588 + 25) * 2] != pf(5 * 588 + 25, 0));
    for (i = 0; i < 588; i++) CHECK(buf[(20 * 588 + i) * 2] == 0.0f);
    for (i = 21 * 588; i < 32 * 588; i++) if (buf[i * 2] != pf(i, 0)) break;
    CHECK_EQ(i, 32 * 588);
    t0 = fa_monotonic_ns();
    while (fa_load32(&g_decode_errors) == 0 && fa_monotonic_ns() - t0 < 2000000000LL) fa_sleep_ms(5);
    CHECK(fa_load32(&g_decode_errors) >= 1);

    /* The disc is taken out: the stream's next read fails, the stream ends. */
    fd.state = FA_CD_NO_DISC;
    k = 1;
    CHECK_EQ(fa_stream_read(s, buf, 64 * 588, &k), FA_ERROR_NO_DISC);
    CHECK_EQ(k, 0);
    /* Even with the same disc back, an old stream does not continue on it. */
    fd.state = FA_CD_READY;
    fd.unit_attention = 1;
    {
        fa_disc_toc t;
        CHECK_EQ(fa_disc_get_toc(d, &t), FA_OK);
    }
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    CHECK_EQ(fa_stream_read(s, buf, 588, &k), FA_ERROR_NO_DISC);
    fa_stream_close(s);
    fa_disc_close(d);
    free(buf);
}

/* The last audio track before a data track: offset reads stop at the track's end
 * instead of failing into the data, which would cost retries, raise a decode error
 * and (being an ILLEGAL REQUEST) must not turn C2 off. */
static void test_mmc_audio_edge(void)
{
    static const int64_t lbas[2] = { 0, 100 };
    uint8_t toc[4 + 8 * 3];
    fake_drive fd;
    fa_disc* d;
    fa_disc_options o;
    fa_stream* s;
    float* buf = (float*)malloc(sizeof(float) * 2 * 100 * 588);
    int64_t got, i;
    CHECK(buf);
    memset(&fd, 0, sizeof(fd));
    fd.toc0 = toc;
    fd.toc0_len = make_toc(toc, 1, 2, lbas, 300);
    toc[4 + 8 + 1] = 0x14;    /* track 2: data */
    fd.flaky_lba = fd.dead_lba = -1;
    fd.data_from = 100;
    fa_disc_options_init(&o);
    o.read_offset = 6;
    d = open_fake(&fd, &o);
    CHECK(d);
    s = open_track(d, 1);
    CHECK(s);
    fa_store32(&g_decode_errors, 0);
    CHECK_EQ(fa_stream_set_event_callback(s, on_event, NULL), FA_OK);
    CHECK_EQ(read_all(s, buf, 100 * 588, &got), FA_OK);
    for (i = 0; i < 100 * 588 - 6; i++) if (buf[i * 2] != pf(i + 6, 0)) break;
    CHECK_EQ(i, 100 * 588 - 6);
    for (; i < 100 * 588; i++) CHECK(buf[i * 2] == 0.0f);   /* past the audio: silence */
    CHECK(fd.reads <= 20);                                  /* 7-sector commands, no per-sector retries */
    fa_sleep_ms(100);
    CHECK_EQ(fa_load32(&g_decode_errors), 0);
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    CHECK_EQ(read_all(s, buf, 588, &got), FA_OK);
    CHECK_EQ(fd.last_read_cdb[9], 0x12);                    /* still with C2 */
    fa_stream_close(s);
    CHECK_EQ(fa_stream_open_disc(g_engine, d, 2, NULL, &s), FA_ERROR_UNSUPPORTED_FORMAT);   /* data */
    fa_disc_close(d);
    free(buf);
}

/* A drive whose OS call fails while the tray is open (Windows ERROR_NOT_READY,
 * Linux ENOMEDIUM) reports "no disc", counts the change and forgets the TOC. */
static void test_mmc_os_no_medium(void)
{
    static const int64_t lbas[1] = { 0 };
    uint8_t toc[4 + 8 * 2];
    fake_drive fd;
    fa_disc* d;
    fa_disc_status st;
    fa_disc_toc t;
    memset(&fd, 0, sizeof(fd));
    fd.toc0 = toc;
    fd.toc0_len = make_toc(toc, 1, 1, lbas, 1000);
    fd.flaky_lba = fd.dead_lba = -1;
    d = open_fake(&fd, NULL);
    CHECK(d);
    CHECK_EQ(fa_disc_get_toc(d, &t), FA_OK);
    CHECK_EQ(t.leadout_sector, 1000);
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.change_count, 0);
    fd.os_no_medium = 1;
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.state, FA_DISC_NO_DISC);
    CHECK_EQ(st.change_count, 1);
    CHECK_EQ(fa_disc_get_toc(d, &t), FA_ERROR_NO_DISC);
    /* Another disc goes in; the drive's "medium changed" was seen by the OS. */
    fd.os_no_medium = 0;
    fd.toc0_len = make_toc(toc, 1, 1, lbas, 2000);
    CHECK_EQ(fa_disc_get_toc(d, &t), FA_OK);
    CHECK_EQ(t.leadout_sector, 2000);
    CHECK_EQ(fa_disc_get_status(d, &st), FA_OK);
    CHECK_EQ(st.change_count, 2);
    fa_disc_close(d);
}

/* CD-Text as real drives return it: several blocks (a Japanese one first), a TAB
 * standing for "same as the previous track", strings across pack boundaries, and a
 * pack with a broken CRC. */
static void test_mmc_cdtext(void)
{
    static const int64_t lbas[3] = { 0, 1000, 2000 };
    uint8_t toc[4 + 8 * 4], cdtext[4 + 18 * 16];
    uint32_t n = 0;
    fake_drive fd;
    fa_disc* d;
    char t[128];
    /* Each entry is the whole text of one pack type in one block (album first,
     * then tracks 1, 2, ...), cut into 12-byte packs the way drives return them. */
    struct { int type, block; const char* text; size_t len; } packs[] = {
        { 0x8F, 0, "\x80", 1 },                                       /* block 0: MS-JIS (skipped) */
        { 0x80, 0, "JAPANESE", 9 },
        { 0x8F, 1, "\x00", 1 },                                       /* block 1: ISO 8859-1 */
        { 0x80, 1, "Album\0Track one title\0\x09\0Three", 30 },       /* track 2 = TAB = as track 1 */
        { 0x81, 1, "Artist\0\0Solo", 13 },                             /* track 1 has no performer */
        { 0x82, 1, "Broken", 7 },                                     /* bad CRC: dropped */
    };
    int p;
    memset(&fd, 0, sizeof(fd));
    fd.toc0 = toc;
    fd.toc0_len = make_toc(toc, 1, 3, lbas, 3000);
    memset(cdtext, 0, sizeof(cdtext));
    n = 4;
    for (p = 0; p < (int)(sizeof(packs) / sizeof(packs[0])); p++) {
        const char* src = packs[p].text;
        size_t len = packs[p].len, off, k;
        int track = 0;
        for (off = 0; off < len; off += 12) {
            uint8_t* q = cdtext + n;
            uint16_t crc;
            size_t m = len - off < 12 ? len - off : 12;
            q[0] = (uint8_t)packs[p].type;
            q[1] = (uint8_t)track;               /* the track of the pack's first character */
            q[2] = (uint8_t)((n - 4) / 18);
            q[3] = (uint8_t)(packs[p].block << 4);
            memcpy(q + 4, src + off, m);
            for (k = off; k < off + m; k++) if (src[k] == 0) track++;
            crc = fa_cdtext_crc(q);
            q[16] = (uint8_t)(crc >> 8);
            q[17] = (uint8_t)crc;
            if (packs[p].type == 0x82) q[17] ^= 0xFF;
            n += 18;
        }
    }
    cdtext[0] = (uint8_t)((n - 2) >> 8);
    cdtext[1] = (uint8_t)(n - 2);
    fd.cdtext = cdtext;
    fd.cdtext_len = n;
    d = open_fake(&fd, NULL);
    CHECK(d);
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Album");
    CHECK_EQ(fa_disc_get_text(d, 1, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Track one title");
    CHECK_EQ(fa_disc_get_text(d, 2, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Track one title");
    CHECK_EQ(fa_disc_get_text(d, 3, FA_DISC_TEXT_TITLE, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Three");
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_PERFORMER, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Artist");
    CHECK_EQ(fa_disc_get_text(d, 1, FA_DISC_TEXT_PERFORMER, t, sizeof(t)), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_disc_get_text(d, 2, FA_DISC_TEXT_PERFORMER, t, sizeof(t)), FA_OK);
    CHECK_STR(t, "Solo");
    CHECK_EQ(fa_disc_get_text(d, 0, FA_DISC_TEXT_SONGWRITER, t, sizeof(t)), FA_ERROR_NOT_FOUND);
    fa_disc_close(d);
}


/* ---- CD+G from the disc ------------------------------------------------------------ */

#define SONG_SECTORS 600                      /* 8 s */
#define SONG_PACKS (SONG_SECTORS * 4)

static uint8_t gf64_mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; b >>= 1; a = (uint8_t)(a << 1); if (a & 0x40) a ^= 0x43; }
    return r;
}

static uint8_t gf64_inv(uint8_t a)
{
    uint8_t x;
    for (x = 1; x < 64; x++) if (gf64_mul(a, x) == 1) return x;
    return 0;
}

static int g_with_parity = 1;

/* Fills the last `k` of the first `n` symbols so that sum c[i] a^(j(n-1-i)) = 0 for
 * j < k (Gaussian elimination over GF(64)). */
static void rs_parity(uint8_t* c, int n, int k)
{
    uint8_t m[4][5];
    int j, i, col, row;
    for (j = 0; j < k; j++) {
        uint8_t root = (uint8_t)(1u << j), w = 1, rhs = 0;
        uint8_t pw[24];
        for (i = n - 1; i >= 0; i--) { pw[i] = w; w = gf64_mul(w, root); }
        for (i = 0; i < n - k; i++) rhs ^= gf64_mul(c[i], pw[i]);
        for (i = 0; i < k; i++) m[j][i] = pw[n - k + i];
        m[j][k] = rhs;
    }
    for (col = 0; col < k; col++) {
        uint8_t inv;
        for (row = col; row < k && !m[row][col]; row++) {}
        if (row != col) for (i = 0; i <= k; i++) { uint8_t t = m[row][i]; m[row][i] = m[col][i]; m[col][i] = t; }
        inv = gf64_inv(m[col][col]);
        for (i = 0; i <= k; i++) m[col][i] = gf64_mul(m[col][i], inv);
        for (row = 0; row < k; row++)
            if (row != col && m[row][col]) {
                uint8_t f = m[row][col];
                for (i = 0; i <= k; i++) m[row][i] ^= gf64_mul(f, m[col][i]);
            }
    }
    for (i = 0; i < k; i++) c[n - k + i] = m[i][k];
}

/* A graphics pack with its Q parity (H = [1 1 1 1; a^3 a^2 a 1] over GF(64)). */
static void put_pack(uint8_t* p, int instr, const uint8_t* data16)
{
    memset(p, 0, 24);
    p[0] = 0x09;
    p[1] = (uint8_t)instr;
    if (data16) memcpy(p + 4, data16, 16);
    if (g_with_parity) {
        rs_parity(p, 4, 2);    /* Q: symbols 2-3 */
        rs_parity(p, 24, 4);   /* P: symbols 20-23 */
    }
}

/* A karaoke track's graphics: screen cleared to colour 1 (red) at the start, then
 * to colour 2 (green) at 2 s; zero packs (no-ops) in between, as on real discs. */
static void make_song(uint8_t* packs)
{
    uint8_t d[16];
    int i;
    memset(packs, 0, (size_t)SONG_PACKS * 24);
    for (i = 0; i < 16; i++) {
        memset(d, 0, sizeof(d));
        d[0] = 1; d[1] = (uint8_t)i;
        put_pack(packs + i * 24, 1, d);                  /* memory preset, repeated */
    }
    memset(d, 0, sizeof(d));
    d[2] = 0x3C;                                         /* colour 1 = red (15, 0, 0) */
    d[4] = 0x00; d[5] = 0x30;                            /* colour 2 = green (0, 15, 0): g = 3 << 2 | 3 */
    d[4] = 0x03;
    put_pack(packs + 16 * 24, 30, d);                    /* load colours 0..7 */
    for (i = 0; i < 16; i++) {
        memset(d, 0, sizeof(d));
        d[0] = 2; d[1] = (uint8_t)i;
        put_pack(packs + (600 + i) * 24, 1, d);
    }
}

static int swap_sym(int n)
{
    return n == 1 ? 18 : n == 18 ? 1 : n == 2 ? 5 : n == 5 ? 2 : n == 3 ? 23 : n == 23 ? 3 : n;
}

/* The subcode as a drive returns it: 0 = packs as they are (drives that de-interleave),
 * 1 = swapped then delayed, 2 = delayed then swapped (the two orders of the disc's
 * interleave). P and Q bits are set to check they are ignored. */
static void interleave(int variant, const uint8_t* packs, uint8_t* raw)
{
    int64_t q;
    int n;
    for (q = 0; q < SONG_PACKS; q++)
        for (n = 0; n < 24; n++) {
            uint8_t v = 0;
            if (variant == 0) v = packs[q * 24 + n];
            else if (variant == 1) {          /* r[q][m] = c[q - m%8][swap(m)] */
                int64_t from = q - n % 8;
                if (from >= 0) v = packs[from * 24 + swap_sym(n)];
            } else {                          /* r[q][swap(n)] = c[q - n%8][n] */
                int64_t from = q - n % 8;
                if (from >= 0) raw[q * 24 + swap_sym(n)] = (uint8_t)(packs[from * 24 + n] | 0xC0);
                else raw[q * 24 + swap_sym(n)] = 0xC0;
                continue;
            }
            raw[q * 24 + n] = (uint8_t)(v | 0xC0);
        }
}

/* The same subcode with the channels one after another (P, Q, R..W, 12 bytes each
 * per sector), as some .sub files hold it. */
static void to_channels(uint8_t* sub)
{
    uint8_t sec[96];
    int64_t s0;
    int k, ch;
    for (s0 = 0; s0 < SONG_SECTORS; s0++) {
        uint8_t* in = sub + s0 * 96;
        memset(sec, 0, sizeof(sec));
        for (k = 0; k < 96; k++)
            for (ch = 0; ch < 8; ch++)
                if (in[k] & (0x80 >> ch)) sec[ch * 12 + k / 8] |= (uint8_t)(0x80 >> (k % 8));
        memcpy(in, sec, 96);
    }
}

static int write_cdg_image(const char* stem, const uint8_t* sub)
{
    char path[64];
    FILE* f;
    uint8_t sec[FA_CD_SECTOR_BYTES];
    int64_t i;
    snprintf(path, sizeof(path), "%s.bin", stem);
    if (!(f = fopen(path, "wb"))) return 0;
    for (i = 0; i < SONG_SECTORS; i++) { put_sector(sec, i); fwrite(sec, 1, sizeof(sec), f); }
    fclose(f);
    if (sub) {
        snprintf(path, sizeof(path), "%s.sub", stem);
        if (!(f = fopen(path, "wb"))) return 0;
        fwrite(sub, 1, (size_t)SONG_SECTORS * FA_CD_SUB_BYTES, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s.cue", stem);
    if (!(f = fopen(path, "wb"))) return 0;
    fprintf(f, "FILE \"%s.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n", stem);
    fclose(f);
    return 1;
}

static void remove_cdg_image(const char* stem)
{
    char path[64];
    snprintf(path, sizeof(path), "%s.bin", stem); remove(path);
    snprintf(path, sizeof(path), "%s.sub", stem); remove(path);
    snprintf(path, sizeof(path), "%s.cue", stem); remove(path);
}

/* Reads the whole track (the graphics arrive with the audio). */
static fa_result play_through(fa_stream* s)
{
    float* buf = (float*)malloc(sizeof(float) * 2 * 4096);
    fa_result r = FA_OK;
    if (!buf) return FA_ERROR_OUT_OF_MEMORY;
    for (;;) {
        fa_frames k = 0;
        r = fa_stream_read(s, buf, 4096, &k);
        if (r != FA_OK || k == 0) break;
    }
    free(buf);
    return r == FA_ERROR_END_OF_STREAM ? FA_OK : r;
}

static int has_cdg(fa_stream* s)
{
    fa_stream_info info;
    return fa_stream_get_info(s, &info) == FA_OK && info.has_cdg;
}

static int same_packs(fa_stream* s, const uint8_t* packs)
{
    int same;
    fa_mutex_lock(&s->cdg_lock);
    same = s->cdg && s->cdg->count == SONG_PACKS && memcmp(s->cdg->packs, packs, (size_t)SONG_PACKS * 24) == 0;
    fa_mutex_unlock(&s->cdg_lock);
    return same;
}

/* Centre pixel of the picture at `t`, as B, G, R (BGRA). */
static uint32_t pixel_at(fa_cdg_surface* f, fa_time t)
{
    fa_cdg_frame fr;
    const uint8_t* p;
    if (fa_cdg_surface_update_at(f, t, &fr) != FA_OK) return 0xFFFFFFFFu;
    p = (const uint8_t*)fr.pixels + (fr.height / 2) * (uint32_t)fr.stride + (fr.width / 2) * 4;
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];   /* 0xRRGGBB */
}

/* Every reading of the subcode a drive may return (and a .sub beside an image) gives
 * the same graphics, drawn in sync. */
static void test_cdg_image(void)
{
    uint8_t* packs = (uint8_t*)malloc((size_t)SONG_PACKS * 24);
    uint8_t* sub = (uint8_t*)malloc((size_t)SONG_PACKS * 24);
    int variant;
    CHECK(packs && sub);
    make_song(packs);
    for (variant = 0; variant < 6; variant++) {
        fa_disc* d;
        /* Passes 3 and 4: a drive that returns no parity (zeros), cooked and raw.
         * Pass 5: a .sub with the channels one after another. */
        g_with_parity = variant < 3 || variant == 5;
        make_song(packs);
        fa_stream* s;
        fa_cdg_surface* f;
        interleave(variant < 3 ? variant : variant == 5 ? 1 : variant - 3, packs, sub);
        if (variant == 5) to_channels(sub);
        CHECK(write_cdg_image("disc_cdg", sub));
        CHECK_EQ(fa_disc_open("disc_cdg.cue", NULL, &d), FA_OK);
        s = open_track(d, 1);
        CHECK(s);
        CHECK_EQ(has_cdg(s), 0);                              /* found while the track plays */
        CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
        CHECK_EQ(play_through(s), FA_OK);
        CHECK_EQ(has_cdg(s), 1);
        CHECK(same_packs(s, packs));
        {
            fa_cdg_info ci;
            CHECK_EQ(fa_stream_get_cdg_info(s, &ci), FA_OK);
            CHECK_EQ(ci.source, FA_CDG_SOURCE_DISC);
            CHECK_EQ(ci.pack_count, SONG_PACKS);
            CHECK_EQ(ci.disc_parity, g_with_parity);     /* how the layout was chosen */
        }
        CHECK_EQ(pixel_at(f, 1000000000LL), 0xFF0000u);   /* red */
        CHECK_EQ(pixel_at(f, 3000000000LL), 0x00FF00u);   /* green after 2 s */
        CHECK_EQ(pixel_at(f, 500000000LL), 0xFF0000u);    /* and back */
        fa_cdg_surface_destroy(f);
        fa_stream_close(s);
        fa_disc_close(d);
        remove_cdg_image("disc_cdg");
    }
    g_with_parity = 1;
    free(packs);
    free(sub);
}

/* Tracks without graphics get none: no .sub at all, or a subcode of zeros. */
static void test_cdg_absent(void)
{
    uint8_t* zeros = (uint8_t*)calloc((size_t)SONG_SECTORS, FA_CD_SUB_BYTES);
    int pass;
    CHECK(zeros);
    for (pass = 0; pass < 2; pass++) {
        fa_disc* d;
        fa_stream* s;
        CHECK(write_cdg_image("disc_nocdg", pass ? zeros : NULL));
        CHECK_EQ(fa_disc_open("disc_nocdg.cue", NULL, &d), FA_OK);
        s = open_track(d, 1);
        CHECK(s);
        CHECK_EQ(play_through(s), FA_OK);
        CHECK_EQ(has_cdg(s), 0);
        fa_stream_close(s);
        fa_disc_close(d);
        remove_cdg_image("disc_nocdg");
    }
    free(zeros);
}

/* Through the MMC layer: subcode follows the C2 bytes; a seek back replays the
 * graphics; a drive that can't read subcode keeps its C2 reads; FA_DISC_NO_CDG
 * asks for none. */
static void test_cdg_mmc(void)
{
    static const int64_t lbas[1] = { 0 };
    uint8_t toc[4 + 8 * 2];
    uint8_t* packs = (uint8_t*)malloc((size_t)SONG_PACKS * 24);
    uint8_t* sub = (uint8_t*)malloc((size_t)SONG_PACKS * 24);
    fake_drive fd;
    fa_disc* d;
    fa_disc_options o;
    fa_stream* s;
    fa_cdg_surface* f;
    float* buf = (float*)malloc(sizeof(float) * 2 * 588 * 100);
    int64_t got;
    CHECK(packs && sub && buf);
    make_song(packs);
    interleave(1, packs, sub);
    memset(&fd, 0, sizeof(fd));
    fd.toc0 = toc;
    fd.toc0_len = make_toc(toc, 1, 1, lbas, SONG_SECTORS);
    fd.flaky_lba = fd.dead_lba = -1;
    fd.sub = sub;
    fd.sub_sectors = SONG_SECTORS;
    d = open_fake(&fd, NULL);
    CHECK(d);
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(fa_cdg_surface_create(s, NULL, &f), FA_OK);
    CHECK_EQ(read_all(s, buf, 588 * 100, &got), FA_OK);      /* the first 100 sectors */
    CHECK_EQ(fd.last_read_cdb[9], 0x12);
    CHECK_EQ(fd.last_read_cdb[10], 0x01);
    CHECK_EQ(pixel_at(f, 1000000000LL), 0xFF0000u);
    CHECK_EQ(fa_stream_seek(s, 0), FA_OK);
    CHECK_EQ(play_through(s), FA_OK);
    CHECK(same_packs(s, packs));
    CHECK_EQ(pixel_at(f, 3000000000LL), 0x00FF00u);
    fa_cdg_surface_destroy(f);
    fa_stream_close(s);
    fa_disc_close(d);

    fd.no_sub = 1;
    d = open_fake(&fd, NULL);
    CHECK(d);
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 588 * 100, &got), FA_OK);
    CHECK_EQ(fd.last_read_cdb[9], 0x12);                     /* still with C2 */
    CHECK_EQ(fd.last_read_cdb[10], 0x00);
    CHECK_EQ(has_cdg(s), 0);
    fa_stream_close(s);
    fa_disc_close(d);

    /* C2 or subcode, not both: CD+G reads go without C2 and still find the graphics. */
    fd.no_sub = 0;
    fd.no_c2_with_sub = 1;
    d = open_fake(&fd, NULL);
    CHECK(d);
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(play_through(s), FA_OK);
    CHECK(same_packs(s, packs));
    CHECK_EQ(fd.last_read_cdb[9], 0x10);
    CHECK_EQ(fd.last_read_cdb[10], 0x01);
    fa_stream_close(s);
    fa_disc_close(d);

    fd.no_c2_with_sub = 0;
    fa_disc_options_init(&o);
    o.flags = FA_DISC_NO_CDG;
    d = open_fake(&fd, &o);
    CHECK(d);
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 588 * 100, &got), FA_OK);
    CHECK_EQ(fd.last_read_cdb[10], 0x00);
    fa_stream_close(s);
    fa_disc_close(d);
    free(packs);
    free(sub);
    free(buf);
}


/* Graphics the app gives a disc stream win over the disc's; a seek leaves no broken
 * packs behind; FA_OPEN_NO_CDG reads no subcode. */
static void test_cdg_rules(void)
{
    uint8_t* packs = (uint8_t*)malloc((size_t)SONG_PACKS * 24);
    uint8_t* sub = (uint8_t*)malloc((size_t)SONG_PACKS * 24);
    uint8_t mine[24 * 4];
    fa_disc* d;
    fa_stream* s;
    fa_open_options o;
    float* buf = (float*)malloc(sizeof(float) * 2 * 588 * 50);
    int64_t got, i;
    CHECK(packs && sub && buf);
    make_song(packs);
    interleave(1, packs, sub);
    CHECK(write_cdg_image("disc_rules", sub));
    CHECK_EQ(fa_disc_open("disc_rules.cue", NULL, &d), FA_OK);

    memset(mine, 0, sizeof(mine));
    put_pack(mine, 1, NULL);
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(fa_stream_load_cdg_memory(s, mine, sizeof(mine)), FA_OK);
    CHECK_EQ(play_through(s), FA_OK);
    fa_mutex_lock(&s->cdg_lock);
    i = s->cdg && s->cdg->count == 4 && memcmp(s->cdg->packs, mine, sizeof(mine)) == 0;
    fa_mutex_unlock(&s->cdg_lock);
    CHECK(i);
    fa_stream_close(s);

    /* 50 sectors, then a jump to 6 s and on to the end: every pack written is the
     * disc's own; the ones the jump left without look-ahead stay empty. */
    s = open_track(d, 1);
    CHECK(s);
    CHECK_EQ(read_all(s, buf, 588 * 50, &got), FA_OK);
    CHECK_EQ(fa_stream_seek(s, 588 * 450), FA_OK);
    CHECK_EQ(play_through(s), FA_OK);
    fa_mutex_lock(&s->cdg_lock);
    CHECK(s->cdg != NULL);
    for (i = 0; i < SONG_PACKS; i++) {
        const uint8_t* got_p = s->cdg->packs + i * 24;
        static const uint8_t zero[24];
        if (memcmp(got_p, packs + i * 24, 24) != 0 && memcmp(got_p, zero, 24) != 0) break;
    }
    fa_mutex_unlock(&s->cdg_lock);
    CHECK_EQ(i, SONG_PACKS);
    fa_stream_close(s);

    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY | FA_OPEN_NO_CDG;
    CHECK_EQ(fa_stream_open_disc(g_engine, d, 1, &o, &s), FA_OK);
    CHECK_EQ(play_through(s), FA_OK);
    CHECK_EQ(has_cdg(s), 0);
    fa_stream_close(s);
    fa_disc_close(d);
    remove_cdg_image("disc_rules");
    free(packs);
    free(sub);
    free(buf);
}

static void test_misc(void)
{
    uint8_t cda[44];
    uint32_t count = 99;
    fa_disc_drive_info info;
    fa_disc* d = NULL;
    memset(cda, 0, sizeof(cda));
    memcpy(cda, "RIFF", 4);
    cda[4] = 36;
    memcpy(cda + 8, "CDDAfmt ", 8);
    cda[16] = 24;
    cda[20] = 1;
    cda[22] = 7;
    CHECK_EQ(fa_disc_cda_track(cda, sizeof(cda)), 7);
    cda[8] = 'X';
    CHECK_EQ(fa_disc_cda_track(cda, sizeof(cda)), 0);
    /* No drive on CI machines; the calls still work. */
    CHECK_EQ(fa_disc_drive_count(&count), FA_OK);
    CHECK(count < 32);
    if (count > 0) {
        CHECK_EQ(fa_disc_drive_get_info(0, &info), FA_OK);
        printf("  drive 0: %s (%s)\n", info.name, info.id);
    }
    CHECK_EQ(fa_disc_drive_get_info(count, &info), FA_ERROR_NOT_FOUND);
    CHECK_EQ(fa_disc_open("no-such-drive", NULL, &d), FA_ERROR_NOT_FOUND);
    CHECK_STR(fa_result_string(FA_ERROR_NO_DISC), "no disc");
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine\n"); return 1; }
    if (!write_image()) { fprintf(stderr, "cannot write the test image\n"); return 1; }
    RUN(test_image_toc);
    RUN(test_image_text);
    RUN(test_image_stream);
    RUN(test_image_offset);
    RUN(test_image_deemphasis);
    RUN(test_image_errors);
    RUN(test_image_formats);
    RUN(test_image_later_disc);
    RUN(test_image_playback);
    RUN(test_mmc_ids);
    RUN(test_mmc_reads);
    RUN(test_mmc_damage_and_removal);
    RUN(test_mmc_audio_edge);
    RUN(test_mmc_os_no_medium);
    RUN(test_mmc_cdtext);
    RUN(test_cdg_image);
    RUN(test_cdg_absent);
    RUN(test_cdg_mmc);
    RUN(test_cdg_rules);
    RUN(test_misc);
    remove_image();
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all disc tests passed\n");
    return 0;
}
