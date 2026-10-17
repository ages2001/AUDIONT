/*
 * AUDIONT - Maxim speaker amplifiers on an SSP port.
 *
 *   MAX98357A / MAX98360A  GPIO-only I2S class-D amp (max98357a.c)
 *   MAX98373               I2C, 16-bit register / 8-bit value (max98373.c, max98373-i2c.c)
 *   MAX98390               I2C, 16/8, boosted amp with DSM (max98390.c)
 *   MAX98927               I2C, 16/8, boosted amp (max98927.c)
 *
 * Channel assignment (all I2C Maxim amps): the "DAI Sel" mux of the Linux driver
 * (PCM_TO_SPK_MONOMIX_CFG: 0 Left, 1 Right, 2 (L+R)/2) picks the channel; with
 * c->Count > 1 index 0 plays left, 1 right, 2 left, 3 right (the Linux Intel
 * machine drivers put the woofer and the tweeter pair of a 4-amp board on the
 * same two slots: max_98390_tdm_mask rx = 0x3 for all four, the left/right
 * choice comes from UCM via "DAI Sel"); a single amp plays (L+R)/2.  In TDM
 * (c->Slots > 2) the amps take the stream from slots 0 and 1 (rx mask 0x3 in
 * every Intel board: sof_maxim_common.c, avs/boards/max98373.c / max98927.c).
 * NOTE: the Linux boards name the first ACPI instance (MX98373:00, MX98927:00,
 * MX98390:00 except on CML) "Right" - the core decides which device gets
 * Index 0; this file follows "Index 0 = left".
 *
 * Frame format: c->Slots == 2 -> I2S (Philips); c->Slots > 2 -> TDM mode 0
 * (DSP_B, frame pulse at the start of slot 0, no delay) as the KBL/SKL boards
 * use for MAX98373/MAX98927 (SND_SOC_DAIFMT_DSP_B | NB_NF, 8 x 16-bit slots).
 * All chips are BCLK-clocked consumers (no MCLK pin in consumer mode); the
 * BSEL field must match the BCLKs per frame (c->Slots * c->Slot), a 25-bit slot
 * width is not supported by these chips.
 *
 * The IV-sense / feedback transmitters stay Hi-Z (PCM_TX_HIZ = 0xFF,
 * PCM_TX_EN = 0): several amps share the SSP RX line and nothing captures
 * the feedback here.  Volume: NULL (the DSP's software volume); the digital
 * gain stays at the Linux driver's default.
 *
 * GPIOs: only the MAX98357A/60A "sdmode" line is driven (Linux: sdmode-gpios,
 * the only GpioIo of the device, index 0).  The optional reset lines of
 * MAX98373 ("maxim,reset"), MAX98390 / MAX98927 ("reset") are _DSD-named
 * lines that the ACPI tables of the Intel boards normally do not have; they
 * are not driven.
 */
#include "codec.h"

/* ------------------------------------------------------------------ */
/* shared helpers                                                       */
/* ------------------------------------------------------------------ */

/* 0 left, 1 right, 2 (L+R)/2 - the "DAI Sel" / MONOMIX_CFG encoding */
static ULONG MaxChan(SST_CODEC *c)
{
    if (c->Count <= 1)
        return 2;
    return c->Index & 1;
}

/* PCM_SR_SET1_SR code (8000 .. 96000), 0xFF unsupported */
static ULONG MaxSr(ULONG rate, ULONG maxRate)
{
    static const ULONG tab[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000 };
    ULONG i;
    if (rate > maxRate)
        return 0xFF;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (tab[i] == rate)
            return i;
    return 0xFF;
}

/* PCM_CLK_SETUP BSEL: index into the chip's BCLKs-per-frame table + 2 (max98xxx_get_bclk_sel) */
static ULONG MaxBsel(const USHORT *tab, ULONG n, ULONG ratio)
{
    ULONG i;
    for (i = 0; i < n; i++)
        if (tab[i] == ratio)
            return i + 2;
    return 0;
}

/* PCM_MODE_CFG CHANSZ (bits 7:6): 16 / 24 / 32-bit slots */
static ULONG MaxChanSz(ULONG slot)
{
    switch (slot) {
    case 16: return 1 << 6;
    case 24: return 2 << 6;
    case 32: return 3 << 6;
    default: return 0;
    }
}

