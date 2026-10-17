#include "codec.h"

/*
 * Realtek RT5640 / RT5642 / RT5639 I2S audio codec (Bay Trail / Cherry Trail / Haswell).
 * Reference: Linux sound/soc/codecs/rt5640.c/.h, sound/soc/codecs/rl6231.c,
 * machine drivers sound/soc/intel/boards/bytcr_rt5640.c and hsw_rt5640.c.
 *
 * I2C: 8-bit register address, 16-bit big-endian value.  Private (index/data) registers
 * PR_xx are reached through RT5640_PRIV_INDEX (0x6a) / RT5640_PRIV_DATA (0x6c).
 *
 * Clocking (slave): PLL1 from BCLK1 -> sysclk = 512 * fs (bytcr_rt5640:
 * byt_rt5640_prepare_and_enable_pll1, RT5640_PLL1_S_BCLK1, rate * 512), I2S pre-divider 2.
 *
 * AIF: Bay Trail CR boards wire SSP0 to AIF1 or AIF2 (BIOS "CHAN" package, which we do not
 * have).  On SSP port 0 both interfaces are configured identically, the stereo DAC mixer
 * takes IF1 and IF2 and the ADC goes to both; the PLL then runs from MCLK when there is one
 * (BYT_RT5640_MCLK_EN is set in every SSP0 quirk), else from BCLK1 (SSP0_AIF1 is the
 * common case).  Other ports use AIF1 only (SSP2-AIF1, the bytcr/cht/hsw default).
 *
 * Outputs (no jack detection): HPOL/HPOR (HPO MIX <- DAC1), class-D speaker SPOL/SPOR
 * (SPK MIX <- DAC1 -> SPKVOL -> SPO MIX), LOUTL/LOUTR (LOUT MIX <- DAC1).  Speaker routing is
 * the bytcr default stereo map (byt_rt5640_stereo_spk_map); boards with
 * BYT_RT5640_MONO_SPEAKER only have SPOL connected and then play the left channel.
 *
 * Capture, c->Mic:
 *   0  headset mic on IN2P, single-ended, MICBIAS1 (byt_rt5640_audio_map "Headset Mic")
 *   1  internal analog mic IN1P/IN1N differential (BYT_RT5640_IN1_MAP + DIFF_MIC)
 *   2  internal analog mic IN3P/IN3N differential (BYT_RT5640_IN3_MAP + DIFF_MIC, the
 *      Bay Trail CR default BYTCR_INPUT_DEFAULTS)
 *   3  DMIC1 (BYT_RT5640_DMIC1_MAP: data on IN1P, clock on GPIO2; Bay Trail T / CHT default)
 *   4  DMIC2 (BYT_RT5640_DMIC2_MAP: data on IN1N, clock on GPIO2)
 *   5  IN2P/IN2N differential ("realtek,in2-differential")
 *   6  IN1P single-ended (IN1_MAP without DIFF_MIC, HSMIC2_ON_IN1)
 *   7  IN3P single-ended (IN3_MAP without DIFF_MIC)
 * Analog inputs: boost +24 dB, both REC mixers take the boost stage (mono mic on both ADC
 * channels).  DMICs are captured as the stereo pair L/R.
 *
 * GPIOs: the Linux driver only knows a DT "realtek,ldo1-en" line; no ACPI GpioIo is used.
 */

/* ---- registers (rt5640.h) ---- */
#define RT5640_RESET            0x00
#define RT5640_SPK_VOL          0x01
#define RT5640_HP_VOL           0x02
#define RT5640_OUTPUT           0x03
#define RT5640_IN1_IN2          0x0d
#define RT5640_IN3_IN4          0x0e
#define RT5640_DAC1_DIG_VOL     0x19
#define RT5640_DAC2_DIG_VOL     0x1a
#define RT5640_DAC2_CTRL        0x1b
#define RT5640_ADC_DIG_VOL      0x1c
#define RT5640_ADC_DATA         0x1d
#define RT5640_STO_ADC_MIXER    0x27
#define RT5640_MONO_ADC_MIXER   0x28
#define RT5640_AD_DA_MIXER      0x29
#define RT5640_STO_DAC_MIXER    0x2a
#define RT5640_DSP_PATH2        0x2e
#define RT5640_REC_L2_MIXER     0x3c
#define RT5640_REC_R2_MIXER     0x3e
#define RT5640_HPO_MIXER        0x45
#define RT5640_SPK_L_MIXER      0x46
#define RT5640_SPK_R_MIXER      0x47
#define RT5640_SPO_L_MIXER      0x48
#define RT5640_SPO_R_MIXER      0x49
#define RT5640_LOUT_MIXER       0x53
#define RT5640_PWR_DIG1         0x61
#define RT5640_PWR_DIG2         0x62
#define RT5640_PWR_ANLG1        0x63
#define RT5640_PWR_ANLG2        0x64
#define RT5640_PWR_MIXER        0x65
#define RT5640_PWR_VOL          0x66
#define RT5640_PRIV_INDEX       0x6a
#define RT5640_PRIV_DATA        0x6c
#define RT5640_I2S1_SDP         0x70
#define RT5640_I2S2_SDP         0x71
#define RT5640_ADDA_CLK1        0x73
#define RT5640_DMIC             0x75
#define RT5640_GLB_CLK          0x80
#define RT5640_PLL_CTRL1        0x81
#define RT5640_PLL_CTRL2        0x82
#define RT5640_ASRC_2           0x84
#define RT5640_DEPOP_M1         0x8e
#define RT5640_DEPOP_M2         0x8f
#define RT5640_DEPOP_M3         0x90
#define RT5640_CHARGE_PUMP      0x91
#define RT5640_MICBIAS          0x93
#define RT5640_JD_CTRL          0xbb
#define RT5640_GPIO_CTRL1       0xc0
#define RT5640_GCTL1            0xfa
#define RT5640_VENDOR_ID2       0xff
/* private registers */
#define RT5640_PR_CHPUMP_INT_REG1 0x24
#define RT5640_PR_MAMP_INT_REG2   0x37
#define RT5640_PR_HP_DCC_INT1     0x77

