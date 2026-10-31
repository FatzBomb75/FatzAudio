/* Tags: the public tag object (fa_tags_*), the common fields mapped from every
 * format's own keys, Vorbis comments and FLAC pictures, and the loudness tags
 * that normalization uses: ReplayGain 2 (REPLAYGAIN_TRACK_GAIN etc., reference
 * -18 LUFS) and R128 (R128_TRACK_GAIN, Q7.8 dB relative to -23 LUFS). The format
 * readers are in tags_read.c (and demux_ogg.c, demux_asf.c); they all deliver to
 * a fa_tag_sink, so each format is parsed once for both uses. */
#include "fa_internal.h"
#include "cdg.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FA_TAGS_MAX_TEXT (16u << 20)     /* lyrics and comments longer than this are skipped */
#define FA_LOUDNESS_TEXT_MAX 512

static int ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return 0;
    }
    return *a == *b;
}

/* "-6.54 dB" -> -6.54, independent of the C locale. NAN if no number. */
static double parse_number(const char* s)
{
    double v = 0.0, scale = 1.0;
    int neg = 0, digits = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    for (; *s >= '0' && *s <= '9'; s++, digits++) v = v * 10.0 + (*s - '0');
    if (*s == '.' || *s == ',') {
        for (s++; *s >= '0' && *s <= '9'; s++, digits++) { scale /= 10.0; v += (*s - '0') * scale; }
    }
    if (!digits) return NAN;
    return neg ? -v : v;
}

int fa_tags_key_is(const char* key, const char* name) { return ieq(key, name); }

void fa_tags_handle(const char* key, const char* value, fa_tag_loudness* out)
{
    double v = parse_number(value);
    if (v != v) return;
    if (ieq(key, "REPLAYGAIN_TRACK_GAIN")) out->track_lufs = -18.0 - v;
    else if (ieq(key, "REPLAYGAIN_ALBUM_GAIN")) out->album_lufs = -18.0 - v;
    else if (ieq(key, "R128_TRACK_GAIN")) out->track_lufs = -23.0 - v / 256.0;
    else if (ieq(key, "R128_ALBUM_GAIN")) out->album_lufs = -23.0 - v / 256.0;
    else if (ieq(key, "REPLAYGAIN_TRACK_PEAK") && v > 0.0) out->track_peak_db = 20.0 * log10(v);
    else if (ieq(key, "REPLAYGAIN_ALBUM_PEAK") && v > 0.0) out->album_peak_db = 20.0 * log10(v);
}

const char* fa_tags_image_mime(const uint8_t* d, size_t n)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (n >= 4 && memcmp(d, "GIF8", 4) == 0) return "image/gif";
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) return "image/webp";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "image/bmp";
    return "";
}

/* ---- Vorbis comments and FLAC pictures ---------------------------------------------- */

