/*
 * AUDIONT - other speaker amplifiers on an SSP port.
 *
 *   SSM4567  "INT343B"   Analog Devices, I2C 8-bit register / 8-bit value (ssm4567.c)
 *   NAU8318  "NVTN2012"  Nuvoton GPIO-only class-D amp (nau8315.c; NAU8315 "NVTN2010" behaves the same)
 *   TAS2563  "TXNW2563"  TI smart amp, I2C 8/8 with book (reg 0x7F) / page (reg 0x00) selection, run
 *                        without firmware in its ROM (DSP bypass) mode with the page-0 setup of the
 *                        Linux tas2562.c driver (which drove the TAS2563 the same way before the
 *                        firmware-based tas2781 driver took it over)
 *
 * Frame format: I2S with 2 slots, DSP_B-style TDM (frame pulse, no delay) with more slots.
 * Channels: index 0 left, 1 right (2/3 repeat), a single amp (L+R)/2 where the chip can mix
 * (TAS2563), else left (SSM4567 has no mixer).  Volume: NULL (DSP software volume).
 * TAS2781 ("TXNW2781") is not supported: it has no usable firmware-less mode in Linux (the
 * driver refuses to start a stream without the board's RCA register blob, and its page-0 map
 * differs from the TAS2562/2563 one).
 */
#include "codec.h"

static ULONG AmpChan(SST_CODEC *c)      /* 0 left, 1 right, 2 mix */
{
    if (c->Count <= 1)
        return 2;
    return c->Index & 1;
}

static const char *AmpChName(ULONG ch)
{
    return ch == 0 ? "left" : ch == 1 ? "right" : "L+R mix";
}

/* ------------------------------------------------------------------ */
/* SSM4567                                                              */
/* ------------------------------------------------------------------ */
#define S67_POWER_CTRL       0x00   /* APWDN_EN 7, BSNS/VSNS/ISNS/BOOST/AMP power-down 6..2, VBAT_ONLY 1, SPWDN 0 */
#define S67_DAC_CTRL         0x02   /* HV 7, MUTE 6, HPF 5, LPM 4, FS 2:0 */
#define S67_DAC_VOLUME       0x03
#define S67_SAI_CTRL_1       0x04   /* BCLK inv 6, TDM_BCLKS 5:4, FSYNC inv 3, LJ 2, TDM 1, PDM 0 */
#define S67_SAI_CTRL_2       0x05   /* AUTO_SLOT 3, TDM_SLOT 2:0 */
#define S67_SAI_PLACEMENT_1  0x06
#define S67_SAI_PLACEMENT_2  0x07
#define S67_SAI_PLACEMENT_3  0x08
#define S67_MCLK_RATIO       0x14
#define S67_BOOST_CTRL_1     0x15
#define S67_SOFT_RESET       0xFF

/*
 * No ID register: Probe soft-resets the chip and checks the reset defaults
 * of registers no other setting touches (SAI placement 1..3 = 01 20 32, MCLK
 * ratio 0x11, boost control 0x03).  The soft reset is harmless: the Linux
 * driver resets at probe and on every power-up.
 */
static BOOLEAN S67Probe(SST_CODEC *c)
{
    ULONG a, b, d, e, f;
    if (!CWr(c, S67_SOFT_RESET, 0x00))
        return FALSE;
    if (!CRd(c, S67_SAI_PLACEMENT_1, &a) || !CRd(c, S67_SAI_PLACEMENT_2, &b) || !CRd(c, S67_SAI_PLACEMENT_3, &d) ||
        !CRd(c, S67_MCLK_RATIO, &e) || !CRd(c, S67_BOOST_CTRL_1, &f))
        return FALSE;
    return a == 0x01 && b == 0x20 && d == 0x32 && e == 0x11 && f == 0x03;
}

/* SAI / DAC setup (Priv[0] = SAI_CTRL_1, Priv[1] = SAI_CTRL_2, Priv[2] = DAC_CTRL without MUTE) */
static BOOLEAN S67Config(SST_CODEC *c, BOOLEAN mute)
{
    return CWr(c, S67_SAI_CTRL_1, c->Priv[0]) &&
           CWr(c, S67_SAI_CTRL_2, c->Priv[1]) &&
           CWr(c, S67_DAC_CTRL, c->Priv[2] | (mute ? 0x40 : 0x00));
}