#define RT5640_DEVICE_ID        0x6231
#define RT5640_ID_MASK          (3 << 1)    /* RESET[2:1]: 0 RT5639, 2 RT5640, 3 RT5642 */
#define RT5640_ID_5639          (0 << 1)

/* xxx_VOL: L_MUTE 15, VOL_L_MUTE 14, R_MUTE 7, VOL_R_MUTE 6, volume 0x08 = 0 dB */
#define RT5640_L_MUTE           (1 << 15)
#define RT5640_VOL_L_MUTE       (1 << 14)
#define RT5640_R_MUTE           (1 << 7)
#define RT5640_VOL_R_MUTE       (1 << 6)
#define RT5640_OUT_0DB          0x0808
#define RT5640_IN_DF1           (1 << 7)    /* IN1_IN2: IN1 differential */
#define RT5640_IN_DF2           (1 << 6)    /* IN1_IN2: IN3 differential, IN3_IN4: IN2 differential */
#define RT5640_BST_SFT1         12
#define RT5640_BST_SFT2         8
#define RT5640_BST_24DB         2           /* bst_tlv: 0, +20, +24, +30, ... */
/* PWR_DIG1 */
#define RT5640_PWR_I2S1         (1 << 15)
#define RT5640_PWR_I2S2         (1 << 14)
#define RT5640_PWR_DAC_L1       (1 << 12)
#define RT5640_PWR_DAC_R1       (1 << 11)
#define RT5640_PWR_DAC_L2       (1 << 7)
#define RT5640_PWR_DAC_R2       (1 << 6)
#define RT5640_PWR_ADC_L        (1 << 2)
#define RT5640_PWR_ADC_R        (1 << 1)
#define RT5640_PWR_CLS_D        (1 << 0)
/* PWR_DIG2 */
#define RT5640_PWR_ADC_SF       (1 << 15)
#define RT5640_PWR_ADC_MF_L     (1 << 14)
#define RT5640_PWR_ADC_MF_R     (1 << 13)
/* PWR_ANLG1 */
#define RT5640_PWR_VREF1        (1 << 15)
#define RT5640_PWR_FV1          (1 << 14)
#define RT5640_PWR_MB           (1 << 13)
#define RT5640_PWR_LM           (1 << 12)
#define RT5640_PWR_BG           (1 << 11)
#define RT5640_PWR_HP_L         (1 << 7)
#define RT5640_PWR_HP_R         (1 << 6)
#define RT5640_PWR_HA           (1 << 5)
#define RT5640_PWR_VREF2        (1 << 4)
#define RT5640_PWR_FV2          (1 << 3)
#define RT5640_PWR_LDO2         (1 << 2)
/* PWR_ANLG2 */
#define RT5640_PWR_BST1         (1 << 15)   /* "BST1" (IN1) */
#define RT5640_PWR_BST2         (1 << 14)   /* "BST3" widget (IN3) */
#define RT5640_PWR_BST4         (1 << 12)   /* "BST2" widget (IN2) */
#define RT5640_PWR_MB1          (1 << 11)
#define RT5640_PWR_PLL          (1 << 9)
/* PWR_MIXER */
#define RT5640_PWR_SM_L         (1 << 13)
#define RT5640_PWR_SM_R         (1 << 12)
#define RT5640_PWR_RM_L         (1 << 11)
#define RT5640_PWR_RM_R         (1 << 10)
/* PWR_VOL */
#define RT5640_PWR_SV_L         (1 << 15)
#define RT5640_PWR_SV_R         (1 << 14)
/* REC_x2_MIXER mutes: BST1 bit 1, BST2(IN3) bit 2, BST4(IN2) bit 4 */
#define RT5640_M_BST1_RM        (1 << 1)
#define RT5640_M_BST2_RM        (1 << 2)
#define RT5640_M_BST4_RM        (1 << 4)
/* STO_ADC_MIXER */
#define RT5640_M_ADC_L1         (1 << 14)
#define RT5640_M_ADC_L2         (1 << 13)
#define RT5640_ADC_1_SRC_ADC    (1 << 12)
#define RT5640_ADC_2_SRC_SFT    10          /* 0 DMIC1, 1 DMIC2 */
#define RT5640_M_ADC_R1         (1 << 6)
#define RT5640_M_ADC_R2         (1 << 5)
/* MONO_ADC_MIXER */
#define RT5640_M_MONO_ADC_L1    (1 << 14)
#define RT5640_M_MONO_ADC_L2    (1 << 13)
#define RT5640_MONO_ADC_L1_ADCL (1 << 12)
#define RT5640_MONO_ADC_L2_SFT  10          /* 0 DMIC L1, 1 DMIC L2 */
#define RT5640_M_MONO_ADC_R1    (1 << 6)
#define RT5640_M_MONO_ADC_R2    (1 << 5)
#define RT5640_MONO_ADC_R1_ADCR (1 << 4)
#define RT5640_MONO_ADC_R2_SFT  2
/* AD_DA_MIXER */
#define RT5640_M_ADCMIX_L       (1 << 15)
#define RT5640_M_IF1_DAC_L      (1 << 14)
#define RT5640_M_ADCMIX_R       (1 << 7)
#define RT5640_M_IF1_DAC_R      (1 << 6)
/* STO_DAC_MIXER */
#define RT5640_M_DAC_L1         (1 << 14)
#define RT5640_M_DAC_L2         (1 << 12)
#define RT5640_M_ANC_DAC_L      (1 << 10)
#define RT5640_M_DAC_R1         (1 << 6)
#define RT5640_M_DAC_R2         (1 << 4)
#define RT5640_M_ANC_DAC_R      (1 << 2)
/* DAC2_CTRL */
#define RT5640_M_DAC_L2_VOL     (1 << 13)
#define RT5640_M_DAC_R2_VOL     (1 << 12)
/* HPO_MIXER: DAC2 15, DAC1 14, HPVOL 13 (mutes) */
#define RT5640_M_DAC2_HM        (1 << 15)
#define RT5640_M_DAC1_HM        (1 << 14)
#define RT5640_M_HPVOL_HM       (1 << 13)
/* SPK_x_MIXER (mutes): RECMIX 5, IN 4, DAC1 3, DAC2 2, OUTMIX 1 */
#define RT5640_SPK_MIX_ALL      0x003e
#define RT5640_M_DAC_X1_SM      (1 << 3)
/* SPO_L_MIXER: DAC R1 15, DAC L1 14, SPKVOL R 13, SPKVOL L 12, BST1 11; SPO_R: DAC R1 13, SV_R 12, BST1 11 */
#define RT5640_M_SV_L_SPM_L     (1 << 12)
#define RT5640_M_SV_R_SPM_R     (1 << 12)
/* LOUT_MIXER: DAC L1 15, DAC R1 14, OUTVOL L 13, OUTVOL R 12 */
#define RT5640_M_DAC_L1_LM      (1 << 15)
#define RT5640_M_DAC_R1_LM      (1 << 14)
/* I2Sx_SDP */
#define RT5640_I2S_MS_S         (1 << 15)
#define RT5640_I2S_DL_16        (0 << 2)
#define RT5640_I2S_DL_24        (2 << 2)
/* ADDA_CLK1 */
#define RT5640_I2S_BCLK_MS1_SFT 15
#define RT5640_I2S_PD1_SFT      12
#define RT5640_I2S_BCLK_MS2_SFT 11
#define RT5640_I2S_PD2_SFT      8
/* DMIC */
#define RT5640_DMIC_1_EN        (1 << 15)
#define RT5640_DMIC_2_EN        (1 << 14)
#define RT5640_DMIC_CLK_SFT     5
#define RT5640_DMIC_CLK_MASK    (7 << 5)
/* GLB_CLK */
#define RT5640_SCLK_SRC_MASK    (3 << 14)
#define RT5640_SCLK_SRC_PLL1    (1 << 14)
#define RT5640_SCLK_SRC_RCCLK   (2 << 14)
#define RT5640_PLL1_SRC_MASK    (3 << 12)
#define RT5640_PLL1_SRC_MCLK    (0 << 12)
#define RT5640_PLL1_SRC_BCLK1   (1 << 12)
#define RT5640_PLL_N_SFT        7
#define RT5640_PLL_M_SFT        12
#define RT5640_PLL_M_BP         (1 << 11)
/* ASRC_2: DA stereo 5, DA mono L 15 / R 14, AD mono L 13 / R 12, AD stereo 11 */
#define RT5640_ASRC_ALL         0xf820
/* DEPOP_M1 */
#define RT5640_HP_CP_MASK       (1 << 3)
#define RT5640_HP_CP_PU         (1 << 3)
#define RT5640_HP_CP_PD         (0 << 3)
#define RT5640_HP_SG_MASK       (1 << 2)
#define RT5640_HP_SG_EN         (1 << 2)
#define RT5640_HP_SG_DIS        (0 << 2)
#define RT5640_HP_CB_MASK       (1 << 0)
#define RT5640_HP_CB_PU         (1 << 0)
/* DEPOP_M2 */
#define RT5640_DEPOP_MASK       (1 << 13)
#define RT5640_DEPOP_AUTO       (0 << 13)
#define RT5640_DEPOP_MAN        (1 << 13)
#define RT5640_DIG_DP_MASK      (1 << 6)
#define RT5640_DIG_DP_EN        (1 << 6)
/* DEPOP_M3: charge pump clock FQ1 [10:8], FQ2 [6:4], FQ3 [2:0]; 7 = 192 kHz, 3 = 12 kHz */
#define RT5640_CP_FQ_MASK       0x0777
#define RT5640_CP_FQ_VAL        ((7 << 8) | (3 << 4) | 7)
/* CHARGE_PUMP */
#define RT5640_PM_HP_MASK       (3 << 8)
#define RT5640_PM_HP_HV         (2 << 8)
/* GPIO_CTRL1 */
#define RT5640_GP2_PIN_DMIC1_SCL (1 << 14)
/* GCTL1 */
#define RT5640_MCLK_DET         (1 << 11)

