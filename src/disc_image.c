/* Disc images: a .cue sheet with BINARY (raw 2352-byte sectors, little-endian
 * audio), MOTOROLA (big-endian audio) or WAVE (44.1 kHz 16-bit stereo PCM) files.
 * The image answers like a drive: an MMC-format TOC, session info and CD-Text
 * packs built from the sheet, sector reads, and catalog/ISRC codes. */
#if !defined(_WIN32)
#  define _FILE_OFFSET_BITS 64
#  define _POSIX_C_SOURCE 200809L
#endif
#include "disc.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG_MAX_FILES 99
#define IMG_MAX_SEGS 512
#define IMG_TEXT_FIELDS 7      /* title, performer, songwriter, composer, arranger, message, genre */

typedef struct img_file {
    FILE* f;
    int64_t data_offset;       /* bytes before the first sector (WAVE header) */
    int64_t sectors;
    int64_t bytes;             /* BINARY/MOTOROLA: file size, for the sector count */
    uint32_t sector_bytes;     /* 2352, or 2048 for MODE1/2048 data */
    FILE* sub;                 /* "<name>.sub" beside it: 96 bytes of P-W subcode per sector */
    int swap;                  /* MOTOROLA: big-endian samples */
} img_file;

typedef struct img_seg {       /* a run of disc sectors: file data, or nothing */
    int64_t lba, count;
    int file;                  /* -1 = silence (PREGAP) or the gap between sessions */
    int64_t file_sector;
    int audio;
} img_seg;

typedef struct img_track {
    int number, audio, pre, dcp, four, session;
    int64_t lba;               /* index 01 */
    char isrc[13];
    char* text[IMG_TEXT_FIELDS];
} img_track;

typedef struct image {
    img_file files[IMG_MAX_FILES];
    int nfiles;
    img_seg segs[IMG_MAX_SEGS];
    int nsegs;
    img_track tracks[FA_DISC_MAX_TRACKS];
    int ntracks;
    int64_t leadout;
    int sessions;
    char catalog[14];
    char* album[IMG_TEXT_FIELDS];
    int has_sub;               /* some file has a .sub: subcode (CD+G) can be read */
    uint8_t* cdtext;           /* READ TOC format 5 response, NULL = none */
    uint32_t cdtext_len;
} image;

static int file_seek(FILE* f, int64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int64_t file_size(FILE* f)
{
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) return -1;
    return _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) return -1;
    return (int64_t)ftello(f);
#endif
}

static void img_free(image* m)
{
    int i, k;
    for (i = 0; i < m->nfiles; i++) {
        if (m->files[i].f) fclose(m->files[i].f);
        if (m->files[i].sub) fclose(m->files[i].sub);
    }
    for (i = 0; i < m->ntracks; i++) for (k = 0; k < IMG_TEXT_FIELDS; k++) free(m->tracks[i].text[k]);
    for (k = 0; k < IMG_TEXT_FIELDS; k++) free(m->album[k]);
    free(m->cdtext);
    free(m);
}

/* ---- CUE parsing ---------------------------------------------------------------- */

static const char* skip_ws(const char* p) { while (*p == ' ' || *p == '\t') p++; return p; }

/* Next word or "quoted string" into out; returns the rest of the line. */
static const char* next_token(const char* p, char* out, size_t cap)
{
    size_t n = 0;
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (n + 1 < cap) out[n++] = *p; p++; }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') { if (n + 1 < cap) out[n++] = *p; p++; }
    }
    out[n] = 0;
    return p;
}

static int word_is(const char* a, const char* b)
{
    size_t n = strlen(b);
    return strlen(a) == n && fa_strncasecmp_ascii(a, b, n) == 0;
}

static int64_t parse_msf(const char* s)
{
    int mm = 0, ss = 0, ff = 0;
    if (sscanf(s, "%d:%d:%d", &mm, &ss, &ff) != 3 || mm < 0 || ss < 0 || ss > 59 || ff < 0 || ff > 74) return -1;
    return ((int64_t)mm * 60 + ss) * 75 + ff;
}

