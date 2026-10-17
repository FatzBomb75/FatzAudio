/* Writes random but valid E-AC-3 streams that use the coding tools real encoders
 * rarely do (spectral extension and its notches, coupling and its changes within
 * a frame, delta bit allocation, block-level SNR offsets, fast gains, frame
 * exponent strategies, transient pre-noise and skip fields, dependent
 * substreams), to check ac3.c against another decoder. Not part of the build:
 * make_dolby_data.sh compiles it together with ac3.c, whose tables and bit
 * allocation it shares, and decodes what it writes with the ffmpeg CLI.
 *
 *   eac3_writer out.ec3 seed layout frames
 *   layout: mono (frames of 1, 2, 3 or 6 blocks), stereo, 5.1, 7.1 (5.1 plus a
 *   dependent substream) or any (a layout picked by the seed). */
#include "../../../src/ac3.c"
#include <stdio.h>

static uint64_t rs = 1;
/* Decoders keep the coupling channel's fast gain and SNR offset across frames. */
static int cpl_fgain_state, cpl_snr_state, first_block = 1;
static uint32_t R(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }
static int RN(int n) { return (int)(R() % (uint32_t)n); }   /* 0..n-1 */
static int P(int pct) { return RN(100) < pct; }

typedef struct { uint8_t b[8192]; size_t pos; } W;
static void put(W* w, int n, uint32_t v)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        if ((v >> i) & 1) w->b[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7));
        w->pos++;
    }
}

typedef struct {
    int acmod, lfeon, blocks, strmtyp, chanmape;
    uint32_t chanmap;
    int spx_pct, cpl_pct, change_pct;   /* how often spx / coupling is on, and changes later */
    int blkswe;
} cfg;

static void gen_exps(W* w, frame* f, int ch, int es, int first, int absexp)
{
    int g, i, r, prev = absexp, n = first;
    for (g = 0; g < f->ngrps[ch]; g++) {
        int dd[3], v;
        for (i = 0; i < 3; i++) {
            int lo = prev - 2 < 0 ? -prev : -2, hi = prev + 2 > 24 ? 24 - prev : 2, dlt;
            /* drift toward 8..20: quiet enough not to clip */
            if (prev < 8 && lo < 0) lo = 0;
            if (prev > 20 && hi > 0) hi = 0;
            dlt = lo + RN(hi - lo + 1);
            prev += dlt;
            dd[i] = dlt + 2;
            for (r = 0; r < (es == 3 ? 4 : es); r++)
                if (n < 256) f->exp[ch][n++] = (uint8_t)prev;
        }
        v = dd[0] * 25 + dd[1] * 5 + dd[2];
        put(w, 7, (uint32_t)v);
    }
}

typedef struct { int n1, n2, n4; } wg;
static void gen_mants(W* w, frame* f, int ch, wg* g)
{
    int bin;
    for (bin = f->strtmant[ch]; bin < f->endmant[ch]; bin++) {
        int bap = f->bap[ch][bin];
        switch (bap) {
        case 0: break;
        case 1: if (g->n1 == 0) { put(w, 5, (uint32_t)RN(27)); g->n1 = 3; } g->n1--; break;
        case 2: if (g->n2 == 0) { put(w, 7, (uint32_t)RN(125)); g->n2 = 3; } g->n2--; break;
        case 3: put(w, 3, (uint32_t)RN(7)); break;
        case 4: if (g->n4 == 0) { put(w, 7, (uint32_t)RN(121)); g->n4 = 2; } g->n4--; break;
        case 5: put(w, 4, (uint32_t)RN(15)); break;
        default: put(w, qbits[bap], R() & ((1u << qbits[bap]) - 1)); break;
        }
    }
}

/* One block plan. */
typedef struct {
    int spxstre, spxinu, chinspx[5], strtf, begf, endf, spxbndstrce;
    int cplstre, cplinu, chincpl[5], cplbegf, cplendf, cplbndstrce;
} plan;