/* mic modes (c->Mic) */
enum { MIC_HS_IN2, MIC_IN1_DIFF, MIC_IN3_DIFF, MIC_DMIC1, MIC_DMIC2, MIC_IN2_DIFF, MIC_IN1, MIC_IN3, MIC_MAX };

typedef struct {
    ULONG   Sysclk;
    USHORT  PllSrc;         /* GLB_CLK PLL1_SRC field */
    UCHAR   Aif2;           /* drive AIF2 too (SSP0) */
    UCHAR   Mic;
    UCHAR   Id;             /* RESET[2:1] */
    UCHAR   Asrc;
    UCHAR   PdIdx;          /* I2S pre-divider index */
    UCHAR   DmicClk;
} RT5640_PRIV;

typedef char RT5640_PRIV_FITS[(sizeof(RT5640_PRIV) <= sizeof(((SST_CODEC *)0)->Priv)) ? 1 : -1];

#define PRIV(c) ((RT5640_PRIV *)(c)->Priv)

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
    BOOLEAN ok = CWr(c, RT5640_PRIV_INDEX, idx);
    ok &= CWr(c, RT5640_PRIV_DATA, val);
    return ok;
}

static BOOLEAN PrUpd(SST_CODEC *c, ULONG idx, ULONG mask, ULONG val)
{
    ULONG v;
    if (!CWr(c, RT5640_PRIV_INDEX, idx) || !CRd(c, RT5640_PRIV_DATA, &v))
        return FALSE;
    return PrWr(c, idx, (v & ~mask) | (val & mask));
}

