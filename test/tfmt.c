/* Host test of the format conversion (gcc -m32 -I../drv -I../inc tfmt.c ../drv/convert.c stub.c -lm):
   WAVE_FORMAT_EXTENSIBLE PCM / float, WAVE_FORMAT_IEEE_FLOAT 32 / 64, 5.1 down-mix, 24-in-32,
   and recording into float.  Prints one line per case, "FAIL" on a wrong level or tone. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "audiont.h"
void OsLog(ULONG l, const char *f, ...) {}

static unsigned char fmtbuf[64];
static int fails;

static void put16(unsigned char *p, int v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static WAVEFORMATEX *mkfmt(int tag, int ch, int rate, int bits, int sub, unsigned mask, int *len)
{
    memset(fmtbuf, 0, sizeof(fmtbuf));
    put16(fmtbuf, tag); put16(fmtbuf + 2, ch); put32(fmtbuf + 4, rate);
    put32(fmtbuf + 8, rate * ch * bits / 8); put16(fmtbuf + 12, ch * bits / 8); put16(fmtbuf + 14, bits);
    *len = 18;
    if (tag == 0xFFFE) {
        static const unsigned char tail[12] = { 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        put16(fmtbuf + 16, 22); put16(fmtbuf + 18, bits); put32(fmtbuf + 20, mask); put32(fmtbuf + 24, sub);
        memcpy(fmtbuf + 28, tail, 12);
        *len = 40;
    }
    return (WAVEFORMATEX *)fmtbuf;
}

/* rms and dominant frequency of one output channel */
static void analyse(const LONG *out, int frames, int ch, double *rms, double *freq)
{
    int i, zc = 0;
    double s = 0;
    for (i = 4000; i < frames; i++) {
        s += (double)out[i * 2 + ch] * out[i * 2 + ch];
        if (i > 4000 && (out[(i - 1) * 2 + ch] < 0) != (out[i * 2 + ch] < 0)) zc++;
    }
    *rms = sqrt(s / (frames - 4000));
    *freq = zc / 2.0 / ((frames - 4000) / 48000.0);
}

/* play one second of the format: channel c gets a sine of freqs[c] at amplitude amps[c] (of full scale) */
static void play(const char *name, int tag, int ch, int rate, int bits, int sub, unsigned mask, const double *freqs,
                 const double *amps, double expL, double fL, double expR, double fR)
{
    int len, i, c, ba = ch * bits / 8, flt = (tag == 3 || (tag == 0xFFFE && sub == 3));
    WAVEFORMATEX *wf = mkfmt(tag, ch, rate, bits, sub, mask, &len);
    PCM_FORMAT pf;
    CONVERTER cv;
    unsigned char *src = malloc(rate * ba), *p = src;
    static LONG out[48000 * 2 + 400];
    ULONG used, total = 0, produced = 0;
    double rl, fl, rr, fr;
    if (!ConvValidFormat(wf, len, &pf)) { printf("FAIL %s: format refused\n", name); fails++; return; }
    for (i = 0; i < rate; i++)
        for (c = 0; c < ch; c++) {
            double v = amps[c] * sin(2 * M_PI * freqs[c] * i / rate);
            if (flt && bits == 32) { float f = (float)v; memcpy(p, &f, 4); p += 4; }
            else if (flt) { memcpy(p, &v, 8); p += 8; }
            else {
                long iv = lrint(v * 2147483647.0);
                if (bits == 16) { put16(p, iv >> 16); p += 2; }
                else if (bits == 24) { p[0] = iv >> 8; p[1] = iv >> 16; p[2] = iv >> 24; p += 3; }
                else if (bits == 32) { put32(p, (unsigned)iv); p += 4; }
                else { *p++ = (iv >> 24) + 128; }
            }
        }
    memset(&cv, 0, sizeof(cv));
    ConvSetup(&cv, &pf, 0);
    memset(out, 0, sizeof(out));
    while (total < (ULONG)(rate * ba) && produced < 48000 - 200) {
        ULONG chunk = 333 + (total % 911), got;
        if (chunk > rate * ba - total) chunk = rate * ba - total;
        got = ConvPlay(&cv, src + total, chunk, &used, out + produced * 2, 48000 - produced, 32768, 32768);
        produced += got; total += used;
        if (!got && !used) break;
    }
    analyse(out, produced, 0, &rl, &fl);
    analyse(out, produced, 1, &rr, &fr);
    {
        /* no step bigger than a sine of that level and frequency makes (a glitch at a buffer boundary) */
        double stepL = rl * 1.4142 * 2 * M_PI * fl / 48000 * 1.3 + 40, stepR = rr * 1.4142 * 2 * M_PI * fr / 48000 * 1.3 + 40;
        int k, glitch = 0;
        for (k = 4001; k < (int)produced; k++) {
            if (fabs((double)out[k * 2] - out[k * 2 - 2]) > stepL || fabs((double)out[k * 2 + 1] - out[k * 2 - 1]) > stepR) {
                glitch++;
            }
        }
        int ok = fabs(rl - expL) <= expL * 0.03 + 30 && fabs(rr - expR) <= expR * 0.03 + 30 &&
                 (fL == 0 || fabs(fl - fL) < fL * 0.02) && (fR == 0 || fabs(fr - fR) < fR * 0.02) && glitch == 0;
        if (glitch) printf("     %d glitches\n", glitch);
        printf("%s %-34s L rms %6.0f (%6.0f) %6.1f Hz  R rms %6.0f (%6.0f) %6.1f Hz\n", ok ? "ok  " : "FAIL", name, rl,
               expL, fl, rr, expR, fr);
        if (!ok) fails++;
    }
    free(src);
}

