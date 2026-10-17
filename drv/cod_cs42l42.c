#include "codec.h"
/*
 * AUDIONT - Cirrus Logic CS42L42 headset codec (Intel Chromebooks GLK/JSL/ADL, ACPI "10134242").
 * Reference: Linux sound/soc/codecs/cs42l42.c, cs42l42-i2c.c, include/sound/cs42l42.h,
 * intel/boards/sof_cs42l42.c.
 *
 * Registers: 16-bit "page:address" numbers, 8-bit values; on I2C the page goes to the page register
 * (address 0x00 of every page) and the low byte is the 8-bit register address (regmap_range_cfg
 * window 0..0xFF).  The current page is cached in Priv[P_PAGE].
 *
 * Reset: Linux takes "reset" (reset-gpios) GPIOD_OUT_LOW, drives it low, then high to release and waits
 * CS42L42_BOOT_TIME_US.  On Chromebook ACPI tables reset-gpios points at the only GpioIo line (the
 * GpioInt is the IRQ), so c->Gpio[0] is used; physical high = out of reset.
 *
 * Clocking: the PLL runs from SCLK (= BCLK).  Its settings only exist as a table (datasheet table 4-5,
 * pll_ratio_table[] in Linux), copied below completely; it covers BCLK = 48 kHz * {32, 48, 50, 64}.
 * IMPORTANT: once OSC_SWITCH selects SCLK the chip core (and its I2C interface) runs from SCLK.  Linux
 * relies on the SSP driving BCLK before unmute and after mute (SOF BCLK early start).  Here Stream(on)
 * runs before the SSP starts, so the clock switch is the very last write of Stream(on) (HP unmute is done
 * before it), and Stream(off) first checks whether the chip still answers before switching back to the
 * internal oscillator.  If it does not (no SCLK, PLL stopped), the chip keeps its running configuration
 * and comes back as soon as BCLK runs again; Init recovers it with the reset GPIO.
 *
 * No jack detection: the jack circuitry is put in the state Linux reaches for a CTIA headset
 * (cs42l42_manual_hs_type_detect(): HS_SWITCH_CTL = CS42L42_HSDET_SW_TYPE1 - HS3 grounded, HSBIAS on HS4
 * (sleeve); then cs42l42_process_hs_type_detect(): HSBIAS 2.7 V, level detect on, DAC_CTL2 HPF only).
 * All interrupts stay masked.  The headset mic is the only input (c->Mic is ignored).
 */

