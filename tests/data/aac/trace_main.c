/* The bit positions ps_rewrite.py needs: decodes an ADTS file with a decoder
 * built with -DFA_AAC_TRACE, which prints them to stderr (make_aac_data.sh). */
#include "aac.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    FILE* f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    uint8_t* data;
    float* pcm = NULL;
    size_t n, pos = 0, cap = 0;
    fa_aac* d = fa_aac_create();
    if (!f || !d) return 1;
    fseek(f, 0, SEEK_END);
    n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc(n);
    if (!data || fread(data, 1, n, f) != n) return 1;
    while (pos + 7 <= n) {
        size_t len = ((size_t)(data[pos + 3] & 3) << 11) | ((size_t)data[pos + 4] << 3) | (data[pos + 5] >> 5);
        int frames, channels;
        uint32_t rate;
        if (len < 7 || pos + len > n) break;
        fa_aac_decode(d, data + pos, len, &pcm, &cap, &frames, &channels, &rate);
        pos += len;
    }
    fa_aac_destroy(d);
    free(pcm);
    free(data);
    fclose(f);
    return 0;
}
