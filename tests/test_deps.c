/* Windows: fatzaudio.dll must not need a compiler runtime DLL next to it (one
 * self-contained binary per platform). Reads the DLL's import table and fails on
 * the C++ / compiler runtimes of MSVC and MinGW. System DLLs are fine.
 *
 * Usage: fa_test_deps path/to/fatzaudio.dll */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const banned[] = { "msvcp", "vcruntime", "concrt", "libstdc++", "libgcc", "libwinpthread" };

static uint8_t* g_data;
static size_t g_size;

static uint32_t u16(size_t at) { return at + 2 <= g_size ? (uint32_t)g_data[at] | (uint32_t)g_data[at + 1] << 8 : 0; }
static uint32_t u32(size_t at) { return at + 4 <= g_size ? u16(at) | u16(at + 2) << 16 : 0; }

/* File offset of a relative virtual address, through the section table; 0 if outside. */
static size_t rva_to_offset(size_t sections, uint32_t count, uint32_t rva)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        size_t s = sections + (size_t)i * 40;
        uint32_t va = u32(s + 12), vsize = u32(s + 8), raw = u32(s + 20), rsize = u32(s + 16);
        uint32_t span = vsize > rsize ? vsize : rsize;
        if (rva >= va && rva < va + span) return (size_t)raw + (rva - va);
    }
    return 0;
}

int main(int argc, char** argv)
{
    FILE* f;
    long len;
    size_t pe, opt, sections, imp, name;
    uint32_t count, magic, rva;
    int failures = 0, found = 0;
    size_t b;
    if (argc != 2) { fprintf(stderr, "usage: %s fatzaudio.dll\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_size = len > 0 ? (size_t)len : 0;
    g_data = (uint8_t*)malloc(g_size ? g_size : 1);
    if (!g_data || fread(g_data, 1, g_size, f) != g_size) { fclose(f); fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    fclose(f);

    pe = u32(0x3C);
    if (u16(0) != 0x5A4D || u32(pe) != 0x00004550) { fprintf(stderr, "%s: not a PE file\n", argv[1]); return 1; }
    count = u16(pe + 6);
    opt = pe + 24;
    sections = opt + u16(pe + 20);
    magic = u16(opt);
    rva = u32(opt + (magic == 0x20b ? 112 : 96) + 8);   /* data directory 1: imports */
    imp = rva ? rva_to_offset(sections, count, rva) : 0;
    for (; imp && imp + 20 <= g_size && u32(imp + 12) != 0; imp += 20) {
        char lower[256];
        size_t i;
        name = rva_to_offset(sections, count, u32(imp + 12));
        if (!name) break;
        for (i = 0; i + 1 < sizeof(lower) && name + i < g_size && g_data[name + i]; i++)
            lower[i] = (char)tolower(g_data[name + i]);
        lower[i] = 0;
        printf("  imports %s\n", lower);
        found++;
        for (b = 0; b < sizeof(banned) / sizeof(banned[0]); b++)
            if (!strncmp(lower, banned[b], strlen(banned[b]))) {
                printf("  FAIL: %s needs %s next to it\n", argv[1], lower);
                failures++;
            }
    }
    free(g_data);
    if (!found) { printf("  FAIL: no imports found (unreadable import table)\n"); return 1; }
    if (failures) return 1;
    printf("PASS\n");
    return 0;
}