#define R_PAGE                  0x0000
#define R_DEVID_AB              0x1001
#define R_DEVID_CD              0x1002
#define R_DEVID_E               0x1003
#define R_REVID                 0x1005
#define R_MCLK_CTL              0x1009
#define R_PWR_CTL1              0x1101
#define R_PWR_CTL2              0x1102
#define R_OSC_SWITCH            0x1107
#define R_TSENSE_CTL            0x1113
#define R_HSDET_CTL1            0x111F
#define R_HSDET_CTL2            0x1120
#define R_HS_SWITCH_CTL         0x1121
#define R_HS_CLAMP_DISABLE      0x1129
#define R_MCLK_SRC_SEL          0x1201
#define R_FSYNC_PW_LOWER        0x1203
#define R_FSYNC_PW_UPPER        0x1204
#define R_FSYNC_P_LOWER         0x1205
#define R_FSYNC_P_UPPER         0x1206
#define R_ASP_CLK_CFG           0x1207
#define R_ASP_FRM_CFG           0x1208
#define R_FS_RATE_EN            0x1209
#define R_IN_ASRC_CLK           0x120A
#define R_OUT_ASRC_CLK          0x120B
#define R_PLL_DIV_CFG1          0x120C
#define R_CODEC_STATUS          0x1308
#define R_DET_INT_STATUS2       0x130A
#define R_PLL_LOCK_STATUS       0x130E
#define R_TSRS_PLUG_STATUS      0x130F
#define R_ADC_OVFL_INT_MASK     0x1316
#define R_MIXER_INT_MASK        0x1317
#define R_SRC_INT_MASK          0x1318
#define R_ASP_RX_INT_MASK       0x1319
#define R_ASP_TX_INT_MASK       0x131A
#define R_CODEC_INT_MASK        0x131B
#define R_SRCPL_INT_MASK        0x131C
#define R_VPMON_INT_MASK        0x131E
#define R_PLL_LOCK_INT_MASK     0x131F
#define R_TSRS_PLUG_INT_MASK    0x1320
#define R_PLL_CTL1              0x1501
#define R_PLL_DIV_FRAC0         0x1502
#define R_PLL_DIV_FRAC1         0x1503
#define R_PLL_DIV_FRAC2         0x1504
#define R_PLL_DIV_INT           0x1505
#define R_PLL_CTL3              0x1508
#define R_PLL_CAL_RATIO         0x150A
#define R_PLL_CTL4              0x151B
#define R_HSBIAS_SC_AUTOCTL     0x1B70
#define R_WAKE_CTL              0x1B71
#define R_TIPSENSE_CTL          0x1B73
#define R_MISC_DET_CTL          0x1B74
#define R_MIC_DET_CTL1          0x1B75
#define R_DET_INT1_MASK         0x1B79
#define R_DET_INT2_MASK         0x1B7A
#define R_HS_BIAS_CTL           0x1C03
#define R_ADC_CTL               0x1D01
#define R_ADC_VOLUME            0x1D03
#define R_DAC_CTL2              0x1F06
#define R_HP_CTL                0x2001
#define R_MIXER_CHA_VOL         0x2301
#define R_MIXER_ADC_VOL         0x2302
#define R_MIXER_CHB_VOL         0x2303
#define R_SP_RX_CH_SEL          0x2501
#define R_ASP_TX_SZ_EN          0x2901
#define R_ASP_TX_CH_EN          0x2902
#define R_ASP_TX_CH_AP_RES      0x2903
#define R_ASP_RX_DAI0_EN        0x2A01
#define R_ASP_RX_DAI0_CH1_AP_RES 0x2A02
#define R_ASP_RX_DAI0_CH2_AP_RES 0x2A05

/* PWR_CTL1 power-down bits */
#define ASP_DAO_PDN             (1 << 7)
#define ASP_DAI_PDN             (1 << 6)
#define MIXER_PDN               (1 << 5)
#define EQ_PDN                  (1 << 4)
#define HP_PDN                  (1 << 3)
#define ADC_PDN                 (1 << 2)
#define PDN_ALL                 (1 << 0)
#define DISCHARGE_FILT          (1 << 4)    /* PWR_CTL2 */
#define ASP_SCLK_EN             (1 << 5)    /* ASP_CLK_CFG */
#define HP_ANA_MUTE             (3 << 2)    /* HP_CTL A/B mute */
#define HSBIAS_CTL_MASK         (3 << 1)    /* MISC_DET_CTL */
#define PDN_MIC_LVL_DET         (1 << 0)
#define DETECT_MODE_MASK        (3 << 3)
#define HSDET_SW_TYPE1          0xA6        /* GNDHS_HS3 | HSB_HS4 | HSB_FILT_HS3 | REF_HS3 (CTIA) */

#define HS_DET_LEVEL_15         0x0F        /* bias_thresholds[0] default */
#define HSBIAS_RAMP_SLOW        2           /* hs-bias-ramp-rate default */
#define HSBIAS_RAMP_TIME2       90          /* ms */
#define TS_DBNCE_1000           5           /* ts-dbnc-rise default */
#define BTN_DET_INIT_DBNCE      100         /* ms, btn-det-init-dbnce default */
#define HP_ADC_EN_TIME_MS       20          /* CS42L42_HP_ADC_EN_TIME_US */
#define PDN_DONE_TIME_MS        65
#define CS42L42_ID              0x42A42

/* Priv[] */
#define P_PAGE      0   /* cached page register, 0xFFFF = unknown */
#define P_PLL       1   /* pll_ratio_table index + 1 (0 = none) */
#define P_SCLK      2   /* OSC_SWITCH set to SCLK */
#define P_MUTE      3
#define P_VOLA      4
#define P_VOLB      5

/* pll_ratio_table[] (datasheet table 4-5) */
typedef struct { ULONG sclk; UCHAR src_sel, prediv, div_int; ULONG frac; UCHAR mode, divout; ULONG mclk_int;
                 UCHAR cal, n; } PLL_PARAMS;
