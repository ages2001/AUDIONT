/*
 * AUDIONT - Realtek speaker amplifiers on an SSP port.
 *
 *   RT1011   "10EC1011"  I2C, 16-bit register / 16-bit value (rt1011.c)
 *   RT1015   "10EC1015"  I2C, 16/16 (rt1015.c)
 *   RT1015P / RT1019P  "RTL1015" / "RTL1019"  GPIO-only auto-mode amps (rt1015p.c)
 *   RT1308   "10EC1308"  I2C, 8-bit register / 32-bit value (rt1308.c, regmap 8/32)
 *
 * Clocks: the system clock comes from the internal PLL with BCLK as its
 * reference (rl6231_pll_calc ported below, computed at run time):
 * RT1011 / RT1015 -> 256 fs (as the Intel machine drivers set up:
 * snd_soc_dai_set_pll(.., PLLx_S_BCLK, bclk, 256 * rate)), RT1308 -> 512 fs
 * (the Intel board uses MCLK -> 512 fs; BCLK is tried first, MCLK if BCLK
 * gives no exact PLL setting).
 *
 * Channels (sof_realtek_common.c): RT1011 index 0/2 slot 0, index 1/3 slot 1
 * (rt1011_tdm_mask rx), RT1015 index 0 slot 0, index 1 slot 1; in both the
 * DAC takes L = slot 0 / R = slot 1 (TDMx_SET_4) and the mono select
 * (Left / Right / (L+R)/2) picks the channel; a single amp plays (L+R)/2.
 * The Linux boards name RT1011:00 / RT1015:00 "Left", which matches
 * "Index 0 = left".  Frame format: I2S with 2 slots, DSP_B (PCM B) TDM with
 * more (the Intel topologies' TDM format; the slot count is programmed).
 * 25-bit slots (CML RT1011: 4 x 25 = 100 fs) use the 24-bit channel length
 * exactly as Linux does (params_width).
 *
 * The amplifiers' feedback (ADCDAT) pins are left as inputs / not enabled:
 * several amps share the SSP RX line and nothing captures IV data here.
 * Volume: NULL (DSP software volume), digital gains stay at the Linux defaults.
 */
#include "codec.h"

/* 0 left, 1 right, 2 (L+R)/2 - the Realtek "Mono LR Select" / "DIN Source" encoding */
static ULONG RtChan(SST_CODEC *c)
{
    if (c->Count <= 1)
        return 2;
    return c->Index & 1;
}

static const char *RtChName(ULONG ch)
{
    return ch == 0 ? "left" : ch == 1 ? "right" : "L+R mix";
}

/* ------------------------------------------------------------------ */
/* rl6231_pll_calc                                                      */
/* ------------------------------------------------------------------ */
/*
 * Fout = Fin * (N + 2) / ((M + 2) * (K + 2)); M bypass: no (M + 2) term,
 * K == -1: K bypass.  Same search as the Linux helper (K range from the
 * 80..150 MHz VCO window, preset table first); maxN limits N for chips with
 * a narrower field (RT1308: 7 bits).  Exact: the result hits Fout exactly.
 */
typedef struct _RL_PLL {
    ULONG   M, N, K;
    BOOLEAN MBp, KBp, Exact;
} RL_PLL;

static ULONG RlDiff(ULONG a, ULONG b)
{
    return a > b ? a - b : b - a;
}

static ULONG RlBestDiv(ULONG in, ULONG max, ULONG div)
{
    ULONG d;
    if (in <= max)
        return 1;
    d = in / max;
    if (in % max)
        d++;
    while (div % d != 0)
        d++;
    return d;
}

