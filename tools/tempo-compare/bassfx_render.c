/* bassfx_render (Windows only): the same render through BASS + bass_fx, used as a
 * black box through their documented public API, so FatzAudio can be compared
 * with what the legacy app uses today. Nothing of BASS is linked or shipped: the
 * DLLs are loaded at run time from the folder given.
 *
 *   bassfx_render <folder with bass.dll and bass_fx.dll> in.wav out.wav
 *                 [--tempo 1.25] [--pitch -3] [--rate 1]
 *
 * --tempo is a multiplier (BASS_ATTRIB_TEMPO takes percent: (x - 1) * 100),
 * --pitch semitones (BASS_ATTRIB_TEMPO_PITCH), --rate a multiplier of the file's
 * sample rate (BASS_ATTRIB_TEMPO_FREQ). bass_fx's tempo options stay at their
 * defaults, as most apps leave them. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From the BASS and bass_fx documentation. */
#define BASS_SAMPLE_FLOAT 256
#define BASS_STREAM_PRESCAN 0x20000
#define BASS_STREAM_DECODE 0x200000
#define BASS_UNICODE 0x80000000
#define BASS_FX_FREESOURCE 0x10000
#define BASS_ATTRIB_TEMPO 0x10000
#define BASS_ATTRIB_TEMPO_PITCH 0x10001
#define BASS_ATTRIB_TEMPO_FREQ 0x10002
#define BASS_ATTRIB_FREQ 1

typedef struct {
    DWORD freq, chans, flags, ctype, origres;
    DWORD plugin, sample;
    const char* filename;
} BASS_CHANNELINFO;

typedef BOOL (WINAPI *init_fn)(int, DWORD, DWORD, HWND, const void*);
typedef DWORD (WINAPI *create_file_fn)(BOOL, const void*, unsigned long long, unsigned long long, DWORD);
typedef DWORD (WINAPI *tempo_create_fn)(DWORD, DWORD);
typedef BOOL (WINAPI *set_attr_fn)(DWORD, DWORD, float);
typedef DWORD (WINAPI *get_data_fn)(DWORD, void*, DWORD);
typedef BOOL (WINAPI *get_info_fn)(DWORD, BASS_CHANNELINFO*);
typedef int (WINAPI *error_fn)(void);
typedef DWORD (WINAPI *version_fn)(void);

/* GetProcAddress, through a generic function pointer (no cast warnings). */
static void (*proc(HMODULE m, const char* name))(void) { return (void (*)(void))GetProcAddress(m, name); }

static void put32(FILE* f, DWORD v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, WORD v) { fwrite(&v, 2, 1, f); }

static void wav_header(FILE* f, DWORD rate, DWORD channels, unsigned long long frames)
{
    unsigned long long data = frames * channels * 4;
    fwrite("RIFF", 1, 4, f); put32(f, (DWORD)(36 + data)); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 3); put16(f, (WORD)channels); put32(f, rate);
    put32(f, rate * channels * 4); put16(f, (WORD)(channels * 4)); put16(f, 32);
    fwrite("data", 1, 4, f); put32(f, (DWORD)data);
}

int main(int argc, char** argv)
{
    char path[MAX_PATH * 2];
    HMODULE bass, fx;
    init_fn BASS_Init;
    create_file_fn BASS_StreamCreateFile;
    tempo_create_fn BASS_FX_TempoCreate;
    set_attr_fn BASS_ChannelSetAttribute;
    get_data_fn BASS_ChannelGetData;
    get_info_fn BASS_ChannelGetInfo;
    error_fn BASS_ErrorGetCode;
    version_fn BASS_FX_GetVersion;
    BASS_CHANNELINFO info;
    DWORD src, tempo_stream;
    float tempo = 1.0f, pitch = 0.0f, rate = 1.0f, buf[8192];
    unsigned long long total = 0;
    FILE* out;
    int i;
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bass folder> in out.wav [--tempo x] [--pitch semitones] [--rate x]\n", argv[0]);
        return 2;
    }
    for (i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--tempo") && i + 1 < argc) tempo = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) pitch = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (float)atof(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    snprintf(path, sizeof(path), "%s\\bass.dll", argv[1]);
    bass = LoadLibraryA(path);
    snprintf(path, sizeof(path), "%s\\bass_fx.dll", argv[1]);
    fx = bass ? LoadLibraryA(path) : NULL;
    if (!bass || !fx) { fprintf(stderr, "cannot load bass.dll and bass_fx.dll from %s\n", argv[1]); return 1; }
    BASS_Init = (init_fn)proc(bass, "BASS_Init");
    BASS_StreamCreateFile = (create_file_fn)proc(bass, "BASS_StreamCreateFile");
    BASS_ChannelSetAttribute = (set_attr_fn)proc(bass, "BASS_ChannelSetAttribute");
    BASS_ChannelGetData = (get_data_fn)proc(bass, "BASS_ChannelGetData");
    BASS_ChannelGetInfo = (get_info_fn)proc(bass, "BASS_ChannelGetInfo");
    BASS_ErrorGetCode = (error_fn)proc(bass, "BASS_ErrorGetCode");
    BASS_FX_TempoCreate = (tempo_create_fn)proc(fx, "BASS_FX_TempoCreate");
    BASS_FX_GetVersion = (version_fn)proc(fx, "BASS_FX_GetVersion");
    if (!BASS_Init || !BASS_StreamCreateFile || !BASS_ChannelSetAttribute || !BASS_ChannelGetData ||
        !BASS_ChannelGetInfo || !BASS_ErrorGetCode || !BASS_FX_TempoCreate || !BASS_FX_GetVersion) {
        fprintf(stderr, "missing BASS functions\n");
        return 1;
    }
    BASS_FX_GetVersion();                       /* loads bass_fx as a plugin of this bass.dll */
    if (!BASS_Init(0, 48000, 0, NULL, NULL)) { fprintf(stderr, "BASS_Init: %d\n", BASS_ErrorGetCode()); return 1; }
    src = BASS_StreamCreateFile(FALSE, argv[2], 0, 0, BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT | BASS_STREAM_PRESCAN);
    if (!src) { fprintf(stderr, "open %s: %d\n", argv[2], BASS_ErrorGetCode()); return 1; }
    tempo_stream = BASS_FX_TempoCreate(src, BASS_STREAM_DECODE | BASS_FX_FREESOURCE | BASS_SAMPLE_FLOAT);
    if (!tempo_stream) { fprintf(stderr, "BASS_FX_TempoCreate: %d\n", BASS_ErrorGetCode()); return 1; }
    BASS_ChannelGetInfo(tempo_stream, &info);
    BASS_ChannelSetAttribute(tempo_stream, BASS_ATTRIB_TEMPO, (tempo - 1.0f) * 100.0f);
    BASS_ChannelSetAttribute(tempo_stream, BASS_ATTRIB_TEMPO_PITCH, pitch);
    if (rate != 1.0f) BASS_ChannelSetAttribute(tempo_stream, BASS_ATTRIB_TEMPO_FREQ, (float)info.freq * rate);
    out = fopen(argv[3], "wb");
    if (!out) { perror(argv[3]); return 1; }
    wav_header(out, info.freq, info.chans, 0);
    for (;;) {
        DWORD got = BASS_ChannelGetData(tempo_stream, buf, sizeof(buf));
        if (got == (DWORD)-1 || got == 0) break;
        fwrite(buf, 1, got, out);
        total += got / (4 * info.chans);
    }
    fseek(out, 0, SEEK_SET);
    wav_header(out, info.freq, info.chans, total);
    fclose(out);
    return 0;
}