static const PLL_PARAMS s_Pll[] = {
    { 1411200,  1, 0x00, 0x80, 0x000000, 0x03, 0x10, 11289600, 128, 2 },
    { 1536000,  1, 0x00, 0x7D, 0x000000, 0x03, 0x10, 12000000, 125, 2 },
    { 2304000,  1, 0x00, 0x55, 0xC00000, 0x02, 0x10, 12288000,  85, 2 },
    { 2400000,  1, 0x00, 0x50, 0x000000, 0x03, 0x10, 12000000,  80, 2 },
    { 2822400,  1, 0x00, 0x40, 0x000000, 0x03, 0x10, 11289600, 128, 1 },
    { 3000000,  1, 0x00, 0x40, 0x000000, 0x03, 0x10, 12000000, 128, 1 },
    { 3072000,  1, 0x00, 0x3E, 0x800000, 0x03, 0x10, 12000000, 125, 1 },
    { 4000000,  1, 0x00, 0x30, 0x800000, 0x03, 0x10, 12000000,  96, 1 },
    { 4096000,  1, 0x00, 0x2E, 0xE00000, 0x03, 0x10, 12000000,  94, 1 },
    { 4800000,  1, 0x01, 0x50, 0x000000, 0x03, 0x10, 12000000,  80, 2 },
    { 4800000,  1, 0x01, 0x50, 0x000000, 0x01, 0x10, 12288000,  82, 2 },
    { 5644800,  1, 0x01, 0x40, 0x000000, 0x03, 0x10, 11289600, 128, 1 },
    { 6000000,  1, 0x01, 0x40, 0x000000, 0x03, 0x10, 12000000, 128, 1 },
    { 6144000,  1, 0x01, 0x3E, 0x800000, 0x03, 0x10, 12000000, 125, 1 },
    { 6144000,  1, 0x01, 0x40, 0x000000, 0x03, 0x10, 12288000, 128, 1 },
    { 9600000,  1, 0x02, 0x50, 0x000000, 0x03, 0x10, 12000000,  80, 2 },
    { 9600000,  1, 0x02, 0x50, 0x000000, 0x01, 0x10, 12288000,  82, 2 },
    { 11289600, 0, 0, 0, 0, 0, 0, 11289600, 0, 1 },
    { 12000000, 0, 0, 0, 0, 0, 0, 12000000, 0, 1 },
    { 12288000, 0, 0, 0, 0, 0, 0, 12288000, 0, 1 },
    { 19200000, 1, 0x03, 0x50, 0x000000, 0x03, 0x10, 12000000,  80, 2 },
    { 19200000, 1, 0x03, 0x50, 0x000000, 0x01, 0x10, 12288000,  82, 2 },
    { 22579200, 1, 0x03, 0x40, 0x000000, 0x03, 0x10, 11289600, 128, 1 },
    { 24000000, 1, 0x03, 0x40, 0x000000, 0x03, 0x10, 12000000, 128, 1 },
    { 24576000, 1, 0x03, 0x40, 0x000000, 0x03, 0x10, 12288000, 128, 1 },
};
#define NPLL (sizeof(s_Pll) / sizeof(s_Pll[0]))

/* cs42l42_shutdown_seq[] (datasheet suspend sequence) */
static const ULONG s_ShutdownSeq[] = {
    R_MIC_DET_CTL1, 0x9F, R_ADC_OVFL_INT_MASK, 0x01, R_MIXER_INT_MASK, 0x0F, R_SRC_INT_MASK, 0x0F,
    R_ASP_RX_INT_MASK, 0x1F, R_ASP_TX_INT_MASK, 0x0F, R_CODEC_INT_MASK, 0x03, R_SRCPL_INT_MASK, 0x7F,
    R_VPMON_INT_MASK, 0x01, R_PLL_LOCK_INT_MASK, 0x01, R_TSRS_PLUG_INT_MASK, 0x0F, R_WAKE_CTL, 0xE1,
    R_DET_INT1_MASK, 0xE0, R_DET_INT2_MASK, 0xFF, R_MIXER_CHA_VOL, 0x3F, R_MIXER_ADC_VOL, 0x3F,
    R_MIXER_CHB_VOL, 0x3F, R_HP_CTL, 0x0F, R_ASP_RX_DAI0_EN, 0x00, R_ASP_CLK_CFG, 0x00,
    R_HSDET_CTL2, 0x00, R_PWR_CTL1, 0xFE, R_PWR_CTL2, 0x8C, R_DAC_CTL2, 0x02,
    R_HS_CLAMP_DISABLE, 0x00, R_MISC_DET_CTL, 0x03, R_TIPSENSE_CTL, 0x02, R_HSBIAS_SC_AUTOCTL, 0x03,
    R_PWR_CTL1, 0xFF,
};

