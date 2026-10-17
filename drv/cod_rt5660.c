#include "codec.h"

/*
 * Realtek RT5660 (ALC5660): stereo DAC with line out (LOUTL/R) and a mono class-D speaker
 * output (SPO), stereo ADC with three analog boost inputs and a DMIC interface.
 * Ported from Linux sound/soc/codecs/rt5660.c / rt5660.h / rl6231.c and the Elkhart Lake
 * machine driver sound/soc/intel/boards/ehl_rt5660.c (speaker on SPO, line out on LOUT,
 * "Headset Mic" on IN1P with MICBIAS1, "Headset Mic2" on IN2P with MICBIAS2, sysclk =
 * PLL1 = 512 fs from BCLK).
 *
 * I2C: 8-bit register, 16-bit value, big endian.  Private registers (PR-xx) are reached
 * through PRIV_INDEX (0x6a) / PRIV_DATA (0x6c).
 *
 * c->Mic:  0 = IN1P single ended + MICBIAS1 (EHL "Headset Mic"), BST1 +24 dB
 *          1 = IN2P + MICBIAS2 (EHL "Headset Mic2"), BST2 +24 dB
 *          2 = codec DMIC1: clock on GPIO1 (DMIC1_SCL), data on GPIO2 (DMIC1_SDA)
 * Both ADC channels carry the (mono) microphone: BSTx feeds RECMIXL and RECMIXR.
 *
 * The speaker is mono: DAC L and R are summed in SPK MIX and the SPKVOL stage is set to
 * -6 dB, so SPO plays (L+R)/2; line out gets DAC L / R directly.
 */

#define R_RESET             0x00
#define R_SPK_VOL           0x01    /* [15] SPO mute, [14] SPKVOL mute, [13:8] vol (inverted, 1.5 dB, 8 = 0 dB) */
#define R_LOUT_VOL          0x02    /* [15]/[7] LOUT L/R mute, [14]/[6] OUTVOL mute, [13:8]/[5:0] vol */
#define R_IN1_IN2           0x0d    /* [15] IN1 diff, [14:8] BST1, [7] IN2 diff, [6:0] BST2 (0.75 dB, 16 = 0 dB) */
#define R_DAC1_DIG_VOL      0x19    /* [15:9] L, [7:1] R: 0..87 = -65.25..0 dB */
#define R_STO1_ADC_DIG_VOL  0x1c    /* [15]/[7] mute, [14:9]/[6:1] vol (23 = 0 dB) */
#define R_ADC_BST_VOL1      0x1e
#define R_STO1_ADC_MIXER    0x27    /* [14]/[6] ADC1 (analog) mute, [13]/[5] ADC2 (DMIC) mute */
#define R_AD_DA_MIXER       0x29    /* [15]/[7] ADC loopback mute, [14]/[6] DAC1 mute */
#define R_STO_DAC_MIXER     0x2a    /* [14] DAC L1->L, [9] R1->L, [6] R1->R, [1] L1->R mutes */
#define R_DIG_INF1_DATA     0x2f
#define R_REC_L2_MIXER      0x3c    /* [3] BST3, [2] BST2, [1] BST1, [0] OUTMIXL mutes */
#define R_REC_R2_MIXER      0x3e
#define R_LOUT_MIXER        0x45    /* [14] DAC1 mute, [13] LOUTVOL mute */
#define R_SPK_MIXER         0x46    /* [5] DACR, [4] BST1, [3] BST3, [2] DACL, [1] OUTMIXL mutes */
#define R_SPO_MIXER         0x48    /* [14] DACR, [13] DACL, [12] SPKVOL, [11] BST1 mutes */
#define R_PWR_DIG1          0x61
#define R_PWR_DIG2          0x62
#define R_PWR_ANLG1         0x63
#define R_PWR_ANLG2         0x64
#define R_PWR_MIXER         0x65
#define R_PWR_VOL           0x66
#define R_PRIV_INDEX        0x6a
#define R_PRIV_DATA         0x6c
#define R_I2S1_SDP          0x70
#define R_ADDA_CLK1         0x73
#define R_DMIC_CTRL1        0x75
#define R_GLB_CLK           0x80
#define R_PLL_CTRL1         0x81
#define R_PLL_CTRL2         0x82
#define R_LOUT_AMP_CTRL     0x8e
#define R_GPIO_CTRL1        0xc0
#define R_ALC_PGA_CTRL2     0xeb
#define R_GEN_CTRL1         0xfa
#define R_VENDOR_ID2        0xff