/* PCM_MODE_CFG FORMAT (bits 5:3): 0 I2S, 3 TDM mode 0 (DSP_B) */
static ULONG MaxFormat(SST_CODEC *c)
{
    return (c->Slots > 2 ? 3 : 0) << 3;
}

static const char *MaxChName(ULONG ch)
{
    return ch == 0 ? "left" : ch == 1 ? "right" : "L+R mix";
}

/* ------------------------------------------------------------------ */
/* MAX98357A / MAX98360A: GPIO-only                                     */
/* ------------------------------------------------------------------ */
/*
 * No control interface.  SD_MODE (ACPI GpioIo 0, "sdmode-gpios") low = shutdown,
 * high = on; the channel (left, right, (L+R)/2) is selected by the board's
 * SD_MODE pull-up resistor, not by software.  Probe always succeeds: the
 * device is present when ACPI lists it, and boards that hard-wire SD_MODE
 * have no GpioIo (sdmode is optional in Linux).  Linux raises SD_MODE at
 * trigger start (sdmode-delay 0 by default) and drops it at trigger stop; here
 * it goes high just before the SSP starts and low after it stopped: the chip
 * stays muted while BCLK is absent and its click/pop suppression covers the
 * clock start.
 */
static BOOLEAN M357Probe(SST_CODEC *c)
{
    (void)c;
    return TRUE;
}

static BOOLEAN M357Init(SST_CODEC *c)
{
    CGpio(c, 0, FALSE);
    c->Caps = CC_PLAY;
    LOGI("%s: GPIO-only amp, sdmode %s\n", c->Hid, c->NGpio ? "GpioIo 0" : "hard-wired");
    return TRUE;
}

static void M357Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    CGpio(c, 0, on);
}

static void M357Shutdown(SST_CODEC *c)
{
    CGpio(c, 0, FALSE);
}

const CODEC_DRV g_DrvMax98357a = { "MAX98357A", "MX98357A\0MX98360A\0", CODEC_AMP, 0, 0,
                                   M357Probe, M357Init, M357Stream, NULL, M357Shutdown };

/* ------------------------------------------------------------------ */
/* MAX98373                                                             */
/* ------------------------------------------------------------------ */
#define M373_SW_RESET        0x2000
#define M373_TX_HIZ_1        0x2020     /* PCM_TX_HIZ_EN_1 */
#define M373_TX_HIZ_2        0x2021
#define M373_TX_SRC_1        0x2022
#define M373_TX_SRC_2        0x2023
#define M373_DATA_FMT        0x2024     /* PCM_DATA_FMT_CFG: CHANSZ 7:6, FORMAT 5:3, TX interleave 2 */
#define M373_CLK_RATIO       0x2026     /* PCM_CLOCK_RATIO: BCLKEDGE 4, BSEL 3:0 */
#define M373_SR_1            0x2027     /* PCM_SR_SETUP_1: SR 3:0 */
#define M373_SR_2            0x2028     /* PCM_SR_SETUP_2: SPK SR 7:4, IVADC SR 3:0 */
#define M373_MONOMIX_1       0x2029     /* PCM_TO_SPK_MONO_MIX_1: CFG 7:6, CH0 slot 3:0 */
#define M373_MONOMIX_2       0x202A     /* CH1 slot */
#define M373_RX_EN           0x202B     /* PCM_RX_EN (DAPM "Amp Enable") */
#define M373_TX_EN           0x202C
#define M373_DIG_VOL         0x203D
#define M373_DSP_CFG         0x203F
#define M373_IV_DSP_CFG      0x2046
#define M373_IV_ADC_EN       0x2047
#define M373_AMP_EN          0x2043     /* SPKFB_EN 1, SPK_EN 0 */
#define M373_AUTO_RESTART    0x20FE
#define M373_GLOBAL_SHDN     0x20FF     /* GLOBAL_EN 0 */
#define M373_REV_ID          0x21FF

static const USHORT g_M373Bsel[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512, 320 };

static BOOLEAN M373Probe(SST_CODEC *c)
{
    ULONG v;
    if (!CRd(c, M373_REV_ID, &v))
        return FALSE;
    return v != 0x00 && v != 0xFF;     /* Linux only logs the revision (0x42 / 0x43 seen) */
}