/* ---- paged register access ---- */
static BOOLEAN Page(SST_CODEC *c, ULONG reg)
{
    ULONG page = (reg >> 8) & 0xFF;
    if (c->Priv[P_PAGE] == page) return TRUE;
    if (!CWr(c, R_PAGE, page)) { c->Priv[P_PAGE] = 0xFFFF; return FALSE; }
    c->Priv[P_PAGE] = page;
    return TRUE;
}

static BOOLEAN Wr(SST_CODEC *c, ULONG reg, ULONG val)
{
    if (!Page(c, reg)) return FALSE;
    if (!CWr(c, reg & 0xFF, val & 0xFF)) { c->Priv[P_PAGE] = 0xFFFF; return FALSE; }
    return TRUE;
}

static BOOLEAN Rd(SST_CODEC *c, ULONG reg, ULONG *val)
{
    if (!Page(c, reg)) return FALSE;
    if (!CRd(c, reg & 0xFF, val)) { c->Priv[P_PAGE] = 0xFFFF; return FALSE; }
    return TRUE;
}

static BOOLEAN Upd(SST_CODEC *c, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;
    if (!Rd(c, reg, &v)) return FALSE;
    if (((v ^ val) & mask) == 0) return TRUE;
    return Wr(c, reg, (v & ~mask) | (val & mask));
}

static BOOLEAN ReadId(SST_CODEC *c, ULONG *id)
{
    ULONG ab, cd, e;
    if (!Rd(c, R_DEVID_AB, &ab) || !Rd(c, R_DEVID_CD, &cd) || !Rd(c, R_DEVID_E, &e)) return FALSE;
    *id = ((ab & 0xFF) << 12) | ((cd & 0xFF) << 4) | ((e & 0xF0) >> 4);   /* cirrus_read_device_id() */
    return TRUE;
}

/* cs42l42_common_probe(): reset low, minimum pulse, release, CS42L42_BOOT_TIME_US */
static void Reset(SST_CODEC *c)
{
    if (c->NGpio) {
        CGpio(c, 0, FALSE);
        CDelay(1);
        CGpio(c, 0, TRUE);
    }
    CDelay(3);
    c->Priv[P_PAGE] = 0xFFFF;
}

static BOOLEAN Probe(SST_CODEC *c)
{
    ULONG id;
    c->Priv[P_PAGE] = 0xFFFF;
    if (!ReadId(c, &id)) {
        if (!c->NGpio) return FALSE;
        Reset(c);                                       /* firmware may have left the chip in reset */
        if (!ReadId(c, &id)) return FALSE;
    }
    return id == CS42L42_ID;
}