static BOOLEAN RlPll(ULONG fin, ULONG fout, ULONG maxN, RL_PLL *p)
{
    static const struct { ULONG in, out; LONG k, n, m; BOOLEAN mbp; } preset[] = {
        { 19200000,  4096000, 23, 14, 1, FALSE },
        { 19200000, 24576000,  3, 30, 3, FALSE },
        { 48000000,  3840000, 23,  2, 0, FALSE },
        {  3840000, 24576000,  3, 30, 0, TRUE },
        {  3840000, 22579200,  3,  5, 0, TRUE },
    };
    LONG k = 0, n = 0, m = 0, kt, mint, maxk, mink;
    ULONG nt, mt, i, div, fi, fo, red, redt, int_, outt, fmax;
    BOOLEAN mbp = FALSE;

    if (fin > 50000000 || fin < 256000 || !fout)
        return FALSE;
    for (i = 0; i < sizeof(preset) / sizeof(preset[0]); i++) {
        if (preset[i].in == fin && preset[i].out == fout && (ULONG)preset[i].n <= maxN) {
            k = preset[i].k; n = preset[i].n; m = preset[i].m; mbp = preset[i].mbp;
            goto found;
        }
    }

    mink = (LONG)(80000000 / fout) - 2;
    maxk = (LONG)(150000000 / fout) - 2;
    if (maxk > 0x1F)
        maxk = 0x1F;
    if (mink > 0x1F)
        mink = maxk = 0x1F;
    fmax = 0xFFFFFFFF / 0x1FF;
    div = RlBestDiv(fin, fmax, CGcd(fin, fout));
    fi = fin / div;
    fo = fout / div;
    redt = RlDiff(fout, fin);
    k = mink;
    mint = mink < -1 ? -1 : mink;
    for (kt = mint; kt <= maxk; kt++) {
        for (nt = 0; nt <= maxN; nt++) {
            if (nt + 2 > 0xFFFFFFFF / fi)
                break;                          /* Fin * (N + 2) must fit 32 bits */
            int_ = fi * (nt + 2);
            if (int_ == fo * (ULONG)(kt + 2)) {
                mbp = TRUE; n = nt; k = kt;
                goto found;
            }
            outt = int_ / (ULONG)(kt + 2);
            red = RlDiff(fo, outt);
            if (red < redt) {
                mbp = TRUE; n = nt; m = 0; k = kt;
                if (red == 0)
                    goto found;
                redt = red;
            }
            for (mt = 0; mt <= 0xF; mt++) {
                outt = int_ / ((mt + 2) * (ULONG)(kt + 2));
                red = RlDiff(fo, outt);
                if (red < redt) {
                    mbp = FALSE; n = nt; m = mt; k = kt;
                    if (red == 0)
                        goto found;
                    redt = red;
                }
            }
        }
    }
found:
    p->KBp = FALSE;
    if (k == -1) {
        p->KBp = TRUE;
        k = 0;
    }
    p->MBp = mbp;
    p->M = (ULONG)m;
    p->N = (ULONG)n;
    p->K = (ULONG)k;
    {
        ULONGLONG lhs = (ULONGLONG)fin * (p->N + 2);
        ULONGLONG rhs = (ULONGLONG)fout * (mbp ? 1 : p->M + 2) * (p->KBp ? 1 : p->K + 2);
        p->Exact = lhs == rhs;
    }
    return TRUE;
}

