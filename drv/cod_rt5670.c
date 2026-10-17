#include "codec.h"

/*
 * Realtek RT5670 / RT5671 / RT5672 I2S audio codec (Cherry Trail / Braswell, cht_bsw_rt5672).
 * Reference: Linux sound/soc/codecs/rt5670.c/.h, sound/soc/codecs/rl6231.c,
 * sound/soc/intel/boards/cht_bsw_rt5672.c.
 *
 * I2C: 8-bit register address, 16-bit big-endian value; private registers PR_xx through
 * RT5670_PRIV_INDEX (0x6a) / RT5670_PRIV_DATA (0x6c).  VENDOR_ID2 (0xff) = 0x6271 for all
 * variants, RESET[2:1] = 0 RT5670, 1 RT5672, 2 RT5671.
 *
 * Clocking (slave): PLL1 from BCLK1 -> sysclk = 512 * fs (the machine driver uses MCLK 19.2 MHz
 * -> 48000 * 512).  As in cht_codec_init, the stereo/mono DA and AD filters run from the
 * I2S1 ASRC (rt5670_sel_asrc_clk_src(..., RT5670_CLK_SEL_I2S1_ASRC)): the codec only takes
 * 32/64 fs I2S natively; a BCLK of 50 fs additionally selects RT5670_TDM_DATA_MODE_50FS
 * (rt5670_set_bclk_ratio).  AIF1 only (cht_bsw_rt5672 uses AIF1 on SSP2 and SSP0).
 *
 * Outputs (no jack detection, all on during playback):
 *   HPOL/HPOR:   DAC1 -> HPOVOL MIXL/R -> HPO MIX (HPVOL) -> HP Amp.  The Linux "HPO MIX DAC1
 *                Switch" control points at bit 0 (HPOVOL MIXL DAC1), so HPVOL is the path
 *                that works under Linux too.
 *   LOUTL/LOUTR: DAC1 -> LOUT MIX -> LOUT Amp
 *   PDM1 L/R:    stereo DAC mixer -> PDM1; on the RT5672 this drives the class-D speaker
 *                amplifier ("SPO Amp", SPOLP/N, SPORP/N), on RT5670/71 it (and PDM2) is the
 *                digital output to an external PDM amplifier.
 * Some boards (Lenovo Miix 2 10, RT5670_GPIO1_IS_EXT_SPK_EN) enable an external speaker
 * amplifier with the codec's own GPIO1; that DMI quirk is not reproduced here.
 *
 * Capture, c->Mic:
 *   0  headset mic IN1P (combo jack, CBJ boost), MICBIAS1 - cht_bsw_rt5672 "Headset Mic"
 *   1  DMIC1, data on IN2P, clock on GPIO2 (cht "Int Mic" DMIC L1/R1; RT5670_DMIC1_IN2P boards:
 *      Braswell reference, Dell Wyse 3040, ThinkPad Tablet 10, Miix 2 10)
 *   2  DMIC2, data on IN3N (RT5670_DMIC2_INR: ThinkPad Tablet 8, Dell Venue 8/10 Pro, Aegex 10)
 *   3  internal analog mic IN2P single-ended, MICBIAS1
 *   4  internal analog mic IN2P/IN2N differential (RT5670_IN2_DIFF)
 * Analog mics: boost +24 dB, RECMIXL and RECMIXR both take the boost stage (mono mic on both
 * ADC channels).  DMIC pairs are captured as stereo L/R.
 *
 * GPIOs: the machine driver maps GpioIo 0 as "headset-gpios" (jack detect input); nothing is
 * driven here.
 */

/* ---- registers (rt5670.h) ---- */
#define RT5670_RESET            0x00
#define RT5670_HP_VOL           0x02
#define RT5670_LOUT1            0x03
#define RT5670_CJ_CTRL1         0x0a
#define RT5670_CJ_CTRL2         0x0b
#define RT5670_IN2              0x0e
#define RT5670_DAC1_DIG_VOL     0x19
#define RT5670_STO1_ADC_DIG_VOL 0x1c
#define RT5670_STO1_ADC_MIXER   0x27
#define RT5670_AD_DA_MIXER      0x29
#define RT5670_STO_DAC_MIXER    0x2a
#define RT5670_PDM_OUT_CTRL     0x31
#define RT5670_REC_L2_MIXER     0x3c
#define RT5670_REC_R2_MIXER     0x3e
#define RT5670_HPO_MIXER        0x45
#define RT5670_LOUT_MIXER       0x53
#define RT5670_PWR_DIG1         0x61
#define RT5670_PWR_DIG2         0x62
#define RT5670_PWR_ANLG1        0x63
#define RT5670_PWR_ANLG2        0x64
#define RT5670_PWR_MIXER        0x65
#define RT5670_PWR_VOL          0x66
#define RT5670_PRIV_INDEX       0x6a
#define RT5670_PRIV_DATA        0x6c
#define RT5670_I2S1_SDP         0x70
#define RT5670_ADDA_CLK1        0x73
#define RT5670_DMIC_CTRL1       0x75
#define RT5670_TDM_CTRL_1       0x77
#define RT5670_GLB_CLK          0x80
#define RT5670_PLL_CTRL1        0x81
#define RT5670_PLL_CTRL2        0x82
#define RT5670_ASRC_1           0x83
#define RT5670_ASRC_2           0x84
#define RT5670_ASRC_3           0x85
#define RT5670_ASRC_8           0x8a
#define RT5670_DEPOP_M1         0x8e
#define RT5670_DEPOP_M2         0x8f
#define RT5670_DEPOP_M3         0x90
#define RT5670_CHARGE_PUMP      0x91
#define RT5670_GPIO_CTRL1       0xc0
#define RT5670_GPIO_CTRL3       0xc2
#define RT5670_JD_CTRL3         0xf8
#define RT5670_DIG_MISC         0xfa
#define RT5670_GEN_CTRL2        0xfb
#define RT5670_GEN_CTRL3        0xfc
#define RT5670_VENDOR_ID        0xfd
#define RT5670_VENDOR_ID2       0xff
/* private registers */
#define RT5670_PR_CHOP_DAC_ADC  0x3d
#define RT5670_PR_MAMP_INT_REG2 0x37
#define RT5670_PR_HP_DCC_INT1   0x77