/* cs42l42_pll_config() + cs42l42_asp_config() + cs42l42_src_config() (hw_params, first stream only) */
static BOOLEAN ClockConfig(SST_CODEC *c)
{
    const PLL_PARAMS *p = 0;
    ULONG i, fsync;
    for (i = 0; i < NPLL; i++)
        if ((s_Pll[i].mclk_int % c->Rate) == 0 && s_Pll[i].sclk == c->Bclk) { p = &s_Pll[i]; break; }
    if (!p) {
        LOGE("CS42L42: SCLK %u Hz not in the PLL table\n", c->Bclk);
        c->Priv[P_PLL] = 0;
        return FALSE;
    }
    c->Priv[P_PLL] = i + 1;
    Upd(c, R_MCLK_CTL, 1 << 1, (p->mclk_int != 12000000 && p->mclk_int != 24000000) << 1);  /* INTERNAL_FS */
    if (!p->src_sel)
        Upd(c, R_PLL_CTL1, 1, 0);                       /* SCLK passed straight through */
    else {
        Upd(c, R_PLL_DIV_CFG1, 3, p->prediv);
        Wr(c, R_PLL_DIV_INT, p->div_int);
        Wr(c, R_PLL_DIV_FRAC0, p->frac & 0xFF);
        Wr(c, R_PLL_DIV_FRAC1, (p->frac >> 8) & 0xFF);
        Wr(c, R_PLL_DIV_FRAC2, (p->frac >> 16) & 0xFF);
        Upd(c, R_PLL_CTL4, 3, p->mode);
        Wr(c, R_PLL_CTL3, p->divout * p->n);
        Wr(c, R_PLL_CAL_RATIO, p->cal);
    }
    /* LRCLK period and 50 % pulse width in SCLKs */
    fsync = c->Bclk / c->Rate;
    if (fsync * c->Rate != c->Bclk || (fsync & 1)) {
        LOGE("CS42L42: unsupported SCLK %u / rate %u\n", c->Bclk, c->Rate);
        return FALSE;
    }
    Wr(c, R_FSYNC_P_LOWER, (fsync - 1) & 0xFF);
    Wr(c, R_FSYNC_P_UPPER, ((fsync - 1) >> 8) & 0xFF);
    Wr(c, R_FSYNC_PW_LOWER, (fsync / 2 - 1) & 0xFF);
    Wr(c, R_FSYNC_PW_UPPER, ((fsync / 2 - 1) >> 8) & 0xFF);
    /* SRCs: 96k-capable, SRC MCLK near 125 * Fs (CLK_IASRC/OASRC_SEL_6 up to 48 kHz) */
    Upd(c, R_FS_RATE_EN, 0xF, 0x3);
    Upd(c, R_IN_ASRC_CLK, 1, c->Rate > 48000);
    Upd(c, R_OUT_ASRC_CLK, 1, c->Rate > 48000);
    return TRUE;
}

/* cs42l42_mute_stream(unmute) tail for the first stream: start PLL, select it, switch the core to SCLK.
 * The OSC_SWITCH write is the last access: after it the chip needs SCLK to answer on I2C. */
static void ClockStart(SST_CODEC *c)
{
    const PLL_PARAMS *p;
    ULONG v = 0, i;
    if (!c->Priv[P_PLL]) return;
    p = &s_Pll[c->Priv[P_PLL] - 1];
    if (p->src_sel) {
        Upd(c, R_PLL_CTL1, 1, 1);                       /* PLL_START */
        if (p->n > 1) {
            CDelay(1);                                  /* CS42L42_PLL_DIVOUT_TIME_US */
            Wr(c, R_PLL_CTL3, p->divout);
        }
        for (i = 0; i < 5; i++) {                       /* PLL_LOCK_POLL / _TIMEOUT */
            if (Rd(c, R_PLL_LOCK_STATUS, &v) && (v & 1)) break;
            CDelay(1);
        }
        if (i == 5) LOGV("CS42L42: PLL not locked yet (SCLK starts with the SSP)\n");
        Upd(c, R_MCLK_SRC_SEL, 1, 1);                   /* PLL drives the glitchless switch */
    }
    Wr(c, R_OSC_SWITCH, 1);                             /* SCLK_PRESENT: internal oscillator off */
    CDelay(1);                                          /* CS42L42_CLOCK_SWITCH_DELAY_US */
    c->Priv[P_SCLK] = 1;
}

/* cs42l42_mute_stream(mute) tail for the last stream: back to the internal oscillator, stop the PLL */
static void ClockStop(SST_CODEC *c)
{
    ULONG v;
    if (!c->Priv[P_SCLK]) return;
    c->Priv[P_PAGE] = 0xFFFF;
    if (!Rd(c, R_OSC_SWITCH, &v)) {
        LOGI("CS42L42: no response without SCLK, chip keeps its clocked state\n");
        return;
    }
    Wr(c, R_OSC_SWITCH, 0);
    CDelay(1);
    Upd(c, R_MCLK_SRC_SEL, 1, 0);                       /* disconnect the PLL before stopping it */
    CDelay(1);
    Upd(c, R_PLL_CTL1, 1, 0);
    c->Priv[P_SCLK] = 0;
}