/* rl6231_get_clk_info: system clock pre-divider index for sclk = 256 fs * pd */
static LONG RlClkDiv(ULONG sclk, ULONG rate)
{
    static const UCHAR pd[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
    ULONG i;
    for (i = 0; i < sizeof(pd); i++)
        if (sclk == (rate << 8) * pd[i])
            return (LONG)i;
    return -1;
}

/* TDM slot count code (2/4/6/8 -> 0..3), 0xFF unsupported */
static ULONG RtSlotsCode(ULONG slots)
{
    switch (slots) {
    case 2: return 0;
    case 4: return 1;
    case 6: return 2;
    case 8: return 3;
    default: return 0xFF;
    }
}

/* channel length code 16/20/24/32 (a 25-bit slot uses 24 like Linux params_width) */
static ULONG RtLenCode(ULONG bits)
{
    if (bits >= 32) return 3;
    if (bits >= 24) return 2;
    if (bits >= 20) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* RT1011                                                               */
/* ------------------------------------------------------------------ */
#define R11_RESET            0x0000
#define R11_CLK_2            0x0004     /* FS_SYS_PRE 15:14, PLL1_SRC 13, PLL2_SRC 12, FS_SYS_DIV 6:4 */
#define R11_PLL_1            0x000A     /* QM 15:12, BPM 11, QN 8:0 */
#define R11_PLL_2            0x000C     /* QK 4:0 */
#define R11_CLK_DET          0x0020     /* EN_MCLK_DET 15 */
#define R11_SIL_DET          0x0022
#define R11_DEVICE_ID        0x007D     /* 0x1011 */
#define R11_DAC_SET_1        0x0102
#define R11_DAC_SET_2        0x0104
#define R11_DAC_SET_3        0x0106     /* DA_MUTE_EN 15 */
#define R11_ADC_SET_1        0x0108
#define R11_ADC_SET_4        0x010E
#define R11_ADC_SET_5        0x0110
#define R11_TDM_TOTAL_SET    0x0111     /* MS 14, TX_DL 10:8, RX_DL 7:5, ADCDAT1/2 pin 4/3, DF 2:0 */
#define R11_TDM1_SET_1       0x0114     /* INV_BCLK 15, CH_TX 11:10, CH_RX 9:8, TX_LEN 6:4, RX_LEN 2:0 */
#define R11_TDM1_SET_2       0x0116     /* DOCK_EN_1 3 */
#define R11_TDM1_SET_4       0x011A     /* TX_L_DAC1_1 14:12, TX_R_DAC1_1 10:8 (slot sources) */
#define R11_TDM2_SET_1       0x011E
#define R11_TDM2_SET_2       0x0120
#define R11_MIXER_1          0x0300
#define R11_A_TIMING_1       0x0313
#define R11_A_TEMP_SEN       0x0316     /* POW_TEMP_REG 2 */
#define R11_POWER_1          0x0322
#define R11_POWER_2          0x0324
#define R11_POWER_3          0x0326
#define R11_POWER_4          0x0328
#define R11_POWER_6          0x032A
#define R11_POWER_7          0x032B
#define R11_POWER_8          0x032C
#define R11_POWER_9          0x032D     /* POW_MNL_SDB 5 */
#define R11_CLASS_D_POS      0x032E
#define R11_BOOST_CON_1      0x0330
#define R11_STP_0            0x050C     /* SPK_TEMP_PROTECT_0: STP_EN 15, RS_CLB_EN 14 */
#define R11_STP_4            0x0510
#define R11_STP_6            0x0512
#define R11_STP_7            0x0516
#define R11_DC_DET_1         0x0519
#define R11_DC_DET_2         0x051A
#define R11_BAT_GAIN_1       0x0536
#define R11_CROSS_BQ_SET_1   0x0702     /* MONO_LR_SEL 6:5 */
#define R11_SYS_RESET_1      0x1007
#define R11_SYS_RESET_2      0x1008
#define R11_SYS_RESET_3      0x1009
#define R11_DC_CALIB_3       0x1204
#define R11_CLASSD_INT_1     0x1300     /* DRIVER_READY_SPK 12 */
#define R11_SINE_GEN_1       0x1500
#define R11_INIT_RECIP_HI    0x1538     /* INIT_RECIPROCAL_REG_24_16 */
#define R11_INIT_RECIP_LO    0x1539
#define R11_INIT_SYN_HI      0x1560     /* INIT_RECIPROCAL_SYN_24_16 */
#define R11_INIT_SYN_LO      0x1562
#define R11_EFUSE_CTRL_1     0x17BB

/* DAPM supplies of the "DAC" widget */
#define R11_PWR1_MASK        0xE0A0     /* LDO2, DAC, CLK12M, ISENSE_SPK, VSENSE_SPK */
#define R11_PWR2_MASK        0x0007     /* PLLEN, BG, BG_MBIAS_LV */
#define R11_PWR3_MASK        0x5FF5     /* DET_VBAT, MBIAS_LV, ADC_I/V/T, POWD_ADC_T, MIX_I/V, SUM_I/V, MIX_T, VREF */
#define R11_PWR4_MASK        0x1600     /* EN_SWR, PASS_BGOK_SWR, PASS_VPOK_SWR */

/*
 * rt1011_calibrate(cali_flag = 1) as run by the Linux probe work: RC-clocked
 * R0 (speaker DC resistance) measurement, then de-pop and reset; the R0
 * result goes to INIT_RECIPROCAL_REG and STP_R0_SELECT = register.  It needs
 * no I2S clock; the loop waits (100 ms steps, at most 50) for three equal
 * readings.  Returns the R0 register value, 0 if it failed.
 */
static ULONG R11Calibrate(SST_CODEC *c)
{
    static const ULONG pre[] = {
        R11_RESET, 0x0000,
        R11_SYS_RESET_3, 0x740F,
        R11_SYS_RESET_3, 0x770F,
        /* RC clock */
        R11_CLK_2, 0x9400,
        R11_PLL_1, 0x0800,
        R11_PLL_2, 0x0020,
        R11_CLK_DET, 0x0800,
        /* ADC / DAC */
        R11_ADC_SET_5, 0x0A20,
        R11_DAC_SET_2, 0xE232,
        R11_ADC_SET_4, 0xC000,
        /* DC detection */
        R11_DC_DET_1, 0xB00C,
        R11_DC_DET_2, 0xCCCC,
        /* power */
        R11_POWER_1, 0xE0E0,
        R11_POWER_3, 0x5003,
        R11_POWER_9, 0xA860,
        R11_DAC_SET_2, 0xA032,
        R11_POWER_2, 0x0007,
        R11_POWER_3, 0x5FF7,
        R11_A_TEMP_SEN, 0x7F44,
        R11_A_TIMING_1, 0x4054,
        R11_BAT_GAIN_1, 0x309C,
        /* DC offset from EFUSE */
        R11_DC_CALIB_3, 0xCB00,
        R11_BOOST_CON_1, 0xE080,
        R11_POWER_4, 0x16F2,
        R11_POWER_6, 0x36AD,
        R11_MIXER_1, 0x3F1D,
        R11_EFUSE_CTRL_1, 0x0D0A,
    };
    static const ULONG stp[] = {
        R11_ADC_SET_1, 0x2925,
        R11_CLASS_D_POS, 0x010E,                /* class D on */
        R11_CLASSD_INT_1, 0x1701,
        R11_STP_0, 0x8000,                      /* STP enable */
        R11_STP_7, 0xF000,
        R11_STP_4, 0x4040,
        R11_STP_0, 0xC000,
        R11_STP_6, 0x07C2,
    };
    static const ULONG depop[] = {
        R11_POWER_9, 0xA840,
        R11_STP_6, 0x0702,
        R11_MIXER_1, 0xFFDD,
        R11_CLASSD_INT_1, 0x0701,
        R11_DAC_SET_3, 0xE004,
        R11_A_TEMP_SEN, 0x7F40,
        R11_POWER_1, 0x0000,
        R11_POWER_2, 0x0000,
        R11_POWER_3, 0x0002,
        R11_POWER_4, 0x00F2,
        R11_RESET, 0x0000,
    };
    ULONG r0[3] = { 0, 0, 0 }, count = 0, tries, hi, lo, r0reg = 0;

    if (!CSeq(c, pre, sizeof(pre) / sizeof(pre[0]) / 2))
        return 0;
    CDelay(30);                                 /* EFUSE read */
    if (!CSeq(c, stp, sizeof(stp) / sizeof(stp[0]) / 2))
        return 0;
    for (tries = 0; tries < 50; tries++) {
        CDelay(100);
        if (!CRd(c, R11_INIT_SYN_HI, &hi) || !CRd(c, R11_INIT_SYN_LO, &lo))
            break;
        r0[count % 3] = (hi << 16) | (lo & 0xFFFF);
        if (r0[count % 3] == 0)
            continue;
        count++;
        if (count >= 3 && r0[0] == r0[1] && r0[1] == r0[2]) {
            r0reg = r0[0];
            break;
        }
    }
    /* de-pop */
    CWr(c, R11_STP_0, 0x0000);
    CDelay(400);
    CSeq(c, depop, sizeof(depop) / sizeof(depop[0]) / 2);
    if (r0reg) {
        CWr(c, R11_INIT_RECIP_HI, (r0reg >> 16) & 0x1FF);
        CWr(c, R11_INIT_RECIP_LO, r0reg & 0xFFFF);
        CWr(c, R11_STP_4, 0x4080);              /* STP_R0_SELECT = register */
    }
    return r0reg;
}

static BOOLEAN R11Probe(SST_CODEC *c)
{
    ULONG v;
    return CRd(c, R11_DEVICE_ID, &v) && v == 0x1011;
}

static BOOLEAN R11Init(SST_CODEC *c)
{
    /* rt1011_reg_init */
    static const ULONG init[] = {
        R11_POWER_9, 0xA840,
        R11_ADC_SET_5, 0x0A20,
        R11_DAC_SET_2, 0xA032,
        R11_DC_DET_1, 0xB00C,
        R11_DC_DET_2, 0xCCCC,
        R11_A_TIMING_1, 0x6054,
        R11_POWER_7, 0x3E55,
        R11_POWER_8, 0x0520,
        R11_BOOST_CON_1, 0xE188,
        R11_POWER_4, 0x16F2,
        R11_CROSS_BQ_SET_1, 0x0004,
        R11_SIL_DET, 0xC313,
        R11_SINE_GEN_1, 0x0707,
        R11_DC_CALIB_3, 0xCB00,
        R11_DAC_SET_1, 0xE702,
        R11_DAC_SET_3, 0x2004,
        R11_ADC_SET_1, 0x2925,                  /* rt1011_calibration_work */
    };
    ULONG ch = RtChan(c), sc = RtSlotsCode(c->Slots), cl = RtLenCode(c->Slot), dl = RtLenCode(c->Valid);
    ULONG r0, sysclk = c->Rate * 256, v;
    LONG pd;
    RL_PLL pll;
    BOOLEAN usePll = TRUE, havePll;

    if (sc == 0xFF) {
        LOGE("RT1011 %s: %u slots not supported\n", c->Path, c->Slots);
        return FALSE;
    }
    if (!R11Probe(c))
        return FALSE;

    r0 = R11Calibrate(c);
    if (!R11Probe(c))                           /* the calibration ends with a reset */
        return FALSE;
    if (!CSeq(c, init, sizeof(init) / sizeof(init[0]) / 2))
        return FALSE;

    /* system clock: PLL1 from BCLK -> 256 fs; MCLK direct if BCLK gives no exact PLL and MCLK = 256 fs * pd */
    havePll = RlPll(c->Bclk, sysclk, 0x1FF, &pll);
    pd = c->Mclk ? RlClkDiv(c->Mclk, c->Rate) : -1;
    if ((!havePll || !pll.Exact) && pd >= 0) {
        usePll = FALSE;
        if (!CUpd(c, R11_CLK_DET, 0x8000, 0x8000) ||
            !CUpd(c, R11_CLK_2, 0xC070, (0u << 14) | ((ULONG)pd << 4)))      /* FS_SYS_PRE = MCLK */
            return FALSE;
    } else {
        if (!havePll) {
            LOGE("RT1011 %s: BCLK %u unusable for the PLL\n", c->Path, c->Bclk);
            return FALSE;
        }
        if (!CUpd(c, R11_CLK_DET, 0x8000, 0x0000) ||
            !CUpd(c, R11_CLK_2, 0xE070, (2u << 14) | (1u << 13)) ||  /* FS_SYS_PRE = PLL1, PLL1_SRC = BCLK */
            !CWr(c, R11_PLL_1, ((pll.MBp ? 0 : pll.M) << 12) | ((ULONG)pll.MBp << 11) | pll.N) ||
            !CWr(c, R11_PLL_2, pll.K))
            return FALSE;
    }

    /* set_dai_fmt: consumer, I2S or PCM B, normal BCLK; hw_params: data length; set_tdm_slot */
    v = (c->Slots > 2 ? 3 : 0) | (dl << 8) | (dl << 5) | (1 << 4) | (1 << 3);   /* ADCDAT1/2 inputs */
    if (!CUpd(c, R11_TDM_TOTAL_SET, 0x47FF, v))
        return FALSE;
    v = (sc << 10) | (sc << 8) | (cl << 4) | cl;
    if (!CUpd(c, R11_TDM1_SET_1, 0x8F77, v) ||
        !CUpd(c, R11_TDM2_SET_1, 0x8F77, v) ||
        !CUpd(c, R11_TDM1_SET_4, 0x7700, (0 << 12) | (1 << 8)) ||               /* L = slot 0, R = slot 1 */
        !CUpd(c, R11_CROSS_BQ_SET_1, 0x0060, ch << 5) ||                        /* mono L / R / (L+R)/2 */
        !CUpd(c, R11_TDM1_SET_2, 0x0008, 0x0008) ||                             /* TDM dock enable */
        !CUpd(c, R11_TDM2_SET_2, 0x0008, 0x0008) ||
        !CUpd(c, R11_DAC_SET_3, 0x8000, 0x8000))                                /* DAC widget off: muted */
        return FALSE;

    c->Priv[0] = usePll;
    c->Caps = CC_PLAY;
    if (usePll)
        LOGI("RT1011 %s: %s, %u x %u-bit %s, R0 %x, PLL BCLK %u -> %u: N %u M %u%s K %u%s\n", c->Path,
             RtChName(ch), c->Slots, c->Slot, c->Slots > 2 ? "TDM" : "I2S", r0, c->Bclk, sysclk, pll.N, pll.M,
             pll.MBp ? " (bypass)" : "", pll.K, pll.Exact ? "" : " (approx.)");
    else
        LOGI("RT1011 %s: %s, %u x %u-bit %s, R0 %x, sysclk MCLK %u\n", c->Path, RtChName(ch), c->Slots, c->Slot,
             c->Slots > 2 ? "TDM" : "I2S", r0, c->Mclk);
    if (!r0)
        LOGE("RT1011 %s: R0 calibration failed, speaker protection uses the EFUSE R0\n", c->Path);
    return TRUE;
}

static void R11Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    if (on) {
        /* supplies of the DAC widget, then the DAC (DA_MUTE_EN inverted), then rt1011_dac_event POST_PMU */
        CUpd(c, R11_POWER_1, R11_PWR1_MASK, R11_PWR1_MASK);
        CUpd(c, R11_POWER_2, R11_PWR2_MASK, c->Priv[0] ? R11_PWR2_MASK : 0x0003);
        CUpd(c, R11_POWER_3, R11_PWR3_MASK, R11_PWR3_MASK);
        CUpd(c, R11_POWER_4, R11_PWR4_MASK, R11_PWR4_MASK);
        CUpd(c, R11_A_TEMP_SEN, 0x0004, 0x0004);
        CUpd(c, R11_DAC_SET_3, 0x8000, 0x0000);
        CUpd(c, R11_STP_0, 0xC000, 0xC000);                 /* STP_EN | STP_RS_CLB_EN */
        CUpd(c, R11_POWER_9, 0x0020, 0x0020);               /* POW_MNL_SDB */
        CDelay(50);
        CUpd(c, R11_CLASSD_INT_1, 0x1000, 0x1000);          /* DRIVER_READY_SPK */
    } else {
        /* PRE_PMD */
        CUpd(c, R11_POWER_9, 0x0020, 0x0000);
        CUpd(c, R11_STP_0, 0xC000, 0x0000);
        CDelay(200);
        CUpd(c, R11_CLASSD_INT_1, 0x1000, 0x0000);
        CUpd(c, R11_DAC_SET_3, 0x8000, 0x8000);
        CUpd(c, R11_A_TEMP_SEN, 0x0004, 0x0000);
        CUpd(c, R11_POWER_4, R11_PWR4_MASK, 0);
        CUpd(c, R11_POWER_3, R11_PWR3_MASK, 0);
        CUpd(c, R11_POWER_2, R11_PWR2_MASK, 0);
        CUpd(c, R11_POWER_1, R11_PWR1_MASK, 0);
        /* rt1011_set_bias_level(OFF) */
        CWr(c, R11_SYS_RESET_1, 0x0000);
        CWr(c, R11_SYS_RESET_2, 0x0000);
        CWr(c, R11_SYS_RESET_3, 0x0001);
        CWr(c, R11_SYS_RESET_1, 0x003F);
        CWr(c, R11_SYS_RESET_2, 0x7FD7);
        CWr(c, R11_SYS_RESET_3, 0x770F);
    }
}

static void R11Shutdown(SST_CODEC *c)
{
    CUpd(c, R11_POWER_9, 0x0020, 0x0000);
    CUpd(c, R11_STP_0, 0xC000, 0x0000);
    CDelay(200);
    CWr(c, R11_RESET, 0x0000);                  /* rt1011_i2c_shutdown */
}

const CODEC_DRV g_DrvRt1011 = { "RT1011", "10EC1011\0", CODEC_AMP, 2, 2,
                                R11Probe, R11Init, R11Stream, NULL, R11Shutdown };

/* ------------------------------------------------------------------ */
/* RT1015                                                               */
/* ------------------------------------------------------------------ */
#define R15_RESET            0x0000
#define R15_CLK2             0x0004     /* CLK_SYS_PRE_SEL 15:14, PLL_SEL 13, FS_PD 6:4 */
#define R15_PLL1             0x000A     /* M 15:12, M_BP 11, N 8:0 */
#define R15_PLL2             0x000C     /* K 4:0 */
#define R15_CLK_DET          0x0020     /* EN_BCLK_DET 15 */
#define R15_DEVICE_ID        0x007D     /* 0x1011 or 0x1015 */
#define R15_PAD_DRV2         0x00F2     /* MONO_LR_SEL 5:4 */
#define R15_GAT_BOOST        0x00F3
#define R15_TDM_MASTER       0x0111     /* MS 14, DL 10:8, DF 2:0 */
#define R15_TDM1_1           0x0114     /* INV_BCLK 15, CH_TX 11:10, CH_RX 9:8, TX_LEN 6:4, RX_LEN 2:0 */
#define R15_TDM1_4           0x011A     /* L_DAC1 slot 14:12, R_DAC1 slot 10:8 */
#define R15_PWR1             0x0322     /* PWR_PLL 9 */
#define R15_PWR9             0x032D
#define R15_DC_DET1          0x0519     /* EN_CLA_D_DC_DET 12 */
#define R15_SYS_RST1         0x1007
#define R15_SYS_RST2         0x1009

static BOOLEAN R15Probe(SST_CODEC *c)
{
    ULONG v;
    return CRd(c, R15_DEVICE_ID, &v) && (v == 0x1011 || v == 0x1015);
}

static BOOLEAN R15Init(SST_CODEC *c)
{
    ULONG ch = RtChan(c), sc = RtSlotsCode(c->Slots), cl = RtLenCode(c->Slot), dl, id = 0, v;
    ULONG sysclk = c->Rate * 256;
    RL_PLL pll;

    if (sc == 0xFF) {
        LOGE("RT1015 %s: %u slots not supported\n", c->Path, c->Slots);
        return FALSE;
    }
    /* I2S_DL: 16 / 20 / 24 (rt1015_hw_params has no 32-bit data length) */
    dl = c->Valid >= 24 ? 2 : c->Valid >= 20 ? 1 : 0;

    CWr(c, R15_RESET, 0x0000);
    if (!CRd(c, R15_DEVICE_ID, &id))
        return FALSE;

    /* rt1015_set_component_pll(PLL_S_BCLK, bclk, 256 fs) + sysclk from PLL (sof_realtek_common rt1015_hw_params) */
    if (!RlPll(c->Bclk, sysclk, 0x1FF, &pll)) {
        LOGE("RT1015 %s: BCLK %u unusable for the PLL\n", c->Path, c->Bclk);
        return FALSE;
    }
    if (!CUpd(c, R15_CLK2, 0xE070, (2u << 14) | (1u << 13)) ||
        !CWr(c, R15_PLL1, ((pll.MBp ? 0 : pll.M) << 12) | ((ULONG)pll.MBp << 11) | pll.N) ||
        !CWr(c, R15_PLL2, pll.K))
        return FALSE;

    /* set_dai_fmt (consumer, I2S / PCM B, normal BCLK), hw_params data length, tdm slots */
    v = (c->Slots > 2 ? 3 : 0) | (dl << 8);
    if (!CUpd(c, R15_TDM_MASTER, 0x4707, v))
        return FALSE;
    v = (sc << 10) | (sc << 8) | (cl << 4) | cl;
    if (!CUpd(c, R15_TDM1_1, 0x8F77, v) ||
        !CUpd(c, R15_TDM1_4, 0x7700, (0 << 12) | (1 << 8)) ||       /* L = slot 0, R = slot 1 */
        !CUpd(c, R15_PAD_DRV2, 0x0030, ch << 4))                    /* Mono LR Select */
        return FALSE;
    /* PLL supply off until the stream runs */
    if (!CUpd(c, R15_PWR1, 0x0200, 0x0000))
        return FALSE;

    c->Caps = CC_PLAY;
    LOGI("RT1015 %s: id %04x, %s, %u x %u-bit %s, PLL BCLK %u -> %u: N %u M %u%s K %u%s\n", c->Path, id,
         RtChName(ch), c->Slots, c->Slot, c->Slots > 2 ? "TDM" : "I2S", c->Bclk, sysclk, pll.N, pll.M,
         pll.MBp ? " (bypass)" : "", pll.K, pll.Exact ? "" : " (approx.)");
    return TRUE;
}

static void R15Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    ULONG v;
    if (rec)
        return;
    if (on) {
        CUpd(c, R15_PWR1, 0x0200, 0x0200);                  /* "PLL" supply */
        /* r1015_dac_event PRE_PMU, boost enabled (bypass_boost = RT1015_Enable_Boost) */
        CWr(c, R15_SYS_RST1, 0x05F7);
        CWr(c, R15_SYS_RST2, 0x0B0A);
        CWr(c, R15_GAT_BOOST, 0xACFE);
        CWr(c, R15_PWR9, 0xAA00);
        CWr(c, R15_GAT_BOOST, 0xECFE);
        /* rt1015_amp_drv_event PRE_PMU: BCLK detection, class-D DC detection */
        if (CRd(c, R15_CLK_DET, &v) && !(v & 0x8000))
            CUpd(c, R15_CLK_DET, 0x8000, 0x8000);
        if (CRd(c, R15_DC_DET1, &v) && !(v & 0x1000))
            CUpd(c, R15_DC_DET1, 0x1000, 0x1000);
        CDelay(50);                                         /* POST_PMU: power_up_delay_ms */
    } else {
        /* r1015_dac_event POST_PMD */
        CWr(c, R15_PWR9, 0xA800);
        CWr(c, R15_SYS_RST1, 0x05F5);
        CWr(c, R15_SYS_RST2, 0x0B9A);
        CUpd(c, R15_PWR1, 0x0200, 0x0000);
    }
}