static BOOLEAN S67Init(SST_CODEC *c)
{
    ULONG ch = AmpChan(c), slot = ch == 2 ? 0 : ch, fs;

    if (c->Rate >= 8000 && c->Rate <= 12000) fs = 0;
    else if (c->Rate >= 16000 && c->Rate <= 24000) fs = 1;
    else if (c->Rate >= 32000 && c->Rate <= 48000) fs = 2;
    else if (c->Rate >= 64000 && c->Rate <= 96000) fs = 3;
    else if (c->Rate >= 128000 && c->Rate <= 192000) fs = 4;
    else {
        LOGE("SSM4567 %s: rate %u not supported\n", c->Path, c->Rate);
        return FALSE;
    }

    if (c->Slots > 2) {
        /* TDM, DSP_B (TDM | LJ); TDM_BCLKS: 32 / 48 / 64 BCLKs per slot */
        if (c->Slot != 32) {
            LOGE("SSM4567 %s: TDM needs 32-bit slots (have %u)\n", c->Path, c->Slot);
            return FALSE;
        }
        c->Priv[0] = 0x02 | 0x04;
    } else {
        c->Priv[0] = 0x00;                      /* I2S, normal BCLK / FSYNC polarity */
    }
    c->Priv[1] = slot & 7;                      /* fixed slot (no auto slot): 0 left, 1 right */
    c->Priv[2] = 0x20 | fs;                     /* HPF on (reset default), DAC FS */

    if (!CWr(c, S67_SOFT_RESET, 0x00) ||
        !S67Config(c, TRUE) ||
        !CWr(c, S67_DAC_VOLUME, 0x40) ||        /* 0 dB */
        !CWr(c, S67_POWER_CTRL, 0xFD))          /* everything powered down, SPWDN */
        return FALSE;

    c->Caps = CC_PLAY;
    LOGI("SSM4567 %s: %s (slot %u), %u x %u-bit %s\n", c->Path, AmpChName(slot), slot, c->Slots, c->Slot,
         c->Slots > 2 ? "TDM" : "I2S");
    return TRUE;
}

static void S67Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    if (on) {
        /* ssm4567_set_power(true): soft reset, leave SPWDN, restore the setup */
        CWr(c, S67_SOFT_RESET, 0x00);
        S67Config(c, TRUE);
        CWr(c, S67_DAC_VOLUME, 0x40);
        CWr(c, S67_POWER_CTRL, 0xFC);           /* SPWDN off, blocks still down */
        /* DAPM: "DAC" (AMP_PWDN) and "Amplifier Boost" (BOOST_PWDN) on; sense blocks stay down */
        CWr(c, S67_POWER_CTRL, 0xF0);
        /* mute_stream(0) */
        CUpd(c, S67_DAC_CTRL, 0x40, 0x00);
    } else {
        CUpd(c, S67_DAC_CTRL, 0x40, 0x40);
        CWr(c, S67_POWER_CTRL, 0xFC);
        CWr(c, S67_POWER_CTRL, 0xFD);           /* bias OFF: SPWDN */
    }
}

static void S67Shutdown(SST_CODEC *c)
{
    CUpd(c, S67_DAC_CTRL, 0x40, 0x40);
    CWr(c, S67_POWER_CTRL, 0xFD);
}

const CODEC_DRV g_DrvSsm4567 = { "SSM4567", "INT343B\0", CODEC_AMP, 1, 1,
                                 S67Probe, S67Init, S67Stream, NULL, S67Shutdown };

/* ------------------------------------------------------------------ */
/* NAU8318 / NAU8315: GPIO-only                                         */
/* ------------------------------------------------------------------ */
/*
 * "enable" (ACPI GpioIo 0) high = on (nau8315_daiops_trigger raises it at
 * trigger start, drops it at stop); no control port, channel strapped on the
 * board.  Probe always succeeds (the GPIO is optional in Linux).
 */
static BOOLEAN N318Probe(SST_CODEC *c)
{
    (void)c;
    return TRUE;
}

static BOOLEAN N318Init(SST_CODEC *c)
{
    CGpio(c, 0, FALSE);
    c->Caps = CC_PLAY;
    LOGI("%s: GPIO-only amp, enable %s\n", c->Hid, c->NGpio ? "GpioIo 0" : "hard-wired");
    return TRUE;
}

static void N318Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    CGpio(c, 0, on);
}

static void N318Shutdown(SST_CODEC *c)
{
    CGpio(c, 0, FALSE);
}

const CODEC_DRV g_DrvNau8318 = { "NAU8318", "NVTN2012\0NVTN2010\0", CODEC_AMP, 0, 0,
                                 N318Probe, N318Init, N318Stream, NULL, N318Shutdown };

/* ------------------------------------------------------------------ */
/* TAS2563 (book 0 / page 0 registers only)                             */
/* ------------------------------------------------------------------ */
#define T63_PAGE             0x00   /* page select (every page) */
#define T63_SW_RESET         0x01
#define T63_PWR_CTRL         0x02   /* ISNS_PD 3, VSNS_PD 2, MODE 1:0 (0 active, 1 mute, 2 shutdown) */
#define T63_PB_CFG1          0x03
#define T63_TDM_CFG0         0x06   /* RAMP_RATE 5, SAMP_RATE 3:1, FRAME_START 0 (1: falling edge) */
#define T63_TDM_CFG1         0x07   /* RX_OFFSET 5:1, RX_EDGE 0 */
#define T63_TDM_CFG2         0x08   /* RX_SCFG 5:4 (I2C offset, L, R, (L+R)/2), RX_WLEN 3:2, RX_SLEN 1:0 */
#define T63_TDM_CFG3         0x09   /* right slot 7:4, left slot 3:0 */
#define T63_TDM_CFG5         0x0B   /* VSNS_TX 6, slot 5:0 */
#define T63_TDM_CFG6         0x0C   /* ISNS_TX 6, slot 5:0 */
#define T63_BOOK             0x7F   /* book select (page 0) */
#define T63_REV_ID           0x7D