static void Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? c->PlayOn : c->RecOn;
    ULONG res = (c->Valid >= 32 ? 32 : c->Valid >= 24 ? 24 : 16) / 8 - 1;  /* ASP_xX_CH_RES: 1 16, 2 24, 3 32 */
    if (on) {
        if (!other) {
            if (c->Priv[P_SCLK]) {
                ULONG v;
                c->Priv[P_PAGE] = 0xFFFF;
                if (!Rd(c, R_REVID, &v)) {
                    LOGI("CS42L42: still clocked from SCLK, configuration kept\n");
                    return;
                }
                ClockStop(c);
            }
            ClockConfig(c);
        }
        if (!rec) {
            /* hw_params: RX ch1 on low LRCLK, ch2 on high LRCLK, channel B = last channel, both slots */
            Upd(c, R_ASP_RX_DAI0_CH1_AP_RES, 0x43, res);
            Upd(c, R_ASP_RX_DAI0_CH2_AP_RES, 0x43, 0x40 | res);
            Upd(c, R_SP_RX_CH_SEL, 3 << 2, 1 << 2);
            Upd(c, R_ASP_RX_DAI0_EN, 0xF << 2, (1 << 2) | (1 << 3));
            /* DAPM: supplies "ASP DAI0", "SCLK"; DAC (HP_PDN) + settle; MIXER */
            Upd(c, R_PWR_CTL1, ASP_DAI_PDN, 0);
            Upd(c, R_ASP_CLK_CFG, ASP_SCLK_EN, ASP_SCLK_EN);
            Upd(c, R_PWR_CTL1, HP_PDN, 0);
            CDelay(HP_ADC_EN_TIME_MS);
            Upd(c, R_PWR_CTL1, MIXER_PDN, 0);
            Upd(c, R_HP_CTL, HP_ANA_MUTE, 0);           /* un-mute the headphone (still on the oscillator) */
        } else {
            /* hw_params: TX ch2 on high LRCLK, resolution */
            Upd(c, R_ASP_TX_CH_AP_RES, 0xCF, 0x40 | (res << 2) | res);
            /* DAPM: supplies "ASP DAO0", "SCLK", "ASP TX EN"; SDOUT1/2; ADC + settle */
            Upd(c, R_PWR_CTL1, ASP_DAO_PDN, 0);
            Upd(c, R_ASP_CLK_CFG, ASP_SCLK_EN, ASP_SCLK_EN);
            Upd(c, R_ASP_TX_SZ_EN, 1, 1);
            Upd(c, R_ASP_TX_CH_EN, 3, 3);               /* mono ADC on both TX channels */
            Upd(c, R_PWR_CTL1, ADC_PDN, 0);
            CDelay(HP_ADC_EN_TIME_MS);
        }
        if (!other) ClockStart(c);
    } else {
        if (!rec) Upd(c, R_HP_CTL, HP_ANA_MUTE, HP_ANA_MUTE);
        if (!other) {
            ClockStop(c);
            if (c->Priv[P_SCLK]) return;                /* chip unreachable */
        }
        if (!rec) {
            Upd(c, R_PWR_CTL1, MIXER_PDN, MIXER_PDN);
            Upd(c, R_PWR_CTL1, HP_PDN, HP_PDN);
            Upd(c, R_PWR_CTL1, ASP_DAI_PDN, ASP_DAI_PDN);
            if (!other) Upd(c, R_ASP_CLK_CFG, ASP_SCLK_EN, 0);
        } else {
            Upd(c, R_PWR_CTL1, ADC_PDN, ADC_PDN);
            Upd(c, R_ASP_TX_CH_EN, 3, 0);
            Upd(c, R_PWR_CTL1, ASP_DAO_PDN, ASP_DAO_PDN);
            if (!other) Upd(c, R_ASP_CLK_CFG, ASP_SCLK_EN, 0);
            Upd(c, R_ASP_TX_SZ_EN, 1, 0);
        }
    }
}

/* "Mixer Volume": MIXER_CHA/CHB_VOL[5:0] inverted, 0 = 0 dB, 1 dB steps, 0x3F = mute */
static void Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    ULONG a = mute ? 0x3F : 0x3E - CVolSteps(left, 0x3E);
    ULONG b = mute ? 0x3F : 0x3E - CVolSteps(right, 0x3E);
    if (c->Priv[P_SCLK] && !c->PlayOn && !c->RecOn) return;   /* chip may be unreachable */
    Upd(c, R_MIXER_CHA_VOL, 0x3F, a);
    Upd(c, R_MIXER_CHB_VOL, 0x3F, b);
    c->Priv[P_MUTE] = mute;
    c->Priv[P_VOLA] = a;
    c->Priv[P_VOLB] = b;
}