static void R15Shutdown(SST_CODEC *c)
{
    CWr(c, R15_PWR9, 0xA800);
    CWr(c, R15_SYS_RST1, 0x05F5);
    CWr(c, R15_SYS_RST2, 0x0B9A);
    CWr(c, R15_RESET, 0x0000);                  /* rt1015_i2c_shutdown */
}

const CODEC_DRV g_DrvRt1015 = { "RT1015", "10EC1015\0", CODEC_AMP, 2, 2,
                                R15Probe, R15Init, R15Stream, NULL, R15Shutdown };

/* ------------------------------------------------------------------ */
/* RT1015P / RT1019P: GPIO-only (rt1015p.c handles both HIDs)           */
/* ------------------------------------------------------------------ */
/*
 * Auto-mode amps without a control port; "sdb" (ACPI GpioIo 0) high enables.
 * One ACPI device often serves both amps of a stereo pair sharing the SDB
 * line; the left / right choice is strapped on the board.  The first enable
 * after power-up runs the internal calibration: wait 300 ms (rt1015p_sdb_event).
 * Linux restricts the DAI to 48 kHz with 24/32-bit samples (64 fs BCLK).
 * Probe always succeeds (the GPIO is optional in Linux).  Priv[0]: calibrated.
 */
static BOOLEAN R15pProbe(SST_CODEC *c)
{
    (void)c;
    return TRUE;
}

