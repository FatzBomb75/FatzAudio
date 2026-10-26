/* The packet list of an MP4/MOV or CAF track (offset, size, time of each packet),
 * kept compact: about 6.5 bytes a packet instead of 24. A 10-minute M4A has some
 * 26,000 packets.
 *
 * Each packet keeps its size (4 bytes) and its duration (2 bytes, or 4 once one
 * needs it). Every 64th packet anchors its offset and time; the others follow on
 * from the packet before them (offset + size, time + duration) unless they are
 * marked as a break, whose offset and time are listed separately (interleaved
 * video, a fragment that starts at its own time). A packet is found by walking from
 * its anchor, at most 63 steps. */
#ifndef FA_PKT_INDEX_H
#define FA_PKT_INDEX_H

#include "fa_internal.h"

#define FA_PKT_MAX_SIZE 0x7FFFFFFFu  /* the size's top bit marks a break */

typedef struct fa_pkt {
    int64_t off;
    int64_t pts;
    uint32_t size;
    uint32_t dur;
} fa_pkt;

typedef struct fa_pkt_anchor {
    int64_t off, pts;
    size_t brk;                      /* breaks listed before this packet */
} fa_pkt_anchor;

typedef struct fa_pkt_break {
    int64_t off, pts;
} fa_pkt_break;

typedef struct fa_pkt_index {
    uint32_t* size;                  /* top bit: a break */
    uint16_t* dur16;                 /* durations, until one needs 32 bits ... */
    uint32_t* dur32;                 /* ... then these (dur16 freed) */
    fa_pkt_anchor* anchor;
    fa_pkt_break* brk;
    size_t n, cap, nbrk, brk_cap;
    int64_t end_off, end_pts;        /* right after the last packet */
} fa_pkt_index;

/* Room for `n` packets in all, so a list of known length is made at its size. */
fa_result fa_pkt_reserve(fa_pkt_index* x, size_t n);
fa_result fa_pkt_add(fa_pkt_index* x, int64_t off, uint32_t size, int64_t pts, uint32_t dur);
/* Grows the last packet by `size` bytes and `dur` (PCM frames joined into one packet). */
fa_result fa_pkt_extend_last(fa_pkt_index* x, uint32_t size, uint32_t dur);
uint32_t fa_pkt_size(const fa_pkt_index* x, size_t i);
uint32_t fa_pkt_dur(const fa_pkt_index* x, size_t i);
void fa_pkt_get(const fa_pkt_index* x, size_t i, fa_pkt* out);
/* The last packet whose time is at or before t (0 when none is). Times must not go
 * back, as in every file the demuxers accept. */
size_t fa_pkt_find(const fa_pkt_index* x, int64_t t);
/* Where to start reading for time t: fa_pkt_find, then back until `packets_back`
 * packets and `bytes_back` bytes come before t (at most 64 packets). */
size_t fa_pkt_seek_back(const fa_pkt_index* x, int64_t t, int packets_back, size_t bytes_back);
int64_t fa_pkt_total_bytes(const fa_pkt_index* x);
void fa_pkt_trim(fa_pkt_index* x);   /* gives back unused room */
void fa_pkt_clear(fa_pkt_index* x);  /* no packets, memory kept */
void fa_pkt_free(fa_pkt_index* x);

#endif
