/*
 * AUDIONT - Realtek RT1308 SoundWire stereo amplifier (non-SDCA), Linux
 * sound/soc/codecs/rt1308-sdw.c, rt1308-sdw.h, rt1308.h.
 *
 *   DP1 (2 channels) -> DATA_PATH channel select -> DAC L / R (DAC_SET mutes) -> class D -> SPOL / SPOR
 *
 * The I2S register map is reached at 0xc000 | (reg << 4) | byte (RT1308_SDW_OFFSET; byte 0 is
 * bits 7:0 of the 32-bit register, byte 3 bits 31:24).
 * Init (rt1308_io_init): software reset, hardware version (0xc710; VER_C differs in two
 * settings), the initial settings, the hibernation flag 0xcf01 set; rt1308_bus_config: the
 * bus clock code in SCP registers 0xe0 / 0xf0 (both banks).  The "realtek,bq-params" ACPI
 * biquad table of Linux is not applied (it is not in the BIOS of the reference designs).
 * Channel select ("RX Channel Select", DATA_PATH bits 1:0: 0 LR, 1 LL, 2 RL, 3 RR): one
 * amplifier LR, with two or more the even Index LL and the odd RR (UCM "rt1308-1/-2").
 * Stream on, in DAPM order: the supplies (POWER bytes 0..2: MBIAS20U, ALDO, DBG, DACL, DAC
 * power, CLK25M, ADC L / R, DLDO, VREF, MIXER L / R, MBIAS4U, PLL2), DAC L / R unmuted
 * (autodisable switches), CLASS D POST_PMU: 30 ms, POWER_STATUS 1:0 on, 40 ms, efuse read
 * (rt1308_apply_calib_params); off: CLASS D PRE_PMD (POWER_STATUS off, 150 ms), mutes, supplies.
 */
#include "sdw.h"

#define R(reg, byte)        (0xc000UL | ((ULONG)(reg) << 4) | (byte))
#define RT1308_RESET        0x00
#define RT1308_DATA_PATH    0x07
#define RT1308_DAC_SET      0x10
#define RT1308_POWER        0x32
#define RT1308_POWER_STATUS 0x36
#define DVOL_MUTE_LR        0xC0        /* DAC_SET byte 3: DVOL_MUTE_R_EN 7, DVOL_MUTE_L_EN 6 */
#define VER_C               2
/* the DAPM supplies of the DAC (POWER bytes 0..2) */
#define PWR0    0xFF            /* MBIAS20U 7, ALDO 6, DBG 5, DACL 4, DAC Power 3, CLK25M 2, ADC_R 1, ADC_L 0 */
#define PWR1    0x37            /* DLDO 5, VREF 4, MIXER_R 2, MIXER_L 1, MBIAS4U 0 */
#define PWR2    0x1F            /* PLL2_LDO 4, PLL2B 3, PLL2F 2, PLL2F2 1, PLL2B2 0 */

/* Priv[] */
#define P_VER   0
#define P_ON    1

BOOLEAN SdwAmpClocks(const SDW_DEV *d, ULONG *mclk, ULONG *bus);   /* sdw_max98373.c */

static BOOLEAN Probe(SDW_DEV *d)
{
    d->Kind = SDW_KIND_AMP;
    d->Play.Num = 1; d->Play.Ch = 2; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = FALSE;   /* simple_ch_prep_sm */
    memset(&d->Rec, 0, sizeof(d->Rec));                 /* no source ports */
    d->Paging = TRUE;
    return TRUE;
}

/* rt1308_clock_config: the bus clock (curr_dr_freq / 2) */
static LONG ClockCode(ULONG bus)
{
    switch (bus) {
    case 12000000: return 0x0;
    case 6000000:  return 0x1;
    case 9600000:  return 0x2;
    case 4800000:  return 0x3;
    case 2400000:  return 0x4;
    case 12288000: return 0x5;
    }
    return -1;
}