static BOOLEAN Rt5640Probe(SST_CODEC *c)
{
    ULONG v = 0;

    if (!CRd(c, RT5640_VENDOR_ID2, &v) || v != RT5640_DEVICE_ID) {
        CDelay(60);         /* rt5640_i2c_probe retries after 60..100 ms */
        if (!CRd(c, RT5640_VENDOR_ID2, &v))
            return FALSE;
    }
    return v == RT5640_DEVICE_ID;
}

/* rt5640_set_bias_level(SND_SOC_BIAS_OFF) */
static void BiasOff(SST_CODEC *c)
{
    CWr(c, RT5640_DEPOP_M1, 0x0004);
    CWr(c, RT5640_DEPOP_M2, 0x1100);
    CUpd(c, RT5640_GCTL1, 0x1, 0);
    CWr(c, RT5640_PWR_DIG1, 0x0000);
    CWr(c, RT5640_PWR_DIG2, 0x0000);
    CWr(c, RT5640_PWR_VOL, 0x0000);
    CWr(c, RT5640_PWR_MIXER, 0x0000);
    CWr(c, RT5640_PWR_ANLG1, 0x0000);
    CWr(c, RT5640_PWR_ANLG2, 0x0000);
}

/* rt5640_set_bias_level(OFF -> STANDBY) */
static void BiasOn(SST_CODEC *c)
{
    ULONG m = RT5640_PWR_VREF1 | RT5640_PWR_MB | RT5640_PWR_BG | RT5640_PWR_VREF2;

    CUpd(c, RT5640_PWR_ANLG1, m, m);
    CDelay(10);
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_FV1 | RT5640_PWR_FV2, RT5640_PWR_FV1 | RT5640_PWR_FV2);
    CUpd(c, RT5640_GCTL1, 0x1, 0x1);
    CUpd(c, RT5640_MICBIAS, 0x0030, 0x0030);
}

/* "Platform Clock" PRE_PMU: PLL1 (programmed in Init) -> sysclk; rt5640_set_dai_sysclk(PLL1) */
static void ClockOn(SST_CODEC *c)
{
    CUpd(c, RT5640_PWR_ANLG2, RT5640_PWR_PLL, RT5640_PWR_PLL);
    CUpd(c, RT5640_GLB_CLK, RT5640_SCLK_SRC_MASK, RT5640_SCLK_SRC_PLL1);
}

/* "Platform Clock" POST_PMD: sysclk back to the internal RC clock */
static void ClockOff(SST_CODEC *c)
{
    CUpd(c, RT5640_PWR_ANLG2, RT5640_PWR_PLL, 0);
    CUpd(c, RT5640_GLB_CLK, RT5640_SCLK_SRC_MASK, RT5640_SCLK_SRC_RCCLK);
}

static ULONG I2sBits(SST_CODEC *c)
{
    return RT5640_PWR_I2S1 | (PRIV(c)->Aif2 ? RT5640_PWR_I2S2 : 0);
}

/* hp_amp_power_on() */
static void HpAmpPowerOn(SST_CODEC *c)
{
    PrUpd(c, RT5640_PR_CHPUMP_INT_REG1, 0x0700, 0x0200);
    CUpd(c, RT5640_DEPOP_M2, RT5640_DEPOP_MASK, RT5640_DEPOP_MAN);
    CUpd(c, RT5640_DEPOP_M1, RT5640_HP_CP_MASK | RT5640_HP_SG_MASK | RT5640_HP_CB_MASK,
         RT5640_HP_CP_PU | RT5640_HP_SG_DIS | RT5640_HP_CB_PU);
    PrWr(c, RT5640_PR_HP_DCC_INT1, 0x9f00);
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_FV1 | RT5640_PWR_FV2, 0);
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_HA, RT5640_PWR_HA);
    CDelay(10);
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_FV1 | RT5640_PWR_FV2, RT5640_PWR_FV1 | RT5640_PWR_FV2);
}