static BOOLEAN Init(SST_CODEC *c)
{
    ULONG e0, id, rev, v;
    c->PlayOn = c->RecOn = FALSE;
    c->Priv[P_SCLK] = 0;
    c->Priv[P_PLL] = 0;
    Reset(c);
    e0 = c->Errors;
    if (!ReadId(c, &id)) return FALSE;
    if (id != CS42L42_ID) {
        LOGE("CS42L42: device id %05x, expected %05x\n", id, CS42L42_ID);
        return FALSE;
    }
    if (!Rd(c, R_REVID, &rev)) return FALSE;

    /* cs42l42_init(): power up the codec core, all blocks still powered down */
    Upd(c, R_PWR_CTL1, ASP_DAO_PDN | ASP_DAI_PDN | MIXER_PDN | EQ_PDN | HP_PDN | ADC_PDN | PDN_ALL,
        ASP_DAO_PDN | ASP_DAI_PDN | MIXER_PDN | EQ_PDN | HP_PDN | ADC_PDN);
    /* cs42l42_handle_device_data() defaults */
    Upd(c, R_TSENSE_CTL, 7, TS_DBNCE_1000);             /* tip sense rise debounce 1 s */
    Upd(c, R_TSENSE_CTL, 7 << 3, 0);                    /* fall debounce 0 */
    Upd(c, R_HS_BIAS_CTL, 3, HSBIAS_RAMP_SLOW);
    /* cs42l42_setup_hs_type_detect() */
    Upd(c, R_MISC_DET_CTL, DETECT_MODE_MASK, 0);        /* must be 0 with ADC and HP off (FILT+) */
    Upd(c, R_MIC_DET_CTL1, 0xFF, 0x80 | HS_DET_LEVEL_15);   /* LATCH_TO_VP, level */
    Upd(c, R_HS_CLAMP_DISABLE, 1, 1);
    Upd(c, R_TSENSE_CTL, 1 << 7, 1 << 7);               /* TS_INV */
    Upd(c, R_TIPSENSE_CTL, (3 << 6) | (1 << 5) | 3, (3 << 6) | (1 << 5) | 2);
    Rd(c, R_TSRS_PLUG_STATUS, &v);
    /* cs42l42_set_interrupt_masks(), with tip sense masked as well (no IRQ handler) */
    Upd(c, R_ADC_OVFL_INT_MASK, 0x01, 0x01);
    Upd(c, R_MIXER_INT_MASK, 0x0F, 0x0F);
    Upd(c, R_SRC_INT_MASK, 0x0F, 0x0F);
    Upd(c, R_ASP_RX_INT_MASK, 0x1F, 0x1F);
    Upd(c, R_ASP_TX_INT_MASK, 0x0F, 0x0F);
    Upd(c, R_CODEC_INT_MASK, 0x03, 0x03);
    Upd(c, R_SRCPL_INT_MASK, 0x65, 0x65);
    Upd(c, R_DET_INT1_MASK, 0xE0, 0xE0);
    Upd(c, R_DET_INT2_MASK, 0xC7, 0xC7);
    Upd(c, R_VPMON_INT_MASK, 0x01, 0x01);
    Upd(c, R_PLL_LOCK_INT_MASK, 0x01, 0x01);
    Upd(c, R_TSRS_PLUG_INT_MASK, 0x0F, 0x0F);

    /* plug event: cs42l42_init_hs_type_detect() up to the HS bias ramp */
    Upd(c, R_MISC_DET_CTL, HSBIAS_CTL_MASK | PDN_MIC_LVL_DET, (1 << 1) | PDN_MIC_LVL_DET);
    Upd(c, R_HSBIAS_SC_AUTOCTL, 0xE7, 3);               /* sense off, HIZ off, tip sense off, trip 3 */
    Upd(c, R_HSDET_CTL2, 0xFB, (0 << 6) | (2 << 4) | 3);    /* manual, disabled */
    Upd(c, R_DAC_CTL2, 0xFF, (8 << 4) | (1 << 2) | (1 << 1) | 1);   /* pulldown, clamp, HPF, monitor */
    Upd(c, R_MISC_DET_CTL, HSBIAS_CTL_MASK | PDN_MIC_LVL_DET, (3 << 1) | PDN_MIC_LVL_DET);  /* HSBIAS 2.7 V */
    CDelay(HSBIAS_RAMP_TIME2);
    /* CTIA result as set by cs42l42_manual_hs_type_detect(): switches for type 1, HSDET manual-disabled */
    Wr(c, R_HS_SWITCH_CTL, HSDET_SW_TYPE1);
    Upd(c, R_HSDET_CTL2, 0xFB, 0);
    Upd(c, R_HSDET_CTL1, 0xFF, 0x77);                   /* comparator levels back to default */
    /* cs42l42_process_hs_type_detect(), CTIA branch (button interrupts stay masked) */
    Upd(c, R_HSBIAS_SC_AUTOCTL, 0xE7, 3);
    Upd(c, R_MIC_DET_CTL1, 0xFF, 0x80 | HS_DET_LEVEL_15);
    Upd(c, R_HSBIAS_SC_AUTOCTL, 0xE7, (1 << 7) | (1 << 6) | 3);   /* hs-bias-sense on, auto HIZ */
    Upd(c, R_MISC_DET_CTL, HSBIAS_CTL_MASK | PDN_MIC_LVL_DET, 3 << 1);    /* level detect on */
    CDelay(BTN_DET_INIT_DBNCE);
    Rd(c, R_DET_INT_STATUS2, &v);
    Upd(c, R_DAC_CTL2, 0xFF, 1 << 1);                   /* no pulldown/clamp, HPF on */

    /* DAI: consumer, I2S 50/50 frame, FSD 1.0 SCLK, normal SCLK polarity */
    Upd(c, R_ASP_FRM_CFG, 0x1F, (1 << 3) | 2);
    Upd(c, R_ASP_CLK_CFG, 0x1F, 3 << 2);                /* ASP_SLAVE_MODE, SCPOL_NOR, LCPOL normal */
    if (c->Slots != 2) LOGI("CS42L42: %u slots - the ASP only does 2-slot I2S\n", c->Slots);

    /* capture: ADC +20 dB boost (only gain stage the headset ADC has), 0 dB ADC volume */
    Upd(c, R_ADC_CTL, 1, 1);                            /* ADC_DIG_BOOST */
    Wr(c, R_ADC_VOLUME, 0x00);
    Upd(c, R_HP_CTL, HP_ANA_MUTE, HP_ANA_MUTE);
    Volume(c, 65535, 65535, FALSE);

    if (c->Errors != e0) return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("CS42L42: rev %02x, reset gpio %s, SCLK %u Hz (%u slots x %u bits), CTIA headset config\n",
         rev & 0xFF, c->NGpio ? "0" : "none", c->Bclk, c->Slots, c->Slot);
    return TRUE;
}