/* record into float32 stereo and check the float values */
static void record_float(void)
{
    int len, i;
    WAVEFORMATEX *wf = mkfmt(0xFFFE, 2, 48000, 32, 3, 3, &len);
    PCM_FORMAT pf;
    CONVERTER cv;
    static SHORT src[4800 * 2];
    static float dst[4800 * 2];
    ULONG produced = 0, n;
    double maxerr = 0;
    if (!ConvValidFormat(wf, len, &pf)) { printf("FAIL record float: refused\n"); fails++; return; }
    for (i = 0; i < 4800; i++) { src[i * 2] = (SHORT)lrint(sin(i * 0.05) * 20000); src[i * 2 + 1] = -src[i * 2]; }
    memset(&cv, 0, sizeof(cv));
    ConvSetup(&cv, &pf, 1);
    n = ConvRecord(&cv, src, 4800, (UCHAR *)dst, sizeof(dst), &produced);
    for (i = 0; i < (int)(produced / 8); i++) {
        double e = fabs(dst[i * 2] - src[i * 2] / 32768.0) + fabs(dst[i * 2 + 1] - src[i * 2 + 1] / 32768.0);
        if (e > maxerr) maxerr = e;
    }
    printf("%s record extensible float32: %u frames, max error %.6f\n", maxerr < 1e-4 && produced > 4000 * 8 ? "ok  " : "FAIL",
           produced / 8, maxerr);
    if (!(maxerr < 1e-4 && produced > 4000 * 8)) fails++;
    (void)n;
}

int main(void)
{
    static const double f2[2] = { 440, 1000 }, a2[2] = { 0.5, 0.5 };
    /* 5.1: FL 440, FR 1000, C silent, LFE loud 50 Hz (left out), BL / BR silent */
    static const double f6[6] = { 440, 1000, 300, 50, 700, 800 }, a6[6] = { 0.5, 0.5, 0, 0.9, 0, 0 };
    static const double f1[1] = { 600 }, a1[1] = { 0.5 };
    double fs = 0.5 * 32768 / sqrt(2.0);
    double k51 = 1.0 / (1 + 0.70709 + 0.70709);            /* front left's share in 5.1 */
    play("PCM 16 stereo 44.1k", 1, 2, 44100, 16, 0, 0, f2, a2, fs, 440, fs, 1000);
    play("PCM 16 mono 22.05k", 1, 1, 22050, 16, 0, 0, f1, a1, fs, 600, fs, 600);
    play("PCM 8 stereo 11.025k", 1, 2, 11025, 8, 0, 0, f2, a2, fs, 440, fs, 1000);
    play("IEEE_FLOAT 32 stereo 48k", 3, 2, 48000, 32, 0, 0, f2, a2, fs, 440, fs, 1000);
    play("IEEE_FLOAT 64 stereo 44.1k", 3, 2, 44100, 64, 0, 0, f2, a2, fs, 440, fs, 1000);
    play("EXTENSIBLE PCM 16 stereo 48k", 0xFFFE, 2, 48000, 16, 1, 3, f2, a2, fs, 440, fs, 1000);
    play("EXTENSIBLE PCM 24-in-32 stereo", 0xFFFE, 2, 48000, 32, 1, 3, f2, a2, fs, 440, fs, 1000);
    play("EXTENSIBLE float 32 stereo 44.1k", 0xFFFE, 2, 44100, 32, 3, 3, f2, a2, fs, 440, fs, 1000);
    play("EXTENSIBLE float 32 5.1 48k", 0xFFFE, 6, 48000, 32, 3, 0x3F, f6, a6, fs * k51, 440, fs * k51, 1000);
    play("PCM 16 5.1 (no mask) 48k", 1, 6, 48000, 16, 0, 0, f6, a6, fs * k51, 440, fs * k51, 1000);
    play("EXTENSIBLE float mono 22.05k", 0xFFFE, 1, 22050, 32, 3, 4, f1, a1, fs, 600, fs, 600);
    record_float();
    {
        int len;
        WAVEFORMATEX *wf = mkfmt(0xFFFE, 2, 48000, 16, 0x92, 3, &len);   /* AC-3 over S/PDIF: not ours */
        int bad = ConvValidFormat(wf, len, NULL);
        printf("%s EXTENSIBLE with a non-PCM subformat refused\n", bad ? "FAIL" : "ok  ");
        if (bad) fails++;
    }
    printf("RESULT tfmt %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