static BOOLEAN M373Init(SST_CODEC *c)
{
    ULONG rev = 0, i, sr, bsel, csz, ch = MaxChan(c);

    /* max98373_reset: SOFT_RESET, then up to 3 x 10 ms until REV_ID answers again */
    CWr(c, M373_SW_RESET, 0x01);            /* may NACK while the chip resets */
    for (i = 0; i < 3; i++) {
        CDelay(10);
        if (CRd(c, M373_REV_ID, &rev))
            break;
    }
    if (i == 3) {
        LOGE("MAX98373 %s: no answer after reset\n", c->Path);
        return FALSE;
    }

    sr = MaxSr(c->Rate, 96000);
    bsel = MaxBsel(g_M373Bsel, sizeof(g_M373Bsel) / sizeof(g_M373Bsel[0]), (ULONG)c->Slots * c->Slot);
    csz = MaxChanSz(c->Slot);
    if (sr == 0xFF || !bsel || !csz) {
        LOGE("MAX98373 %s: unsupported rate %u / %u x %u-bit slots\n", c->Path, c->Rate, c->Slots, c->Slot);
        return FALSE;
    }

    {
        const ULONG seq[] = {
            M373_GLOBAL_SHDN, 0x00,
            M373_RX_EN, 0x00,
            /* max98373_probe: IV transmitters Hi-Z (also not enabled: shared TX line) */
            M373_TX_HIZ_1, 0xFF,
            M373_TX_HIZ_2, 0xFF,
            M373_TX_EN, 0x00,
            M373_IV_ADC_EN, 0x00,
            M373_TX_SRC_1, 0x10,                    /* i_slot 1, v_slot 0 (defaults) */
            M373_TX_SRC_2, 0x02,                    /* spkfb_slot 2 */
            /* DAI Sel: CFG = channel, CH0 from slot 0, CH1 from slot 1 (rx mask 0x3) */
            M373_MONOMIX_1, ch << 6,
            M373_MONOMIX_2, 0x01,
            M373_DSP_CFG, 0x03,                     /* DC blocker + dither */
            M373_IV_DSP_CFG, 0x07,
            M373_AUTO_RESTART, 0x0F,                /* OVC / thermal / clock-monitor auto restart */
            M373_DIG_VOL, 0x00,                     /* 0 dB */
            /* set_fmt: I2S or TDM mode 0, normal BCLK edge; hw_params: slot size, rate */
            M373_DATA_FMT, csz | MaxFormat(c),
            M373_CLK_RATIO, bsel,
            M373_SR_1, sr,
            M373_SR_2, (sr << 4) | sr,
        };
        if (!CSeq(c, seq, sizeof(seq) / sizeof(seq[0]) / 2))
            return FALSE;
    }
    /* speaker enable (the DAPM path then only toggles PCM_RX_EN and GLOBAL_EN) */
    if (!CUpd(c, M373_AMP_EN, 0x03, 0x01))
        return FALSE;

    c->Caps = CC_PLAY;
    LOGI("MAX98373 %s: rev %02x, %s, %u x %u-bit %s, BSEL %u\n", c->Path, rev, MaxChName(ch),
         c->Slots, c->Slot, c->Slots > 2 ? "TDM" : "I2S", bsel);
    return TRUE;
}

static void M373Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    if (on) {
        /* DAPM: "Amp Enable" DAC (PCM_RX_EN bit 0) powers up, then POST_PMU: GLOBAL_EN, 30 ms */
        CUpd(c, M373_RX_EN, 0x01, 0x01);
        CUpd(c, M373_GLOBAL_SHDN, 0x01, 0x01);
        CDelay(30);
    } else {
        /* PRE_PMD: GLOBAL_EN off, 30 ms, then the DAC widget */
        CUpd(c, M373_GLOBAL_SHDN, 0x01, 0x00);
        CDelay(30);
        CUpd(c, M373_RX_EN, 0x01, 0x00);
    }
}

static void M373Shutdown(SST_CODEC *c)
{
    CUpd(c, M373_GLOBAL_SHDN, 0x01, 0x00);
    CDelay(30);
    CWr(c, M373_RX_EN, 0x00);
    CUpd(c, M373_AMP_EN, 0x03, 0x00);
}

const CODEC_DRV g_DrvMax98373 = { "MAX98373", "MX98373\0", CODEC_AMP, 2, 1,
                                  M373Probe, M373Init, M373Stream, NULL, M373Shutdown };

