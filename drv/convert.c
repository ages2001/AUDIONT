/*
 * AUDIONT - PCM format conversion and sample-rate conversion
 *
 * Playback : PCM 8/16/24/32-bit and IEEE float 32/64-bit, 1 - 8 channels (more than
 *            two mixed down), WAVE_FORMAT_PCM / IEEE_FLOAT / EXTENSIBLE, 1..200 kHz
 *            -> 48 kHz stereo
 * Recording: 48 kHz stereo -> any of those formats
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

/* the subformat GUID of WAVEFORMATEXTENSIBLE: {xxxxxxxx-0000-0010-8000-00aa00389b71},
   1 = PCM, 3 = IEEE float */
static const UCHAR g_GuidTail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

BOOLEAN ConvValidFormat(const WAVEFORMATEX *wf, ULONG len, PCM_FORMAT *out)
{
    const UCHAR *x = (const UCHAR *)wf;
    USHORT bits, tag;
    USHORT isFloat = 0;
    ULONG mask = 0, i;
    if (!wf || len < 14) return FALSE;
    tag = wf->wFormatTag;
    bits = (len >= 16) ? wf->wBitsPerSample : 8;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        /* VLC, browsers (cubeb / Chromium), DirectSound and WASAPI layers on XP use it */
        ULONG sub;
        if (len < 40 || wf->cbSize < 22) return FALSE;
        for (i = 0; i < 12; i++)
            if (x[28 + i] != g_GuidTail[i]) return FALSE;
        sub = x[24] | ((ULONG)x[25] << 8) | ((ULONG)x[26] << 16) | ((ULONG)x[27] << 24);
        if (sub == WAVE_FORMAT_IEEE_FLOAT) isFloat = 1;
        else if (sub != WAVE_FORMAT_PCM) return FALSE;
        mask = x[20] | ((ULONG)x[21] << 8) | ((ULONG)x[22] << 16) | ((ULONG)x[23] << 24);
    } else if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = 1;
    } else if (tag != WAVE_FORMAT_PCM) {
        return FALSE;
    }
    if (wf->nChannels < 1 || wf->nChannels > CONV_MAX_CH) return FALSE;
    if (isFloat ? (bits != 32 && bits != 64) : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return FALSE;
    if (wf->nSamplesPerSec < 1000 || wf->nSamplesPerSec > 200000) return FALSE;
    if (wf->nBlockAlign != wf->nChannels * (bits / 8)) return FALSE;
    if (out) {
        out->Rate = wf->nSamplesPerSec;
        out->Channels = wf->nChannels;
        out->Bits = bits;
        out->BlockAlign = wf->nBlockAlign;
        out->Float = isFloat;
        out->ChannelMask = mask;
    }
    return TRUE;
}

/* the speaker of each channel (WAVEFORMATEXTENSIBLE order: the set bits of the mask from the
   lowest) and its share of left / right (Q14), summed to at most 1.0 per side */
static void MixMatrix(CONVERTER *cv)
{
    /* the default order of WAVE_FORMAT_PCM / a mask of 0 (SPEAKER_* bits) */
    static const ULONG defMask[CONV_MAX_CH + 1] = { 0, 0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x70F, 0x63F };
    const PCM_FORMAT *f = &cv->Fmt;
    ULONG mask = f->ChannelMask, bit = 0, c;
    LONG sumL = 0, sumR = 0;
    if (!mask || f->Channels <= 2) mask = defMask[f->Channels];
    for (c = 0; c < CONV_MAX_CH; c++) cv->MixL[c] = cv->MixR[c] = 0;
    if (f->Channels == 1) { cv->MixL[0] = cv->MixR[0] = 16384; return; }
    if (f->Channels == 2) { cv->MixL[0] = 16384; cv->MixR[1] = 16384; return; }
    for (c = 0; c < f->Channels; c++) {
        LONG l = 0, r = 0;
        while (bit < 32 && !(mask & (1UL << bit))) bit++;
        if (bit >= 32) break;                       /* more channels than speakers named: unused */
        switch (1UL << bit) {
        case 0x1:     l = 16384; break;             /* front left */
        case 0x2:     r = 16384; break;             /* front right */
        case 0x40:    l = 13926; r = 2458; break;   /* front left / right of centre */
        case 0x80:    l = 2458; r = 13926; break;
        case 0x10: case 0x200: case 0x1000: case 0x8000: l = 11585; break;   /* back / side / top left */
        case 0x20: case 0x400: case 0x4000: case 0x20000: r = 11585; break;  /* back / side / top right */
        case 0x8:     break;                        /* LFE: left out */
        default:      l = r = 11585; break;         /* centres */
        }
        cv->MixL[c] = (SHORT)l; cv->MixR[c] = (SHORT)r;
        sumL += l; sumR += r;
        bit++;
    }
    /* no clipping when every channel is at full scale */
    if (sumL < sumR) sumL = sumR;
    if (sumL > 16384)
        for (c = 0; c < CONV_MAX_CH; c++) {
            cv->MixL[c] = (SHORT)(((LONG)cv->MixL[c] * 16384) / sumL);
            cv->MixR[c] = (SHORT)(((LONG)cv->MixR[c] * 16384) / sumL);
        }
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
    MixMatrix(cv);
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

/* IEEE float (as its bits) to the 16-bit scale, without the FPU (kernel mode) */
static LONG FloatTo16(ULONG u)
{
    LONG e = (LONG)((u >> 23) & 0xFF), v;
    ULONG m = (u & 0x7FFFFF) | 0x800000;
    if (e == 0) return 0;                           /* zero / denormal */
    e -= 135;                                       /* value * 32768 = m * 2^(e - 150 + 15) */
    if (e >= 0) v = e > 7 ? 32768 : (LONG)MIN(m << e, 32768UL);
    else v = e <= -24 ? 0 : (LONG)(m >> -e);
    if (v > 32767 && !(u & 0x80000000UL)) v = 32767;
    return (u & 0x80000000UL) ? -v : v;
}

static LONG DoubleTo16(ULONG lo, ULONG hi)
{
    LONG e = (LONG)((hi >> 20) & 0x7FF), v;
    ULONGLONG m = ((ULONGLONG)(hi & 0xFFFFF) << 32 | lo) | (1ULL << 52);
    if (e == 0) return 0;
    e -= 1060;                                      /* value * 32768 = m * 2^(e - 1075 + 15) */
    if (e >= 0) v = 32768;
    else v = e <= -53 ? 0 : (LONG)MIN(m >> -e, 32768ULL);
    if (v > 32767 && !(hi & 0x80000000UL)) v = 32767;
    return (hi & 0x80000000UL) ? -v : v;
}

static __inline__ LONG Sample(const PCM_FORMAT *f, const UCHAR *p)
{
    if (f->Float) {
        if (f->Bits == 32) return FloatTo16(p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24));
        return DoubleTo16(p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24),
                          p[4] | ((ULONG)p[5] << 8) | ((ULONG)p[6] << 16) | ((ULONG)p[7] << 24));
    }
    switch (f->Bits) {
    case 8:  return ((LONG)p[0] - 128) << 8;
    case 16: return (SHORT)(p[0] | (p[1] << 8));
    case 24: return (SHORT)(p[1] | (p[2] << 8));
    default: return (SHORT)(p[2] | (p[3] << 8));
    }
}