static char* dup_str(const char* s)
{
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* WAVE files: the data chunk must be 44.1 kHz 16-bit stereo PCM. */
static fa_result open_wave(img_file* f)
{
    uint8_t h[12], ck[8], fmt[16];
    int64_t pos = 12;
    int fmt_ok = 0;
    if (fread(h, 1, 12, f->f) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
        return FA_ERROR_UNSUPPORTED_FORMAT;
    for (;;) {
        uint32_t len;
        if (file_seek(f->f, pos) != 0 || fread(ck, 1, 8, f->f) != 8) return FA_ERROR_UNSUPPORTED_FORMAT;
        len = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) | ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0 && len >= 16) {
            if (fread(fmt, 1, 16, f->f) != 16) return FA_ERROR_UNSUPPORTED_FORMAT;
            fmt_ok = fmt[0] == 1 && fmt[1] == 0 && fmt[2] == 2 && fmt[3] == 0 &&
                     ((uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16)) == 44100 && fmt[14] == 16;
        } else if (memcmp(ck, "data", 4) == 0) {
            if (!fmt_ok) return FA_ERROR_UNSUPPORTED_FORMAT;
            f->data_offset = pos + 8;
            f->sectors = (int64_t)len / FA_CD_SECTOR_BYTES;
            return FA_OK;
        }
        pos += 8 + (int64_t)len + (len & 1);
    }
}

static fa_result open_file(image* m, const char* cue_path, const char* name, const char* type)
{
    char path[1024];
    const char* slash = strrchr(cue_path, '/');
    img_file* f;
    int64_t size;
#ifdef _WIN32
    const char* bs = strrchr(cue_path, '\\');
    if (!slash || (bs && bs > slash)) slash = bs;
#endif
    if (m->nfiles >= IMG_MAX_FILES) return FA_ERROR_UNSUPPORTED_FORMAT;
    if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':') || !slash)
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%.*s%s", (int)(slash - cue_path + 1), cue_path, name);
    f = &m->files[m->nfiles];
    memset(f, 0, sizeof(*f));
    f->f = fa_fopen_utf8(path, "rb");
    if (!f->f) return FA_ERROR_NOT_FOUND;
    m->nfiles++;
    f->sector_bytes = FA_CD_SECTOR_BYTES;
    if (word_is(type, "WAVE")) {
        fa_result r = open_wave(f);
        if (r != FA_OK) return r;
    } else if (word_is(type, "BINARY") || word_is(type, "MOTOROLA")) {
        f->swap = word_is(type, "MOTOROLA");
        size = file_size(f->f);
        if (size < 0) return FA_ERROR_IO;
        f->bytes = size;
        f->sectors = size / FA_CD_SECTOR_BYTES;
    } else {
        return FA_ERROR_UNSUPPORTED_FORMAT;   /* MP3, AIFF, FLAC ... images */
    }
    {
        /* Subcode beside the audio, as rippers that keep CD+G write it. */
        char* dot = strrchr(path, '.');
        char* sep = strrchr(path, '/');
#ifdef _WIN32
        char* bsep = strrchr(path, '\\');
        if (!sep || (bsep && bsep > sep)) sep = bsep;
#endif
        if (dot && (!sep || dot > sep) && (size_t)(dot - path) + 5 <= sizeof(path)) {
            memcpy(dot, ".sub", 5);
            f->sub = fa_fopen_utf8(path, "rb");
            if (!f->sub) { memcpy(dot, ".SUB", 5); f->sub = fa_fopen_utf8(path, "rb"); }
            if (f->sub) m->has_sub = 1;
        }
    }
    return FA_OK;
}