static size_t gen_frame(uint8_t* out, const cfg* c)
{
    static W w;
    frame F, *f = &F;
    plan pl[6];
    int nfch = acmod_nfchans[c->acmod], blk, ch, i, bnd;
    int expstre, snroffststr, transproce, blkswe, bamode, frmfgaincode, dbaflde, skipflde, spxattene;
    int numblkscod = c->blocks == 1 ? 0 : c->blocks == 2 ? 1 : c->blocks == 3 ? 2 : 3;
    int expstr[6][7], firstspx[5], firstcpl[5], firstleak = 1, dbaset[7] = { 0 };
    size_t frmsiz_pos, bytes;
    memset(&w, 0, sizeof(w));
    memset(f, 0, sizeof(*f));
    f->fgain[AC3_CPL] = cpl_fgain_state;
    f->snroffset[AC3_CPL] = cpl_snr_state;
    f->h.eac3 = 1; f->h.acmod = c->acmod; f->h.lfeon = c->lfeon; f->h.blocks = c->blocks; f->h.strmtyp = c->strmtyp;
    f->nfchans = nfch; f->fscod = 0; f->sr_shift = 0;

    /* Plan coupling and spectral extension per block. */
    for (blk = 0; blk < c->blocks; blk++) {
        plan* p = &pl[blk];
        if (blk > 0 && !P(c->change_pct)) { *p = pl[blk - 1]; p->spxstre = p->cplstre = 0; continue; }
        memset(p, 0, sizeof(*p));
        p->spxstre = 1;
        p->spxinu = P(c->spx_pct);
        if (p->spxinu) {
            int sb_begin, sb_end, any = 0;
            for (ch = 0; ch < nfch; ch++) { p->chinspx[ch] = c->acmod == 1 ? 1 : P(70); any |= p->chinspx[ch]; }
            if (!any) p->chinspx[0] = 1;
            do {
                /* Ends above bin 181 blend in noise, which no two decoders share. */
                p->strtf = RN(4); p->begf = RN(8); p->endf = RN(6);
                sb_begin = p->begf < 6 ? p->begf + 2 : p->begf * 2 - 3;
                sb_end = p->endf < 3 ? p->endf + 5 : p->endf * 2 + 3;
            } while (sb_begin >= sb_end || p->strtf * 12 + 25 >= sb_begin * 12 + 25);
            p->spxbndstrce = blk == 0 ? P(50) : 1;   /* resent when the range changes */
        }
        p->cplstre = 1;
        p->cplinu = c->acmod > 1 && P(c->cpl_pct);
        if (p->cplinu) {
            int endsub, n = 0;
            for (ch = 0; ch < nfch; ch++) { p->chincpl[ch] = c->acmod == 2 ? 1 : P(70); n += p->chincpl[ch]; }
            if (n == 0) p->chincpl[0] = 1;
            if (p->spxinu) {
                int sb_begin = p->begf < 6 ? p->begf + 2 : p->begf * 2 - 3;
                endsub = sb_begin - 1;
                if (endsub < 1) { p->cplinu = 0; goto nocpl; }
                p->cplbegf = RN(endsub < 16 ? endsub : 16);
            } else {
                p->cplendf = RN(16);
                endsub = p->cplendf + 3;
                p->cplbegf = RN(endsub < 16 ? endsub : 16);
            }
            p->cplbndstrce = blk == 0 ? P(50) : 1;
        }
    nocpl:;
    }

    /* syncinfo + bsi */
    put(&w, 16, 0x0B77);
    put(&w, 2, (uint32_t)c->strmtyp);
    put(&w, 3, 0);
    frmsiz_pos = w.pos;
    put(&w, 11, 0);
    put(&w, 2, 0);
    put(&w, 2, (uint32_t)numblkscod);
    put(&w, 3, (uint32_t)c->acmod);
    put(&w, 1, (uint32_t)c->lfeon);
    put(&w, 5, 16);
    put(&w, 5, 27);                 /* dialnorm */
    if (P(50)) { put(&w, 1, 1); put(&w, 8, (uint32_t)RN(256)); }   /* compre, compr */
    else put(&w, 1, 0);
    if (c->strmtyp == 1) { put(&w, 1, (uint32_t)c->chanmape); if (c->chanmape) put(&w, 16, c->chanmap); }
    put(&w, 1, 0);                  /* mixmdate */
    put(&w, 1, 0);                  /* infomdate */
    if (c->strmtyp == 0 && c->blocks != 6) put(&w, 1, 0);   /* convsync */
    put(&w, 1, 0);                  /* addbsie */

    /* audfrm */
    expstre = 1;
    if (c->blocks == 6) {
        int static_plan = 1;
        for (blk = 1; blk < 6; blk++) if (pl[blk].spxstre || pl[blk].cplstre) static_plan = 0;
        expstre = static_plan ? P(50) : 1;
        put(&w, 1, (uint32_t)expstre);
        put(&w, 1, 0);              /* ahte */
    }
    snroffststr = RN(3);
    transproce = P(30);
    blkswe = c->blkswe;
    bamode = P(50);
    frmfgaincode = P(50);
    dbaflde = P(50);
    skipflde = P(30);
    spxattene = P(50);
    put(&w, 2, (uint32_t)snroffststr);
    put(&w, 1, (uint32_t)transproce);
    put(&w, 1, (uint32_t)blkswe);
    put(&w, 1, 1);                  /* dithflage: dithflag 0 everywhere */
    put(&w, 1, (uint32_t)bamode);
    put(&w, 1, (uint32_t)frmfgaincode);
    put(&w, 1, (uint32_t)dbaflde);
    put(&w, 1, (uint32_t)skipflde);
    put(&w, 1, (uint32_t)spxattene);
    if (c->acmod > 1)
        for (blk = 0; blk < c->blocks; blk++) {
            if (blk > 0) put(&w, 1, (uint32_t)pl[blk].cplstre);
            if (pl[blk].cplstre) put(&w, 1, (uint32_t)pl[blk].cplinu);
        }
    if (expstre) {
        for (blk = 0; blk < c->blocks; blk++) {
            int change = blk == 0 || pl[blk].spxstre || pl[blk].cplstre;
            if (pl[blk].cplinu) {
                int cplon = blk == 0 || !pl[blk - 1].cplinu;
                expstr[blk][AC3_CPL] = change || cplon ? 1 + RN(3) : RN(4);
                put(&w, 2, (uint32_t)expstr[blk][AC3_CPL]);
            } else {
                expstr[blk][AC3_CPL] = 0;
            }
            for (ch = 0; ch < nfch; ch++) {
                expstr[blk][ch] = change ? 1 + RN(3) : RN(4);
                put(&w, 2, (uint32_t)expstr[blk][ch]);
            }
        }
    } else {
        int ncpl = 0, v;
        for (blk = 0; blk < 6; blk++) ncpl += pl[blk].cplinu;
        if (c->acmod > 1 && ncpl > 0) {
            v = RN(32);
            put(&w, 5, (uint32_t)v);
            for (blk = 0; blk < 6; blk++) expstr[blk][AC3_CPL] = frmexpstr[v][blk];
        } else {
            for (blk = 0; blk < 6; blk++) expstr[blk][AC3_CPL] = 0;
        }
        for (ch = 0; ch < nfch; ch++) {
            v = RN(32);
            put(&w, 5, (uint32_t)v);
            for (blk = 0; blk < 6; blk++) expstr[blk][ch] = frmexpstr[v][blk];
        }
    }
    for (blk = 0; blk < c->blocks; blk++) {
        expstr[blk][AC3_LFE] = c->lfeon ? (blk == 0 ? 1 : RN(2)) : 0;
        if (c->lfeon) put(&w, 1, (uint32_t)expstr[blk][AC3_LFE]);
    }
    if (c->strmtyp == 0) {
        if (c->blocks == 6) for (ch = 0; ch < nfch; ch++) put(&w, 5, (uint32_t)RN(32));
        else put(&w, 1, 0);
    }
    if (snroffststr == 0) {
        int csnr = (nfch > 2 ? 2 : 8) + RN(18), fsnr = RN(16), v = (((csnr - 15) << 4) + fsnr) << 2;
        put(&w, 6, (uint32_t)csnr);
        put(&w, 4, (uint32_t)fsnr);
        for (ch = 0; ch < AC3_NCH; ch++) f->snroffset[ch] = v;
    }
    if (transproce)
        for (ch = 0; ch < nfch; ch++) {
            int on = P(50);
            put(&w, 1, (uint32_t)on);
            if (on) { put(&w, 10, R() & 1023); put(&w, 8, R() & 255); }
        }
    for (ch = 0; ch < nfch; ch++) f->spxattencod[ch] = -1;
    if (spxattene)
        for (ch = 0; ch < nfch; ch++) {
            int on = P(60);
            put(&w, 1, (uint32_t)on);
            if (on) { f->spxattencod[ch] = RN(32); put(&w, 5, (uint32_t)f->spxattencod[ch]); }
        }
    if (c->blocks > 1) put(&w, 1, 0);   /* blkstrtinfoe */
    for (ch = 0; ch < 5; ch++) firstspx[ch] = firstcpl[ch] = 1;

    for (blk = 0; blk < c->blocks; blk++) {
        plan* p = &pl[blk];
        int cplinu = p->cplinu;
        wg g;
        /* In the stream's first block only the first channel is short: FFmpeg
         * mixes up the overlap where channels first differ, harmless while
         * there is none yet. */
        for (ch = 0; ch < nfch; ch++) if (blkswe) put(&w, 1, first_block ? (uint32_t)(ch == 0) : (uint32_t)P(20));
        first_block = 0;
        for (ch = 0; ch < nfch; ch++) put(&w, 1, 0);          /* dithflag */
        for (i = 0; i < (c->acmod == 0 ? 2 : 1); i++) {
            int on = P(30);
            put(&w, 1, (uint32_t)on);
            if (on) put(&w, 8, (uint32_t)RN(256));
        }
        /* spx strategy */
        if (blk > 0) put(&w, 1, (uint32_t)p->spxstre);
        if (p->spxstre) {
            put(&w, 1, (uint32_t)p->spxinu);
            if (p->spxinu) {
                int sb_begin = p->begf < 6 ? p->begf + 2 : p->begf * 2 - 3;
                int sb_end = p->endf < 3 ? p->endf + 5 : p->endf * 2 + 3;
                if (c->acmod != 1) for (ch = 0; ch < nfch; ch++) put(&w, 1, (uint32_t)p->chinspx[ch]);
                put(&w, 2, (uint32_t)p->strtf);
                put(&w, 3, (uint32_t)p->begf);
                put(&w, 3, (uint32_t)p->endf);
                f->spx_copy_start = p->strtf * 12 + 25;
                f->spx_start = sb_begin * 12 + 25;
                f->spx_end = sb_end * 12 + 25;
                if (blk == 0) memcpy(f->spxbndstrc, def_spxbndstrc, sizeof(f->spxbndstrc));
                put(&w, 1, (uint32_t)p->spxbndstrce);
                if (p->spxbndstrce)
                    for (bnd = sb_begin + 1; bnd < sb_end; bnd++) { f->spxbndstrc[bnd] = (uint8_t)P(50); put(&w, 1, f->spxbndstrc[bnd]); }
                f->nspxbnd = band_sizes(f->spxbndstrc, sb_begin, sb_end, f->spxbndsz);
            }
        }
        f->spxinu = p->spxinu;
        for (ch = 0; ch < nfch; ch++) f->chinspx[ch] = p->spxinu && p->chinspx[ch];
        if (!p->spxinu) for (ch = 0; ch < 5; ch++) firstspx[ch] = 1;
        if (p->spxinu)
            for (ch = 0; ch < nfch; ch++) {
                int send;
                if (!f->chinspx[ch]) { firstspx[ch] = 1; continue; }
                send = firstspx[ch] || p->spxstre || P(50);
                if (!firstspx[ch]) put(&w, 1, (uint32_t)send);
                firstspx[ch] = 0;
                if (send) {
                    put(&w, 5, 31);   /* spxblnd: no noise below bin 181 */
                    put(&w, 2, (uint32_t)RN(2));
                    for (bnd = 0; bnd < f->nspxbnd; bnd++) { put(&w, 4, (uint32_t)RN(4)); put(&w, 2, (uint32_t)RN(4)); }
                }
            }
        /* coupling strategy */
        if (p->cplstre && cplinu) {
            int endsub;
            dbaset[AC3_CPL] = 0;
            endsub = p->spxinu ? (f->spx_start - 37) / 12 : p->cplendf + 3;
            put(&w, 1, 0);          /* ecplinu */
            if (c->acmod != 2) for (ch = 0; ch < nfch; ch++) put(&w, 1, (uint32_t)p->chincpl[ch]);
            if (c->acmod == 2) { f->phsflginu = P(50); put(&w, 1, (uint32_t)f->phsflginu); }
            else f->phsflginu = 0;
            put(&w, 4, (uint32_t)p->cplbegf);
            if (!p->spxinu) put(&w, 4, (uint32_t)p->cplendf);
            f->strtmant[AC3_CPL] = p->cplbegf * 12 + 37;
            f->endmant[AC3_CPL] = endsub * 12 + 37;
            if (blk == 0) memcpy(f->cplbndstrc, def_cplbndstrc, sizeof(f->cplbndstrc));
            put(&w, 1, (uint32_t)p->cplbndstrce);
            if (p->cplbndstrce)
                for (bnd = p->cplbegf + 1; bnd < endsub; bnd++) { f->cplbndstrc[bnd] = (uint8_t)P(50); put(&w, 1, f->cplbndstrc[bnd]); }
            f->ncplbnd = band_sizes(f->cplbndstrc, p->cplbegf, endsub, f->cplbndsz);
            for (ch = 0; ch < nfch; ch++) f->chincpl[ch] = p->chincpl[ch];
        } else if (p->cplstre) {
            dbaset[AC3_CPL] = 0;
            for (ch = 0; ch < 5; ch++) { f->chincpl[ch] = 0; firstcpl[ch] = 1; }
            firstleak = 1;
        }
        if (cplinu) {
            int any = 0;
            for (ch = 0; ch < nfch; ch++) {
                int send;
                if (!f->chincpl[ch]) { firstcpl[ch] = 1; continue; }
                send = firstcpl[ch] || p->cplstre || P(50);
                if (!firstcpl[ch]) put(&w, 1, (uint32_t)send);
                firstcpl[ch] = 0;
                if (send) {
                    any = 1;
                    put(&w, 2, (uint32_t)RN(2));
                    for (bnd = 0; bnd < f->ncplbnd; bnd++) { put(&w, 4, (uint32_t)RN(4)); put(&w, 4, (uint32_t)RN(16)); }
                }
            }
            if (c->acmod == 2 && any && f->phsflginu)
                for (bnd = 0; bnd < f->ncplbnd; bnd++) put(&w, 1, (uint32_t)RN(2));
        }
        /* rematrixing */
        if (c->acmod == 2) {
            int send = blk == 0 || P(50);
            if (blk > 0) put(&w, 1, (uint32_t)send);
            if (send) {
                f->nrematbd = 4;
                if (cplinu && f->strtmant[AC3_CPL] <= 61) f->nrematbd -= 1 + (f->strtmant[AC3_CPL] == 37);
                else if (f->spxinu && f->spx_start <= 61) f->nrematbd--;
                for (bnd = 0; bnd < f->nrematbd; bnd++) put(&w, 1, (uint32_t)RN(2));
            }
        }
        /* bandwidths */
        for (ch = 0; ch < nfch; ch++) {
            int es = expstr[blk][ch];
            if (es == 0) continue;
            f->strtmant[ch] = 0;
            if (f->chincpl[ch] && cplinu) f->endmant[ch] = f->strtmant[AC3_CPL];
            else if (f->chinspx[ch]) f->endmant[ch] = f->spx_start;
            else { int bw = RN(61); put(&w, 6, (uint32_t)bw); f->endmant[ch] = bw * 3 + 73; }
            f->ngrps[ch] = (f->endmant[ch] + (3 << (es - 1)) - 4) / (3 << (es - 1));
        }
        if (cplinu && expstr[blk][AC3_CPL] != 0) {
            int es = expstr[blk][AC3_CPL], absexp = RN(12);
            f->ngrps[AC3_CPL] = (f->endmant[AC3_CPL] - f->strtmant[AC3_CPL]) / (3 << (es - 1));
            put(&w, 4, (uint32_t)absexp);
            gen_exps(&w, f, AC3_CPL, es, f->strtmant[AC3_CPL], absexp << 1);
        }
        for (ch = 0; ch < nfch; ch++) {
            int es = expstr[blk][ch];
            if (es != 0) {
                int absexp = 4 + RN(10);
                put(&w, 4, (uint32_t)absexp);
                f->exp[ch][0] = (uint8_t)absexp;
                gen_exps(&w, f, ch, es, 1, absexp);
                put(&w, 2, (uint32_t)RN(4));
            }
        }
        if (c->lfeon && expstr[blk][AC3_LFE]) {
            int absexp = 4 + RN(10);
            put(&w, 4, (uint32_t)absexp);
            f->strtmant[AC3_LFE] = 0; f->endmant[AC3_LFE] = 7; f->ngrps[AC3_LFE] = 2;
            f->exp[AC3_LFE][0] = (uint8_t)absexp;
            gen_exps(&w, f, AC3_LFE, 1, 1, absexp);
        }
        /* bit allocation parameters */
        if (bamode) {
            int send = blk == 0 || P(30);
            put(&w, 1, (uint32_t)send);
            if (send) {
                int a = RN(4), b2 = RN(4), cc = RN(4), d = RN(4), e = RN(7);
                put(&w, 2, (uint32_t)a); put(&w, 2, (uint32_t)b2); put(&w, 2, (uint32_t)cc); put(&w, 2, (uint32_t)d); put(&w, 3, (uint32_t)e);
                f->sdecay = slowdec[a]; f->fdecay = fastdec[b2]; f->sgain = slowgain[cc]; f->dbknee = dbpbtab[d]; f->floorv = floortab[e];
            }
        } else if (blk == 0) {
            f->sdecay = slowdec[2]; f->fdecay = fastdec[1]; f->sgain = slowgain[1]; f->dbknee = dbpbtab[2]; f->floorv = floortab[7];
        }
        /* snr offsets */
        if (snroffststr != 0) {
            int send = blk == 0;
            if (blk == 0) put(&w, 1, 1);
            if (send) {
                int csnr = (nfch > 2 ? 2 : 8) + RN(18);
                put(&w, 6, (uint32_t)csnr);
                if (snroffststr == 1) {
                    int fs = RN(16), v = (((csnr - 15) << 4) + fs) << 2;
                    put(&w, 4, (uint32_t)fs);
                    for (ch = 0; ch < AC3_NCH; ch++) if (ch != AC3_CPL || cplinu) f->snroffset[ch] = v;
                } else {
                    int fs;
                    if (cplinu) { fs = RN(16); put(&w, 4, (uint32_t)fs); f->snroffset[AC3_CPL] = (((csnr - 15) << 4) + fs) << 2; }
                    for (ch = 0; ch < nfch; ch++) { fs = RN(16); put(&w, 4, (uint32_t)fs); f->snroffset[ch] = (((csnr - 15) << 4) + fs) << 2; }
                    if (c->lfeon) { fs = RN(16); put(&w, 4, (uint32_t)fs); f->snroffset[AC3_LFE] = (((csnr - 15) << 4) + fs) << 2; }
                }
            }
        }
        /* fast gains */
        {
            int send = frmfgaincode && P(50);
            if (frmfgaincode) put(&w, 1, (uint32_t)send);
            if (send) {
                int v;
                if (cplinu) { v = RN(8); put(&w, 3, (uint32_t)v); f->fgain[AC3_CPL] = fastgain[v]; }
                for (ch = 0; ch < nfch; ch++) { v = RN(8); put(&w, 3, (uint32_t)v); f->fgain[ch] = fastgain[v]; }
                if (c->lfeon) { v = RN(8); put(&w, 3, (uint32_t)v); f->fgain[AC3_LFE] = fastgain[v]; }
            } else if (blk == 0) {
                for (ch = 0; ch < AC3_NCH; ch++) if (ch != AC3_CPL || cplinu) f->fgain[ch] = fastgain[4];
            }
        }
        if (c->strmtyp == 0) put(&w, 1, 0);   /* convsnroffste */
        if (cplinu) {
            int send = firstleak || P(40);
            if (!firstleak) put(&w, 1, (uint32_t)send);
            if (send) { f->cplfleak = RN(8); f->cplsleak = RN(8); put(&w, 3, (uint32_t)f->cplfleak); put(&w, 3, (uint32_t)f->cplsleak); }
            firstleak = 0;
        }
        /* delta bit allocation */
        {
            int send = dbaflde && (P(40) || (blk > 0 && p->cplstre));
            if (dbaflde) put(&w, 1, (uint32_t)send);
            if (send) {
                int k;
                /* Reuse (0) only of segments sent earlier in this frame; none for
                 * coupling in a block that moves it (old segments may not fit). */
                for (k = 0; k <= nfch; k++) {
                    int cc = k == 0 ? AC3_CPL : k - 1;
                    if (cc == AC3_CPL && !cplinu) continue;
                    f->deltbae[cc] = cc == AC3_CPL && blk > 0 && p->cplstre ? 2 : RN(3);
                    if (f->deltbae[cc] == 0 && !dbaset[cc]) f->deltbae[cc] = 2;
                    if (f->deltbae[cc] == 1) dbaset[cc] = 1;
                    put(&w, 2, (uint32_t)f->deltbae[cc]);
                }
                for (k = 0; k <= nfch; k++) {
                    int cc = k == 0 ? AC3_CPL : k - 1, seg, band;
                    if (cc == AC3_CPL && !cplinu) continue;
                    if (f->deltbae[cc] != 1) continue;
                    f->deltnseg[cc] = 1 + RN(4);
                    put(&w, 3, (uint32_t)(f->deltnseg[cc] - 1));
                    band = masktab[f->strtmant[cc]];
                    for (seg = 0; seg < f->deltnseg[cc]; seg++) {
                        int off = RN(6), len = 1 + RN(6);
                        if (band + off + len > 49) { off = 0; len = 0; }
                        f->deltoffst[cc][seg] = (uint8_t)off; f->deltlen[cc][seg] = (uint8_t)len; f->deltba[cc][seg] = (uint8_t)RN(8);
                        put(&w, 5, (uint32_t)off); put(&w, 4, (uint32_t)len); put(&w, 3, f->deltba[cc][seg]);
                        band += off + len;
                    }
                }
            } else if (blk == 0) {
                for (ch = 0; ch < AC3_NCH; ch++) f->deltbae[ch] = 2;
            }
        }
        if (skipflde) {
            int send = P(30);
            put(&w, 1, (uint32_t)send);
            if (send) { int n = RN(4); put(&w, 9, (uint32_t)n); for (i = 0; i < n; i++) put(&w, 8, R() & 255); }
        }
        if (cplinu && bit_alloc(f, AC3_CPL) < 0) { fprintf(stderr, "ba cpl\n"); exit(1); }
        for (ch = 0; ch < nfch; ch++) if (bit_alloc(f, ch) < 0) { fprintf(stderr, "ba\n"); exit(1); }
        if (c->lfeon && bit_alloc(f, AC3_LFE) < 0) { fprintf(stderr, "ba lfe\n"); exit(1); }
        memset(&g, 0, sizeof(g));
        {
            int got = 0;
            for (ch = 0; ch < nfch; ch++) {
                gen_mants(&w, f, ch, &g);
                if (cplinu && f->chincpl[ch] && !got) { gen_mants(&w, f, AC3_CPL, &g); got = 1; }
            }
            if (c->lfeon) gen_mants(&w, f, AC3_LFE, &g);
        }
    }
    /* pad + crcrsv + crc2 */
    bytes = ((w.pos + 17 + 15) / 16) * 2;
    if (bytes > 4096) return 0;
    cpl_fgain_state = f->fgain[AC3_CPL];
    cpl_snr_state = f->snroffset[AC3_CPL];
    {
        size_t words = bytes / 2 - 1;
        size_t save = w.pos;
        w.pos = frmsiz_pos;
        put(&w, 11, (uint32_t)words);
        w.pos = save;
    }
    {
        uint16_t crc = crc16(w.b + 2, bytes - 4);
        w.b[bytes - 2] = (uint8_t)(crc >> 8);
        w.b[bytes - 1] = (uint8_t)crc;
    }
    memcpy(out, w.b, bytes);
    return bytes;
}