static BOOLEAN R15pInit(SST_CODEC *c)
{
    CGpio(c, 0, FALSE);
    c->Priv[0] = 0;
    c->Caps = CC_PLAY;
    if (c->Rate != 48000 || c->Slot < 24)
        LOGE("%s: auto-mode amp expects 48 kHz with 24/32-bit slots (have %u Hz, %u-bit)\n", c->Hid, c->Rate,
             c->Slot);
    LOGI("%s: GPIO-only amp, sdb %s\n", c->Hid, c->NGpio ? "GpioIo 0" : "hard-wired");
    return TRUE;
}

static void R15pStream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    if (on) {
        CGpio(c, 0, TRUE);
        if (!c->Priv[0]) {
            CDelay(300);
            c->Priv[0] = 1;
        }
    } else {
        CGpio(c, 0, FALSE);
    }
}

static void R15pShutdown(SST_CODEC *c)
{
    CGpio(c, 0, FALSE);
    c->Priv[0] = 0;                             /* rt1015p_suspend: calibrate again after power loss */
}

const CODEC_DRV g_DrvRt1015p = { "RT1015P", "RTL1015\0RTL1019\0", CODEC_AMP, 0, 0,
                                 R15pProbe, R15pInit, R15pStream, NULL, R15pShutdown };