/* rt5640_pmu_depop() ("HP Amp" POST_PMU) */
static void PmuDepop(SST_CODEC *c)
{
    CUpd(c, RT5640_DEPOP_M2, RT5640_DEPOP_MASK | RT5640_DIG_DP_MASK, RT5640_DEPOP_AUTO | RT5640_DIG_DP_EN);
    CUpd(c, RT5640_CHARGE_PUMP, RT5640_PM_HP_MASK, RT5640_PM_HP_HV);
    CUpd(c, RT5640_DEPOP_M3, RT5640_CP_FQ_MASK, RT5640_CP_FQ_VAL);
    PrWr(c, RT5640_PR_MAMP_INT_REG2, 0x1c00);
    CUpd(c, RT5640_DEPOP_M1, RT5640_HP_CP_MASK | RT5640_HP_SG_MASK, RT5640_HP_CP_PD | RT5640_HP_SG_EN);
    PrUpd(c, RT5640_PR_CHPUMP_INT_REG1, 0x0700, 0x0400);
}

/* playback path in DAPM order (supply, mixer, pga, kcontrol, post) */
static void PlayUp(SST_CODEC *c)
{
    ULONG dac = RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1;
    ULONG lr = RT5640_L_MUTE | RT5640_R_MUTE;

    if (PRIV(c)->Aif2)
        dac |= RT5640_PWR_DAC_L2 | RT5640_PWR_DAC_R2;
    /* supplies: I2S, DAC power, "Improve SPK Amp Drv", "HP L/R Amp"; then "Improve HP Amp Drv" */
    CUpd(c, RT5640_PWR_DIG1, I2sBits(c) | dac | RT5640_PWR_CLS_D, I2sBits(c) | dac | RT5640_PWR_CLS_D);
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_HP_L | RT5640_PWR_HP_R, RT5640_PWR_HP_L | RT5640_PWR_HP_R);
    HpAmpPowerOn(c);
    /* mixers: SPK MIXL/R */
    CUpd(c, RT5640_PWR_MIXER, RT5640_PWR_SM_L | RT5640_PWR_SM_R, RT5640_PWR_SM_L | RT5640_PWR_SM_R);
    /* PGAs: SPKVOL L/R, "HP Amp" (depop), "LOUT amp" */
    CUpd(c, RT5640_PWR_VOL, RT5640_PWR_SV_L | RT5640_PWR_SV_R, RT5640_PWR_SV_L | RT5640_PWR_SV_R);
    /* "LOUT amp" POST_PMU (its hp_amp_power_on() already ran for "Improve HP Amp Drv"), run
     * before the "HP Amp" depop so that it does not undo rt5640_pmu_depop() */
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_LM, RT5640_PWR_LM);
    CUpd(c, RT5640_OUTPUT, lr, 0);
    PmuDepop(c);
    /* autodisable switches: "Speaker L/R Playback", "HP L/R Playback" */
    CUpd(c, RT5640_SPK_VOL, lr, 0);
    CUpd(c, RT5640_HP_VOL, lr, 0);
    CDelay(80);                                 /* "HP Post" */
}

static void PlayDown(SST_CODEC *c)
{
    ULONG dac = RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1 | RT5640_PWR_DAC_L2 | RT5640_PWR_DAC_R2;
    ULONG lr = RT5640_L_MUTE | RT5640_R_MUTE;

    /* kcontrols first: mute HP / speaker */
    CUpd(c, RT5640_HP_VOL, lr, lr);
    CUpd(c, RT5640_SPK_VOL, lr, lr);
    /* PGAs: "HP Amp" PRE_PMD waits 70 ms, "LOUT amp" PRE_PMD mutes and powers LOUT down */
    CDelay(70);
    CUpd(c, RT5640_OUTPUT, lr, lr);
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_LM, 0);
    CUpd(c, RT5640_PWR_VOL, RT5640_PWR_SV_L | RT5640_PWR_SV_R, 0);
    CUpd(c, RT5640_PWR_MIXER, RT5640_PWR_SM_L | RT5640_PWR_SM_R, 0);
    /* supplies */
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_HP_L | RT5640_PWR_HP_R, 0);
    CUpd(c, RT5640_PWR_DIG1, dac | RT5640_PWR_CLS_D | (c->RecOn ? 0 : I2sBits(c)), 0);
}

