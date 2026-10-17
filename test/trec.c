#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "audiont.h"
int main(int argc, char **argv)
{
    int rate = atoi(argv[1]), bits = atoi(argv[2]), ch = atoi(argv[3]); double f = atof(argv[4]);
    int ba = ch * bits / 8, i;
    static SHORT src[48000 * 2];
    static UCHAR dst[200000 * 8];
    CONVERTER cv; PCM_FORMAT pf = { rate, ch, bits, ba, 0 };
    ULONG total = 0, used = 0, produced, n;
    for (i = 0; i < 48000; i++) { src[i * 2] = (SHORT)lrint(sin(2 * M_PI * f * i / 48000) * 16000); src[i * 2 + 1] = src[i * 2]; }
    memset(&cv, 0, sizeof(cv)); ConvSetup(&cv, &pf, 1);
    /* odd destination buffer sizes to exercise partial frames */
    while (used < 48000) {
        ULONG room = 777 + (total % 333);
        n = ConvRecord(&cv, src + used * 2, (48000 - used) > 500 ? 500 : 48000 - used, dst + total, room, &produced);
        used += n; total += produced;
        if (!n && !produced) break;
    }
    fwrite(dst, 1, total, stdout);
    fprintf(stderr, "bytes %u frames %u (expect ~%d)\n", total, total / ba, rate);
    return 0;
}