#define RT5670_DEVICE_ID        0x6271
#define RT5670_ID_MASK          (3 << 1)
#define RT5670_ID_5670          (0 << 1)
#define RT5670_ID_5672          (1 << 1)
#define RT5670_ID_5671          (2 << 1)

#define RT5670_L_MUTE           (1 << 15)
#define RT5670_R_MUTE           (1 << 7)
#define RT5670_LR_MUTE          (RT5670_L_MUTE | RT5670_R_MUTE)
#define RT5670_OUT_0DB          0x0808      /* out_vol_tlv field 8 = 0 dB */
/* CJ_CTRL1 / CJ_CTRL2 (combo jack), IN2 */
#define RT5670_BST_SFT1         12
#define RT5670_BST_24DB         2
#define RT5670_CBJ_BST1_EN      (1 << 2)
#define RT5670_CBJ_MN_JD        (1 << 12)
#define RT5670_CBJ_DET_MODE     (1 << 7)
#define RT5670_IN_DF2           (1 << 6)
/* PWR_DIG1 */
#define RT5670_PWR_I2S1         (1 << 15)
#define RT5670_PWR_DAC_L1       (1 << 12)
#define RT5670_PWR_DAC_R1       (1 << 11)
#define RT5670_PWR_ADC_L        (1 << 2)
#define RT5670_PWR_ADC_R        (1 << 1)
/* PWR_DIG2 */
#define RT5670_PWR_ADC_S1F      (1 << 15)
#define RT5670_PWR_DAC_S1F      (1 << 11)
#define RT5670_PWR_PDM1         (1 << 7)
#define RT5670_PWR_PDM2         (1 << 6)
/* PWR_ANLG1 */
#define RT5670_PWR_VREF1        (1 << 15)
#define RT5670_PWR_FV1          (1 << 14)
#define RT5670_PWR_MB           (1 << 13)
#define RT5670_PWR_LM           (1 << 12)
#define RT5670_PWR_BG           (1 << 11)
#define RT5670_PWR_HP_L         (1 << 7)
#define RT5670_PWR_HP_R         (1 << 6)
#define RT5670_PWR_HA           (1 << 5)
#define RT5670_PWR_VREF2        (1 << 4)
#define RT5670_PWR_FV2          (1 << 3)
#define RT5670_LDO_SEL_MASK     (7 << 0)
/* PWR_ANLG2 */
#define RT5670_PWR_BST1         (1 << 15)
#define RT5670_PWR_BST2         (1 << 13)
#define RT5670_PWR_MB1          (1 << 11)
#define RT5670_PWR_PLL          (1 << 9)
#define RT5670_PWR_BST1_P       (1 << 6)
#define RT5670_PWR_BST2_P       (1 << 4)
/* PWR_MIXER */
#define RT5670_PWR_RM_L         (1 << 11)
#define RT5670_PWR_RM_R         (1 << 10)
/* PWR_VOL */
#define RT5670_PWR_HV_L         (1 << 11)
#define RT5670_PWR_HV_R         (1 << 10)
#define RT5670_PWR_MIC_DET      (1 << 5)
/* STO1_ADC_MIXER */
#define RT5670_M_ADC_L1         (1 << 14)
#define RT5670_M_ADC_L2         (1 << 13)
#define RT5670_ADC_1_SRC_ADC    (1 << 12)
#define RT5670_ADC_2_SRC_DMIC   (1 << 11)
#define RT5670_DMIC_SRC_SFT     8           /* 0 DMIC1, 1 DMIC2, 2 DMIC3 */
#define RT5670_M_ADC_R1         (1 << 6)
#define RT5670_M_ADC_R2         (1 << 5)
/* AD_DA_MIXER: stereo ADC mix 15/7, DAC1 14/6 ("DAC1 Playback Switch"), DAC1 source IF1 = 0 */
#define RT5670_M_ADCMIX_L       (1 << 15)
#define RT5670_M_DAC1_L         (1 << 14)
#define RT5670_M_ADCMIX_R       (1 << 7)
#define RT5670_M_DAC1_R         (1 << 6)
/* STO_DAC_MIXER (mutes): DAC L1 14, DAC L2 12, DAC R1->L 9, DAC R1 6, DAC R2 4, DAC L1->R 1 */
#define RT5670_M_DAC_L1         (1 << 14)
#define RT5670_M_DAC_R1         (1 << 6)
/* PDM_OUT_CTRL: PDM1 L src 15 (1 = stereo DAC), mute 14, R src 13, mute 12; PDM2 11/10/9/8 */
#define RT5670_PDM1_SRC_STO     ((1 << 15) | (1 << 13))
#define RT5670_M_PDM1           ((1 << 14) | (1 << 12))
#define RT5670_PDM2_SRC_STO     ((1 << 11) | (1 << 9))
#define RT5670_M_PDM2           ((1 << 10) | (1 << 8))
/* REC_x2_MIXER mutes */
#define RT5670_M_BST2_RM        (1 << 3)
#define RT5670_M_BST1_RM        (1 << 1)
/* HPO_MIXER: HPVOL 14, DAC1 13, HPOVOL MIXR: INR1 3 / DACR1 2, HPOVOL MIXL: INL1 1 / DACL1 0 */
#define RT5670_M_HPVOL_HM       (1 << 14)
#define RT5670_M_DACR1_HMR      (1 << 2)
#define RT5670_M_DACL1_HML      (1 << 0)
/* LOUT_MIXER */
#define RT5670_M_DAC_L1_LM      (1 << 15)
#define RT5670_M_DAC_R1_LM      (1 << 14)
/* I2S1_SDP / ADDA_CLK1 */
#define RT5670_I2S_MS_S         (1 << 15)
#define RT5670_I2S_DL_16        (0 << 2)
#define RT5670_I2S_DL_24        (2 << 2)
#define RT5670_I2S_BCLK_MS1     (1 << 15)
#define RT5670_I2S_PD1_SFT      12
#define RT5670_I2S_PD1_MASK     (7 << 12)
/* DMIC_CTRL1 */
#define RT5670_DMIC_1_EN        (1 << 15)
#define RT5670_DMIC_2_EN        (1 << 14)
#define RT5670_DMIC_2_DP_MASK   (1 << 10)
#define RT5670_DMIC_2_DP_IN3N   (1 << 10)
#define RT5670_DMIC_CLK_MASK    (7 << 5)
#define RT5670_DMIC_CLK_SFT     5
#define RT5670_DMIC_1_DP_MASK   (3 << 0)
#define RT5670_DMIC_1_DP_IN2P   (1 << 0)
/* GLB_CLK */
#define RT5670_SCLK_SRC_MASK    (3 << 14)
#define RT5670_SCLK_SRC_PLL1    (1 << 14)
#define RT5670_SCLK_SRC_RCCLK   (2 << 14)
#define RT5670_PLL1_SRC_MASK    (7 << 11)
#define RT5670_PLL1_SRC_MCLK    (0 << 11)
#define RT5670_PLL1_SRC_BCLK1   (1 << 11)
#define RT5670_PLL_N_SFT        7
#define RT5670_PLL_M_SFT        12
#define RT5670_PLL_M_BP         (1 << 11)
/* ASRC_1 supplies: I2S1 ASRC 11, DAC STO ASRC 10, DMIC STO1 ASRC 7, ADC STO1 ASRC 3 */
#define RT5670_I2S1_ASRC        (1 << 11)
#define RT5670_DAC_STO_ASRC     (1 << 10)
#define RT5670_DMIC_STO1_ASRC   (1 << 7)
#define RT5670_ADC_STO1_ASRC    (1 << 3)
/* ASRC_2: DA stereo [15:12], DA mono L [11:8], DA mono R [7:4], AD stereo1 [3:0];
 * ASRC_3: AD mono L [7:4], AD mono R [3:0]; 1 = RT5670_CLK_SEL_I2S1_ASRC */
