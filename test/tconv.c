#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "audiont.h"
void OsLog(ULONG l, const char *f, ...) {}
int main(int argc, char **argv)
{
    int rate = atoi(argv[1]), bits = atoi(argv[2]), ch = atoi(argv[3]); double f = atof(argv[4]);
    int frames = rate, ba = ch * bits / 8, i, c;
    unsigned char *src = malloc(frames * ba), *p = src;
    static LONG out[48000 * 2 * 2];
    CONVERTER cv; PCM_FORMAT pf = { rate, ch, bits, ba, 0 };
    ULONG used, total = 0, produced = 0;
    for (i = 0; i < frames; i++) {
        int v = (int)lrint(sin(2 * M_PI * f * i / rate) * 16000);
        for (c = 0; c < ch; c++) {
            if (bits == 8) *p++ = (v >> 8) + 128;
            else if (bits == 16) { *p++ = v; *p++ = v >> 8; }
            else if (bits == 24) { *p++ = 0; *p++ = v; *p++ = v >> 8; }
            else { *p++ = 0; *p++ = 0; *p++ = v; *p++ = v >> 8; }
        }
    }
    memset(&cv, 0, sizeof(cv)); ConvSetup(&cv, &pf, 0);
    /* feed in odd-sized chunks */
    while (total < (ULONG)(frames * ba)) {
        ULONG chunk = 1000 + (total % 777);
        if (chunk > frames * ba - total) chunk = frames * ba - total;
        while (1) {
            ULONG got = ConvPlay(&cv, src + total, chunk, &used, out + produced * 2, 97, 32768, 32768);
            produced += got; total += used; chunk -= used;
            if (!got) break;
        }
        if (chunk) { total += 0; }
        if (produced > 48000 * 2 - 200) break;
    }

    for (i = 0; i < (int)produced * 2; i++) { short s = out[i]; fwrite(&s, 2, 1, stdout); }
    fprintf(stderr, "\nproduced %u frames, consumed %u of %u bytes\n", produced, total, frames * ba);
    return 0;
}