typedef struct cue_state {
    int file;                  /* current file, -1 = none */
    int64_t file_done;         /* sectors of the current file already laid out */
    int64_t lba;               /* next free disc sector */
    int session;
    int new_session;           /* the next track starts a later session */
    int64_t pregap;            /* PREGAP of the current track, not laid out yet */
    int track_has_index;       /* the current track has an INDEX yet */
} cue_state;

static fa_result add_seg(image* m, int64_t count, int file, int64_t file_sector, int audio)
{
    img_seg* s;
    if (count <= 0) return FA_OK;
    if (m->nsegs >= IMG_MAX_SEGS) return FA_ERROR_UNSUPPORTED_FORMAT;
    s = &m->segs[m->nsegs++];
    s->count = count;
    s->file = file;
    s->file_sector = file_sector;
    s->audio = audio;
    s->lba = 0;   /* set by the caller */
    return FA_OK;
}

/* Lays out the current file up to its sector `upto`, then `gap` sectors of nothing. */
static fa_result lay_out(image* m, cue_state* st, int64_t upto, int64_t gap, int audio)
{
    fa_result r;
    if (st->file >= 0 && upto > st->file_done) {
        r = add_seg(m, upto - st->file_done, st->file, st->file_done, audio);
        if (r != FA_OK) return r;
        m->segs[m->nsegs - 1].lba = st->lba;
        st->lba += upto - st->file_done;
        st->file_done = upto;
    }
    if (gap > 0) {
        r = add_seg(m, gap, -1, 0, audio);
        if (r != FA_OK) return r;
        m->segs[m->nsegs - 1].lba = st->lba;
        st->lba += gap;
    }
    return FA_OK;
}

static int text_field(const char* word)
{
    static const char* const names[] = { "TITLE", "PERFORMER", "SONGWRITER", "COMPOSER", "ARRANGER", "MESSAGE" };
    int i;
    for (i = 0; i < 6; i++) if (word_is(word, names[i])) return i;
    return -1;
}