#define PR_CHOP_DAC_ADC     0x3d

#define RT5660_DEVICE_ID    0x6338

/* PWR_DIG1 */
#define PWR_I2S1            0x8000
#define PWR_DAC_L1          0x1000
#define PWR_DAC_R1          0x0800
#define PWR_ADC_L           0x0004
#define PWR_ADC_R           0x0002
#define PWR_CLS_D           0x0001
/* PWR_DIG2 */
#define PWR_ADC_S1F         0x8000
#define PWR_DAC_S1F         0x0800
/* PWR_ANLG1 */
#define PWR_VREF1           0x8000
#define PWR_FV1             0x4000
#define PWR_MB              0x2000
#define PWR_BG              0x0800
#define PWR_HA              0x0020
#define PWR_VREF2           0x0010
#define PWR_FV2             0x0008
#define PWR_LDO2            0x0004
/* PWR_ANLG2 */
#define PWR_BST1            0x8000
#define PWR_BST2            0x4000
#define PWR_MB1             0x0800
#define PWR_MB2             0x0400
#define PWR_PLL             0x0200
/* PWR_MIXER */
#define PWR_SM              0x2000
#define PWR_RM_L            0x0800
#define PWR_RM_R            0x0400
/* PWR_VOL */
#define PWR_SV              0x8000
/* GEN_CTRL1 */
#define GEN_VREF_HP         0x0800
#define GEN_AUTO_DIS_AMP    0x0040
#define GEN_MCLK_DET        0x0020
#define GEN_POW_CLKDET      0x0002
#define GEN_DIG_GATE        0x0001
/* LOUT_AMP_CTRL */
#define LOUT_CO_EN          0x0010
#define LOUT_CB_PU          0x0001

#define SPK_VOL_M6DB        12      /* SPK_VOL step for -6 dB: 0 dB = 8, 1.5 dB per step */
#define BST_P24DB           48      /* BST1/2: -12 dB + 0.75 dB * 48 = +24 dB */

/* c->Priv[] */
#define P_PLL       0       /* sysclk from PLL1 */
#define P_VREF      1       /* bias VREF powered (set_bias_level STANDBY from OFF done) */
#define P_PLAY      2
#define P_REC       3
#define P_SYSCLK    4
#define P_PD        5       /* I2S pre divider code */

static BOOLEAN PrWr(SST_CODEC *c, ULONG reg, ULONG val)
{
    return CWr(c, R_PRIV_INDEX, reg) && CWr(c, R_PRIV_DATA, val);
}

static BOOLEAN PrUpd(SST_CODEC *c, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;

    if (!CWr(c, R_PRIV_INDEX, reg) || !CRd(c, R_PRIV_DATA, &v))
        return FALSE;
    return PrWr(c, reg, (v & ~mask) | (val & mask));
}