/* ------------------------------------------------------------------ */
/* MAX98390                                                             */
/* ------------------------------------------------------------------ */
/*
 * Linux loads a DSM (speaker protection) parameter blob (dsm_param*.bin) and
 * then sets R23E1 DSP_GLOBAL_EN; the probe goes on without it when the file
 * is missing.  This driver never loads it: DSP_GLOBAL_EN stays 0 (reset
 * default), so the PCM goes straight to the class-D stage without the
 * speaker-protection / excursion limiter.  The boost and amplifier stay at the
 * chip defaults plus the static writes of max98390_init_regs.
 */
#define M390_SW_RESET        0x2000
#define M390_CLK_MON         0x2012
#define M390_DAT_MON         0x2014
#define M390_RX_EN_A         0x201B
#define M390_RX_EN_B         0x201C
#define M390_TX_EN_A         0x201D
#define M390_TX_EN_B         0x201E
#define M390_TX_HIZ_A        0x201F
#define M390_TX_HIZ_B        0x2020
#define M390_CH_SRC_1        0x2021     /* PCM_RX_CH_SRC (DAI Sel) 1:0 */
#define M390_MODE_CFG        0x2024     /* CHANSZ 7:6, FORMAT 5:3, BCLKEDGE 2 */
#define M390_MASTER_MODE     0x2025     /* MODE 1:0 (0 = consumer) */
#define M390_CLK_SETUP       0x2026     /* BSEL 3:0 */
#define M390_SR_SETUP        0x2027     /* SR 3:0 */
#define M390_AMP_EN          0x203A     /* AMP_EN 0 */
#define M390_PWR_GATE_CTL    0x2050
#define M390_ENV_HEADROOM    0x2076
#define M390_BOOST_BYPASS1   0x207C
#define M390_FET_SCALING3    0x2081
#define M390_DSP_GLOBAL_EN   0x23E1
#define M390_GLOBAL_EN       0x23FF
#define M390_REV_ID          0x24FF

static const USHORT g_M390Bsel[] = { 32, 48, 64, 96, 128, 192, 256, 320, 384, 512 };

static BOOLEAN M390Probe(SST_CODEC *c)
{
    ULONG v;
    if (!CRd(c, M390_REV_ID, &v))
        return FALSE;
    return v != 0x00 && v != 0xFF;
}

static BOOLEAN M390Init(SST_CODEC *c)
{
    ULONG rev = 0, sr, bsel, csz, ch = MaxChan(c);

    CWr(c, M390_SW_RESET, 0x01);
    CDelay(20);                                 /* reset settle time (max98390_probe) */
    if (!CRd(c, M390_REV_ID, &rev)) {
        LOGE("MAX98390 %s: no answer after reset\n", c->Path);
        return FALSE;
    }

    sr = MaxSr(c->Rate, 48000);
    bsel = MaxBsel(g_M390Bsel, sizeof(g_M390Bsel) / sizeof(g_M390Bsel[0]), (ULONG)c->Slots * c->Slot);
    csz = MaxChanSz(c->Slot);
    if (sr == 0xFF || !bsel || !csz) {
        LOGE("MAX98390 %s: unsupported rate %u / %u x %u-bit slots\n", c->Path, c->Rate, c->Slots, c->Slot);
        return FALSE;
    }

    {
        const ULONG seq[] = {
            M390_GLOBAL_EN, 0x00,
            /* max98390_init_regs */
            M390_CLK_MON, 0x6F,
            M390_DAT_MON, 0x00,
            M390_PWR_GATE_CTL, 0x00,
            M390_RX_EN_A, 0x03,                     /* slots 0 and 1 */
            M390_RX_EN_B, 0x00,
            M390_ENV_HEADROOM, 0x0E,
            M390_BOOST_BYPASS1, 0x46,
            M390_FET_SCALING3, 0x03,
            /* IV sense not transmitted (shared TX line) */
            M390_TX_EN_A, 0x00,
            M390_TX_EN_B, 0x00,
            M390_TX_HIZ_A, 0xFF,
            M390_TX_HIZ_B, 0xFF,
            M390_DSP_GLOBAL_EN, 0x00,               /* DSM bypassed: no parameter file */
            /* set_fmt / hw_params */
            M390_MODE_CFG, csz | MaxFormat(c),
            M390_SR_SETUP, sr,
        };
        if (!CSeq(c, seq, sizeof(seq) / sizeof(seq[0]) / 2))
            return FALSE;
    }
    if (!CUpd(c, M390_MASTER_MODE, 0x03, 0x00) ||          /* clock consumer */
        !CUpd(c, M390_CLK_SETUP, 0x0F, bsel) ||
        !CUpd(c, M390_CH_SRC_1, 0x03, ch) ||                /* DAI Sel */
        !CUpd(c, M390_AMP_EN, 0x01, 0x00))
        return FALSE;

    c->Caps = CC_PLAY;
    LOGI("MAX98390 %s: rev %02x, %s, %u x %u-bit %s, BSEL %u, DSM bypassed\n", c->Path, rev, MaxChName(ch),
         c->Slots, c->Slot, c->Slots > 2 ? "TDM" : "I2S", bsel);
    return TRUE;
}