static void RecUp(SST_CODEC *c)
{
    RT5640_PRIV *p = PRIV(c);
    ULONG filt = RT5640_PWR_ADC_SF | (p->Aif2 ? RT5640_PWR_ADC_MF_L | RT5640_PWR_ADC_MF_R : 0);

    /* supplies: I2S, ADC filters, then mic supplies / DMIC clock */
    CUpd(c, RT5640_PWR_DIG1, I2sBits(c), I2sBits(c));
    CUpd(c, RT5640_PWR_DIG2, filt, filt);
    if (p->Mic == MIC_DMIC1 || p->Mic == MIC_DMIC2) {
        ULONG en = p->Mic == MIC_DMIC1 ? RT5640_DMIC_1_EN : RT5640_DMIC_2_EN;
        CUpd(c, RT5640_DMIC, RT5640_DMIC_CLK_MASK, (ULONG)p->DmicClk << RT5640_DMIC_CLK_SFT);
        CUpd(c, RT5640_DMIC, en, en);
        return;
    }
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_LDO2, RT5640_PWR_LDO2);
    CUpd(c, RT5640_PWR_ANLG2, RT5640_PWR_MB1, RT5640_PWR_MB1);
    /* mixer: RECMIXL/R; PGA: boost stage; ADC */
    CUpd(c, RT5640_PWR_MIXER, RT5640_PWR_RM_L | RT5640_PWR_RM_R, RT5640_PWR_RM_L | RT5640_PWR_RM_R);
    {
        ULONG b = (p->Mic == MIC_IN1_DIFF || p->Mic == MIC_IN1) ? RT5640_PWR_BST1 :
                  (p->Mic == MIC_IN3_DIFF || p->Mic == MIC_IN3) ? RT5640_PWR_BST2 : RT5640_PWR_BST4;
        CUpd(c, RT5640_PWR_ANLG2, b, b);
    }
    CUpd(c, RT5640_PWR_DIG1, RT5640_PWR_ADC_L | RT5640_PWR_ADC_R, RT5640_PWR_ADC_L | RT5640_PWR_ADC_R);
}

static void RecDown(SST_CODEC *c)
{
    CUpd(c, RT5640_PWR_DIG1, RT5640_PWR_ADC_L | RT5640_PWR_ADC_R, 0);
    CUpd(c, RT5640_PWR_ANLG2, RT5640_PWR_BST1 | RT5640_PWR_BST2 | RT5640_PWR_BST4, 0);
    CUpd(c, RT5640_PWR_MIXER, RT5640_PWR_RM_L | RT5640_PWR_RM_R, 0);
    CUpd(c, RT5640_DMIC, RT5640_DMIC_1_EN | RT5640_DMIC_2_EN, 0);
    CUpd(c, RT5640_PWR_ANLG2, RT5640_PWR_MB1, 0);
    CUpd(c, RT5640_PWR_ANLG1, RT5640_PWR_LDO2, 0);
    CUpd(c, RT5640_PWR_DIG2, RT5640_PWR_ADC_SF | RT5640_PWR_ADC_MF_L | RT5640_PWR_ADC_MF_R, 0);
    if (!c->PlayOn)
        CUpd(c, RT5640_PWR_DIG1, I2sBits(c), 0);
}