#define RT5670_ASRC2_I2S1       0x1111
#define RT5670_ASRC3_I2S1       0x0011
/* CHARGE_PUMP */
#define RT5670_OSW_LR_MASK      ((1 << 11) | (1 << 10))
#define RT5670_PM_HP_MASK       (3 << 8)
#define RT5670_PM_HP_HV         (2 << 8)
/* GPIO_CTRL1, DIG_MISC, GEN_CTRL3 */
#define RT5670_GP2_PIN_DMIC1_SCL (1 << 14)
#define RT5670_MCLK_DET         (1 << 3)
#define RT5670_TDM_DATA_MODE_50FS (1 << 11)

enum { MIC_HS_IN1, MIC_DMIC1, MIC_DMIC2, MIC_IN2, MIC_IN2_DIFF, MIC_MAX };

typedef struct {
    ULONG   Sysclk;
    USHORT  PllSrc;
    UCHAR   Id;             /* RESET[2:1] */
    UCHAR   Mic;
    UCHAR   VId;
    UCHAR   PdIdx;
    UCHAR   DmicClk;
    UCHAR   Muted;
} RT5670_PRIV;

typedef char RT5670_PRIV_FITS[(sizeof(RT5670_PRIV) <= sizeof(((SST_CODEC *)0)->Priv)) ? 1 : -1];

#define PRIV(c) ((RT5670_PRIV *)(c)->Priv)

/* ------------------------------------------------------------------ */
/* rl6231 helpers (sound/soc/codecs/rl6231.c), integer only            */
/* ------------------------------------------------------------------ */
#define RL_PLL_INP_MAX 50000000
#define RL_PLL_INP_MIN 256000
#define RL_PLL_N_MAX   0x1ff
#define RL_PLL_K_MAX   0x1f
#define RL_PLL_M_MAX   0xf

typedef struct { BOOLEAN MBp, KBp; LONG M, N, K; } RL_PLL;

static const struct { ULONG In, Out; LONG K, N, M; BOOLEAN MBp, KBp; } s_RlPreset[] = {
    { 19200000,  4096000, 23, 14, 1, FALSE, FALSE },
    { 19200000, 24576000,  3, 30, 3, FALSE, FALSE },
    { 48000000,  3840000, 23,  2, 0, FALSE, FALSE },
    {  3840000, 24576000,  3, 30, 0, TRUE,  FALSE },
    {  3840000, 22579200,  3,  5, 0, TRUE,  FALSE },
};

static ULONG RlDiff(ULONG a, ULONG b) { return a > b ? a - b : b - a; }

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