/* ------------------------------------------------------------------ */
/* RT1308 (8-bit register, 32-bit value)                                */
/* ------------------------------------------------------------------ */
#define R08_RESET            0x00
#define R08_PLL_1            0x03       /* K 28:24, M_BYPASS 23, M 21:16, N 14:8 */
#define R08_CLK_1            0x06       /* DIV_FS_SYS 31:28, SEL_FS_SYS 26:24 */
#define R08_DATA_PATH        0x07       /* RX channel select 25:24: LR, LL, RL, RR */
#define R08_CLK_2            0x08       /* SEL_PLL_SRC 26:24 */
#define R08_SIL_DET          0x09
#define R08_CLK_DET          0x0A       /* MCLK_DET_EN 25 */
#define R08_DAC_SET          0x10
#define R08_I2S_SET_1        0x15       /* DF_SEL 13:12 */
#define R08_I2S_SET_2        0x16       /* DL_SEL 26:24, BCLK_INV 14 */
#define R08_I2C_I2S_SDW_SET  0x17
#define R08_IV_SENSE         0x1A
#define R08_DC_CAL_2         0x21
#define R08_PVDD_OFFSET_CTL  0x24
#define R08_CLASS_D_SET_2    0x31
#define R08_POWER            0x32
#define R08_VREF             0x34
#define R08_POWER_STATUS     0x36       /* PDB_MN 25, PDB_REG 24 */
#define R08_VEN_DEV_ID       0x70       /* 0x10ec13xx */
#define R08_EFUSE_1          0x7F
#define R08_PADS_1           0x90
#define R08_DUMMY_REG        0xF0