void fa_tags_parse_flac_picture(const uint8_t* p, size_t n, int format, fa_tag_sink* sink)
{
    uint32_t type, ml, dl, size;
    size_t pos = 0;
    char mime[64];
    char* desc;
    if (!sink->picture || n < 32) return;
    type = fa_be32(p);
    ml = fa_be32(p + 4);
    pos = 8;
    if (ml > n - pos) return;
    snprintf(mime, sizeof(mime), "%.*s", (int)(ml < 63 ? ml : 63), (const char*)(p + pos));
    pos += ml;
    if (n - pos < 4) return;
    dl = fa_be32(p + pos);
    pos += 4;
    if (dl > n - pos) return;
    desc = (char*)malloc(dl + 1);
    if (!desc) return;
    memcpy(desc, p + pos, dl);
    desc[dl] = '\0';
    pos += dl;
    if (n - pos < 20) { free(desc); return; }
    pos += 16;                            /* width, height, depth, colours */
    size = fa_be32(p + pos);
    pos += 4;
    if (size <= n - pos && type <= 20) sink->picture(sink, format, (int)type, mime, desc, p + pos, size);
    free(desc);
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Base64 in place: returns the decoded size (stops at '=' or a bad character). */
static size_t b64_decode(uint8_t* s, size_t n)
{
    size_t i, k = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (i = 0; i < n; i++) {
        int v = b64_value((char)s[i]);
        if (v < 0) {
            if (s[i] == '\r' || s[i] == '\n' || s[i] == ' ') continue;
            break;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; s[k++] = (uint8_t)(acc >> bits); }
    }
    return k;
}

void fa_tags_parse_vorbis_comments(const uint8_t* p, size_t n, fa_tag_sink* sink)
{
    size_t pos = 0;
    uint32_t count, i;
    if (n < 8) return;
    pos = 4 + (size_t)fa_le32(p);            /* vendor string */
    if (pos + 4 > n || pos < 4) return;
    if (pos > 4 && pos - 4 <= sink->max_text && fa_utf8_valid(p + 4, pos - 4)) {
        char* vendor = fa_strndup((const char*)p + 4, pos - 4);
        if (vendor) sink->text(sink, FA_TAG_FORMAT_VORBIS, "VENDOR", vendor);
        free(vendor);
    }
    count = fa_le32(p + pos);
    pos += 4;
    for (i = 0; i < count && pos + 4 <= n; i++) {
        uint32_t len = fa_le32(p + pos);
        const uint8_t* item;
        size_t k;
        pos += 4;
        if (len > n - pos) return;
        item = p + pos;
        pos += len;
        for (k = 0; k < len && item[k] != '='; k++) {}
        if (k == len || k == 0 || k > 255) continue;
        if (k == 22 && memcmp(item, "METADATA_BLOCK_PICTURE", 22) == 0) {
            uint8_t* raw;
            size_t m;
            if (!sink->picture) continue;
            raw = (uint8_t*)malloc(len - k);
            if (!raw) continue;
            memcpy(raw, item + k + 1, len - k - 1);
            m = b64_decode(raw, len - k - 1);
            fa_tags_parse_flac_picture(raw, m, FA_TAG_FORMAT_VORBIS, sink);
            free(raw);
        } else if (len - k - 1 <= sink->max_text) {
            char key[256];
            char* value = (char*)malloc(len - k);
            if (!value) continue;
            memcpy(key, item, k);
            key[k] = '\0';
            memcpy(value, item + k + 1, len - k - 1);
            value[len - k - 1] = '\0';
            sink->text(sink, FA_TAG_FORMAT_VORBIS, key, value);
            free(value);
        }
    }
}

/* ---- loudness ----------------------------------------------------------------------- */

typedef struct loudness_sink {
    fa_tag_sink sink;
    fa_tag_loudness* out;
} loudness_sink;

/* The first value of each kind wins, in the readers' priority order (FLAC's own
 * comments before an ID3v2 tag in front of them, ID3v2 before APEv2), as for the
 * common fields. */
static void loudness_text(fa_tag_sink* sink, int format, const char* key, const char* value)
{
    fa_tag_loudness* out = ((loudness_sink*)sink)->out;
    fa_tag_loudness v;
    (void)format;
    v.track_lufs = v.album_lufs = v.track_peak_db = v.album_peak_db = NAN;
    fa_tags_handle(key, value, &v);
    if (isnan(out->track_lufs)) out->track_lufs = v.track_lufs;
    if (isnan(out->album_lufs)) out->album_lufs = v.album_lufs;
    if (isnan(out->track_peak_db)) out->track_peak_db = v.track_peak_db;
    if (isnan(out->album_peak_db)) out->album_peak_db = v.album_peak_db;
}

void fa_tags_read_loudness(fa_source* src, fa_tag_loudness* out)
{
    loudness_sink ls;
    out->track_lufs = out->album_lufs = NAN;
    out->track_peak_db = out->album_peak_db = NAN;
    memset(&ls, 0, sizeof(ls));
    ls.sink.text = loudness_text;
    ls.sink.max_text = FA_LOUDNESS_TEXT_MAX;
    ls.out = out;
    fa_tags_collect(src, &ls.sink);
}

/* ---- the tag object ------------------------------------------------------------------ */

typedef struct tag_item_store {
    char* key;
    char* value;
    int32_t format;
} tag_item_store;

typedef struct tag_picture_store {
    int32_t type, format;
    char* mime;
    char* description;
    uint8_t* data;
    size_t size;
} tag_picture_store;

struct fa_tags {
    fa_tag_sink sink;                /* first: the readers' callbacks get the object */
    tag_item_store* items;
    uint32_t nitems, cap_items;
    tag_picture_store* pics;
    uint32_t npics, cap_pics;
    char* fields[FA_TAG_FIELD_COUNT];
    int field_format[FA_TAG_FIELD_COUNT];
    char* field_key[FA_TAG_FIELD_COUNT];
    size_t field_len[FA_TAG_FIELD_COUNT], field_cap[FA_TAG_FIELD_COUNT];
    int have_info;
    fa_stream_info info;
};

/* The ID3v1 genre list with Winamp's extensions (0..191). */
static const char* const id3_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk",
    "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes",
    "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
    "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass",
    "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic",
    "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove",
    "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle",
    "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore Techno",
    "Terror", "Indie", "BritPop", "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "J-Pop", "Synthpop", "Abstract", "Art Rock",
    "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM", "Eclectic", "Electro",
    "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient", "Industro-Goth", "Jam Band", "Krautrock",
    "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz", "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock",
    "Trop Rock", "World Music", "Neoclassical", "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep",
    "Garage Rock", "Psybient"
};
#define ID3_GENRES ((int)(sizeof(id3_genres) / sizeof(id3_genres[0])))