static BOOLEAN T63Book0(SST_CODEC *c)
{
    return CWr(c, T63_PAGE, 0x00) && CWr(c, T63_BOOK, 0x00) && CWr(c, T63_PAGE, 0x00);
}

/*
 * The chip has no documented part-number register in the Linux drivers;
 * Probe selects book 0 / page 0 and requires the revision register and the
 * PWR_CTRL mode field to read back sensibly.
 */
static BOOLEAN T63Probe(SST_CODEC *c)
{
    ULONG rev, pwr;
    if (!T63Book0(c))
        return FALSE;
    if (!CRd(c, T63_REV_ID, &rev) || !CRd(c, T63_PWR_CTRL, &pwr))
        return FALSE;
    return rev != 0xFF && (pwr & 0x03) != 0x03;
}

static BOOLEAN T63Init(SST_CODEC *c)
{
    ULONG ch = AmpChan(c), sr, slen, wlen, rev = 0, scfg;

    switch (c->Rate) {
    case 8000:   sr = 0; break;
    case 16000:  sr = 1; break;
    case 24000:  sr = 2; break;
    case 32000:  sr = 3; break;
    case 48000:  sr = 4; break;
    case 96000:  sr = 5; break;
    case 192000: sr = 6; break;
    default:
        LOGE("TAS2563 %s: rate %u not supported\n", c->Path, c->Rate);
        return FALSE;
    }
    switch (c->Slot) {
    case 16: slen = 0; break;
    case 24: slen = 1; break;
    case 32: slen = 2; break;
    default:
        LOGE("TAS2563 %s: %u-bit slots not supported\n", c->Path, c->Slot);
        return FALSE;
    }
    wlen = c->Valid >= 32 ? 3 : c->Valid >= 24 ? 2 : c->Valid >= 20 ? 1 : 0;
    scfg = ch == 0 ? 1 : ch == 1 ? 2 : 3;

    if (!T63Book0(c) || !CWr(c, T63_SW_RESET, 0x01))
        return FALSE;
    CDelay(2);
    if (!T63Book0(c) || !CRd(c, T63_REV_ID, &rev))
        return FALSE;
    {
        const ULONG seq[] = {
            T63_PWR_CTRL, 0x0E,                                 /* shutdown, I/V sense powered down */
            /* hw_params: 48 kHz family, no 44.1 ramp; I2S: frame starts on the falling FSYNC edge */
            T63_TDM_CFG0, (sr << 1) | (c->Slots > 2 ? 0x00 : 0x01),
            /* set_dai_fmt: RX offset 1 for I2S, 0 for DSP_B; rising-edge latch */
            T63_TDM_CFG1, (c->Slots > 2 ? 0 : 1) << 1,
            /* ASI1 source + word / slot length */
            T63_TDM_CFG2, (scfg << 4) | (wlen << 2) | slen,
            T63_TDM_CFG3, (1 << 4) | 0,                         /* right = slot 1, left = slot 0 */
            T63_TDM_CFG5, 0x02,                                 /* V sense not transmitted */
            T63_TDM_CFG6, 0x00,                                 /* I sense not transmitted */
        };
        if (!CSeq(c, seq, sizeof(seq) / sizeof(seq[0]) / 2))
            return FALSE;
    }
    c->Caps = CC_PLAY;
    LOGI("TAS2563 %s: rev %02x, ROM mode (no firmware), %s, %u x %u-bit %s\n", c->Path, rev, AmpChName(ch),
         c->Slots, c->Slot, c->Slots > 2 ? "TDM" : "I2S");
    return TRUE;
}

static void T63Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    T63Book0(c);
    if (on) {
        /* tas2562_dac_event POST_PMU (mute), then mute_stream(0) (active) */
        CUpd(c, T63_PWR_CTRL, 0x03, 0x01);
        CUpd(c, T63_PWR_CTRL, 0x03, 0x00);
    } else {
        CUpd(c, T63_PWR_CTRL, 0x03, 0x01);
        CUpd(c, T63_PWR_CTRL, 0x03, 0x02);
    }
}

static void T63Shutdown(SST_CODEC *c)
{
    T63Book0(c);
    CUpd(c, T63_PWR_CTRL, 0x03, 0x02);
}

const CODEC_DRV g_DrvTas2563 = { "TAS2563", "TXNW2563\0", CODEC_AMP, 1, 1,
                                 T63Probe, T63Init, T63Stream, NULL, T63Shutdown };