static __inline__ void Decode(const CONVERTER *cv, const UCHAR *p, LONG *l, LONG *r)
{
    const PCM_FORMAT *f = &cv->Fmt;
    ULONG bps = f->Bits / 8, c;
    LONG al = 0, ar = 0;
    if (f->Channels == 1) { *l = *r = Sample(f, p); return; }
    if (f->Channels == 2) { *l = Sample(f, p); *r = Sample(f, p + bps); return; }
    for (c = 0; c < f->Channels; c++) {
        LONG v = Sample(f, p + c * bps);
        al += v * cv->MixL[c];
        ar += v * cv->MixR[c];
    }
    *l = (al + 8192) >> 14;
    *r = (ar + 8192) >> 14;
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
            Decode(cv, cv->Carry, &l, &r);
            RsPush(rs, l, r);
            cv->CarryLen = 0;
            continue;
        }
        if (srcBytes - used >= ba) {
            Decode(cv, src + used, &l, &r);
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

/* one 16-bit-scale sample into the stream format (float: exact, integer only) */
static __inline__ void PutSample(const PCM_FORMAT *f, UCHAR *p, LONG v)
{
    if (f->Float) {
        ULONG u = 0, hi = 0, lo = 0, a = (ULONG)(v < 0 ? -v : v), e = 0;
        if (a) {
            /* v / 32768 as a float: normalise a (1..32768) */
            while (!(a & 0x8000)) { a <<= 1; e++; }  /* a in [2^15, 2^16) */
            if (f->Bits == 32) {
                u = ((ULONG)(127 - e) << 23) | ((a & 0x7FFF) << 8);
                if (v < 0) u |= 0x80000000UL;
            } else {
                hi = ((ULONG)(1023 - e) << 20) | ((a & 0x7FFF) << 5);
                if (v < 0) hi |= 0x80000000UL;
            }
        }
        if (f->Bits == 32) { p[0] = (UCHAR)u; p[1] = (UCHAR)(u >> 8); p[2] = (UCHAR)(u >> 16); p[3] = (UCHAR)(u >> 24); }
        else {
            p[0] = (UCHAR)lo; p[1] = (UCHAR)(lo >> 8); p[2] = (UCHAR)(lo >> 16); p[3] = (UCHAR)(lo >> 24);
            p[4] = (UCHAR)hi; p[5] = (UCHAR)(hi >> 8); p[6] = (UCHAR)(hi >> 16); p[7] = (UCHAR)(hi >> 24);
        }
        return;
    }
    switch (f->Bits) {
    case 8:  p[0] = (UCHAR)((v >> 8) + 128); break;
    case 16: p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); break;
    case 24: p[0] = 0; p[1] = (UCHAR)v; p[2] = (UCHAR)(v >> 8); break;
    default: p[0] = 0; p[1] = 0; p[2] = (UCHAR)v; p[3] = (UCHAR)(v >> 8); break;
    }
}

static __inline__ void Encode(const PCM_FORMAT *f, UCHAR *p, LONG l, LONG r)
{
    ULONG bps = f->Bits / 8, c;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    if (f->Channels == 1) { PutSample(f, p, (l + r) >> 1); return; }
    PutSample(f, p, l);
    PutSample(f, p + bps, r);
    for (c = 2; c < f->Channels; c++) PutSample(f, p + c * bps, 0);   /* more channels: silent */
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
