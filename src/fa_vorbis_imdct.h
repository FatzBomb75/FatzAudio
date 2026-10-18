/* libvorbis's inverse MDCT goes through FatzAudio's (src/mdct.c): the patched
 * third_party/vorbis/lib/mdct.c includes this, so it needs nothing else. */
#ifndef FA_VORBIS_IMDCT_H
#define FA_VORBIS_IMDCT_H

/* A transform of n samples (a power of two from 8): NULL when out of memory. */
void* fa_vorbis_imdct_create(int n);
/* n samples from the n/2 coefficients at the start of x, in place. */
void fa_vorbis_imdct(void* t, float* x);
void fa_vorbis_imdct_free(void* t);

#endif
