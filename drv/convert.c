/*
 * AUDIONT - PCM format conversion and sample-rate conversion
 *
 * Playback : any PCM (8/16/24/32-bit, mono/stereo, 1..200 kHz) -> 48 kHz stereo
 * Recording: 48 kHz stereo -> any PCM format
 *
 * Sample-rate conversion uses a polyphase Kaiser-windowed sinc filter
 * (16 taps when upsampling, widened for downsampling so the cut-off follows
 * the output Nyquist frequency).  Phase stepping is exact rational
 * arithmetic, so there is no long-term drift.  Filter weights for every
 * phase are cached per stream when the rate ratio allows it.
 *
 * Partial frames that straddle buffer boundaries (mmdrv splits buffers at
 * arbitrary byte offsets) are carried over, in both directions.
 *
 * Integer only - no FPU state is touched in kernel mode.
 */
#include "audiont.h"
#include "sinctab.h"

#define RS_MASK (RS_HIST - 1)

BOOLEAN ConvValidFormat(const WAVEFORMATEX *wf, ULONG len, PCM_FORMAT *out)
{
    USHORT bits;
    if (!wf || len < 14) return FALSE;
    if (wf->wFormatTag != WAVE_FORMAT_PCM) return FALSE;
    bits = (len >= 16) ? wf->wBitsPerSample : 8;
    if (wf->nChannels < 1 || wf->nChannels > 2) return FALSE;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return FALSE;
    if (wf->nSamplesPerSec < 1000 || wf->nSamplesPerSec > 200000) return FALSE;
    if (wf->nBlockAlign != wf->nChannels * (bits / 8)) return FALSE;
    if (out) {
        out->Rate = wf->nSamplesPerSec;
        out->Channels = wf->nChannels;
        out->Bits = bits;
        out->BlockAlign = wf->nBlockAlign;
        out->Float = 0;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* resampler                                                           */
/* ------------------------------------------------------------------ */
static ULONG Gcd(ULONG a, ULONG b) { while (b) { ULONG t = a % b; a = b; b = t; } return a; }

static void RsWeights(const RESAMPLER *rs, ULONG phase, SHORT *w)
{
    ULONG taps = rs->Half * 2, j;
    LONG frac = (LONG)(((ULONGLONG)phase << 16) / rs->OutRate);
    LONG sum = 0;
    for (j = 0; j < taps; j++) {
        LONG k = (LONG)j - (LONG)(rs->Half - 1);
        LONG t = k * 65536 - frac;
        ULONG ts = (ULONG)(((ULONGLONG)(t < 0 ? -t : t) * rs->ScaleQ16) >> 16);
        LONG h = 0;
        if (ts < (ULONG)SINC_ZC * 65536) {
            ULONG idx = ts >> 6, sub = ts & 63;
            h = g_SincTab[idx] + (((g_SincTab[idx + 1] - g_SincTab[idx]) * (LONG)sub) >> 6);
            h = (LONG)(((LONGLONG)h * rs->ScaleQ16) >> 16);
        }
        w[j] = (SHORT)h;
        sum += h;
    }
    /* normalise to unity DC gain */
    if (sum > 0 && sum != 16384)
        for (j = 0; j < taps; j++) w[j] = (SHORT)(((LONG)w[j] * 16384 + sum / 2) / sum);
}

void RsFree(RESAMPLER *rs)
{
    if (rs->Cache) OsFree(rs->Cache);
    rs->Cache = NULL;
}

void RsReset(RESAMPLER *rs)
{
    memset(rs->Hist, 0, sizeof(rs->Hist));
    rs->Phase = 0;
    rs->Base = rs->Written = rs->Passthrough ? 0 : rs->Half;
}

void RsInit(RESAMPLER *rs, ULONG in, ULONG out)
{
    ULONG half, p, bytes;
    RsFree(rs);
    rs->InRate = in; rs->OutRate = out;
    rs->Passthrough = (in == out);
    if (in > out) {
        rs->ScaleQ16 = (ULONG)(((ULONGLONG)out << 16) / in);
        half = (SINC_ZC * 65536 + rs->ScaleQ16 - 1) / rs->ScaleQ16;
        if (half > RS_MAX_HALF) half = RS_MAX_HALF;
    } else {
        rs->ScaleQ16 = 65536;
        half = SINC_ZC;
    }
    rs->Half = rs->Passthrough ? 0 : half;
    rs->Gcd = Gcd(in, out);
    rs->NPhases = out / rs->Gcd;
    rs->Cache = NULL;
    if (!rs->Passthrough) {
        bytes = rs->NPhases * rs->Half * 2 * sizeof(SHORT);
        if (bytes <= 65536) {
            rs->Cache = OsAlloc(bytes);
            if (rs->Cache)
                for (p = 0; p < rs->NPhases; p++)
                    RsWeights(rs, p * rs->Gcd, rs->Cache + p * rs->Half * 2);
        }
    }
    RsReset(rs);
}

BOOLEAN RsCanOut(const RESAMPLER *rs)
{
    return rs->Base + rs->Half < rs->Written;
}

void RsPush(RESAMPLER *rs, LONG l, LONG r)
{
    SHORT *h = rs->Hist[rs->Written & RS_MASK];
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    h[0] = (SHORT)l; h[1] = (SHORT)r;
    rs->Written++;
}

void RsOut(RESAMPLER *rs, LONG *l, LONG *r)
{
    if (rs->Passthrough) {
        SHORT *h = rs->Hist[rs->Base & RS_MASK];
        *l = h[0]; *r = h[1];
        rs->Base++;
        return;
    } else {
        SHORT wbuf[RS_MAX_HALF * 2];
        const SHORT *w;
        ULONG taps = rs->Half * 2, j, idx = rs->Base - (rs->Half - 1);
        LONG al = 0, ar = 0;
        if (rs->Cache) w = rs->Cache + (rs->Phase / rs->Gcd) * taps;
        else { RsWeights(rs, rs->Phase, wbuf); w = wbuf; }
        for (j = 0; j < taps; j++) {
            const SHORT *h = rs->Hist[(idx + j) & RS_MASK];
            al += w[j] * h[0];
            ar += w[j] * h[1];
        }
        *l = (al + 8192) >> 14;
        *r = (ar + 8192) >> 14;
        rs->Phase += rs->InRate;
        while (rs->Phase >= rs->OutRate) { rs->Phase -= rs->OutRate; rs->Base++; }
    }
}

/*
 * Block helpers for the USB back-end: one call per block instead of one per
 * frame.  RsFromRing produces 'frames' stereo output frames from a 48 kHz
 * ring, reading at *pos while *pos < limit (silence after it, counted in
 * *starved).  RsToRing resamples 'frames' input frames into a ring at *pos.
 */
void RsFromRing(RESAMPLER *rs, const SHORT *ring, ULONG ringFrames, ULONGLONG *pos, ULONGLONG limit,
                LONG *out, ULONG frames, ULONG *starved)
{
    ULONG f;
    ULONGLONG p = *pos;
    for (f = 0; f < frames; f++) {
        while (!RsCanOut(rs)) {
            if (p < limit) {
                const SHORT *s = ring + (ULONG)(p % ringFrames) * 2;
                RsPush(rs, s[0], s[1]);
            } else {
                RsPush(rs, 0, 0);
                (*starved)++;
            }
            p++;
        }
        RsOut(rs, out + f * 2, out + f * 2 + 1);
    }
    *pos = p;
}

void RsToRing(RESAMPLER *rs, const LONG *in, ULONG frames, SHORT *ring, ULONG ringFrames, ULONGLONG *pos)
{
    ULONG f;
    ULONGLONG p = *pos;
    for (f = 0; f < frames; f++) {
        RsPush(rs, in[f * 2], in[f * 2 + 1]);
        while (RsCanOut(rs)) {
            SHORT *d = ring + (ULONG)(p % ringFrames) * 2;
            LONG l, r;
            RsOut(rs, &l, &r);
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            d[0] = (SHORT)l;
            d[1] = (SHORT)r;
            p++;
        }
    }
    *pos = p;
}

/* ------------------------------------------------------------------ */
/* converter                                                           */
/* ------------------------------------------------------------------ */
BOOLEAN ConvSetup(CONVERTER *cv, const PCM_FORMAT *f, BOOLEAN record)
{
    cv->Fmt = *f;
    if (record) RsInit(&cv->Rs, ENG_RATE, f->Rate);
    else        RsInit(&cv->Rs, f->Rate, ENG_RATE);
    cv->CarryLen = 0;
    cv->PendLen = cv->PendOff = 0;
    return TRUE;
}

void ConvReset(CONVERTER *cv)
{
    RsReset(&cv->Rs);
    cv->CarryLen = 0;
    cv->PendLen = cv->PendOff = 0;
}

void ConvFree(CONVERTER *cv) { RsFree(&cv->Rs); }

static __inline__ void Decode(const PCM_FORMAT *f, const UCHAR *p, LONG *l, LONG *r)
{
    LONG a, b;
    switch (f->Bits) {
    case 8:
        a = ((LONG)p[0] - 128) << 8;
        b = f->Channels == 2 ? ((LONG)p[1] - 128) << 8 : a;
        break;
    case 16:
        a = (SHORT)(p[0] | (p[1] << 8));
        b = f->Channels == 2 ? (SHORT)(p[2] | (p[3] << 8)) : a;
        break;
    case 24:
        a = (SHORT)(p[1] | (p[2] << 8));
        b = f->Channels == 2 ? (SHORT)(p[4] | (p[5] << 8)) : a;
        break;
    default: /* 32 */
        a = (SHORT)(p[2] | (p[3] << 8));
        b = f->Channels == 2 ? (SHORT)(p[6] | (p[7] << 8)) : a;
        break;
    }
    *l = a; *r = b;
}

/*
 * Produce up to 'frames' 48 kHz frames from src, adding them (scaled by
 * volL/volR, Q15) into dst.  *consumed receives the number of source bytes
 * used; a trailing partial frame is always consumed into the carry buffer.
 */
ULONG ConvPlay(CONVERTER *cv, const UCHAR *src, ULONG srcBytes, ULONG *consumed,
               LONG *dst, ULONG frames, LONG volL, LONG volR)
{
    const PCM_FORMAT *f = &cv->Fmt;
    RESAMPLER *rs = &cv->Rs;
    ULONG ba = f->BlockAlign, used = 0, out = 0;
    LONG l, r;

    while (out < frames) {
        if (RsCanOut(rs)) {
            RsOut(rs, &l, &r);
            dst[out * 2]     += (l * volL) >> 15;
            dst[out * 2 + 1] += (r * volR) >> 15;
            out++;
            continue;
        }
        if (cv->CarryLen) {
            ULONG need = ba - cv->CarryLen, take = MIN(need, srcBytes - used);
            memcpy(cv->Carry + cv->CarryLen, src + used, take);
            cv->CarryLen += take; used += take;
            if (cv->CarryLen < ba) break;
            Decode(f, cv->Carry, &l, &r);
            RsPush(rs, l, r);
            cv->CarryLen = 0;
            continue;
        }
        if (srcBytes - used >= ba) {
            Decode(f, src + used, &l, &r);
            RsPush(rs, l, r);
            used += ba;
            continue;
        }
        if (srcBytes - used) {
            cv->CarryLen = srcBytes - used;
            memcpy(cv->Carry, src + used, cv->CarryLen);
            used = srcBytes;
        }
        break;
    }
    *consumed = used;
    return out;
}

static __inline__ void Encode(const PCM_FORMAT *f, UCHAR *p, LONG l, LONG r)
{
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    if (f->Channels == 1) l = (l + r) >> 1;
    switch (f->Bits) {
    case 8:
        p[0] = (UCHAR)((l >> 8) + 128);
        if (f->Channels == 2) p[1] = (UCHAR)((r >> 8) + 128);
        break;
    case 16:
        p[0] = (UCHAR)l; p[1] = (UCHAR)(l >> 8);
        if (f->Channels == 2) { p[2] = (UCHAR)r; p[3] = (UCHAR)(r >> 8); }
        break;
    case 24:
        p[0] = 0; p[1] = (UCHAR)l; p[2] = (UCHAR)(l >> 8);
        if (f->Channels == 2) { p[3] = 0; p[4] = (UCHAR)r; p[5] = (UCHAR)(r >> 8); }
        break;
    default:
        p[0] = 0; p[1] = 0; p[2] = (UCHAR)l; p[3] = (UCHAR)(l >> 8);
        if (f->Channels == 2) { p[4] = 0; p[5] = 0; p[6] = (UCHAR)r; p[7] = (UCHAR)(r >> 8); }
        break;
    }
}

/*
 * Convert 48 kHz stereo 16-bit frames into the stream format, filling dst.
 * Returns the number of source frames consumed (less than 'frames' only
 * when dst is full); *produced receives the number of bytes written.
 */
ULONG ConvRecord(CONVERTER *cv, const SHORT *src, ULONG frames, UCHAR *dst,
                 ULONG dstBytes, ULONG *produced)
{
    const PCM_FORMAT *f = &cv->Fmt;
    RESAMPLER *rs = &cv->Rs;
    ULONG ba = f->BlockAlign, outb = 0, i = 0;
    LONG l, r;
    for (;;) {
        if (cv->PendLen) {
            ULONG n = MIN(cv->PendLen, dstBytes - outb);
            memcpy(dst + outb, cv->Pend + cv->PendOff, n);
            outb += n; cv->PendOff += n; cv->PendLen -= n;
            if (cv->PendLen) break;              /* dst full */
        }
        if (outb >= dstBytes) break;
        if (RsCanOut(rs)) {
            RsOut(rs, &l, &r);
            if (dstBytes - outb >= ba) {
                Encode(f, dst + outb, l, r);
                outb += ba;
            } else {
                Encode(f, cv->Pend, l, r);
                cv->PendLen = ba; cv->PendOff = 0;
            }
            continue;
        }
        if (i >= frames) break;
        RsPush(rs, src[i * 2], src[i * 2 + 1]);
        i++;
    }
    *produced = outb;
    return i;
}