/* rl6231_pll_calc: Fout = Fin * (N + 2) / ((M + 2) * (K + 2)), M/K optionally bypassed */
static BOOLEAN RlPllCalc(ULONG fin, ULONG fout, RL_PLL *p)
{
    LONG k, n = 0, m = 0, kt, nt, mt, mink, maxk;
    BOOLEAN mbp = FALSE, kbp = FALSE;
    ULONG red, redt, pllout, in_t, out_t, div, fi, fo, i;

    if (fin > RL_PLL_INP_MAX || fin < RL_PLL_INP_MIN || !fout)
        return FALSE;

    for (i = 0; i < sizeof(s_RlPreset) / sizeof(s_RlPreset[0]); i++) {
        if (fin == s_RlPreset[i].In && fout == s_RlPreset[i].Out) {
            k = s_RlPreset[i].K;
            m = s_RlPreset[i].M;
            n = s_RlPreset[i].N;
            mbp = s_RlPreset[i].MBp;
            kbp = s_RlPreset[i].KBp;
            goto found;
        }
    }

    redt = RlDiff(fout, fin);
    mink = (LONG)(80000000 / fout) - 2;
    maxk = (LONG)(150000000 / fout) - 2;
    if (maxk > RL_PLL_K_MAX)
        maxk = RL_PLL_K_MAX;
    if (mink > RL_PLL_K_MAX)
        mink = maxk = RL_PLL_K_MAX;
    div = RlBestDiv(fin, 0xffffffffUL / RL_PLL_N_MAX, CGcd(fin, fout));
    fi = fin / div;
    fo = fout / div;
    k = mink;
    if (mink < -1)
        mink = -1;
    for (kt = mink; kt <= maxk; kt++) {
        for (nt = 0; nt <= RL_PLL_N_MAX; nt++) {
            in_t = fi * (ULONG)(nt + 2);
            pllout = fo * (ULONG)(kt + 2);
            if (in_t == pllout) {
                mbp = TRUE;
                n = nt;
                k = kt;
                goto found;
            }
            out_t = in_t / (ULONG)(kt + 2);
            red = RlDiff(fo, out_t);
            if (red < redt) {
                mbp = TRUE;
                n = nt;
                m = 0;
                k = kt;
                if (red == 0)
                    goto found;
                redt = red;
            }
            for (mt = 0; mt <= RL_PLL_M_MAX; mt++) {
                out_t = in_t / ((ULONG)(mt + 2) * (ULONG)(kt + 2));
                red = RlDiff(fo, out_t);
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
    /* only an approximation */

found:
    if (k == -1) {
        kbp = TRUE;
        k = 0;
    }
    p->MBp = mbp;
    p->KBp = kbp;
    p->M = m;
    p->N = n;
    p->K = k;
    return TRUE;
}

static const ULONG s_RlPd[] = { 1, 2, 3, 4, 6, 8, 12, 16 };

/* rl6231_get_clk_info: index of the pre-divider giving 256 * fs from sysclk */
static LONG RlClkInfo(ULONG sclk, ULONG rate)
{
    ULONG i;
    for (i = 0; i < 8; i++)
        if (sclk == (rate << 8) * s_RlPd[i])
            return (LONG)i;
    return -1;
}

/* rl6231_calc_dmic_clk: highest DMIC clock <= 1.536 MHz */
static LONG RlDmicClk(ULONG rate)
{
    static const ULONG div[] = { 2, 3, 4, 6, 8, 12 };
    ULONG i;
    if (rate < 1000000 * div[0])
        return -1;
    for (i = 0; i < 6; i++) {
        if ((div[i] % 3) == 0)
            continue;
        if (1536000 * div[i] >= rate)
            return (LONG)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */

static BOOLEAN PrWr(SST_CODEC *c, ULONG idx, ULONG val)
{
    BOOLEAN ok = CWr(c, RT5670_PRIV_INDEX, idx);
    ok &= CWr(c, RT5670_PRIV_DATA, val);
    return ok;
}

static BOOLEAN PrUpd(SST_CODEC *c, ULONG idx, ULONG mask, ULONG val)
{
    ULONG v;
    if (!CWr(c, RT5670_PRIV_INDEX, idx) || !CRd(c, RT5670_PRIV_DATA, &v))
        return FALSE;
    return PrWr(c, idx, (v & ~mask) | (val & mask));
}

static BOOLEAN Rt5670Probe(SST_CODEC *c)
{
    ULONG v;
    return CRd(c, RT5670_VENDOR_ID2, &v) && v == RT5670_DEVICE_ID;
}

static BOOLEAN UsesDmic(RT5670_PRIV *p) { return p->Mic == MIC_DMIC1 || p->Mic == MIC_DMIC2; }

/* rt5670_set_bias_level: OFF -> STANDBY -> PREPARE */
static void BiasOn(SST_CODEC *c)
{
    ULONG m = RT5670_PWR_VREF1 | RT5670_PWR_MB | RT5670_PWR_BG | RT5670_PWR_VREF2;
    ULONG fv = RT5670_PWR_FV1 | RT5670_PWR_FV2;

    /* STANDBY */
    CUpd(c, RT5670_PWR_ANLG1, RT5670_PWR_VREF1 | RT5670_PWR_VREF2 | fv, 0);
    CUpd(c, RT5670_PWR_ANLG1, RT5670_LDO_SEL_MASK, 0x3);
    /* PREPARE */
    CUpd(c, RT5670_PWR_ANLG1, m, m);
    CDelay(10);
    CUpd(c, RT5670_PWR_ANLG1, fv, fv);
    CUpd(c, RT5670_CHARGE_PUMP, RT5670_OSW_LR_MASK, 0);
    CUpd(c, RT5670_DIG_MISC, 0x1, 0x1);
    CUpd(c, RT5670_PWR_ANLG1, RT5670_LDO_SEL_MASK, 0x5);
}

/* rt5670_set_bias_level: PREPARE -> STANDBY -> OFF (no jack-detect mode) */
static void BiasOff(SST_CODEC *c)
{
    ULONG all = RT5670_PWR_VREF1 | RT5670_PWR_MB | RT5670_PWR_BG | RT5670_PWR_VREF2 |
                RT5670_PWR_FV1 | RT5670_PWR_FV2;

    CUpd(c, RT5670_PWR_ANLG1, RT5670_PWR_VREF1 | RT5670_PWR_VREF2 | RT5670_PWR_FV1 | RT5670_PWR_FV2, 0);
    CUpd(c, RT5670_PWR_ANLG1, RT5670_LDO_SEL_MASK, 0x3);
    CUpd(c, RT5670_PWR_ANLG1, all, 0);
    CUpd(c, RT5670_DIG_MISC, 0x1, 0x0);
}

/* "Platform Clock" + "PLL1" supply: rt5670_set_codec_sysclk(PLL1) */
static void ClockOn(SST_CODEC *c)
{
    CUpd(c, RT5670_GLB_CLK, RT5670_SCLK_SRC_MASK, RT5670_SCLK_SRC_PLL1);
    CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_PLL, RT5670_PWR_PLL);
}

static void ClockOff(SST_CODEC *c)
{
    CUpd(c, RT5670_GLB_CLK, RT5670_SCLK_SRC_MASK, RT5670_SCLK_SRC_RCCLK);
    CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_PLL, 0);
}

static ULONG PdmMask(SST_CODEC *c)
{
    return RT5670_M_PDM1 | (PRIV(c)->Id == RT5670_ID_5672 ? 0 : RT5670_M_PDM2);
}

static ULONG PdmPower(SST_CODEC *c)
{
    return RT5670_PWR_PDM1 | (PRIV(c)->Id == RT5670_ID_5672 ? 0 : RT5670_PWR_PDM2);
}

static void PlayUp(SST_CODEC *c)
{
    ULONG v;

    /* supplies: I2S1, DAC stereo1 filter, DAC L1/R1 power, HP L/R amp, Mic Det Power, PDM power;
     * subseq 1: I2S1 ASRC, DAC STO ASRC, "Improve HP Amp Drv" (rt5670_hp_power_event) */
    CUpd(c, RT5670_PWR_DIG1, RT5670_PWR_I2S1, RT5670_PWR_I2S1);
    CUpd(c, RT5670_PWR_DIG2, RT5670_PWR_DAC_S1F | PdmPower(c), RT5670_PWR_DAC_S1F | PdmPower(c));
    v = RT5670_PWR_DAC_L1 | RT5670_PWR_DAC_R1;
    CUpd(c, RT5670_PWR_DIG1, v, v);
    v = RT5670_PWR_HP_L | RT5670_PWR_HP_R;
    CUpd(c, RT5670_PWR_ANLG1, v, v);
    CUpd(c, RT5670_PWR_VOL, RT5670_PWR_MIC_DET, RT5670_PWR_MIC_DET);
    v = RT5670_I2S1_ASRC | RT5670_DAC_STO_ASRC;
    CUpd(c, RT5670_ASRC_1, v, v);
    CUpd(c, RT5670_CHARGE_PUMP, RT5670_PM_HP_MASK, RT5670_PM_HP_HV);
    CUpd(c, RT5670_GEN_CTRL2, 0x0400, 0x0400);
    v = RT5670_PWR_HA | RT5670_PWR_FV1 | RT5670_PWR_FV2;
    CUpd(c, RT5670_PWR_ANLG1, v, v);
    CWr(c, RT5670_DEPOP_M2, 0x3100);
    CWr(c, RT5670_DEPOP_M1, 0x8009);
    PrWr(c, RT5670_PR_HP_DCC_INT1, 0x9f00);
    CDelay(20);
    CWr(c, RT5670_DEPOP_M1, 0x8019);
    /* muxes: PDM1 (and PDM2) L/R unmute */
    CUpd(c, RT5670_PDM_OUT_CTRL, PdmMask(c), 0);
    /* mixers: HPOVOL MIXL/R, LOUT MIX */
    v = RT5670_PWR_HV_L | RT5670_PWR_HV_R;
    CUpd(c, RT5670_PWR_VOL, v, v);
    CUpd(c, RT5670_PWR_ANLG1, RT5670_PWR_LM, RT5670_PWR_LM);
    /* PGA "HP Amp" POST_PMU: headphone unmute sequence (rt5670_hp_event) */
    PrWr(c, RT5670_PR_MAMP_INT_REG2, 0xb400);
    CWr(c, RT5670_DEPOP_M3, 0x0772);
    CWr(c, RT5670_DEPOP_M1, 0x805d);
    CWr(c, RT5670_DEPOP_M1, 0x831d);
    CUpd(c, RT5670_GEN_CTRL2, 0x0300, 0x0300);
    CUpd(c, RT5670_HP_VOL, RT5670_LR_MUTE, 0);
    CDelay(80);
    CWr(c, RT5670_DEPOP_M1, 0x8019);
    /* autodisable kcontrols: "LOUT L/R Playback" */
    CUpd(c, RT5670_LOUT1, RT5670_LR_MUTE, 0);
}

static void PlayDown(SST_CODEC *c)
{
    ULONG v;

    CUpd(c, RT5670_LOUT1, RT5670_LR_MUTE, RT5670_LR_MUTE);
    /* PGA "HP Amp" PRE_PMD: headphone mute sequence */
    PrWr(c, RT5670_PR_MAMP_INT_REG2, 0xb400);
    CWr(c, RT5670_DEPOP_M3, 0x0772);
    CWr(c, RT5670_DEPOP_M1, 0x803d);
    CDelay(10);
    CWr(c, RT5670_DEPOP_M1, 0x831d);
    CDelay(10);
    CUpd(c, RT5670_HP_VOL, RT5670_LR_MUTE, RT5670_LR_MUTE);
    CDelay(20);
    CUpd(c, RT5670_GEN_CTRL2, 0x0300, 0x0);
    CWr(c, RT5670_DEPOP_M1, 0x8019);
    CWr(c, RT5670_DEPOP_M3, 0x0707);
    PrWr(c, RT5670_PR_MAMP_INT_REG2, 0xfc00);
    /* mixers, muxes */
    CUpd(c, RT5670_PWR_ANLG1, RT5670_PWR_LM, 0);
    CUpd(c, RT5670_PWR_VOL, RT5670_PWR_HV_L | RT5670_PWR_HV_R, 0);
    CUpd(c, RT5670_PDM_OUT_CTRL, PdmMask(c), PdmMask(c));
    /* supplies, subseq 1 first: "Improve HP Amp Drv" PRE_PMD, ASRC */
    CWr(c, RT5670_DEPOP_M1, 0x0004);
    CDelay(30);
    CUpd(c, RT5670_ASRC_1, RT5670_DAC_STO_ASRC | (c->RecOn ? 0 : RT5670_I2S1_ASRC), 0);
    v = RT5670_PWR_HP_L | RT5670_PWR_HP_R;
    CUpd(c, RT5670_PWR_ANLG1, v, 0);
    CUpd(c, RT5670_PWR_DIG1, RT5670_PWR_DAC_L1 | RT5670_PWR_DAC_R1, 0);
    CUpd(c, RT5670_PWR_DIG2, RT5670_PWR_DAC_S1F | PdmPower(c), 0);
    if (!c->RecOn || UsesDmic(PRIV(c)))
        CUpd(c, RT5670_PWR_VOL, RT5670_PWR_MIC_DET, 0);
    if (!c->RecOn)
        CUpd(c, RT5670_PWR_DIG1, RT5670_PWR_I2S1, 0);
}

static void RecUp(SST_CODEC *c)
{
    RT5670_PRIV *p = PRIV(c);
    ULONG v;

    /* supplies: I2S1, ADC stereo1 filter; DMIC CLK + DMICx power, or MICBIAS1, Mic Det Power,
     * ADC power, ADC clock; subseq 1: I2S1 ASRC, ADC STO1 ASRC, DMIC STO1 ASRC */
    CUpd(c, RT5670_PWR_DIG1, RT5670_PWR_I2S1, RT5670_PWR_I2S1);
    CUpd(c, RT5670_PWR_DIG2, RT5670_PWR_ADC_S1F, RT5670_PWR_ADC_S1F);
    if (UsesDmic(p)) {
        v = p->Mic == MIC_DMIC1 ? RT5670_DMIC_1_EN : RT5670_DMIC_2_EN;
        CUpd(c, RT5670_DMIC_CTRL1, RT5670_DMIC_CLK_MASK, (ULONG)p->DmicClk << RT5670_DMIC_CLK_SFT);
        CUpd(c, RT5670_DMIC_CTRL1, v, v);
        v = RT5670_I2S1_ASRC | RT5670_ADC_STO1_ASRC | RT5670_DMIC_STO1_ASRC;
        CUpd(c, RT5670_ASRC_1, v, v);
        return;
    }
    CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_MB1, RT5670_PWR_MB1);
    CUpd(c, RT5670_PWR_VOL, RT5670_PWR_MIC_DET, RT5670_PWR_MIC_DET);
    v = RT5670_PWR_ADC_L | RT5670_PWR_ADC_R;
    CUpd(c, RT5670_PWR_DIG1, v, v);
    PrUpd(c, RT5670_PR_CHOP_DAC_ADC, 1 << 12, 1 << 12);       /* "ADC clock" */
    v = RT5670_I2S1_ASRC | RT5670_ADC_STO1_ASRC;
    CUpd(c, RT5670_ASRC_1, v, v);
    /* mixer: RECMIX; PGA: BST1 / BST2, then BSTx_P (POST_PMU) */
    v = RT5670_PWR_RM_L | RT5670_PWR_RM_R;
    CUpd(c, RT5670_PWR_MIXER, v, v);
    if (p->Mic == MIC_HS_IN1) {
        CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_BST1, RT5670_PWR_BST1);
        CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_BST1_P, RT5670_PWR_BST1_P);
    } else {
        CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_BST2, RT5670_PWR_BST2);
        CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_BST2_P, RT5670_PWR_BST2_P);
    }
}