/* DAPM supplies: MBIAS20U, ALDO, DBG, DACL, DAC1, CLK25M, ADC_R/L, DLDO, VREF, MIXER_R/L, MBIAS4U, PLL2 x5 */
#define R08_PWR_MASK         0xFF371F00

static BOOLEAN R08Probe(SST_CODEC *c)
{
    ULONG v;
    return CRd(c, R08_VEN_DEV_ID, &v) && (v & 0xFFFFFF00) == 0x10EC1300;   /* last byte: revision */
}

static BOOLEAN R08Init(SST_CODEC *c)
{
    static const ULONG init[] = {
        R08_I2C_I2S_SDW_SET, 0x01014005,
        R08_CLASS_D_SET_2, 0x227F5501,
        R08_PADS_1, 0x50150505,
        R08_VREF, 0x18100000,
        R08_IV_SENSE, 0x87010000,
        R08_DUMMY_REG, 0x00000200,
        R08_SIL_DET, 0xE1C30000,
        R08_DC_CAL_2, 0x00FFFF00,
        R08_CLK_DET, 0x01000000,
        R08_POWER_STATUS, 0x08800000,
        R08_DAC_SET, 0xAFAF0700,
    };
    ULONG id = 0, sysclk = c->Rate * 512, dl, sel;
    LONG pd;
    RL_PLL pll;
    BOOLEAN fromMclk = FALSE;

    if (!CRd(c, R08_VEN_DEV_ID, &id))
        return FALSE;
    /* rt1308_efuse (i2c probe) */
    CWr(c, R08_RESET, 0);
    if (!CWr(c, R08_POWER_STATUS, 0x01800000))
        return FALSE;
    CDelay(100);
    CWr(c, R08_EFUSE_1, 0x44FE0F00);
    CDelay(20);
    CWr(c, R08_PVDD_OFFSET_CTL, 0x10000000);
    /* rt1308_reg_init */
    if (!CSeq(c, init, sizeof(init) / sizeof(init[0]) / 2))
        return FALSE;

    /* PLL -> 512 fs: BCLK first (N field is 7 bits), MCLK if BCLK gives no exact setting */
    if (!RlPll(c->Bclk, sysclk, 0x7F, &pll) || !pll.Exact) {
        RL_PLL pm;
        if (c->Mclk && RlPll(c->Mclk, sysclk, 0x7F, &pm) && pm.Exact) {
            pll = pm;
            fromMclk = TRUE;
        } else if (!RlPll(c->Bclk, sysclk, 0x7F, &pll)) {
            LOGE("RT1308 %s: no PLL setting from BCLK %u / MCLK %u\n", c->Path, c->Bclk, c->Mclk);
            return FALSE;
        }
    }
    pd = RlClkDiv(sysclk, c->Rate);                         /* 512 fs -> index 1 */
    sel = fromMclk ? 0 : 1;                                 /* SEL_PLL_SRC MCLK / BCLK */
    if (!CUpd(c, R08_CLK_2, 0x07000000, sel << 24) ||
        !CUpd(c, R08_CLK_DET, 0x02000000, fromMclk ? 0x02000000 : 0) ||
        !CWr(c, R08_PLL_1, (pll.K << 24) | ((ULONG)pll.MBp << 23) | ((pll.MBp ? 0 : pll.M) << 16) | (pll.N << 8)) ||
        !CUpd(c, R08_CLK_1, 0xF7000000, ((ULONG)pd << 28) | (2u << 24)))   /* SEL_FS_SYS = PLL */
        return FALSE;

    /* set_dai_fmt: consumer, I2S / PCM B, normal BCLK; hw_params: data length 16 / 20 / 24 */
    dl = c->Valid >= 24 ? 2 : c->Valid >= 20 ? 1 : 0;
    if (!CUpd(c, R08_I2S_SET_1, 0x00003000, (c->Slots > 2 ? 3u : 0u) << 12) ||
        !CUpd(c, R08_I2S_SET_2, 0x07004000, dl << 24))
        return FALSE;
    /* stereo amp: LR normally; on a shared port index 0 plays LL (left), index 1 RR (right) */
    sel = c->Count <= 1 ? 0 : (c->Index & 1) ? 3 : 1;
    if (!CUpd(c, R08_DATA_PATH, 0x03000000, sel << 24))
        return FALSE;
    if (!CUpd(c, R08_POWER, R08_PWR_MASK, 0))
        return FALSE;

    c->Caps = CC_PLAY;
    LOGI("RT1308 %s: id %08x, channels %s, %u x %u-bit %s, PLL %s %u -> %u: N %u M %u%s K %u%s\n", c->Path, id,
         sel == 0 ? "LR" : sel == 1 ? "LL" : "RR", c->Slots, c->Slot, c->Slots > 2 ? "TDM" : "I2S",
         fromMclk ? "MCLK" : "BCLK", fromMclk ? c->Mclk : c->Bclk, sysclk, pll.N, pll.M, pll.MBp ? " (bypass)" : "",
         pll.K, pll.Exact ? "" : " (approx.)");
    return TRUE;
}

static void R08Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec)
        return;
    if (on) {
        CUpd(c, R08_POWER, R08_PWR_MASK, R08_PWR_MASK);
        /* rt1308_classd_event POST_PMU */
        CDelay(30);
        CUpd(c, R08_POWER_STATUS, 0x03000000, 0x03000000);
        CDelay(40);
    } else {
        /* PRE_PMD */
        CUpd(c, R08_POWER_STATUS, 0x03000000, 0);
        CDelay(150);
        CUpd(c, R08_POWER, R08_PWR_MASK, 0);
    }
}

static void R08Shutdown(SST_CODEC *c)
{
    CUpd(c, R08_POWER_STATUS, 0x03000000, 0);
    CDelay(150);
    CUpd(c, R08_POWER, R08_PWR_MASK, 0);
    CWr(c, R08_RESET, 0);                       /* rt1308_i2c_shutdown */
}

const CODEC_DRV g_DrvRt1308 = { "RT1308", "10EC1308\0", CODEC_AMP, 1, 4,
                                R08Probe, R08Init, R08Stream, NULL, R08Shutdown };