static void M390Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    if (on) {
        /* max98390_dac_event POST_PMU: AMP_EN, then GLOBAL_EN */
        CUpd(c, M390_AMP_EN, 0x01, 0x01);
        CUpd(c, M390_GLOBAL_EN, 0x01, 0x01);
    } else {
        /* POST_PMD: GLOBAL_EN off, then AMP_EN */
        CUpd(c, M390_GLOBAL_EN, 0x01, 0x00);
        CUpd(c, M390_AMP_EN, 0x01, 0x00);
    }
}

static void M390Shutdown(SST_CODEC *c)
{
    CUpd(c, M390_GLOBAL_EN, 0x01, 0x00);
    CUpd(c, M390_AMP_EN, 0x01, 0x00);
}

const CODEC_DRV g_DrvMax98390 = { "MAX98390", "MX98390\0", CODEC_AMP, 2, 1,
                                  M390Probe, M390Init, M390Stream, NULL, M390Shutdown };

/* ------------------------------------------------------------------ */
/* MAX98927                                                             */
/* ------------------------------------------------------------------ */
#define M927_RX_EN_A         0x0018     /* PCM_RX_EN_A: CH0..7 */
#define M927_RX_EN_B         0x0019
#define M927_TX_EN_A         0x001A
#define M927_TX_EN_B         0x001B
#define M927_TX_HIZ_A        0x001C
#define M927_TX_HIZ_B        0x001D
#define M927_TX_SRC_A        0x001E
#define M927_MODE_CFG        0x0020     /* CHANSZ 7:6, FORMAT 5:3, BCLKEDGE 2 */
#define M927_MASTER_MODE     0x0021     /* MODE 1:0 */
#define M927_CLK_SETUP       0x0022     /* BSEL 3:0 */
#define M927_SR_1            0x0023     /* SR 3:0 */
#define M927_SR_2            0x0024     /* SPK SR 7:4, IVADC SR 3:0 */
#define M927_MONOMIX_A       0x0025     /* CFG 7:6 (DAI Sel), CH0 slot 3:0 */
#define M927_MONOMIX_B       0x0026     /* CH1 slot */
#define M927_PDM_RX_CTRL     0x0035
#define M927_AMP_VOL         0x0036
#define M927_DSP_CFG         0x0037
#define M927_AMP_EN          0x003A
#define M927_SPK_SRC_SEL     0x003B     /* 1:0: 0 PCM, 3 PDM */
#define M927_SPK_GAIN        0x003C
#define M927_MEAS_EN         0x003E
#define M927_MEAS_DSP_CFG    0x003F
#define M927_BOOST_CTRL0     0x0040
#define M927_BOOST_CTRL1     0x0042
#define M927_MEAS_ADC_CFG    0x0043
#define M927_ADC_BASE_MSB    0x0044
#define M927_ADC_BASE_LSB    0x0045
#define M927_BO_L4_AMP1_C1   0x007F
#define M927_ENV_HEADROOM    0x0082
#define M927_ENV_CTRL        0x0086
#define M927_ENV_VOUT_READ   0x0087
#define M927_GLOBAL_SHDN     0x00FF
#define M927_SOFT_RESET      0x0100
#define M927_REV_ID          0x01FF

static const USHORT g_M927Bsel[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512 };

static BOOLEAN M927Probe(SST_CODEC *c)
{
    ULONG v;
    if (!CRd(c, M927_REV_ID, &v))
        return FALSE;
    return v != 0x00 && v != 0xFF;     /* reset default 0x40 */
}