static int all_digits(const char* s, size_t n)
{
    size_t i;
    if (n == 0) return 0;
    for (i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* "13", "(13)", "(13)Rock", "(RX)", "(CR)" as a genre name, written to `out`. */
static void genre_name(const char* v, int index_offset, char* out, size_t cap)
{
    size_t n = strlen(v);
    if (v[0] == '(') {
        const char* close = strchr(v, ')');
        if (close && close[1]) { snprintf(out, cap, "%s", close + 1); return; }
        if (close && all_digits(v + 1, (size_t)(close - v - 1))) {
            int g = atoi(v + 1);
            snprintf(out, cap, "%s", g >= 0 && g < ID3_GENRES ? id3_genres[g] : v);
            return;
        }
        if (strcmp(v, "(RX)") == 0) { snprintf(out, cap, "Remix"); return; }
        if (strcmp(v, "(CR)") == 0) { snprintf(out, cap, "Cover"); return; }
    } else if (all_digits(v, n) && n < 4) {
        int g = atoi(v) - index_offset;
        if (g >= 0 && g < ID3_GENRES) { snprintf(out, cap, "%s", id3_genres[g]); return; }
    }
    snprintf(out, cap, "%s", v);
}

enum { MAP_SPLIT = 0x100, MAP_GENRE = 0x200, MAP_GENRE_PLUS1 = 0x400, MAP_PLUS1 = 0x800, MAP_EXACT = 0x1000 };

typedef struct field_map {
    int format;
    const char* key;
    int field;                       /* fa_tag_field | MAP_* */
} field_map;

static const field_map field_maps[] = {
    { FA_TAG_FORMAT_ID3V2, "TIT2", FA_TAG_TITLE }, { FA_TAG_FORMAT_ID3V2, "TPE1", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_ID3V2, "TALB", FA_TAG_ALBUM }, { FA_TAG_FORMAT_ID3V2, "TPE2", FA_TAG_ALBUM_ARTIST },
    { FA_TAG_FORMAT_ID3V2, "TCOM", FA_TAG_COMPOSER }, { FA_TAG_FORMAT_ID3V2, "TCON", FA_TAG_GENRE | MAP_GENRE },
    { FA_TAG_FORMAT_ID3V2, "TDRC", FA_TAG_DATE }, { FA_TAG_FORMAT_ID3V2, "TYER", FA_TAG_DATE },
    { FA_TAG_FORMAT_ID3V2, "TRCK", FA_TAG_TRACK | MAP_SPLIT }, { FA_TAG_FORMAT_ID3V2, "TPOS", FA_TAG_DISC | MAP_SPLIT },
    { FA_TAG_FORMAT_ID3V2, "COMM", FA_TAG_COMMENT }, { FA_TAG_FORMAT_ID3V2, "USLT", FA_TAG_LYRICS },
    { FA_TAG_FORMAT_ID3V2, "TCOP", FA_TAG_COPYRIGHT }, { FA_TAG_FORMAT_ID3V2, "TSSE", FA_TAG_ENCODER },
    { FA_TAG_FORMAT_ID3V2, "TBPM", FA_TAG_BPM },

    { FA_TAG_FORMAT_ID3V1, "TITLE", FA_TAG_TITLE }, { FA_TAG_FORMAT_ID3V1, "ARTIST", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_ID3V1, "ALBUM", FA_TAG_ALBUM }, { FA_TAG_FORMAT_ID3V1, "YEAR", FA_TAG_DATE },
    { FA_TAG_FORMAT_ID3V1, "COMMENT", FA_TAG_COMMENT }, { FA_TAG_FORMAT_ID3V1, "TRACK", FA_TAG_TRACK },
    { FA_TAG_FORMAT_ID3V1, "GENRE", FA_TAG_GENRE | MAP_GENRE },

    { FA_TAG_FORMAT_APE, "Title", FA_TAG_TITLE }, { FA_TAG_FORMAT_APE, "Artist", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_APE, "Album", FA_TAG_ALBUM }, { FA_TAG_FORMAT_APE, "Album Artist", FA_TAG_ALBUM_ARTIST },
    { FA_TAG_FORMAT_APE, "AlbumArtist", FA_TAG_ALBUM_ARTIST }, { FA_TAG_FORMAT_APE, "Composer", FA_TAG_COMPOSER },
    { FA_TAG_FORMAT_APE, "Genre", FA_TAG_GENRE }, { FA_TAG_FORMAT_APE, "Year", FA_TAG_DATE },
    { FA_TAG_FORMAT_APE, "Track", FA_TAG_TRACK | MAP_SPLIT }, { FA_TAG_FORMAT_APE, "Disc", FA_TAG_DISC | MAP_SPLIT },
    { FA_TAG_FORMAT_APE, "Comment", FA_TAG_COMMENT }, { FA_TAG_FORMAT_APE, "Lyrics", FA_TAG_LYRICS },
    { FA_TAG_FORMAT_APE, "Copyright", FA_TAG_COPYRIGHT }, { FA_TAG_FORMAT_APE, "Encoder", FA_TAG_ENCODER },
    { FA_TAG_FORMAT_APE, "BPM", FA_TAG_BPM },

    { FA_TAG_FORMAT_VORBIS, "TITLE", FA_TAG_TITLE }, { FA_TAG_FORMAT_VORBIS, "ARTIST", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_VORBIS, "ALBUM", FA_TAG_ALBUM }, { FA_TAG_FORMAT_VORBIS, "ALBUMARTIST", FA_TAG_ALBUM_ARTIST },
    { FA_TAG_FORMAT_VORBIS, "ALBUM ARTIST", FA_TAG_ALBUM_ARTIST }, { FA_TAG_FORMAT_VORBIS, "COMPOSER", FA_TAG_COMPOSER },
    { FA_TAG_FORMAT_VORBIS, "GENRE", FA_TAG_GENRE }, { FA_TAG_FORMAT_VORBIS, "DATE", FA_TAG_DATE },
    { FA_TAG_FORMAT_VORBIS, "YEAR", FA_TAG_DATE }, { FA_TAG_FORMAT_VORBIS, "TRACKNUMBER", FA_TAG_TRACK | MAP_SPLIT },
    { FA_TAG_FORMAT_VORBIS, "TRACKTOTAL", FA_TAG_TRACK_TOTAL }, { FA_TAG_FORMAT_VORBIS, "TOTALTRACKS", FA_TAG_TRACK_TOTAL },
    { FA_TAG_FORMAT_VORBIS, "DISCNUMBER", FA_TAG_DISC | MAP_SPLIT }, { FA_TAG_FORMAT_VORBIS, "DISCTOTAL", FA_TAG_DISC_TOTAL },
    { FA_TAG_FORMAT_VORBIS, "TOTALDISCS", FA_TAG_DISC_TOTAL }, { FA_TAG_FORMAT_VORBIS, "COMMENT", FA_TAG_COMMENT },
    { FA_TAG_FORMAT_VORBIS, "DESCRIPTION", FA_TAG_COMMENT }, { FA_TAG_FORMAT_VORBIS, "LYRICS", FA_TAG_LYRICS },
    { FA_TAG_FORMAT_VORBIS, "UNSYNCEDLYRICS", FA_TAG_LYRICS }, { FA_TAG_FORMAT_VORBIS, "COPYRIGHT", FA_TAG_COPYRIGHT },
    { FA_TAG_FORMAT_VORBIS, "ENCODER", FA_TAG_ENCODER }, { FA_TAG_FORMAT_VORBIS, "BPM", FA_TAG_BPM },

    { FA_TAG_FORMAT_MP4, "\xC2\xA9nam", FA_TAG_TITLE | MAP_EXACT }, { FA_TAG_FORMAT_MP4, "\xC2\xA9" "ART", FA_TAG_ARTIST | MAP_EXACT },
    { FA_TAG_FORMAT_MP4, "\xC2\xA9" "alb", FA_TAG_ALBUM | MAP_EXACT }, { FA_TAG_FORMAT_MP4, "aART", FA_TAG_ALBUM_ARTIST | MAP_EXACT },
    { FA_TAG_FORMAT_MP4, "\xC2\xA9wrt", FA_TAG_COMPOSER | MAP_EXACT }, { FA_TAG_FORMAT_MP4, "\xC2\xA9gen", FA_TAG_GENRE | MAP_EXACT },
    { FA_TAG_FORMAT_MP4, "gnre", FA_TAG_GENRE | MAP_GENRE_PLUS1 | MAP_EXACT }, { FA_TAG_FORMAT_MP4, "\xC2\xA9" "day", FA_TAG_DATE | MAP_EXACT },
    { FA_TAG_FORMAT_MP4, "trkn", FA_TAG_TRACK | MAP_SPLIT | MAP_EXACT }, { FA_TAG_FORMAT_MP4, "disk", FA_TAG_DISC | MAP_SPLIT | MAP_EXACT },
    { FA_TAG_FORMAT_MP4, "\xC2\xA9" "cmt", FA_TAG_COMMENT | MAP_EXACT }, { FA_TAG_FORMAT_MP4, "\xC2\xA9lyr", FA_TAG_LYRICS | MAP_EXACT },
    { FA_TAG_FORMAT_MP4, "cprt", FA_TAG_COPYRIGHT | MAP_EXACT }, { FA_TAG_FORMAT_MP4, "\xC2\xA9too", FA_TAG_ENCODER | MAP_EXACT },
    { FA_TAG_FORMAT_MP4, "tmpo", FA_TAG_BPM | MAP_EXACT },

    { FA_TAG_FORMAT_ASF, "Title", FA_TAG_TITLE }, { FA_TAG_FORMAT_ASF, "Author", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_ASF, "WM/AlbumTitle", FA_TAG_ALBUM }, { FA_TAG_FORMAT_ASF, "WM/AlbumArtist", FA_TAG_ALBUM_ARTIST },
    { FA_TAG_FORMAT_ASF, "WM/Composer", FA_TAG_COMPOSER }, { FA_TAG_FORMAT_ASF, "WM/Genre", FA_TAG_GENRE },
    { FA_TAG_FORMAT_ASF, "WM/Year", FA_TAG_DATE }, { FA_TAG_FORMAT_ASF, "WM/TrackNumber", FA_TAG_TRACK | MAP_SPLIT },
    { FA_TAG_FORMAT_ASF, "WM/Track", FA_TAG_TRACK | MAP_PLUS1 }, { FA_TAG_FORMAT_ASF, "WM/PartOfSet", FA_TAG_DISC | MAP_SPLIT },
    { FA_TAG_FORMAT_ASF, "Description", FA_TAG_COMMENT }, { FA_TAG_FORMAT_ASF, "WM/Lyrics", FA_TAG_LYRICS },
    { FA_TAG_FORMAT_ASF, "Copyright", FA_TAG_COPYRIGHT }, { FA_TAG_FORMAT_ASF, "WM/ToolName", FA_TAG_ENCODER },
    { FA_TAG_FORMAT_ASF, "WM/BeatsPerMinute", FA_TAG_BPM },

    { FA_TAG_FORMAT_RIFF, "INAM", FA_TAG_TITLE }, { FA_TAG_FORMAT_RIFF, "IART", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_RIFF, "IPRD", FA_TAG_ALBUM }, { FA_TAG_FORMAT_RIFF, "IMUS", FA_TAG_COMPOSER },
    { FA_TAG_FORMAT_RIFF, "IGNR", FA_TAG_GENRE }, { FA_TAG_FORMAT_RIFF, "ICRD", FA_TAG_DATE },
    { FA_TAG_FORMAT_RIFF, "ITRK", FA_TAG_TRACK | MAP_SPLIT }, { FA_TAG_FORMAT_RIFF, "IPRT", FA_TAG_TRACK | MAP_SPLIT },
    { FA_TAG_FORMAT_RIFF, "ICMT", FA_TAG_COMMENT }, { FA_TAG_FORMAT_RIFF, "ICOP", FA_TAG_COPYRIGHT },
    { FA_TAG_FORMAT_RIFF, "ISFT", FA_TAG_ENCODER },

    { FA_TAG_FORMAT_AIFF, "NAME", FA_TAG_TITLE }, { FA_TAG_FORMAT_AIFF, "AUTH", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_AIFF, "(c) ", FA_TAG_COPYRIGHT }, { FA_TAG_FORMAT_AIFF, "ANNO", FA_TAG_COMMENT },

    { FA_TAG_FORMAT_MATROSKA, "TITLE", FA_TAG_TITLE }, { FA_TAG_FORMAT_MATROSKA, "ARTIST", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_MATROSKA, "ALBUM/TITLE", FA_TAG_ALBUM }, { FA_TAG_FORMAT_MATROSKA, "ALBUM", FA_TAG_ALBUM },
    { FA_TAG_FORMAT_MATROSKA, "ALBUM/ARTIST", FA_TAG_ALBUM_ARTIST }, { FA_TAG_FORMAT_MATROSKA, "ALBUM_ARTIST", FA_TAG_ALBUM_ARTIST },
    { FA_TAG_FORMAT_MATROSKA, "COMPOSER", FA_TAG_COMPOSER }, { FA_TAG_FORMAT_MATROSKA, "GENRE", FA_TAG_GENRE },
    { FA_TAG_FORMAT_MATROSKA, "ALBUM/GENRE", FA_TAG_GENRE }, { FA_TAG_FORMAT_MATROSKA, "DATE_RELEASED", FA_TAG_DATE },
    { FA_TAG_FORMAT_MATROSKA, "DATE_RECORDED", FA_TAG_DATE }, { FA_TAG_FORMAT_MATROSKA, "ALBUM/DATE_RELEASED", FA_TAG_DATE },
    { FA_TAG_FORMAT_MATROSKA, "DATE", FA_TAG_DATE }, { FA_TAG_FORMAT_MATROSKA, "PART_NUMBER", FA_TAG_TRACK | MAP_SPLIT },
    { FA_TAG_FORMAT_MATROSKA, "TRACK", FA_TAG_TRACK | MAP_SPLIT }, { FA_TAG_FORMAT_MATROSKA, "ALBUM/TOTAL_PARTS", FA_TAG_TRACK_TOTAL },
    { FA_TAG_FORMAT_MATROSKA, "COMMENT", FA_TAG_COMMENT }, { FA_TAG_FORMAT_MATROSKA, "DESCRIPTION", FA_TAG_COMMENT },
    { FA_TAG_FORMAT_MATROSKA, "LYRICS", FA_TAG_LYRICS }, { FA_TAG_FORMAT_MATROSKA, "COPYRIGHT", FA_TAG_COPYRIGHT },
    { FA_TAG_FORMAT_MATROSKA, "ENCODER", FA_TAG_ENCODER }, { FA_TAG_FORMAT_MATROSKA, "BPM", FA_TAG_BPM },
    { FA_TAG_FORMAT_MATROSKA, "INFO/TITLE", FA_TAG_TITLE },

    { FA_TAG_FORMAT_MIDI, "TITLE", FA_TAG_TITLE }, { FA_TAG_FORMAT_MIDI, "ARTIST", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_MIDI, "COPYRIGHT", FA_TAG_COPYRIGHT },
    { FA_TAG_FORMAT_TRACKER, "TITLE", FA_TAG_TITLE }, { FA_TAG_FORMAT_TRACKER, "ARTIST", FA_TAG_ARTIST },
    { FA_TAG_FORMAT_TRACKER, "COMMENT", FA_TAG_COMMENT },

    { FA_TAG_FORMAT_LYRICS3, "LYR", FA_TAG_LYRICS }, { FA_TAG_FORMAT_LYRICS3, "ETT", FA_TAG_TITLE },
    { FA_TAG_FORMAT_LYRICS3, "EAR", FA_TAG_ARTIST }, { FA_TAG_FORMAT_LYRICS3, "EAL", FA_TAG_ALBUM },
    { FA_TAG_FORMAT_LYRICS3, "INF", FA_TAG_COMMENT }
};

/* Limits for hostile tags: items and pictures kept, and the length of a field
 * joined from repeated keys. Real files stay far below all three. */
#define MAX_TAG_ITEMS 65536u
#define MAX_JOINED 65536u
#define MAX_TAG_PICTURES 1024u

/* The first format to give a field owns it; more values of the same key from that
 * format (repeated Vorbis comments) are joined with "; ", up to MAX_JOINED bytes. */
static void set_field(fa_tags* t, int field, int format, const char* key, const char* value)
{
    if (!value[0]) return;
    if (!t->fields[field]) {
        t->fields[field] = fa_strdup(value);
        t->field_key[field] = fa_strdup(key);
        t->field_format[field] = format;
        t->field_len[field] = t->field_cap[field] = t->fields[field] ? strlen(value) : 0;
    } else if (t->field_format[field] == format && t->field_key[field] && ieq(t->field_key[field], key) &&
               strcmp(t->fields[field], value) != 0) {
        size_t a = t->field_len[field], b = strlen(value);
        if (b > MAX_JOINED || a + 2 + b > MAX_JOINED) return;
        if (a + 2 + b > t->field_cap[field]) {
            /* Grown in steps, so many joins do not copy the field each time. */
            size_t cap = 2 * (a + 2 + b);
            char* j;
            if (cap > MAX_JOINED) cap = MAX_JOINED;
            j = (char*)realloc(t->fields[field], cap + 1);
            if (!j) return;
            t->fields[field] = j;
            t->field_cap[field] = cap;
        }
        memcpy(t->fields[field] + a, "; ", 2);
        memcpy(t->fields[field] + a + 2, value, b + 1);
        t->field_len[field] = a + 2 + b;
    }
}

static void map_item(fa_tags* t, int format, const char* key, const char* value)
{
    size_t i;
    for (i = 0; i < sizeof(field_maps) / sizeof(field_maps[0]); i++) {
        const field_map* m = &field_maps[i];
        int field;
        if (m->format != format) continue;
        if ((m->field & MAP_EXACT) ? strcmp(m->key, key) != 0 : !ieq(m->key, key)) continue;
        field = m->field & 0xFF;
        if (m->field & MAP_SPLIT) {
            /* "3/12": number and total. */
            const char* slash = strchr(value, '/');
            if (slash) {
                char* no = fa_strndup(value, (size_t)(slash - value));
                if (no) { set_field(t, field, format, key, no); free(no); }
                set_field(t, field + 1, format, key, slash + 1);
            } else {
                set_field(t, field, format, key, value);
            }
        } else if (m->field & (MAP_GENRE | MAP_GENRE_PLUS1)) {
            char g[256];
            genre_name(value, (m->field & MAP_GENRE_PLUS1) ? 1 : 0, g, sizeof(g));
            set_field(t, field, format, key, g);
        } else if (m->field & MAP_PLUS1) {
            char num[24];
            const char* d = value;
            if (!all_digits(value, strlen(value))) return;
            while (d[0] == '0' && d[1]) d++;              /* zero padding does not count */
            if (strlen(d) > 9) return;                    /* atoi() + 1 must not overflow */
            snprintf(num, sizeof(num), "%d", atoi(d) + 1);
            set_field(t, field, format, key, num);
        } else {
            set_field(t, field, format, key, value);
        }
        return;
    }
}

static void collect_text(fa_tag_sink* sink, int format, const char* key, const char* value)
{
    fa_tags* t = (fa_tags*)sink;
    tag_item_store* it;
    if (!value[0]) return;
    if (t->nitems >= MAX_TAG_ITEMS) {
        map_item(t, format, key, value);   /* not kept as an item, but a field may still need it */
        return;
    }
    if (t->nitems == t->cap_items) {
        uint32_t cap = t->cap_items ? t->cap_items * 2 : 16;
        tag_item_store* p = (tag_item_store*)realloc(t->items, sizeof(*p) * cap);
        if (!p) return;
        t->items = p;
        t->cap_items = cap;
    }
    it = &t->items[t->nitems];
    it->key = fa_strdup(key);
    it->value = fa_strdup(value);
    it->format = format;
    if (!it->key || !it->value) { free(it->key); free(it->value); return; }
    t->nitems++;
    map_item(t, format, key, value);
}

static void collect_picture(fa_tag_sink* sink, int format, int type, const char* mime, const char* description,
                            const uint8_t* data, size_t size)
{
    fa_tags* t = (fa_tags*)sink;
    tag_picture_store* p;
    if (size == 0 || t->npics >= MAX_TAG_PICTURES) return;
    if (t->npics == t->cap_pics) {
        uint32_t cap = t->cap_pics ? t->cap_pics * 2 : 4;
        tag_picture_store* q = (tag_picture_store*)realloc(t->pics, sizeof(*q) * cap);
        if (!q) return;
        t->pics = q;
        t->cap_pics = cap;
    }
    p = &t->pics[t->npics];
    /* A MIME type that is missing or not one ("jpg", "-->" links) is taken from the data. */
    if (!mime || !strchr(mime, '/')) mime = fa_tags_image_mime(data, size);
    if (strcmp(mime, "-->") == 0) return;
    p->type = type >= 0 && type <= 20 ? type : FA_PICTURE_OTHER;
    p->format = format;
    p->mime = fa_strdup(mime);
    p->description = fa_strdup(description ? description : "");
    p->data = (uint8_t*)malloc(size);
    p->size = size;
    if (!p->mime || !p->description || !p->data) { free(p->mime); free(p->description); free(p->data); return; }
    memcpy(p->data, data, size);
    t->npics++;
}

static fa_tags* tags_new(uint32_t flags)
{
    fa_tags* t = (fa_tags*)calloc(1, sizeof(fa_tags));
    if (!t) return NULL;
    t->sink.text = collect_text;
    t->sink.picture = (flags & FA_TAGS_NO_PICTURES) ? NULL : collect_picture;
    t->sink.max_text = FA_TAGS_MAX_TEXT;
    return t;
}

FA_API void FA_CALL fa_tags_close(fa_tags* t)
{
    uint32_t i;
    if (!t) return;
    for (i = 0; i < t->nitems; i++) { free(t->items[i].key); free(t->items[i].value); }
    for (i = 0; i < t->npics; i++) { free(t->pics[i].mime); free(t->pics[i].description); free(t->pics[i].data); }
    for (i = 0; i < FA_TAG_FIELD_COUNT; i++) { free(t->fields[i]); free(t->field_key[i]); }
    free(t->items);
    free(t->pics);
    free(t);
}

static int tags_have_format(const fa_tags* t, int format)
{
    uint32_t i;
    for (i = 0; i < t->nitems; i++) if (t->items[i].format == format) return 1;
    return 0;
}

/* Tags and (unless FA_TAGS_NO_INFO) the properties of the file in `src`. With
 * `audio_only`, a file no decoder recognises is skipped (returns 0) before any of
 * its tags are taken. */
static int read_source(fa_tags* t, fa_source* src, const char* name, uint32_t flags, int audio_only)
{
    if (audio_only || !(flags & FA_TAGS_NO_INFO)) {
        fa_decoder dec;
        fa_source_seek(src, 0);
        fa_result r = fa_decoder_open(&dec, src, name, NULL, -1, NULL);
        if (r == FA_OK) {
            if (!(flags & FA_TAGS_NO_INFO)) {
                fa_decoder_get_info(&dec, &t->info);
                t->have_info = 1;
            }
            fa_decoder_close(&dec);
        } else if (r == FA_ERROR_NO_SOUNDFONT) {
            /* A MIDI file: no soundfont to play it with, but its tags and length can be read. */
            fa_source_seek(src, 0);
            if (fa_midi_read_tags(src, (flags & FA_TAGS_NO_INFO) ? NULL : &t->info, &t->sink) == FA_OK && !(flags & FA_TAGS_NO_INFO))
                t->have_info = 1;
            fa_source_seek(src, 0);
            return 1;
        } else if (audio_only) {
            return 0;
        }
        fa_source_seek(src, 0);
    }
    fa_tags_collect(src, &t->sink);
    return 1;
}

/* A karaoke ZIP: the audio entry (the one named like the .cdg first). A stored
 * entry is read in place, through a window on `src`. */
static fa_result read_zip(fa_tags* t, fa_source* src, uint32_t flags)
{
    fa_zip_entry* e;
    uint32_t count, i, pass;
    int cdg = -1, ok;
    int64_t size = src->length;
    fa_result r = fa_zip_list(src, &e, &count);
    if (r != FA_OK) return FA_OK;        /* not readable: no tags */
    for (i = 0; i < count && cdg < 0; i++) if (fa_cdg_is_cdg_name(e[i].name)) cdg = (int)i;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < count; i++) {
            size_t stem = cdg >= 0 ? strlen(e[cdg].name) - 4 : 0;
            int preferred = cdg >= 0 && strncmp(e[i].name, e[cdg].name, stem) == 0 && e[i].name[stem] == '.';
            int64_t offset;
            if ((int)i == cdg || fa_cdg_is_cdg_name(e[i].name) || preferred != (pass == 0) || e[i].size == 0) continue;
            if (e[i].method == 0) {
                if (fa_zip_entry_offset(src, &e[i], &offset) != FA_OK ||
                    fa_source_set_window(src, offset, (int64_t)e[i].size) != FA_OK)
                    continue;
                /* Not audio (a text file in the archive): the next entry. */
                ok = read_source(t, src, e[i].name, flags, 1);
                fa_source_set_window(src, 0, size);
            } else {
                uint8_t* audio;
                fa_source mem;
                /* Not audio (a video, a PDF): told from a 64 KB prefix, not by unpacking it. */
                if (e[i].size > FA_ZIP_PROBE_BYTES) {
                    uint8_t* head = (uint8_t*)malloc(FA_ZIP_PROBE_BYTES);
                    size_t got = 0;
                    fa_result pr;
                    if (!head) continue;
                    pr = fa_zip_extract_prefix(src, &e[i], head, FA_ZIP_PROBE_BYTES, &got);
                    ok = pr != FA_OK || fa_decoder_recognizes(head, got);
                    free(head);
                    if (!ok) continue;
                }
                if (fa_zip_extract(src, &e[i], FA_ZIP_DEFAULT_MEMORY_LIMIT, NULL, &audio) != FA_OK) continue;
                fa_source_open_memory_ref(&mem, audio, (size_t)e[i].size);
                ok = read_source(t, &mem, e[i].name, flags, 1);
                fa_source_close(&mem);
                free(audio);
            }
            if (!ok) continue;
            if (t->have_info) { t->info.has_cdg = cdg >= 0; t->info.file_size = size; }
            free(e);
            return FA_OK;
        }
    }
    free(e);
    return FA_OK;
}