static void Shutdown(SST_CODEC *c)
{
    ULONG v, i;
    if (c->PlayOn) Stream(c, FALSE, FALSE);
    c->PlayOn = FALSE;
    if (c->RecOn) Stream(c, TRUE, FALSE);
    c->RecOn = FALSE;
    if (!c->Priv[P_SCLK]) {
        for (i = 0; i < sizeof(s_ShutdownSeq) / sizeof(s_ShutdownSeq[0]); i += 2)
            Wr(c, s_ShutdownSeq[i], s_ShutdownSeq[i + 1]);
        CDelay(PDN_DONE_TIME_MS);
        for (i = 0; i < 48; i++) {                      /* CS42L42_PDN_DONE_TIMEOUT_US 235 ms */
            if (!Rd(c, R_CODEC_STATUS, &v) || (v & 1)) break;
            CDelay(5);
        }
        Upd(c, R_PWR_CTL2, DISCHARGE_FILT, DISCHARGE_FILT);  /* discharge FILT+ */
    }
    if (c->NGpio) CGpio(c, 0, FALSE);                   /* back into reset */
    c->Priv[P_PAGE] = 0xFFFF;
    c->Priv[P_SCLK] = 0;
}

const CODEC_DRV g_DrvCs42l42 = { "CS42L42", "10134242\0", CODEC_HEADSET, 1, 1,
                                 Probe, Init, Stream, Volume, Shutdown };