static BOOLEAN M927Init(SST_CODEC *c)
{
    ULONG rev = 0, sr, bsel, csz, ch = MaxChan(c);

    CWr(c, M927_SOFT_RESET, 0x01);
    CDelay(5);
    if (!CRd(c, M927_REV_ID, &rev)) {
        LOGE("MAX98927 %s: no answer after reset\n", c->Path);
        return FALSE;
    }

    sr = MaxSr(c->Rate, 48000);
    bsel = MaxBsel(g_M927Bsel, sizeof(g_M927Bsel) / sizeof(g_M927Bsel[0]), (ULONG)c->Slots * c->Slot);
    csz = MaxChanSz(c->Slot);
    if (sr == 0xFF || !bsel || !csz) {
        LOGE("MAX98927 %s: unsupported rate %u / %u x %u-bit slots\n", c->Path, c->Rate, c->Slots, c->Slot);
        return FALSE;
    }

    {
        const ULONG seq[] = {
            M927_GLOBAL_SHDN, 0x00,
            M927_AMP_EN, 0x00,
            /* max98927_probe */
            M927_TX_HIZ_A, 0xFF,
            M927_TX_HIZ_B, 0xFF,
            M927_TX_EN_A, 0x00,                     /* IV not transmitted (shared TX line) */
            M927_TX_EN_B, 0x00,
            M927_MEAS_EN, 0x00,
            M927_TX_SRC_A, 0x10,                    /* i slot 1, v slot 0 (defaults) */
            M927_MONOMIX_A, ch << 6,                /* DAI Sel, CH0 from slot 0 */
            M927_MONOMIX_B, 0x01,                   /* CH1 from slot 1 */
            M927_AMP_VOL, 0x38,                     /* Linux initial digital volume */
            M927_SPK_GAIN, 0x05,                    /* and speaker (PCM) gain */
            M927_DSP_CFG, 0x03,                     /* DC blocker + dither */
            M927_MEAS_DSP_CFG, 0xF7,
            M927_BOOST_CTRL0, 0x1C,                 /* boost output voltage */
            M927_BOOST_CTRL1, 0x3E,                 /* boost current limit */
            M927_MEAS_ADC_CFG, 0x04,
            M927_ADC_BASE_MSB, 0x00,
            M927_ADC_BASE_LSB, 0x24,
            M927_BO_L4_AMP1_C1, 0x06,               /* brownout level 4 */
            M927_ENV_HEADROOM, 0x08,                /* envelope tracking */
            M927_ENV_CTRL, 0x01,
            M927_ENV_VOUT_READ, 0x10,
            /* set_fmt (PCM, not PDM): both RX channels from slots 0 / 1, speaker source PCM */
            M927_RX_EN_A, 0x03,
            M927_RX_EN_B, 0x00,
            M927_MODE_CFG, csz | MaxFormat(c),
            /* hw_params */
            M927_SR_1, sr,
            M927_SR_2, (sr << 4) | sr,
        };
        if (!CSeq(c, seq, sizeof(seq) / sizeof(seq[0]) / 2))
            return FALSE;
    }
    if (!CUpd(c, M927_MASTER_MODE, 0x03, 0x00) ||
        !CUpd(c, M927_SPK_SRC_SEL, 0x03, 0x00) ||
        !CUpd(c, M927_PDM_RX_CTRL, 0x01, 0x00) ||
        !CUpd(c, M927_CLK_SETUP, 0x0F, bsel))
        return FALSE;

    c->Caps = CC_PLAY;
    LOGI("MAX98927 %s: rev %02x, %s, %u x %u-bit %s, BSEL %u\n", c->Path, rev, MaxChName(ch),
         c->Slots, c->Slot, c->Slots > 2 ? "TDM" : "I2S", bsel);
    return TRUE;
}

static void M927Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    if (on) {
        /* DAPM "Amp Enable" DAC (AMP_EN bit 0), POST_PMU: AMP_EN, GLOBAL_EN */
        CUpd(c, M927_AMP_EN, 0x01, 0x01);
        CUpd(c, M927_GLOBAL_SHDN, 0x01, 0x01);
    } else {
        /* POST_PMD: GLOBAL_EN off, then AMP_EN */
        CUpd(c, M927_GLOBAL_SHDN, 0x01, 0x00);
        CUpd(c, M927_AMP_EN, 0x01, 0x00);
    }
}

static void M927Shutdown(SST_CODEC *c)
{
    CUpd(c, M927_GLOBAL_SHDN, 0x01, 0x00);
    CUpd(c, M927_AMP_EN, 0x01, 0x00);
}

const CODEC_DRV g_DrvMax98927 = { "MAX98927", "MX98927\0", CODEC_AMP, 2, 1,
                                  M927Probe, M927Init, M927Stream, NULL, M927Shutdown };