static fa_result read_any(fa_tags* t, fa_source* src, const char* path, uint32_t flags)
{
    const uint8_t* head;
    size_t n = fa_source_peek(src, &head);
    if (fa_zip_probe(head, n)) return read_zip(t, src, flags);
    read_source(t, src, path, flags, 0);
    if (t->have_info && path) {
        fa_cdg_data* g = fa_cdg_find_sibling(path);
        t->info.has_cdg = g != NULL;
        fa_cdg_data_free(g);
    }
    return FA_OK;
}

FA_API fa_result FA_CALL fa_tags_read_path(const char* path, uint32_t flags, fa_tags** out_tags)
{
    fa_source src;
    fa_tags* t;
    fa_result r;
    if (out_tags) *out_tags = NULL;
    if (!path || !out_tags) return FA_ERROR_INVALID_ARG;
    r = fa_source_open_path(&src, path);
    if (r != FA_OK) return r;
    t = tags_new(flags);
    if (!t) { fa_source_close(&src); return FA_ERROR_OUT_OF_MEMORY; }
    r = read_any(t, &src, path, flags);
    fa_source_close(&src);
    if (r != FA_OK) { fa_tags_close(t); return r; }
    *out_tags = t;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_tags_read_memory(const void* data, size_t size, uint32_t flags, fa_tags** out_tags)
{
    fa_source src;
    fa_tags* t;
    fa_result r;
    if (out_tags) *out_tags = NULL;
    if (!data || size == 0 || !out_tags) return FA_ERROR_INVALID_ARG;
    r = fa_source_open_memory_ref(&src, data, size);
    if (r != FA_OK) return r;
    t = tags_new(flags);
    if (!t) { fa_source_close(&src); return FA_ERROR_OUT_OF_MEMORY; }
    r = read_any(t, &src, NULL, flags);
    fa_source_close(&src);
    if (r != FA_OK) { fa_tags_close(t); return r; }
    *out_tags = t;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_stream_get_tags(fa_stream* stream, uint32_t flags, fa_tags** out_tags)
{
    fa_tags* t;
    fa_result r = FA_OK;
    if (out_tags) *out_tags = NULL;
    if (!stream || !out_tags) return FA_ERROR_INVALID_ARG;
    t = tags_new(flags);
    if (!t) return FA_ERROR_OUT_OF_MEMORY;
    /* The file is read again through a source of its own: the stream's source
     * belongs to its decode thread. Memory blocks stay fixed while the stream is open. */
    if (stream->path || stream->source.kind == FA_SOURCE_MEMORY || stream->source.shared) {
        fa_source src;
        /* A stored ZIP entry is reopened as its bytes in the archive, an unpacked
         * one (temp file) through the stream's shared file. */
        if (stream->source.shared) r = fa_source_open_shared(&src, stream->source.shared);
        else if (stream->path) r = fa_source_open_path_window(&src, stream->path, stream->path_offset, stream->path_length);
        else r = fa_source_open_memory_ref(&src, stream->source.mem, stream->source.mem_size);
        if (r == FA_OK) {
            /* A ZIP (MP3+G) gives the tags of the audio file inside it. */
            r = read_any(t, &src, stream->path_name[0] ? stream->path_name : NULL, flags | FA_TAGS_NO_INFO);
            fa_source_close(&src);
        }
    }
    if (stream->midi && !tags_have_format(t, FA_TAG_FORMAT_MIDI)) {
        fa_midi_info mi;
        if (fa_stream_get_midi_info(stream, &mi) == FA_OK) {
            collect_text(&t->sink, FA_TAG_FORMAT_MIDI, "TITLE", mi.title);
            collect_text(&t->sink, FA_TAG_FORMAT_MIDI, "ARTIST", mi.artist);
            collect_text(&t->sink, FA_TAG_FORMAT_MIDI, "COPYRIGHT", mi.copyright);
        }
    }
    if (stream->tracker) {
        fa_tracker_info ti;
        size_t n = 0;
        ti.struct_size = sizeof(ti);
        if (fa_stream_get_tracker_info(stream, &ti) == FA_OK) {
            collect_text(&t->sink, FA_TAG_FORMAT_TRACKER, "TITLE", ti.title);
            collect_text(&t->sink, FA_TAG_FORMAT_TRACKER, "ARTIST", ti.artist);
        }
        if (fa_stream_get_tracker_text(stream, FA_TRACKER_TEXT_MESSAGE, 0, NULL, 0, &n) == FA_OK && n > 0) {
            char* msg = (char*)malloc(n + 1);
            if (msg && fa_stream_get_tracker_text(stream, FA_TRACKER_TEXT_MESSAGE, 0, msg, n + 1, NULL) == FA_OK)
                collect_text(&t->sink, FA_TAG_FORMAT_TRACKER, "COMMENT", msg);
            free(msg);
        }
    }
    if (!(flags & FA_TAGS_NO_INFO) && fa_stream_get_info(stream, &t->info) == FA_OK) t->have_info = 1;
    if (r != FA_OK) { fa_tags_close(t); return r; }
    *out_tags = t;
    return FA_OK;
}

FA_API const char* FA_CALL fa_tags_get(const fa_tags* tags, fa_tag_field field)
{
    if (!tags || (int)field < 0 || field >= FA_TAG_FIELD_COUNT || !tags->fields[field]) return "";
    return tags->fields[field];
}

FA_API uint32_t FA_CALL fa_tags_get_item_count(const fa_tags* tags) { return tags ? tags->nitems : 0; }

FA_API fa_result FA_CALL fa_tags_get_item(const fa_tags* tags, uint32_t index, fa_tag_item* out_item)
{
    if (out_item) memset(out_item, 0, sizeof(*out_item));
    if (!tags || !out_item) return FA_ERROR_INVALID_ARG;
    if (index >= tags->nitems) return FA_ERROR_NOT_FOUND;
    out_item->key = tags->items[index].key;
    out_item->value = tags->items[index].value;
    out_item->format = tags->items[index].format;
    return FA_OK;
}

FA_API uint32_t FA_CALL fa_tags_get_picture_count(const fa_tags* tags) { return tags ? tags->npics : 0; }

FA_API fa_result FA_CALL fa_tags_get_picture(const fa_tags* tags, uint32_t index, fa_tag_picture* out_picture)
{
    const tag_picture_store* p;
    if (out_picture) memset(out_picture, 0, sizeof(*out_picture));
    if (!tags || !out_picture) return FA_ERROR_INVALID_ARG;
    if (index >= tags->npics) return FA_ERROR_NOT_FOUND;
    p = &tags->pics[index];
    out_picture->type = p->type;
    out_picture->mime = p->mime;
    out_picture->description = p->description;
    out_picture->data = p->data;
    out_picture->size = p->size;
    out_picture->format = p->format;
    return FA_OK;
}

FA_API fa_result FA_CALL fa_tags_get_cover(const fa_tags* tags, fa_tag_picture* out_picture)
{
    uint32_t i;
    if (out_picture) memset(out_picture, 0, sizeof(*out_picture));
    if (!tags || !out_picture) return FA_ERROR_INVALID_ARG;
    for (i = 0; i < tags->npics; i++)
        if (tags->pics[i].type == FA_PICTURE_FRONT_COVER) return fa_tags_get_picture(tags, i, out_picture);
    return fa_tags_get_picture(tags, 0, out_picture);
}

FA_API fa_result FA_CALL fa_tags_get_info(const fa_tags* tags, fa_stream_info* out_info)
{
    if (out_info) memset(out_info, 0, sizeof(*out_info));
    if (!tags || !out_info) return FA_ERROR_INVALID_ARG;
    if (!tags->have_info) return FA_ERROR_NOT_FOUND;
    *out_info = tags->info;
    return FA_OK;
}