static void RecDown(SST_CODEC *c)
{
    CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_BST1_P | RT5670_PWR_BST2_P, 0);
    CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_BST1 | RT5670_PWR_BST2, 0);
    CUpd(c, RT5670_PWR_MIXER, RT5670_PWR_RM_L | RT5670_PWR_RM_R, 0);
    CUpd(c, RT5670_ASRC_1, RT5670_ADC_STO1_ASRC | RT5670_DMIC_STO1_ASRC | (c->PlayOn ? 0 : RT5670_I2S1_ASRC), 0);
    CUpd(c, RT5670_DMIC_CTRL1, RT5670_DMIC_1_EN | RT5670_DMIC_2_EN, 0);
    PrUpd(c, RT5670_PR_CHOP_DAC_ADC, 1 << 12, 0);
    CUpd(c, RT5670_PWR_DIG1, RT5670_PWR_ADC_L | RT5670_PWR_ADC_R, 0);
    CUpd(c, RT5670_PWR_ANLG2, RT5670_PWR_MB1, 0);
    CUpd(c, RT5670_PWR_DIG2, RT5670_PWR_ADC_S1F, 0);
    if (!c->PlayOn) {
        CUpd(c, RT5670_PWR_VOL, RT5670_PWR_MIC_DET, 0);
        CUpd(c, RT5670_PWR_DIG1, RT5670_PWR_I2S1, 0);
    }
}

