/*
 * AUDIONT - Realtek RT1316 SoundWire smart amplifier, Linux
 * sound/soc/codecs/rt1316-sdw.c.
 *
 *   DP1 (2 channels) -> UDMPU21 (channel select) -> FU21 -> class D
 *   power domains PDE 23 / 27 / 22 (amplifier), PDE 24 (I/V sense, DP2: unused)
 *
 * Init: software reset (0xc000 = 2) and the blind-write table of
 * rt1316_io_init (XU24 bypass off, I/V sense setup); the UDMPU21 cluster
 * selects the channel: with two amplifiers the first plays L,L and the second
 * R,R (the ALSA UCM "rt1316-N RX Channel Select" setting), one plays L,R.
 * Stream: PDEs to PS0 then FU21 unmuted (DAPM: CLASS D POST_PMU after the
 * DAC switch); off in reverse.
 */
#include "sdw.h"

#define FN_AMP          0x04
#define ENT_PDE23       0x31
#define ENT_PDE27       0x32
#define ENT_PDE22       0x33
#define ENT_PDE24       0x34
#define ENT_XU24        0x24
#define ENT_FU21        0x03
#define ENT_UDMPU21     0x02
#define CTL_BYPASS      0x01
#define CTL_UDMPU_CLUSTER 0x10

static const ULONG g_Blind[] = {
    0xc710, 0x17, 0xc711, 0x80, 0xc712, 0x26, 0xc713, 0x06, 0xc714, 0x80, 0xc715, 0x06,
    0xc702, 0x0a, 0xc703, 0x0a, 0xc001, 0x45, 0xc003, 0x00, 0xc004, 0x11, 0xc005, 0x00,
    0xc006, 0x00, 0xc106, 0x00, 0xc007, 0x11, 0xc008, 0x11, 0xc009, 0x00,
    0x2f0a, 0x00, 0xd101, 0xf0, 0xd103, 0x9b, 0x2f36, 0x8e, 0x3206, 0x80, 0x3211, 0x0b,
    0x3216, 0x06, 0xc614, 0x20, 0xc615, 0x0a, 0xc616, 0x02, 0xc617, 0x00, 0xc60b, 0x10,
    0xc60e, 0x05, 0xc102, 0x00, 0xc090, 0xb0, 0xc00f, 0x01, 0xc09c, 0x7b,
    0xc602, 0x07, 0xc603, 0x07, 0xc0a3, 0x71, 0xc00b, 0x30, 0xc093, 0x80, 0xc09d, 0x80,
    0xc0b0, 0x77, 0xc010, 0xa5, 0xc050, 0x83, 0x2f55, 0x03, 0x3217, 0xb5, 0x3202, 0x02,
    SDW_SDCA_CTL(FN_AMP, ENT_XU24, CTL_BYPASS, 0), 0x00,
    /* I/V sense */
    0x2232, 0x80, 0xc0b0, 0x77, 0xc011, 0x00, 0xc020, 0x00, 0xc023, 0x00, 0x3101, 0x00,
    0x3004, 0xa0, 0x3005, 0xb1, 0xc007, 0x11, 0xc008, 0x11, 0xc009, 0x00, 0xc022, 0xd6,
    0xc025, 0xd6,
    0xd001, 0x03, 0xd002, 0xbf, 0xd003, 0x03, 0xd004, 0xbf,
};

static BOOLEAN Probe(SDW_DEV *d)
{
    d->Kind = SDW_KIND_AMP;
    d->Play.Num = 1; d->Play.Ch = 2; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = FALSE;
    memset(&d->Rec, 0, sizeof(d->Rec));                 /* DP2 I/V feedback not used */
    d->Paging = TRUE;
    return TRUE;
}

static void Power(SDW_DEV *d, ULONG ps)
{
    SdcaPower(d, FN_AMP, ENT_PDE23, ps, 0);
    SdcaPower(d, FN_AMP, ENT_PDE27, ps, 0);
    SdcaPower(d, FN_AMP, ENT_PDE22, ps, 0);
}

static BOOLEAN Init(SDW_DEV *d)
{
    ULONG e0 = d->Errors;
    UCHAR sel = 0;                                      /* "L,R" */
    if (!SdwWrite(d, 0xc000, 0x02)) return FALSE;       /* software reset */
    SdwSeq(d, g_Blind, ARRAYSIZE(g_Blind) / 2);
    if (d->Count == 2) sel = d->Index ? 5 : 1;          /* "R,R" / "L,L" */
    SdwWrite(d, SDW_SDCA_CTL(FN_AMP, ENT_UDMPU21, CTL_UDMPU_CLUSTER, 0), sel);
    SdcaMute(d, FN_AMP, ENT_FU21, 3, TRUE);
    Power(d, SDCA_PS3);
    SdcaPower(d, FN_AMP, ENT_PDE24, SDCA_PS3, 0);
    LOGI("sdw: %s: RT1316 %s, %u bus errors in init", d->Name, d->Count == 2 ? (d->Index ? "right" : "left") : "stereo",
         d->Errors - e0);
    return d->Errors - e0 < 8;
}

static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    if (rec) return;
    if (on) {
        Power(d, SDCA_PS0);
        SdcaMute(d, FN_AMP, ENT_FU21, 3, FALSE);
    } else {
        SdcaMute(d, FN_AMP, ENT_FU21, 3, TRUE);
        Power(d, SDCA_PS3);
    }
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    SdcaMute(d, FN_AMP, ENT_FU21, 3, TRUE);
    Power(d, SDCA_PS3);
}

static const USHORT g_Parts[] = { 0x1316, 0 };
const SDW_DRV g_SdwRt1316 = { "RT1316", 0x025D, g_Parts, SDW_KIND_AMP, Probe, Init, Stream, Shutdown };