/* ------------------------------------------------------------------ */
/* rl6231 helpers                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    ULONG   M, N, K;
    BOOLEAN MBypass, KBypass;
} PLL_CODE;

static ULONG AbsDiff(ULONG a, ULONG b)
{
    return a > b ? a - b : b - a;
}

static ULONG FindBestDiv(ULONG in, ULONG max, ULONG div)
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

/* rl6231_pll_calc: Fout = Fin * (N + 2) / ((M + 2) * (K + 2)), M optional (bypass) */
static BOOLEAN PllCalc(ULONG fin, ULONG fout, PLL_CODE *p)
{
    static const ULONG preset[][7] = {     /* in, out, k, n, m, m_bp, k_bp */
        { 19200000,  4096000, 23, 14, 1, 0, 0 },
        { 19200000, 24576000,  3, 30, 3, 0, 0 },
        { 48000000,  3840000, 23,  2, 0, 0, 0 },
        {  3840000, 24576000,  3, 30, 0, 1, 0 },
        {  3840000, 22579200,  3,  5, 0, 1, 0 },
    };
    LONG k, n = 0, m = 0, kt, nt, mt, mink, maxk;
    ULONG red, redt, inT, outT, pllOut, div, fi, fo, i;
    BOOLEAN mbp = FALSE, kbp = FALSE;

    if (fin > 50000000 || fin < 256000 || fout == 0)
        return FALSE;
    for (i = 0; i < sizeof(preset) / sizeof(preset[0]); i++) {
        if (fin == preset[i][0] && fout == preset[i][1]) {
            k = (LONG)preset[i][2];
            n = (LONG)preset[i][3];
            m = (LONG)preset[i][4];
            mbp = preset[i][5] != 0;
            kbp = preset[i][6] != 0;
            goto found;
        }
    }
    redt = AbsDiff(fout, fin);
    mink = (LONG)(80000000 / fout) - 2;
    maxk = (LONG)(150000000 / fout) - 2;
    if (maxk > 0x1f)
        maxk = 0x1f;
    if (mink > 0x1f)
        mink = maxk = 0x1f;
    div = FindBestDiv(fin, 0xffffffffUL / 0x1ff, CGcd(fin, fout));
    fi = fin / div;
    fo = fout / div;
    k = mink;
    if (mink < -1)
        mink = -1;
    for (kt = mink; kt <= maxk; kt++) {
        for (nt = 0; nt <= 0x1ff; nt++) {
            inT = fi * (ULONG)(nt + 2);
            pllOut = fo * (ULONG)(kt + 2);
            if (inT == pllOut) {
                mbp = TRUE;
                n = nt;
                k = kt;
                goto found;
            }
            outT = inT / (ULONG)(kt + 2);
            red = AbsDiff(fo, outT);
            if (red < redt) {
                mbp = TRUE;
                n = nt;
                m = 0;
                k = kt;
                if (red == 0)
                    goto found;
                redt = red;
            }
            for (mt = 0; mt <= 0xf; mt++) {
                outT = inT / ((ULONG)(mt + 2) * (ULONG)(kt + 2));
                red = AbsDiff(fo, outT);
                if (red < redt) {
                    mbp = FALSE;
                    n = nt;
                    m = mt;
                    k = kt;
                    if (red == 0)
                        goto found;
                    redt = red;
                }
            }
        }
    }
    LOGE("RT5660: PLL %u -> %u Hz only approximated\n", fin, fout);
found:
    if (k == -1) {
        kbp = TRUE;
        k = 0;
    }
    p->M = (ULONG)m;
    p->N = (ULONG)n;
    p->K = (ULONG)k;
    p->MBypass = mbp;
    p->KBypass = kbp;
    return TRUE;
}