static BOOLEAN Rt5670Init(SST_CODEC *c)
{
    RT5670_PRIV *p = PRIV(c);
    BOOLEAN ok = TRUE;
    ULONG v, fin, recm, adcm, frame;
    LONG pd, dm;
    RL_PLL pll;

    memset(p, 0, sizeof(*p));
    p->Mic = (UCHAR)(c->Mic < MIC_MAX ? c->Mic : MIC_HS_IN1);
    p->Sysclk = c->Rate * 512;
    if (c->Bclk >= RL_PLL_INP_MIN) {
        fin = c->Bclk;
        p->PllSrc = RT5670_PLL1_SRC_BCLK1;
    } else {
        fin = c->Mclk;
        p->PllSrc = RT5670_PLL1_SRC_MCLK;
    }
    if (!RlPllCalc(fin, p->Sysclk, &pll)) {
        LOGE("RT5670: no usable PLL reference (BCLK %u MCLK %u)\n", c->Bclk, c->Mclk);
        return FALSE;
    }
    pd = RlClkInfo(p->Sysclk, c->Rate);
    p->PdIdx = (UCHAR)(pd < 0 ? 1 : pd);
    dm = RlDmicClk(p->Sysclk / s_RlPd[p->PdIdx]);
    p->DmicClk = (UCHAR)(dm < 0 ? 4 : dm);

    /* rt5670_i2c_probe: reset, HP amp off / VREF2 on, 100 ms, reset again */
    if (!CWr(c, RT5670_RESET, 0))
        return FALSE;
    ok &= CUpd(c, RT5670_PWR_ANLG1, RT5670_PWR_HP_L | RT5670_PWR_HP_R | RT5670_PWR_VREF2, RT5670_PWR_VREF2);
    CDelay(100);
    ok &= CWr(c, RT5670_RESET, 0);
    ok &= CRd(c, RT5670_VENDOR_ID, &v);
    p->VId = (UCHAR)v;
    ok &= CWr(c, RT5670_GPIO_CTRL3, p->VId >= 4 ? 0x0980 : 0x0d00);
    ok &= PrWr(c, 0x14, 0x9a8a);                /* init_list */
    ok &= PrWr(c, 0x38, 0x1fe1);
    ok &= PrWr(c, 0x3d, 0x3640);
    ok &= CWr(c, RT5670_ASRC_8, 0x0123);
    ok &= CUpd(c, RT5670_DIG_MISC, RT5670_MCLK_DET, RT5670_MCLK_DET);
    if (p->Mic == MIC_IN2_DIFF)
        ok &= CUpd(c, RT5670_IN2, RT5670_IN_DF2, RT5670_IN_DF2);
    if (UsesDmic(p)) {
        ok &= CUpd(c, RT5670_GPIO_CTRL1, RT5670_GP2_PIN_DMIC1_SCL, RT5670_GP2_PIN_DMIC1_SCL);
        if (p->Mic == MIC_DMIC1)
            ok &= CUpd(c, RT5670_DMIC_CTRL1, RT5670_DMIC_1_DP_MASK, RT5670_DMIC_1_DP_IN2P);
        else
            ok &= CUpd(c, RT5670_DMIC_CTRL1, RT5670_DMIC_2_DP_MASK, RT5670_DMIC_2_DP_IN3N);
    }
    /* rt5670_probe: variant */
    ok &= CRd(c, RT5670_RESET, &v);
    p->Id = (UCHAR)(v & RT5670_ID_MASK);
    if (p->Id == (3 << 1)) {
        LOGE("RT5670: unknown variant (RESET %04x)\n", v);
        return FALSE;
    }
    BiasOff(c);

    /* combo jack in headset mode (rt5670_headset_detect(insert) minus the type readback) */
    if (p->Mic == MIC_HS_IN1) {
        ok &= CUpd(c, RT5670_PWR_ANLG1, RT5670_PWR_MB | RT5670_PWR_BG, RT5670_PWR_MB | RT5670_PWR_BG);
        ok &= CUpd(c, RT5670_PWR_VOL, RT5670_PWR_MIC_DET, RT5670_PWR_MIC_DET);
        ok &= CUpd(c, RT5670_GEN_CTRL3, 0x4, 0x0);
        ok &= CUpd(c, RT5670_CJ_CTRL2, RT5670_CBJ_DET_MODE | RT5670_CBJ_MN_JD, RT5670_CBJ_MN_JD);
        ok &= CUpd(c, RT5670_CJ_CTRL1, RT5670_CBJ_BST1_EN, RT5670_CBJ_BST1_EN);
        ok &= CWr(c, RT5670_JD_CTRL3, 0x00f0);
        ok &= CUpd(c, RT5670_CJ_CTRL2, RT5670_CBJ_MN_JD, RT5670_CBJ_MN_JD);
        ok &= CUpd(c, RT5670_CJ_CTRL2, RT5670_CBJ_MN_JD, 0);
        CDelay(300);
        ok &= CUpd(c, RT5670_PWR_VOL, RT5670_PWR_MIC_DET, 0);
        ok &= CUpd(c, RT5670_PWR_ANLG1, RT5670_PWR_MB | RT5670_PWR_BG, 0);
    }

    /* cht_codec_init: all DA/AD filters on the I2S1 ASRC */
    ok &= CUpd(c, RT5670_ASRC_2, 0xffff, RT5670_ASRC2_I2S1);
    ok &= CUpd(c, RT5670_ASRC_3, 0x00ff, RT5670_ASRC3_I2S1);

    /* PLL1 (rt5670_set_dai_pll), sysclk on the RC clock until a stream runs */
    ok &= CUpd(c, RT5670_GLB_CLK, RT5670_SCLK_SRC_MASK | RT5670_PLL1_SRC_MASK, RT5670_SCLK_SRC_RCCLK | p->PllSrc);
    ok &= CWr(c, RT5670_PLL_CTRL1, ((ULONG)pll.N << RT5670_PLL_N_SFT) | (ULONG)pll.K);
    ok &= CWr(c, RT5670_PLL_CTRL2, ((ULONG)(pll.MBp ? 0 : pll.M) << RT5670_PLL_M_SFT) |
              (pll.MBp ? RT5670_PLL_M_BP : 0));

    /* DAI: slave, I2S, normal polarity, word length, pre-divider, 64 fs BCLK mode for > 32 bit
     * frames (rt5670_hw_params), 50 fs data mode (rt5670_set_bclk_ratio), TDM slots */
    frame = (ULONG)c->Slots * c->Slot;
    ok &= CWr(c, RT5670_I2S1_SDP, RT5670_I2S_MS_S | (c->Valid > 16 ? RT5670_I2S_DL_24 : RT5670_I2S_DL_16));
    ok &= CUpd(c, RT5670_ADDA_CLK1, RT5670_I2S_BCLK_MS1 | RT5670_I2S_PD1_MASK,
               (frame > 32 ? RT5670_I2S_BCLK_MS1 : 0) | ((ULONG)p->PdIdx << RT5670_I2S_PD1_SFT));
    ok &= CUpd(c, RT5670_GEN_CTRL3, RT5670_TDM_DATA_MODE_50FS, (frame % 50) == 0 ? RT5670_TDM_DATA_MODE_50FS : 0);
    if (c->Slots > 2) {
        ULONG sl = c->Slots >= 8 ? 3 : c->Slots >= 6 ? 2 : 1;
        ULONG wd = c->Slot >= 32 ? 3 : c->Slot >= 24 ? 2 : c->Slot >= 20 ? 1 : 0;
        ok &= CUpd(c, RT5670_TDM_CTRL_1, 0x7c00, (1 << 14) | (sl << 12) | (wd << 10));
    }

    /* playback routing: IF1 -> DAC1 MIX -> stereo DAC mixer -> DAC1 / PDM1 (+ PDM2) */
    ok &= CWr(c, RT5670_AD_DA_MIXER, RT5670_M_ADCMIX_L | RT5670_M_ADCMIX_R);
    ok &= CWr(c, RT5670_STO_DAC_MIXER, 0x5656 & ~(RT5670_M_DAC_L1 | RT5670_M_DAC_R1));
    ok &= CWr(c, RT5670_DAC1_DIG_VOL, 0xafaf); /* 0 dB */
    ok &= CUpd(c, RT5670_PDM_OUT_CTRL, RT5670_PDM1_SRC_STO | RT5670_PDM2_SRC_STO | RT5670_M_PDM1 | RT5670_M_PDM2,
               RT5670_PDM1_SRC_STO | RT5670_PDM2_SRC_STO | RT5670_M_PDM1 | RT5670_M_PDM2);
    ok &= CWr(c, RT5670_HPO_MIXER, 0xe00f & ~(RT5670_M_HPVOL_HM | RT5670_M_DACL1_HML | RT5670_M_DACR1_HMR));
    ok &= CWr(c, RT5670_LOUT_MIXER, 0xf000 & ~(RT5670_M_DAC_L1_LM | RT5670_M_DAC_R1_LM));
    ok &= CWr(c, RT5670_HP_VOL, RT5670_LR_MUTE | RT5670_OUT_0DB);
    ok &= CWr(c, RT5670_LOUT1, RT5670_LR_MUTE | RT5670_OUT_0DB);

    /* capture routing: boost +24 dB, RECMIX <- BST1 / BST2, stereo1 ADC (ADC or DMIC1/2) */
    ok &= CUpd(c, RT5670_CJ_CTRL1, 0xf << RT5670_BST_SFT1, RT5670_BST_24DB << RT5670_BST_SFT1);
    ok &= CUpd(c, RT5670_IN2, 0xf << RT5670_BST_SFT1, RT5670_BST_24DB << RT5670_BST_SFT1);
    recm = 0x007f & ~(p->Mic == MIC_HS_IN1 ? RT5670_M_BST1_RM : UsesDmic(p) ? 0 : RT5670_M_BST2_RM);
    ok &= CWr(c, RT5670_REC_L2_MIXER, recm);
    ok &= CWr(c, RT5670_REC_R2_MIXER, recm);
    if (UsesDmic(p))
        adcm = RT5670_M_ADC_L1 | RT5670_M_ADC_R1 | RT5670_ADC_1_SRC_ADC | RT5670_ADC_2_SRC_DMIC |
               ((ULONG)(p->Mic == MIC_DMIC2 ? 1 : 0) << RT5670_DMIC_SRC_SFT);
    else
        adcm = RT5670_M_ADC_L2 | RT5670_M_ADC_R2 | RT5670_ADC_1_SRC_ADC | RT5670_ADC_2_SRC_DMIC;
    ok &= CWr(c, RT5670_STO1_ADC_MIXER, adcm);
    ok &= CWr(c, RT5670_STO1_ADC_DIG_VOL, 0x2f2f);  /* 0 dB, unmuted */

    if (!ok)
        return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("RT567%c (vid %u): PLL %s %u -> %u N=%d K=%d M=%d%s, mic mode %u, %u fs BCLK\n",
         p->Id == RT5670_ID_5672 ? '2' : p->Id == RT5670_ID_5671 ? '1' : '0', p->VId,
         p->PllSrc == RT5670_PLL1_SRC_MCLK ? "MCLK" : "BCLK1", fin, p->Sysclk, pll.N, pll.K,
         pll.MBp ? -1 : pll.M, pll.KBp ? " (K bypass)" : "", p->Mic, frame);
    return TRUE;
}