int main(int argc, char** argv)
{
    FILE* o;
    const char* layout;
    int n, k, am = 0, lf = 0;
    uint8_t buf[8192];
    if (argc != 5) { fprintf(stderr, "usage: eac3_writer out.ec3 seed mono|stereo|5.1|7.1|any frames\n"); return 2; }
    o = fopen(argv[1], "wb");
    if (!o) return 1;
    layout = argv[3];
    n = atoi(argv[4]);
    rs = (uint64_t)strtoull(argv[2], NULL, 10) * 2654435761u + 88172645463325252ull;
    ac3_init_tables();
    for (k = 0; k < n; k++) {
        cfg c;
        size_t len;
        memset(&c, 0, sizeof(c));
        c.change_pct = 20;
        c.blocks = 6;
        c.spx_pct = 70;
        c.cpl_pct = 50;
        c.blkswe = 1;
        if (strcmp(layout, "mono") == 0) {
            static const int blocks[4] = { 1, 2, 3, 6 };
            c.acmod = 1;
            c.blocks = blocks[RN(4)];
        } else if (strcmp(layout, "stereo") == 0) {
            c.acmod = 2;
        } else if (strcmp(layout, "any") == 0) {
            static const int blocks[4] = { 1, 2, 3, 6 };
            if (k == 0) { am = 1 + RN(7); lf = RN(2); }
            c.acmod = am;
            c.lfeon = lf;
            c.blocks = blocks[RN(4)];
            c.change_pct = 40;
        } else {
            c.acmod = 7;
            c.lfeon = 1;
        }
        if (strcmp(layout, "7.1") == 0) {
            /* 5.1, then a dependent substream with Ls, Rs and the back pair. No
             * short blocks: with a dependent substream FFmpeg clears or moves
             * some channels' overlap in every frame where block lengths differ. */
            c.blkswe = 0;
            while ((len = gen_frame(buf, &c)) == 0) {}
            fwrite(buf, 1, len, o);
            c.acmod = 6;
            c.lfeon = 0;
            c.strmtyp = 1;
            c.chanmape = 1;
            c.chanmap = (0x8000u >> 3) | (0x8000u >> 4) | (0x8000u >> 6);
        }
        while ((len = gen_frame(buf, &c)) == 0) {}
        fwrite(buf, 1, len, o);
    }
    fclose(o);
    return 0;
}