static BOOLEAN Init(SDW_DEV *d)
{
    static const ULONG init[] = {
        0xc103, 0xc0, 0xc030, 0x17, 0xc031, 0x81, 0xc032, 0x26, 0xc040, 0x80, 0xc041, 0x80, 0xc042, 0x06,
        0xc052, 0x0a, 0xc080, 0x0a, 0xc060, 0x02, 0xc061, 0x75, 0xc062, 0x05, 0xc171, 0x07, 0xc173, 0x0d,
    };
    static const ULONG init2[] = {
        0xc900, 0x5a, 0xc1a0, 0x84, 0xc1a1, 0x01, 0xc360, 0x78, 0xc361, 0x87, 0xc0a1, 0x71, 0xc210, 0x00,
        0xc070, 0x00, 0xc100, 0xd7, 0xc101, 0xd7,
    };
    ULONG e0 = d->Errors, mclk = 0, bus = 0;
    LONG code;
    UCHAR ver = 0, sel = 0;                             /* "LR" */
    if (!SdwWrite(d, R(RT1308_RESET, 0), 0)) return FALSE;     /* sw reset */
    SdwRead(d, 0xc710, &ver);
    d->Priv[P_VER] = ver;
    SdwSeq(d, init, ARRAYSIZE(init) / 2);
    if (ver == VER_C) { SdwWrite(d, 0xc311, 0x7f); SdwWrite(d, 0xc300, 0x09); }
    else { SdwWrite(d, 0xc311, 0x4f); SdwWrite(d, 0xc300, 0x0b); }
    SdwSeq(d, init2, ARRAYSIZE(init2) / 2);
    SdwWrite(d, 0xcf01, 0x01);                          /* hibernation flag: preset done */
    /* rt1308_bus_config */
    code = SdwAmpClocks(d, &mclk, &bus) ? ClockCode(bus) : -1;
    if (code >= 0) { SdwWrite(d, 0xe0, (UCHAR)code); SdwWrite(d, 0xf0, (UCHAR)code); }
    else LOGE("sdw: %s: bus clock %u Hz not supported by the RT1308 (0xe0 / 0xf0 left at reset)", d->Name, bus);
    if (d->Count >= 2) sel = (d->Index & 1) ? 3 : 1;    /* "RR" / "LL" */
    SdwUpdate(d, R(RT1308_DATA_PATH, 0), 0x03, sel);
    d->Priv[P_ON] = FALSE;
    LOGI("sdw: %s: RT1308 hw ver %u, bus %u Hz (code %d), %s (amplifier %u of %u), %u bus errors in init", d->Name, ver,
         bus, code, sel == 3 ? "RR" : sel == 1 ? "LL" : "LR", d->Index, d->Count, d->Errors - e0);
    return d->Errors - e0 < 8;
}

/* rt1308_apply_calib_params: the efuse read (Linux only logs the values) */
static void Calib(SDW_DEV *d)
{
    UCHAR b[8];
    ULONG i;
    static const USHORT regs[8] = { 0xc860, 0xc861, 0xc862, 0xc863, 0xc870, 0xc871, 0xc872, 0xc873 };
    SdwWrite(d, 0xc7f0, 0x04);
    SdwWrite(d, 0xc7f1, 0xfe);
    SdwDelay(100);
    SdwWrite(d, 0xc7f0, 0x44);
    SdwDelay(20);
    SdwWrite(d, 0xc240, 0x10);
    for (i = 0; i < 8; i++) { b[i] = 0; SdwRead(d, regs[i], &b[i]); }
    LOGV("sdw: %s: efuse m_btl_l %04x c_btl_l %04x m_btl_r %04x c_btl_r %04x", d->Name, (b[0] << 8) | b[1],
         (b[2] << 8) | b[3], (b[4] << 8) | b[5], (b[6] << 8) | b[7]);
}

static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    if (rec) return;
    if (on) {
        SdwUpdate(d, R(RT1308_POWER, 0), PWR0, PWR0);
        SdwUpdate(d, R(RT1308_POWER, 1), PWR1, PWR1);
        SdwUpdate(d, R(RT1308_POWER, 2), PWR2, PWR2);
        SdwUpdate(d, R(RT1308_DAC_SET, 3), DVOL_MUTE_LR, 0);
        SdwDelay(30);
        SdwUpdate(d, R(RT1308_POWER_STATUS, 0), 0x03, 0x03);
        SdwDelay(40);
        Calib(d);
    } else {
        SdwUpdate(d, R(RT1308_POWER_STATUS, 0), 0x03, 0);
        SdwDelay(150);
        SdwUpdate(d, R(RT1308_DAC_SET, 3), DVOL_MUTE_LR, DVOL_MUTE_LR);
        SdwUpdate(d, R(RT1308_POWER, 2), PWR2, 0);
        SdwUpdate(d, R(RT1308_POWER, 1), PWR1, 0);
        SdwUpdate(d, R(RT1308_POWER, 0), PWR0, 0);
    }
    d->Priv[P_ON] = on;
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    if (d->Priv[P_ON]) Stream(d, FALSE, FALSE);
}

static const USHORT g_Parts[] = { 0x1308, 0 };
const SDW_DRV g_SdwRt1308 = { "RT1308", 0x025D, g_Parts, SDW_KIND_AMP, Probe, Init, Stream, Shutdown };