static BOOLEAN Rt5640Init(SST_CODEC *c)
{
    RT5640_PRIV *p = PRIV(c);
    BOOLEAN ok = TRUE;
    ULONG v, sdp, fin, recm, adcm, monom;
    LONG pd, dm;
    RL_PLL pll;

    memset(p, 0, sizeof(*p));
    p->Mic = (UCHAR)(c->Mic < MIC_MAX ? c->Mic : MIC_HS_IN2);
    p->Aif2 = c->Port == 0;
    p->Sysclk = c->Rate * 512;

    /* PLL reference: BCLK1; on SSP0 (AIF1 or AIF2) MCLK when there is one */
    if (p->Aif2 && c->Mclk) {
        fin = c->Mclk;
        p->PllSrc = RT5640_PLL1_SRC_MCLK;
    } else if (c->Bclk >= RL_PLL_INP_MIN) {
        fin = c->Bclk;
        p->PllSrc = RT5640_PLL1_SRC_BCLK1;
    } else {
        fin = c->Mclk;
        p->PllSrc = RT5640_PLL1_SRC_MCLK;
    }
    if (!RlPllCalc(fin, p->Sysclk, &pll)) {
        LOGE("RT5640: no usable PLL reference (BCLK %u MCLK %u)\n", c->Bclk, c->Mclk);
        return FALSE;
    }
    p->Asrc = p->PllSrc == RT5640_PLL1_SRC_MCLK;    /* bytcr: ASRC for all filters */
    pd = RlClkInfo(p->Sysclk, c->Rate);
    p->PdIdx = (UCHAR)(pd < 0 ? 1 : pd);
    dm = RlDmicClk(p->Sysclk / s_RlPd[p->PdIdx]);     /* set_dmic_clk */
    p->DmicClk = (UCHAR)(dm < 0 ? 4 : dm);

    /* rt5640_i2c_probe: reset, init_list patch (private registers), MCLK detect */
    if (!CWr(c, RT5640_RESET, 0))
        return FALSE;
    CDelay(1);
    ok &= PrWr(c, 0x3d, 0x3600);
    ok &= PrWr(c, 0x12, 0x0aa8);
    ok &= PrWr(c, 0x14, 0x0aaa);
    ok &= PrWr(c, 0x21, 0xe0e0);
    ok &= PrWr(c, 0x23, 0x1804);
    ok &= CUpd(c, RT5640_GCTL1, RT5640_MCLK_DET, RT5640_MCLK_DET);

    /* rt5640_probe: bias off, GCTL1 / MICBIAS / DSP_PATH2 (DAC2 from IF2) */
    BiasOff(c);
    ok &= CUpd(c, RT5640_GCTL1, 0x0301, 0x0301);
    ok &= CUpd(c, RT5640_MICBIAS, 0x0030, 0x0030);
    ok &= CUpd(c, RT5640_DSP_PATH2, 0xfc00, 0x0c00);
    ok &= CRd(c, RT5640_RESET, &v);
    p->Id = (UCHAR)((v & RT5640_ID_MASK) >> 1);

    /* inputs: differential mode, boost +24 dB */
    v = 0;
    if (p->Mic == MIC_IN1_DIFF)
        v |= RT5640_IN_DF1;
    if (p->Mic == MIC_IN3_DIFF)
        v |= RT5640_IN_DF2;
    ok &= CWr(c, RT5640_IN1_IN2, v | (RT5640_BST_24DB << RT5640_BST_SFT1) | (RT5640_BST_24DB << RT5640_BST_SFT2));
    ok &= CWr(c, RT5640_IN3_IN4, (p->Mic == MIC_IN2_DIFF ? RT5640_IN_DF2 : 0) |
              (RT5640_BST_24DB << RT5640_BST_SFT2));
    /* DMIC: clock on GPIO2, data on IN1P (DMIC1) / IN1N (DMIC2) = register default */
    if (p->Mic == MIC_DMIC1 || p->Mic == MIC_DMIC2)
        ok &= CUpd(c, RT5640_GPIO_CTRL1, RT5640_GP2_PIN_DMIC1_SCL, RT5640_GP2_PIN_DMIC1_SCL);

    /* byt_rt5640_init: RC clock until a stream runs; ASRC when the PLL is not locked to BCLK */
    ok &= CUpd(c, RT5640_GLB_CLK, RT5640_SCLK_SRC_MASK, RT5640_SCLK_SRC_RCCLK);
    ok &= CUpd(c, RT5640_ASRC_2, RT5640_ASRC_ALL, p->Asrc ? RT5640_ASRC_ALL : 0);
    ok &= CUpd(c, RT5640_JD_CTRL, 0x3, p->Asrc ? 0x3 : 0);

    /* rt5640_set_dai_pll */
    ok &= CUpd(c, RT5640_GLB_CLK, RT5640_PLL1_SRC_MASK, p->PllSrc);
    ok &= CWr(c, RT5640_PLL_CTRL1, ((ULONG)pll.N << RT5640_PLL_N_SFT) | (ULONG)pll.K);
    ok &= CWr(c, RT5640_PLL_CTRL2, ((ULONG)(pll.MBp ? 0 : pll.M) << RT5640_PLL_M_SFT) |
              (pll.MBp ? RT5640_PLL_M_BP : 0));

    /* DAI: slave, I2S, normal polarity, word length (rt5640_set_dai_fmt / hw_params) */
    if (c->Slots != 2)
        LOGE("RT5640: %u slots per frame, the codec only does 2-slot I2S\n", c->Slots);
    sdp = RT5640_I2S_MS_S | (c->Valid > 16 ? RT5640_I2S_DL_24 : RT5640_I2S_DL_16);
    ok &= CWr(c, RT5640_I2S1_SDP, sdp);
    ok &= CWr(c, RT5640_I2S2_SDP, sdp);
    v = (c->Slots * c->Slot > 32) ? 1 : 0;
    ok &= CUpd(c, RT5640_ADDA_CLK1, 0xff00,
               (v << RT5640_I2S_BCLK_MS1_SFT) | ((ULONG)p->PdIdx << RT5640_I2S_PD1_SFT) |
               (v << RT5640_I2S_BCLK_MS2_SFT) | ((ULONG)p->PdIdx << RT5640_I2S_PD2_SFT));

    /* playback routing: IF1 -> DAC MIX -> stereo DAC mixer (+ IF2 on SSP0) -> DAC1 */
    ok &= CWr(c, RT5640_AD_DA_MIXER, RT5640_M_ADCMIX_L | RT5640_M_ADCMIX_R);
    ok &= CWr(c, RT5640_STO_DAC_MIXER, RT5640_M_ANC_DAC_L | RT5640_M_ANC_DAC_R |
              (p->Aif2 ? 0 : RT5640_M_DAC_L2 | RT5640_M_DAC_R2));
    ok &= CWr(c, RT5640_DAC1_DIG_VOL, 0xafaf);  /* 0 dB */
    ok &= CWr(c, RT5640_DAC2_DIG_VOL, 0xafaf);
    ok &= CWr(c, RT5640_DAC2_CTRL, 0);
    /* HPO MIX <- DAC1; SPK MIX <- DAC1; SPOL <- SPKVOL L, SPOR <- SPKVOL R; LOUT MIX <- DAC1 */
    ok &= CWr(c, RT5640_HPO_MIXER, RT5640_M_DAC2_HM | RT5640_M_HPVOL_HM);
    ok &= CWr(c, RT5640_SPK_L_MIXER, RT5640_SPK_MIX_ALL & ~RT5640_M_DAC_X1_SM);
    ok &= CWr(c, RT5640_SPK_R_MIXER, RT5640_SPK_MIX_ALL & ~RT5640_M_DAC_X1_SM);
    ok &= CWr(c, RT5640_SPO_L_MIXER, 0xf800 & ~RT5640_M_SV_L_SPM_L);
    ok &= CWr(c, RT5640_SPO_R_MIXER, 0x3800 & ~RT5640_M_SV_R_SPM_R);
    ok &= CWr(c, RT5640_LOUT_MIXER, 0xf000 & ~(RT5640_M_DAC_L1_LM | RT5640_M_DAC_R1_LM));
    /* analog volumes 0 dB, channel (volume) switches on, output switches off until playback */
    ok &= CWr(c, RT5640_SPK_VOL, RT5640_L_MUTE | RT5640_R_MUTE | RT5640_OUT_0DB);
    ok &= CWr(c, RT5640_HP_VOL, RT5640_L_MUTE | RT5640_R_MUTE | RT5640_OUT_0DB);
    ok &= CWr(c, RT5640_OUTPUT, RT5640_L_MUTE | RT5640_R_MUTE | RT5640_OUT_0DB);

    /* capture routing */
    recm = 0x007f;                              /* REC_x2_MIXER: all sources muted */
    adcm = RT5640_ADC_1_SRC_ADC;                /* stereo ADC mixer */
    monom = RT5640_MONO_ADC_L1_ADCL | RT5640_MONO_ADC_R1_ADCR;
    switch (p->Mic) {
    case MIC_IN1_DIFF: case MIC_IN1:
        recm &= ~RT5640_M_BST1_RM;
        break;
    case MIC_IN3_DIFF: case MIC_IN3:
        recm &= ~RT5640_M_BST2_RM;
        break;
    case MIC_DMIC1: case MIC_DMIC2:
        break;
    default:
        recm &= ~RT5640_M_BST4_RM;
        break;
    }
    if (p->Mic == MIC_DMIC1 || p->Mic == MIC_DMIC2) {
        v = p->Mic == MIC_DMIC1 ? 0 : 1;
        adcm |= RT5640_M_ADC_L1 | RT5640_M_ADC_R1 | (v << RT5640_ADC_2_SRC_SFT);
        monom |= RT5640_M_MONO_ADC_L1 | RT5640_M_MONO_ADC_R1 |
                 (v << RT5640_MONO_ADC_L2_SFT) | (v << RT5640_MONO_ADC_R2_SFT);
    } else {
        adcm |= RT5640_M_ADC_L2 | RT5640_M_ADC_R2;
        monom |= RT5640_M_MONO_ADC_L2 | RT5640_M_MONO_ADC_R2;
    }
    if (!p->Aif2)
        monom |= RT5640_M_MONO_ADC_L1 | RT5640_M_MONO_ADC_R1 | RT5640_M_MONO_ADC_L2 | RT5640_M_MONO_ADC_R2;
    ok &= CWr(c, RT5640_REC_L2_MIXER, recm);
    ok &= CWr(c, RT5640_REC_R2_MIXER, recm);
    ok &= CWr(c, RT5640_STO_ADC_MIXER, adcm);
    ok &= CWr(c, RT5640_MONO_ADC_MIXER, monom);
    ok &= CWr(c, RT5640_ADC_DIG_VOL, 0x2f2f);   /* 0 dB, unmuted */
    ok &= CWr(c, RT5640_ADC_DATA, 0x2f2f);
    ok &= CUpd(c, RT5640_GCTL1, (1 << 13) | (1 << 12), 0);  /* M_MONO_ADC_L/R off */

    if (!ok)
        return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("RT56%02u: PLL %s %u -> %u N=%d K=%d M=%d%s, mic mode %u, AIF%s, ASRC %u\n",
         p->Id == 0 ? 39 : p->Id == 3 ? 42 : 40,
         p->PllSrc == RT5640_PLL1_SRC_MCLK ? "MCLK" : "BCLK1", fin, p->Sysclk,
         pll.N, pll.K, pll.MBp ? -1 : pll.M, pll.KBp ? " (K bypass)" : "", p->Mic,
         p->Aif2 ? "1+2" : "1", p->Asrc);
    return TRUE;
}