/* rl6231_get_clk_info: sysclk = 256 fs * pd[i] -> i, -1 if none */
static LONG ClkInfo(ULONG sclk, ULONG rate)
{
    static const ULONG pd[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
    ULONG i;

    if (!sclk || !rate)
        return -1;
    for (i = 0; i < 8; i++)
        if (sclk == (rate << 8) * pd[i])
            return (LONG)i;
    return -1;
}

/* rl6231_calc_dmic_clk: divider index (2, 3, 4, 6, 8, 12; multiples of 3 skipped) for <= 1.536 MHz */
static LONG DmicClk(ULONG rate)
{
    static const ULONG div[] = { 2, 3, 4, 6, 8, 12 };
    ULONG i;

    if (rate < 2000000)
        return -1;
    for (i = 0; i < 6; i++) {
        if (div[i] % 3 == 0)
            continue;
        if (1536000 * div[i] >= rate)
            return (LONG)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */

static BOOLEAN R5660Probe(SST_CODEC *c)
{
    ULONG v;

    return CRd(c, R_VENDOR_ID2, &v) && v == RT5660_DEVICE_ID;
}

/*
 * System clock 512 fs (24.576 MHz at 48 kHz) as ehl_rt5660 sets it: PLL1 from BCLK1; if BCLK
 * is outside the PLL input range use MCLK - directly when it is 256 fs * {1,2,3,4,6,8,12,16},
 * else through PLL1.  Writes GLB_CLK / PLL_CTRL1 / PLL_CTRL2 / ADDA_CLK1.
 */
static BOOLEAN R5660Clocks(SST_CODEC *c)
{
    ULONG sysclk = c->Rate * 512, src = 0, in = 0;
    LONG pd;
    PLL_CODE pll;

    c->Priv[P_PLL] = 0;
    if (c->Bclk >= 256000 && c->Bclk <= 50000000) {
        src = 1;                                    /* PLL1_SRC_BCLK1 */
        in = c->Bclk;
    } else if (c->Mclk && ClkInfo(c->Mclk, c->Rate) >= 0) {
        sysclk = c->Mclk;                           /* RT5660_SCLK_S_MCLK */
    } else if (c->Mclk >= 256000 && c->Mclk <= 50000000) {
        src = 0;                                    /* PLL1_SRC_MCLK */
        in = c->Mclk;
    } else {
        LOGE("RT5660: no usable clock (BCLK %u, MCLK %u)\n", c->Bclk, c->Mclk);
        return FALSE;
    }
    pd = ClkInfo(sysclk, c->Rate);
    if (pd < 0)
        return FALSE;
    c->Priv[P_SYSCLK] = sysclk;
    c->Priv[P_PD] = (ULONG)pd;

    if (in) {
        if (!PllCalc(in, sysclk, &pll))
            return FALSE;
        c->Priv[P_PLL] = 1;
        CUpd(c, R_GLB_CLK, 0xc000, 0x4000);         /* SCLK_SRC_PLL1 */
        CUpd(c, R_GLB_CLK, 0x3000, src << 12);
        CWr(c, R_PLL_CTRL1, (pll.N << 7) | pll.K);
        CWr(c, R_PLL_CTRL2, ((pll.MBypass ? 0 : pll.M) << 12) | ((ULONG)pll.MBypass << 11));
        LOGI("RT5660: PLL1 %s %u -> %u Hz: N %u K %u M %u%s\n", src ? "BCLK" : "MCLK", in, sysclk,
             pll.N, pll.K, pll.M, pll.MBypass ? " (bypass)" : "");
    } else {
        CUpd(c, R_GLB_CLK, 0xc000, 0x0000);         /* SCLK_SRC_MCLK */
        LOGI("RT5660: sysclk = MCLK %u Hz\n", sysclk);
    }
    /* rt5660_hw_params: BCLK_MS1 (64 fs when the frame is > 32 bits), I2S pre divider */
    CUpd(c, R_ADDA_CLK1, 0xf000, (((ULONG)c->Valid * 2 > 32) << 15) | ((ULONG)pd << 12));
    return TRUE;
}

static void R5660Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    CUpd(c, R_DAC1_DIG_VOL, 0xfefe, (CVolSteps(left, 87) << 9) | (CVolSteps(right, 87) << 1));
    /* DAC1 MIXL/R "DAC1 Switch" serves as the mute; ADC loopback stays muted */
    CWr(c, R_AD_DA_MIXER, mute ? 0xc0c0 : 0x8080);
}

/* microphone supplies / boost of the selected input */
static ULONG MicBias(SST_CODEC *c)  { return c->Mic == 1 ? PWR_MB2 : PWR_MB1; }
static ULONG MicBst(SST_CODEC *c)   { return c->Mic == 1 ? PWR_BST2 : PWR_BST1; }

static void R5660Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN play = c->Priv[P_PLAY] != 0, cap = c->Priv[P_REC] != 0;
    BOOLEAN dmic = c->Mic == 2;

    if (on) {
        BOOLEAN first = !play && !cap;

        if (rec ? cap : play)
            return;
        if (first) {
            if (!c->Priv[P_VREF]) {
                /* set_bias_level STANDBY (from OFF) */
                CUpd(c, R_PWR_ANLG1, PWR_VREF1 | PWR_MB | PWR_BG | PWR_VREF2,
                     PWR_VREF1 | PWR_MB | PWR_BG | PWR_VREF2);
                CDelay(15);
                CUpd(c, R_PWR_ANLG1, PWR_FV1 | PWR_FV2, PWR_FV1 | PWR_FV2);
                c->Priv[P_VREF] = 1;
            }
            CUpd(c, R_GEN_CTRL1, GEN_DIG_GATE, GEN_DIG_GATE);  /* PREPARE */
        }
        /* supplies in DAPM order (register ascending) */
        if (rec && dmic) {
            /* rt5660_set_dmic_clk: sysclk / I2S pre divider (rl6231_get_pre_div) */
            static const UCHAR pdv[8] = { 1, 2, 3, 4, 6, 8, 12, 16 };
            LONG i = DmicClk(c->Priv[P_SYSCLK] / pdv[c->Priv[P_PD] & 7]);

            if (i >= 0)
                CUpd(c, R_DMIC_CTRL1, 0x00e0, (ULONG)i << 5);
            else
                LOGE("RT5660: no DMIC clock divider for %u Hz\n", c->Priv[P_SYSCLK]);
        }
        CUpd(c, R_PWR_DIG1, PWR_I2S1 | (rec && !dmic ? PWR_ADC_L | PWR_ADC_R : 0),
             PWR_I2S1 | (rec && !dmic ? PWR_ADC_L | PWR_ADC_R : 0));
        if (rec)
            CUpd(c, R_PWR_DIG2, PWR_ADC_S1F, PWR_ADC_S1F);
        else
            CUpd(c, R_PWR_DIG2, PWR_DAC_S1F, PWR_DAC_S1F);
        if (rec && !dmic) {
            CUpd(c, R_PWR_ANLG1, PWR_LDO2, PWR_LDO2);
            CUpd(c, R_PWR_ANLG2, MicBias(c), MicBias(c));
        }
        if (c->Priv[P_PLL])
            CUpd(c, R_PWR_ANLG2, PWR_PLL, PWR_PLL);
        if (rec && dmic)
            CUpd(c, R_DMIC_CTRL1, 0x8000, 0x8000);              /* "DMIC Power" */
        if (rec && !dmic)
            PrUpd(c, PR_CHOP_DAC_ADC, 0x1000, 0x1000);          /* "ADC clock" */
        if (!rec)
            CUpd(c, R_GEN_CTRL1, GEN_VREF_HP, GEN_VREF_HP);

        if (rec) {
            if (!dmic) {
                CUpd(c, R_PWR_MIXER, PWR_RM_L | PWR_RM_R, PWR_RM_L | PWR_RM_R);
                CUpd(c, R_PWR_ANLG2, MicBst(c), MicBst(c));
            }
            CUpd(c, R_STO1_ADC_DIG_VOL, 0x8080, 0x0000);        /* "Stereo1 ADC MIXL/R" */
            c->Priv[P_REC] = 1;
        } else {
            CUpd(c, R_PWR_DIG1, PWR_DAC_L1 | PWR_DAC_R1, PWR_DAC_L1 | PWR_DAC_R1);
            CUpd(c, R_PWR_MIXER, PWR_SM, PWR_SM);               /* "SPK MIX" */
            CUpd(c, R_PWR_VOL, PWR_SV, PWR_SV);                 /* "SPKVOL" */
            CUpd(c, R_PWR_DIG1, PWR_CLS_D, PWR_CLS_D);          /* "SPK amp" */
            CUpd(c, R_PWR_ANLG1, PWR_HA, PWR_HA);               /* "LOUT amp" */
            CUpd(c, R_LOUT_AMP_CTRL, LOUT_CO_EN | LOUT_CB_PU, LOUT_CO_EN | LOUT_CB_PU);  /* POST_PMU */
            /* output switches: SPO + SPKVOL unmuted at -6 dB, LOUT L/R unmuted (DAC direct path) */
            CWr(c, R_SPK_VOL, SPK_VOL_M6DB << 8);
            CUpd(c, R_LOUT_VOL, 0x8080, 0x0000);
            c->Priv[P_PLAY] = 1;
        }
        return;
    }

    if (!(rec ? cap : play))
        return;
    if (rec) {
        CUpd(c, R_STO1_ADC_DIG_VOL, 0x8080, 0x8080);
        if (!dmic) {
            CUpd(c, R_PWR_ANLG2, MicBst(c), 0);
            CUpd(c, R_PWR_MIXER, PWR_RM_L | PWR_RM_R, 0);
        }
        c->Priv[P_REC] = 0;
    } else {
        CUpd(c, R_LOUT_VOL, 0x8080, 0x8080);
        CWr(c, R_SPK_VOL, 0xc000 | (SPK_VOL_M6DB << 8));
        CUpd(c, R_PWR_DIG1, PWR_CLS_D, 0);
        CUpd(c, R_LOUT_AMP_CTRL, LOUT_CO_EN | LOUT_CB_PU, 0);  /* rt5660_lout_event PRE_PMD */
        CUpd(c, R_PWR_ANLG1, PWR_HA, 0);
        CUpd(c, R_PWR_VOL, PWR_SV, 0);
        CUpd(c, R_PWR_MIXER, PWR_SM, 0);
        CUpd(c, R_PWR_DIG1, PWR_DAC_L1 | PWR_DAC_R1, 0);
        c->Priv[P_PLAY] = 0;
    }
    play = c->Priv[P_PLAY] != 0;
    cap = c->Priv[P_REC] != 0;
    /* supplies */
    if (rec) {
        if (dmic)
            CUpd(c, R_DMIC_CTRL1, 0x8000, 0);
        else {
            CUpd(c, R_PWR_DIG1, PWR_ADC_L | PWR_ADC_R, 0);
            CUpd(c, R_PWR_ANLG2, MicBias(c), 0);
            CUpd(c, R_PWR_ANLG1, PWR_LDO2, 0);
            PrUpd(c, PR_CHOP_DAC_ADC, 0x1000, 0);
        }
        CUpd(c, R_PWR_DIG2, PWR_ADC_S1F, 0);
    } else {
        CUpd(c, R_PWR_DIG2, PWR_DAC_S1F, 0);
        CUpd(c, R_GEN_CTRL1, GEN_VREF_HP, 0);
    }
    if (!play && !cap) {
        CUpd(c, R_PWR_DIG1, PWR_I2S1, 0);
        CUpd(c, R_PWR_ANLG2, PWR_PLL, 0);
        CUpd(c, R_GEN_CTRL1, GEN_DIG_GATE, 0);              /* set_bias_level OFF (VREF stays, as Linux) */
    }
}

static BOOLEAN R5660Init(SST_CODEC *c)
{
    ULONG e0 = c->Errors, v = 0, rec;

    if (!CRd(c, R_VENDOR_ID2, &v) || v != RT5660_DEVICE_ID) {
        LOGE("RT5660: id %x\n", v);
        return FALSE;
    }
    c->Priv[P_PLAY] = c->Priv[P_REC] = c->Priv[P_VREF] = 0;

    /* rt5660_i2c_probe */
    if (!CWr(c, R_RESET, 0))
        return FALSE;
    CWr(c, R_ALC_PGA_CTRL2, 0x44c3);                        /* rt5660_patch */
    PrWr(c, PR_CHOP_DAC_ADC, 0x2600);
    CUpd(c, R_GEN_CTRL1, GEN_AUTO_DIS_AMP | GEN_MCLK_DET | GEN_POW_CLKDET,
         GEN_AUTO_DIS_AMP | GEN_MCLK_DET | GEN_POW_CLKDET);
    if (c->Mic == 2) {
        CUpd(c, R_GPIO_CTRL1, 0x3000, 0x1000);              /* GP1 = DMIC1_SCL */
        CUpd(c, R_DMIC_CTRL1, 0x0800, 0x0000);              /* data from GPIO2 */
        CUpd(c, R_GPIO_CTRL1, 0x4000, 0x4000);              /* GP2 = DMIC1_SDA */
    }

    /* clocks, DAI: slave, normal BCLK, I2S, word length */
    if (!R5660Clocks(c))
        return FALSE;
    if (c->Slots > 2)
        LOGE("RT5660: %u slot TDM not supported, using I2S slots 0/1\n", c->Slots);
    CUpd(c, R_I2S1_SDP, 0x8083, 0x8000);                    /* I2S_MS_S, BP_NOR, DF_I2S */
    CUpd(c, R_I2S1_SDP, 0x000c, c->Valid >= 24 ? 0x0008 : c->Valid >= 20 ? 0x0004 : 0x0000);

    /* playback routing: IF1 DAC L/R -> DAC1 MIX -> Stereo DAC MIX (L1->L, R1->R) -> DAC L1/R1 */
    CWr(c, R_DIG_INF1_DATA, 0x0000);
    CWr(c, R_STO_DAC_MIXER, 0x0202);
    R5660Volume(c, 65535, 65535, FALSE);
    /* DAC L + R -> SPK MIX -> SPKVOL (-6 dB) -> SPO MIX -> class D; DAC 1 -> LOUT MIX -> LOUT amp */
    CUpd(c, R_SPK_MIXER, 0x003e, 0x001a);
    CUpd(c, R_SPO_MIXER, 0x7800, 0x6800);
    CWr(c, R_SPK_VOL, 0xc000 | (SPK_VOL_M6DB << 8));
    CUpd(c, R_LOUT_MIXER, 0x6000, 0x2000);
    CWr(c, R_LOUT_VOL, 0xc8c8);

    /* capture routing: BSTx -> RECMIXL + RECMIXR -> ADC L/R -> Sto1 ADC MIX (ADC1), or DMIC (ADC2) */
    rec = c->Mic == 1 ? 0x0004 : 0x0002;                    /* BST2 / BST1 switch */
    CUpd(c, R_REC_L2_MIXER, 0x000f, 0x000f & ~rec);
    CUpd(c, R_REC_R2_MIXER, 0x000f, 0x000f & ~rec);
    CUpd(c, R_IN1_IN2, 0xffff, (BST_P24DB << 8) | BST_P24DB);   /* single ended, +24 dB */
    CWr(c, R_STO1_ADC_MIXER, c->Mic == 2 ? 0x4040 : 0x2020);
    CWr(c, R_STO1_ADC_DIG_VOL, 0xafaf);                     /* 0 dB, muted until capture */
    CWr(c, R_ADC_BST_VOL1, 0x0000);

    c->Caps = CC_PLAY | CC_REC;
    LOGI("RT5660: sysclk %u Hz (%s), pre div %u, I2S %u x %u bit, %u valid, mic %s\n", c->Priv[P_SYSCLK],
         c->Priv[P_PLL] ? "PLL1" : "MCLK", c->Priv[P_PD], c->Slots, c->Slot, c->Valid,
         c->Mic == 2 ? "DMIC1" : c->Mic == 1 ? "IN2P/MICBIAS2" : "IN1P/MICBIAS1");
    return c->Errors == e0;
}

static void R5660Shutdown(SST_CODEC *c)
{
    if (c->Priv[P_PLAY])
        R5660Stream(c, FALSE, FALSE);
    if (c->Priv[P_REC])
        R5660Stream(c, TRUE, FALSE);
    CWr(c, R_SPK_VOL, 0xc000 | (SPK_VOL_M6DB << 8));
    CUpd(c, R_LOUT_VOL, 0x8080, 0x8080);
    CWr(c, R_PWR_DIG1, 0);
    CWr(c, R_PWR_DIG2, 0);
    CWr(c, R_PWR_MIXER, 0);
    CWr(c, R_PWR_VOL, 0);
    CUpd(c, R_PWR_ANLG2, PWR_BST1 | PWR_BST2 | PWR_MB1 | PWR_MB2 | PWR_PLL, 0);
    CUpd(c, R_PWR_ANLG1, PWR_FV1 | PWR_FV2 | PWR_HA | PWR_LDO2, 0);
    CUpd(c, R_PWR_ANLG1, PWR_VREF1 | PWR_MB | PWR_BG | PWR_VREF2, 0);
    CUpd(c, R_GEN_CTRL1, GEN_DIG_GATE | GEN_VREF_HP, 0);
    c->Priv[P_VREF] = 0;
    CWr(c, R_RESET, 0);                                     /* rt5660_remove */
}

const CODEC_DRV g_DrvRt5660 = {
    "RT5660", "10EC5660\0" "10EC3277\0", CODEC_HEADSET, 1, 2,
    R5660Probe, R5660Init, R5660Stream, R5660Volume, R5660Shutdown
};