static fa_result parse_cue(image* m, const char* cue_path, char* text)
{
    cue_state st;
    char* line = text;
    img_track* cur = NULL;
    fa_result r;
    int cur_audio = 1;
    memset(&st, 0, sizeof(st));
    st.file = -1;
    st.session = 1;
    m->sessions = 1;
    /* Skip a UTF-8 byte order mark. */
    if ((uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) line += 3;
    while (line && *line) {
        char* end = line + strcspn(line, "\r\n");
        char word[64], arg[512], arg2[64];
        const char* p;
        char saved = *end;
        int field;
        *end = 0;
        p = next_token(line, word, sizeof(word));
        if (word_is(word, "FILE")) {
            p = next_token(p, arg, sizeof(arg));
            next_token(p, arg2, sizeof(arg2));
            if (st.file >= 0) {
                r = lay_out(m, &st, m->files[st.file].sectors, 0, cur_audio);
                if (r != FA_OK) return r;
            }
            r = open_file(m, cue_path, arg, arg2[0] ? arg2 : "BINARY");
            if (r != FA_OK) return r;
            st.file = m->nfiles - 1;
            st.file_done = 0;
        } else if (word_is(word, "TRACK")) {
            int num = 0;
            p = next_token(p, arg, sizeof(arg));
            next_token(p, arg2, sizeof(arg2));
            num = atoi(arg);
            if (st.file < 0 || num < 1 || num > 99 || m->ntracks >= FA_DISC_MAX_TRACKS ||
                (m->ntracks > 0 && num != m->tracks[m->ntracks - 1].number + 1))
                return FA_ERROR_UNSUPPORTED_FORMAT;
            cur = &m->tracks[m->ntracks++];
            memset(cur, 0, sizeof(*cur));
            cur->number = num;
            cur->audio = word_is(arg2, "AUDIO");
            cur->lba = -1;
            if (st.new_session) { st.session++; st.new_session = 0; }
            cur->session = st.session;
            if (cur->session > m->sessions) m->sessions = cur->session;
            cur_audio = cur->audio;
            if (word_is(arg2, "MODE1/2048") && m->files[st.file].bytes > 0) {
                img_file* f = &m->files[st.file];   /* a data file of 2048-byte sectors */
                f->sector_bytes = 2048;
                f->sectors = f->bytes / 2048;
            }
            st.pregap = 0;
            st.track_has_index = 0;
        } else if (word_is(word, "INDEX") && cur) {
            int idx;
            int64_t at;
            p = next_token(p, arg, sizeof(arg));
            next_token(p, arg2, sizeof(arg2));
            idx = atoi(arg);
            at = parse_msf(arg2);
            if (at < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
            if (!st.track_has_index && cur->session > 1 &&
                (m->ntracks < 2 || m->tracks[m->ntracks - 2].session != cur->session)) {
                /* The first index of a later session: the gap between sessions comes first. */
                r = lay_out(m, &st, at, FA_CD_SESSION_GAP, cur_audio);
                if (r != FA_OK) return r;
            }
            st.track_has_index = 1;
            if (idx == 1) {
                r = lay_out(m, &st, at, st.pregap, cur_audio);
                if (r != FA_OK) return r;
                st.pregap = 0;
                cur->lba = st.lba;
            }
        } else if (word_is(word, "PREGAP") && cur) {
            next_token(p, arg, sizeof(arg));
            st.pregap = parse_msf(arg);
            if (st.pregap < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
        } else if (word_is(word, "FLAGS") && cur) {
            for (;;) {
                p = next_token(p, arg, sizeof(arg));
                if (!arg[0]) break;
                if (word_is(arg, "PRE")) cur->pre = 1;
                else if (word_is(arg, "DCP")) cur->dcp = 1;
                else if (word_is(arg, "4CH")) cur->four = 1;
            }
        } else if (word_is(word, "ISRC") && cur) {
            next_token(p, cur->isrc, sizeof(cur->isrc));
        } else if (word_is(word, "CATALOG")) {
            next_token(p, m->catalog, sizeof(m->catalog));
        } else if ((field = text_field(word)) >= 0) {
            char** slot;
            next_token(p, arg, sizeof(arg));
            slot = cur ? &cur->text[field] : &m->album[field];
            free(*slot);
            *slot = dup_str(arg);
        } else if (word_is(word, "REM")) {
            p = next_token(p, arg2, sizeof(arg2));
            if (word_is(arg2, "SESSION")) {
                next_token(p, arg, sizeof(arg));
                if (atoi(arg) > st.session) st.new_session = 1;
            } else if (word_is(arg2, "GENRE") && !cur) {
                next_token(p, arg, sizeof(arg));
                free(m->album[6]);
                m->album[6] = dup_str(arg);
            }
        }
        *end = saved;
        line = *end ? end + 1 : NULL;
        if (line && saved == '\r' && *line == '\n') line++;
    }
    if (st.file >= 0) {
        r = lay_out(m, &st, m->files[st.file].sectors, 0, cur_audio);
        if (r != FA_OK) return r;
    }
    if (m->ntracks == 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    {
        int i;
        for (i = 0; i < m->ntracks; i++) if (m->tracks[i].lba < 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    /* Segments only know which track type they belong to by position: mark data. */
    {
        int s, t;
        for (s = 0; s < m->nsegs; s++) {
            int audio = 1;
            for (t = 0; t < m->ntracks; t++) if (m->tracks[t].lba <= m->segs[s].lba) audio = m->tracks[t].audio;
            m->segs[s].audio = audio;
        }
    }
    m->leadout = st.lba;
    return m->leadout > m->tracks[m->ntracks - 1].lba ? FA_OK : FA_ERROR_UNSUPPORTED_FORMAT;
}

/* ---- CD-Text packs ------------------------------------------------------------- */

/* UTF-8 to ISO 8859-1 ('?' for what it cannot hold). */
static size_t to_latin1(const char* s, uint8_t* out, size_t cap)
{
    size_t n = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p && n < cap) {
        uint32_t c = *p++;
        if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
        else if (c >= 0x80) {
            while ((*p & 0xC0) == 0x80) p++;
            c = '?';
        }
        out[n++] = (uint8_t)(c > 0xFF ? '?' : c);
    }
    return n;
}

typedef struct pack_writer {
    uint8_t* buf;
    uint32_t n, cap;
    uint32_t counts[16];
} pack_writer;

static void put_pack(pack_writer* w, int type, int track, int charpos, const uint8_t* text12)
{
    uint8_t* p;
    uint16_t crc;
    if (w->n >= w->cap || (type != 0x8F && w->n >= w->cap - 3)) return;   /* keep room for the size packs */
    p = w->buf + (size_t)w->n * 18;
    p[0] = (uint8_t)type;
    p[1] = (uint8_t)track;
    p[2] = (uint8_t)w->n;
    p[3] = (uint8_t)(charpos > 15 ? 15 : charpos);   /* block 0, single-byte */
    memcpy(p + 4, text12, 12);
    crc = fa_cdtext_crc(p);
    p[16] = (uint8_t)(crc >> 8);
    p[17] = (uint8_t)crc;
    w->counts[type & 0x0F]++;
    w->n++;
}

/* One pack type: the album's string, then each track's, NUL-terminated, cut into
 * 12-byte packs. The genre pack (album only) starts with a 2-byte genre code
 * (0 = unused). Each byte remembers its track and position for the pack header. */
static void put_type(pack_writer* w, image* m, int type, int field)
{
    uint8_t stream[8192], owner[8192], charpos[8192];
    size_t len = 0, pos;
    int t, any = m->album[field] != NULL, last = field < 6 ? m->ntracks : 0;
    for (t = 0; t < last; t++) any |= m->tracks[t].text[field] != NULL;
    if (!any) return;
    for (t = -1; t < last; t++) {
        const char* s = t < 0 ? m->album[field] : m->tracks[t].text[field];
        size_t start, i;
        uint8_t track = (uint8_t)(t < 0 ? 0 : m->tracks[t].number);
        if (len + 600 > sizeof(stream)) break;
        if (field == 6) { stream[len] = 0; stream[len + 1] = 0; owner[len] = owner[len + 1] = 0; charpos[len] = charpos[len + 1] = 0; len += 2; }
        start = len;
        len += to_latin1(s ? s : "", stream + len, 500);
        stream[len++] = 0;
        for (i = start; i < len; i++) { owner[i] = track; charpos[i] = (uint8_t)(i - start > 15 ? 15 : i - start); }
    }
    for (pos = 0; pos < len; pos += 12) {
        uint8_t chunk[12];
        size_t k = len - pos < 12 ? len - pos : 12;
        memset(chunk, 0, sizeof(chunk));
        memcpy(chunk, stream + pos, k);
        put_pack(w, type, owner[pos], charpos[pos], chunk);
    }
}

static void build_cdtext(image* m)
{
    pack_writer w;
    int f, any = 0, t;
    uint8_t info[36];
    for (f = 0; f < IMG_TEXT_FIELDS; f++) {
        any |= m->album[f] != NULL;
        for (t = 0; t < m->ntracks && f < 6; t++) any |= m->tracks[t].text[f] != NULL;
    }
    if (!any) return;
    memset(&w, 0, sizeof(w));
    w.cap = 255;
    w.buf = (uint8_t*)calloc(1, 4 + (size_t)w.cap * 18);
    if (!w.buf) return;
    w.buf += 4;
    for (f = 0; f < 6; f++) put_type(&w, m, 0x80 + f, f);
    put_type(&w, m, 0x87, 6);
    /* Size information: three packs describing the block. */
    memset(info, 0, sizeof(info));
    info[0] = 0x00;                                  /* ISO 8859-1 */
    info[1] = (uint8_t)m->tracks[0].number;
    info[2] = (uint8_t)m->tracks[m->ntracks - 1].number;
    w.counts[0x0F] = 3;
    for (f = 0; f < 16; f++) info[4 + f] = (uint8_t)w.counts[f];
    info[20] = (uint8_t)(w.n + 3 - 1);               /* last sequence number of block 0 */
    info[28] = 0x09;                                 /* English */
    w.counts[0x0F] = 0;
    for (f = 0; f < 3; f++) put_pack(&w, 0x8F, f, 0, info + 12 * f);
    w.buf -= 4;
    m->cdtext_len = 4 + w.n * 18;
    w.buf[0] = (uint8_t)((m->cdtext_len - 2) >> 8);
    w.buf[1] = (uint8_t)(m->cdtext_len - 2);
    m->cdtext = w.buf;
}

/* ---- operations ------------------------------------------------------------------ */

static fa_result img_inquiry(void* h, char vendor[16], char product[32], char revision[8])
{
    (void)h;
    snprintf(vendor, 16, "FatzAudio");
    snprintf(product, 32, "Disc image");
    snprintf(revision, 8, "1");
    return FA_OK;
}

static fa_result img_test_ready(void* h, int* out_state, int* out_changed)
{
    (void)h;
    *out_state = FA_CD_READY;
    *out_changed = 0;
    return FA_OK;
}

static void put_be32(uint8_t* p, int64_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static fa_result img_read_toc(void* h, int format, int track, uint8_t* buf, uint32_t cap, uint32_t* out_len)
{
    image* m = (image*)h;
    uint8_t resp[4 + 8 * 101];
    const uint8_t* src = resp;
    uint32_t len = 0;
    int i;
    (void)track;
    memset(resp, 0, sizeof(resp));
    if (format == 0) {
        resp[2] = (uint8_t)m->tracks[0].number;
        resp[3] = (uint8_t)m->tracks[m->ntracks - 1].number;
        len = 4;
        for (i = 0; i <= m->ntracks; i++) {
            uint8_t* e = resp + len;
            if (i < m->ntracks) {
                const img_track* t = &m->tracks[i];
                e[1] = (uint8_t)(0x10 | (t->audio ? 0 : 0x04) | (t->pre ? 0x01 : 0) | (t->dcp ? 0x02 : 0) | (t->four ? 0x08 : 0));
                e[2] = (uint8_t)t->number;
                put_be32(e + 4, t->lba);
            } else {
                e[1] = 0x10;
                e[2] = 0xAA;
                put_be32(e + 4, m->leadout);
            }
            len += 8;
        }
    } else if (format == 1) {
        const img_track* first_last = &m->tracks[0];
        for (i = 0; i < m->ntracks; i++)
            if (m->tracks[i].session == m->sessions) { first_last = &m->tracks[i]; break; }
        resp[2] = 1;
        resp[3] = (uint8_t)m->sessions;
        resp[5] = (uint8_t)(0x10 | (first_last->audio ? 0 : 0x04));
        resp[6] = (uint8_t)first_last->number;
        put_be32(resp + 8, first_last->lba);
        len = 12;
    } else if (format == 5) {
        if (!m->cdtext) return FA_ERROR_IO;   /* a drive answers ILLEGAL REQUEST: no CD-Text */
        src = m->cdtext;
        len = m->cdtext_len;
    } else {
        return FA_ERROR_IO;
    }
    if (src == resp) { resp[0] = (uint8_t)((len - 2) >> 8); resp[1] = (uint8_t)(len - 2); }
    if (len > cap) len = cap;
    memcpy(buf, src, len);
    *out_len = len;
    return FA_OK;
}

static fa_result img_read_cd(void* h, int64_t lba, uint32_t count, uint32_t areas, uint8_t* buf)
{
    image* m = (image*)h;
    uint32_t stride = FA_CD_SECTOR_BYTES + ((areas & FA_CD_AREA_C2) ? FA_CD_C2_BYTES : 0) +
                      ((areas & FA_CD_AREA_SUB) ? FA_CD_SUB_BYTES : 0);
    uint32_t sub_at = FA_CD_SECTOR_BYTES + ((areas & FA_CD_AREA_C2) ? FA_CD_C2_BYTES : 0), i;
    if ((areas & FA_CD_AREA_SUB) && !m->has_sub) return FA_ERROR_NOT_IMPLEMENTED;
    memset(buf, 0, (size_t)count * stride);
    for (i = 0; i < count; i++) {
        int64_t at = lba + i;
        uint8_t* out = buf + (size_t)i * stride;
        int s;
        if (at < 0 || at >= m->leadout) return FA_ERROR_IO;
        for (s = 0; s < m->nsegs; s++) {
            const img_seg* g = &m->segs[s];
            img_file* f;
            if (at < g->lba || at >= g->lba + g->count) continue;
            if (!g->audio) return FA_ERROR_IO;      /* a data sector read as CD-DA */
            if (g->file < 0) break;                 /* pregap silence */
            f = &m->files[g->file];
            if (file_seek(f->f, f->data_offset + (g->file_sector + (at - g->lba)) * (int64_t)f->sector_bytes) != 0 ||
                fread(out, 1, FA_CD_SECTOR_BYTES, f->f) != FA_CD_SECTOR_BYTES)
                return FA_ERROR_IO;
            if (f->swap) {
                int k;
                for (k = 0; k < FA_CD_SECTOR_BYTES; k += 2) { uint8_t t = out[k]; out[k] = out[k + 1]; out[k + 1] = t; }
            }
            /* A short or missing .sub leaves zeros: no graphics there. */
            if ((areas & FA_CD_AREA_SUB) && f->sub &&
                file_seek(f->sub, (g->file_sector + (at - g->lba)) * (int64_t)FA_CD_SUB_BYTES) == 0 &&
                fread(out + sub_at, 1, FA_CD_SUB_BYTES, f->sub) != FA_CD_SUB_BYTES)
                memset(out + sub_at, 0, FA_CD_SUB_BYTES);
            break;
        }
    }
    return FA_OK;
}

static fa_result img_read_code(void* h, int which, int track, char* out, size_t cap)
{
    image* m = (image*)h;
    int i;
    out[0] = 0;
    if (which == FA_CD_CODE_MCN) snprintf(out, cap, "%s", m->catalog);
    else for (i = 0; i < m->ntracks; i++) if (m->tracks[i].number == track) snprintf(out, cap, "%s", m->tracks[i].isrc);
    return out[0] ? FA_OK : FA_ERROR_NOT_FOUND;
}

static void img_close(void* h) { img_free((image*)h); }

static const fa_disc_ops img_ops = {
    img_inquiry, img_test_ready, img_read_toc, img_read_cd, img_read_code,
    NULL, NULL, NULL, NULL, img_close
};

fa_result fa_disc_image_open(const char* cue_path, const fa_disc_ops** out_ops, void** out_handle)
{
    FILE* f = fa_fopen_utf8(cue_path, "rb");
    image* m;
    char* text;
    int64_t size;
    fa_result r;
    if (!f) return FA_ERROR_NOT_FOUND;
    size = file_size(f);
    if (size <= 0 || size > (1 << 20) || file_seek(f, 0) != 0) { fclose(f); return FA_ERROR_UNSUPPORTED_FORMAT; }
    text = (char*)malloc((size_t)size + 1);
    m = (image*)calloc(1, sizeof(*m));
    if (!text || !m) { free(text); free(m); fclose(f); return FA_ERROR_OUT_OF_MEMORY; }
    if (fread(text, 1, (size_t)size, f) != (size_t)size) { free(text); free(m); fclose(f); return FA_ERROR_IO; }
    fclose(f);
    text[size] = 0;
    r = parse_cue(m, cue_path, text);
    free(text);
    if (r != FA_OK) { img_free(m); return r; }
    build_cdtext(m);
    *out_ops = &img_ops;
    *out_handle = m;
    return FA_OK;
}