static void Rt5670Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (on) {
        if (!c->PlayOn && !c->RecOn) {
            BiasOn(c);
            ClockOn(c);
        }
        if (rec)
            RecUp(c);
        else
            PlayUp(c);
    } else {
        if (rec)
            RecDown(c);
        else
            PlayDown(c);
        if (!(rec ? c->PlayOn : c->RecOn)) {
            ClockOff(c);
            BiasOff(c);
        }
    }
}

/* DAC1 digital volume: 0xaf = 0 dB, 0.375 dB steps; mute with the DAC1 inputs of DAC1 MIXL/R
 * (AD_DA_MIXER, Linux "DAC1 Playback Switch") */
static void Rt5670Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    CWr(c, RT5670_DAC1_DIG_VOL, (CVolSteps(left, 175) << 8) | CVolSteps(right, 175));
    CUpd(c, RT5670_AD_DA_MIXER, RT5670_M_DAC1_L | RT5670_M_DAC1_R, mute ? RT5670_M_DAC1_L | RT5670_M_DAC1_R : 0);
    PRIV(c)->Muted = mute ? TRUE : FALSE;
}

static void Rt5670Shutdown(SST_CODEC *c)
{
    if (c->PlayOn)
        PlayDown(c);
    if (c->RecOn)
        RecDown(c);
    CUpd(c, RT5670_HP_VOL, RT5670_LR_MUTE, RT5670_LR_MUTE);
    CUpd(c, RT5670_LOUT1, RT5670_LR_MUTE, RT5670_LR_MUTE);
    ClockOff(c);
    BiasOff(c);
    /* jack-out state of the combo jack, then rt5670_remove */
    CUpd(c, RT5670_GEN_CTRL3, 0x4, 0x4);
    CUpd(c, RT5670_CJ_CTRL1, RT5670_CBJ_BST1_EN, 0);
    CWr(c, RT5670_RESET, 0);
}

const CODEC_DRV g_DrvRt5670 = { "RT5670", "10EC5670\0" "10EC5672\0" "10EC5640\0", CODEC_HEADSET, 1, 2,
                                Rt5670Probe, Rt5670Init, Rt5670Stream, Rt5670Volume, Rt5670Shutdown };
