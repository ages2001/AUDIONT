#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audiont.h"
ULONG OsRegGetDword(const WCHAR *n, ULONG d) { return d; }
void OsLog(ULONG l, const char *f, ...) {}
int OsSprintf(char *b, int s, const char *f, ...) { b[0]=0; return 0; }
void OsWcsCopyA(WCHAR *d, const char *s, ULONG m) {}
void EngineKick(CARD *c) { c->Playing = 1; }
void EngineProcess(CARD *c) {}
void EngineLock(CARD *c) {}
void EngineUnlock(CARD *c) {}
LONG CtrlGain(CARD *c, ULONG line, int r) { return 32768; }
static CARD card;
static LONG mix[48000 * 2];
static void run(FILE *f, ULONG frames)
{
    while (frames) {
        ULONG n = frames > 128 ? 128 : frames, i;
        memset(mix, 0, n * 8);
        SynthRender(&card, mix, n);
        for (i = 0; i < n * 2; i++) { LONG v = mix[i]; if (v > 32767) v = 32767; if (v < -32768) v = -32768; short s = v; fwrite(&s, 2, 1, f); }
        card.WrittenFrames += n; card.PlayedFrames = card.WrittenFrames;
        frames -= n;
    }
}
static void msg(UCHAR a, UCHAR b, UCHAR c) { UCHAR m[3] = { a, b, c }; SynthMidiBytes(&card, m, (a & 0xF0) == 0xC0 ? 2 : 3); }
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "wb");
    int prog, i;
    SynthCreate(&card);
    card.LatencyFrames = 0;
    /* sequence: for selected programs play A4 (69) for 0.6 s, gap 0.2 s */
    for (i = 2; i < argc; i++) {
        prog = atoi(argv[i]);
        if (prog >= 0) { msg(0xC0, prog, 0); msg(0x90, 69, 100); run(f, 28800); msg(0x80, 69, 0); run(f, 9600); }
        else { msg(0x99, -prog, 110); run(f, 24000); }
    }
    fclose(f);
    return 0;
}