static void Rt5640Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
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

/* DAC1/DAC2 digital volume: 0xaf = 0 dB, 0.375 dB steps down to -65.625 dB; mute in the
 * IF1 -> DAC MIX switch (AD_DA_MIXER) and DAC2_CTRL */
static void Rt5640Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    ULONG v = (CVolSteps(left, 175) << 8) | CVolSteps(right, 175);

    CWr(c, RT5640_DAC1_DIG_VOL, v);
    CWr(c, RT5640_DAC2_DIG_VOL, v);
    CUpd(c, RT5640_AD_DA_MIXER, RT5640_M_IF1_DAC_L | RT5640_M_IF1_DAC_R,
         mute ? RT5640_M_IF1_DAC_L | RT5640_M_IF1_DAC_R : 0);
    CUpd(c, RT5640_DAC2_CTRL, RT5640_M_DAC_L2_VOL | RT5640_M_DAC_R2_VOL,
         mute ? RT5640_M_DAC_L2_VOL | RT5640_M_DAC_R2_VOL : 0);
}

static void Rt5640Shutdown(SST_CODEC *c)
{
    if (c->PlayOn)
        PlayDown(c);
    if (c->RecOn)
        RecDown(c);
    CUpd(c, RT5640_HP_VOL, RT5640_L_MUTE | RT5640_R_MUTE, RT5640_L_MUTE | RT5640_R_MUTE);
    CUpd(c, RT5640_SPK_VOL, RT5640_L_MUTE | RT5640_R_MUTE, RT5640_L_MUTE | RT5640_R_MUTE);
    ClockOff(c);
    BiasOff(c);
    CWr(c, RT5640_RESET, 0);                    /* rt5640_remove */
}

const CODEC_DRV g_DrvRt5640 = { "RT5640", "10EC5640\0" "10EC5642\0" "INTCCFFD\0", CODEC_HEADSET, 1, 2,
                                Rt5640Probe, Rt5640Init, Rt5640Stream, Rt5640Volume, Rt5640Shutdown };
