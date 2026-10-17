#include "codec.h"

/*
 * Realtek RT5663 I2S headset codec (ACPI "10EC5663", Kaby Lake Chromebooks: kbl_rt5663_*,
 * AVS "avs_rt5663").  Two silicon versions with different register sets / sequences:
 *   VENDOR_ID_2 (0xff) = 0x6406: CODEC_VER_0 ("V1" here), 0x6451: CODEC_VER_1 ("V2").
 * Reference: Linux sound/soc/codecs/rt5663.c/.h, rl6231.c, sound/soc/intel/avs/boards/rt5663.c.
 *
 * I2C: 16-bit register address, 16-bit value, big endian.
 *
 * Clocking (I2S slave): sysclk = 512 fs (24.576 MHz at 48 kHz, the MCLK the AVS board uses),
 * PLL1 from BCLK1 with rl6231_pll_calc (preferred); fallback MCLK direct (MCLK == 512 fs) or
 * PLL1 from MCLK, both with the filters on ASRC (I2S1 tracking) as avs_rt5663_hw_params does.
 * I2S pre-divider PD1 = sysclk / 256 fs (rt5663_hw_params).
 *
 * Output: HPOL/HPOR only.  ADDA MIXL/R -> DAC L1/R1 -> (V2: STO1 DAC MIX) -> DAC -> HP amp
 * (rt5663_hp_event / rt5663_charge_pump_event sequences), V2 also "HPO Playback" switch.
 * V1 HP analog gain (STO_DRE_9/10) is set to 0 dB (reset default is -24 dB).
 *
 * Capture, c->Mic:
 *   0  headset mic on the combo jack IN1 (V2: BST1 CBJ -> RECMIX1L, V1: BST1), +24 dB boost,
 *      ADC L on both channels (V2: STO1 ADC R mux = ADC L, V1: ADCDAT1 data swap L/L as
 *      rt5663_i2c_probe sets).  Init runs the "jack inserted" sequence of rt5663(_v2)_jack_detect
 *      (micbias, CBJ, type detection); a headphone / no-plug result is ignored and the
 *      headset state is kept (headset mode forced).
 *   1  V2 only: IN2P/IN2N (BST2 -> RECMIX1L), the BST2 gain has no Linux control (reset
 *      value); on V1 this falls back to 0.
 *   2  headset mic, headset state forced without running the type detection.
 *
 * No jack detection: the JD1 / IRQ configuration of rt5663_i2c_probe is kept as Linux
 * writes it, the button (inline command) IRQ is not enabled.
 * GPIOs: the Linux driver uses none.
 */

#define RT5663_DEVICE_ID_2      0x6451      /* CODEC_VER_1 */
#define RT5663_DEVICE_ID_1      0x6406      /* CODEC_VER_0 */

/* ---- registers (rt5663.h) ---- */
#define R63_RESET               0x0000
#define R63_VENDOR_ID           0x00fd
#define R63_VENDOR_ID_2         0x00ff
#define R63_HP_AMP_2            0x0003      /* V2 */
#define R63_AEC_BST             0x000b      /* V2 */
#define R63_RECMIX              0x0010
#define R63_CBJ_TYPE_2          0x0011      /* V2 */
#define R63_CBJ_TYPE_3          0x0012      /* V2 */
#define R63_STO1_DAC_DIG_VOL    0x0019
#define R63_STO1_ADC_DIG_VOL    0x001c
#define R63_STO1_BOOST          0x001f
#define R63_STO1_ADC_MIXER      0x0026
#define R63_AD_DA_MIXER         0x0029
#define R63_STO_DAC_MIXER       0x002a
#define R63_BYPASS_STO_DAC      0x002d
#define R63_RECMIX1L            0x003c      /* V2 */
#define R63_PWR_DIG_1           0x0061
#define R63_PWR_DIG_2           0x0062
#define R63_PWR_ANLG_1          0x0063
#define R63_PWR_ANLG_2          0x0064
#define R63_PWR_ANLG_3          0x0065
#define R63_PWR_VOL             0x0067
#define R63_SIG_CLK_DET         0x006b
#define R63_PRE_DIV_GATING_1    0x006e
#define R63_PRE_DIV_GATING_2    0x006f
#define R63_I2S1_SDP            0x0070
#define R63_ADDA_CLK_1          0x0073
#define R63_ADDA_RST            0x0074
#define R63_TDM_1               0x0077
#define R63_TDM_2               0x0078
#define R63_TDM_5               0x007b
#define R63_TDM_9               0x007f
#define R63_GLB_CLK             0x0080
#define R63_PLL_1               0x0081
#define R63_PLL_2               0x0082
#define R63_ASRC_1              0x0083
#define R63_ASRC_2              0x0084
#define R63_ASRC_3              0x0085
#define R63_ASRC_4              0x0086
#define R63_DEPOP_1             0x008e
#define R63_DEPOP_2             0x008f
#define R63_HP_CHARGE_PUMP_1    0x0091
#define R63_HP_CHARGE_PUMP_2    0x0092
#define R63_RC_CLK              0x0094
#define R63_AUTO_1MRC_CLK       0x009f
#define R63_IRQ_1               0x00b6
#define R63_INT_ST_2            0x00bf
#define R63_GPIO_1              0x00c0
#define R63_GPIO_2              0x00c1
#define R63_GPIO_3              0x00c2
#define R63_IL_CMD_2            0x00dc
#define R63_IL_CMD_5            0x00df
#define R63_EM_JACK_TYPE_1      0x00e6
#define R63_EM_JACK_TYPE_2      0x00e7
#define R63_STO1_HPF_ADJ1       0x00ec
#define R63_DIG_MISC            0x00fa
#define R63_DIG_VOL_ZCD         0x0100
#define R63_ANA_BIAS_CUR_1      0x0108
#define R63_ANA_BIAS_CUR_4      0x010b
#define R63_BIAS_CUR_8          0x0111
#define R63_DACREF_LDO          0x0112
#define R63_VREFADJ_OP          0x0117
#define R63_VREF_RECMIX         0x0118
#define R63_CHARGE_PUMP_1       0x0125
#define R63_CHARGE_PUMP_2       0x0128
#define R63_CHOP_DAC_L          0x013a
#define R63_CHOP_ADC            0x013b
#define R63_CALIB_ADC           0x013c
#define R63_STO_DRE_1           0x0160
#define R63_STO_DRE_9           0x0168
#define R63_STO_DRE_10          0x0169
#define R63_HP_IMP_SEN_19       0x01d2
#define R63_HP_LOGIC_2          0x01db
#define R63_HP_CALIB_1          0x01dd
#define R63_HP_CALIB_1_1        0x01de
#define R63_HP_CALIB_2          0x01df
#define R63_HP_CALIB_3          0x01e0
#define R63_HP_CALIB_7          0x01e5
#define R63_HP_BIAS             0x0202
#define R63_CBJ_1               0x0250
#define R63_CBJ_2               0x0251
#define R63_DUMMY_1             0x02fa
#define R63_DUMMY_2             0x02fb

/* PWR_DIG_1 */
#define PD1_I2S1                0x8000
#define PD1_DAC_L1              0x0800
#define PD1_DAC_R1              0x0400
#define PD1_LDO_DACREF          0x0100      /* V2 "LDO DAC" */
#define PD1_ADC_L1              0x0010
/* PWR_DIG_2 */
#define PD2_ADC_S1F             0x8000
#define PD2_DAC_S1F             0x0400
/* PWR_ANLG_1 */
#define PA1_VREF1               0x8000
#define PA1_FV1                 0x4000
#define PA1_VREF2               0x2000
#define PA1_FV2                 0x1000
#define PA1_MB                  0x0200
#define PA1_AMP_HP_MASK         0x000c
#define PA1_AMP_HP_3X           0x0004
#define PA1_LDO1_DVO_MASK       0x0003      /* 0 = 0.9 V */
/* PWR_ANLG_2 */
#define PA2_BST1                0x8000
#define PA2_BST2                0x4000
#define PA2_MB1                 0x0800
#define PA2_MB2                 0x0400
#define PA2_BST2_OP             0x0040
#define PA2_JD1                 0x0008
#define PA2_RECMIX1             0x0002      /* "RECMIX1L Power" */
/* PWR_ANLG_3 */
#define PA3_CBJ                 0x0200
#define PA3_PLL                 0x0040
#define PA3_LDO2                0x0004
/* PWR_VOL */
#define PVOL_V2_MIC_DET         0x0020
/* SIG_CLK_DET */
#define CLKDET_EN_ANA_AUTO      0x8000
#define CLKDET_PWR              0x0001
/* I2S1_SDP */
#define SDP_MS_SLAVE            0x8000
#define SDP_BP_INV              0x0100
#define SDP_DL_MASK             0x0030      /* 16 / 20 / 24 / 8 bit = 0..3 */
#define SDP_DL_SFT              4
#define SDP_DF_MASK             0x0007
/* ADDA_CLK_1 */
#define ADDA_I2S_PD1_MASK       0x7000
#define ADDA_I2S_PD1_SFT        12
/* TDM_1 (V1) / TDM_2 (V2) */
#define TDM_MODE_TDM            0x8000
#define TDM_IN_CH_SFT           10
#define TDM_IN_CH_MASK          0x0c00
#define TDM_OUT_CH_SFT          8
#define TDM_OUT_CH_MASK         0x0300
#define TDM_IN_LEN_SFT          6
#define TDM_IN_LEN_MASK         0x00c0
#define TDM_OUT_LEN_SFT         4
#define TDM_OUT_LEN_MASK        0x0030
/* TDM_2 (V1): ADCDAT1 data swap */
#define TDM2_SWAP_ADCDAT1_MASK  0xc000
#define TDM2_SWAP_ADCDAT1_LL    0x8000
/* TDM_5 (V1) / TDM_9 (V2): channel length (set_bclk_ratio) */
#define TDM_LENGTN_MASK         0x0003
/* GLB_CLK */
#define GLB_SCLK_SRC_MASK       0xc000
#define GLB_SCLK_SRC_PLL1       0x4000
#define GLB_V1_PLL1_SRC_MASK    0x3800
#define GLB_V1_PLL1_SRC_BCLK1   0x0800
#define GLB_V2_PLL1_SRC_MASK    0x0700
#define GLB_V2_PLL1_SRC_BCLK1   0x0100
/* PLL_1 / PLL_2 */
#define PLL1_N_SFT              7
#define PLL2_M_SFT              12
#define PLL2_M_BP               0x0800
/* ASRC_1 (widgets "I2S ASRC", "DAC ASRC", "ADC ASRC" on both versions) */
#define ASRC1_I2S1              0x0800
#define ASRC1_DAC_STO1          0x0400
#define ASRC1_ADC_STO1          0x0008
/* ASRC_2 / ASRC_3: filter tracking source, 1 = I2S1 */
#define ASRC2_DA_STO1_TRACK_MASK 0x7000
#define ASRC2_DA_STO1_TRACK_I2S1 0x1000
#define ASRC2_AD_STO1_TRACK_MASK 0x0007     /* V1 */
#define ASRC2_AD_STO1_TRACK_I2S1 0x0001
#define ASRC3_V2_AD_TRACK_MASK  0x7000      /* V2 */
#define ASRC3_V2_AD_TRACK_I2S1  0x1000
/* HP_CHARGE_PUMP_1 */
#define HCP_SI_HP               0x1000
#define HCP_OSW_L               0x0800
#define HCP_OSW_R               0x0400
#define HCP_OVCD_HP_EN          0x0004
/* RC_CLK */
#define RC_DIG_1M_CLK_EN        0x0100
/* AUTO_1MRC_CLK */
#define A1M_IRQ_POW_SAV         0x8000
#define A1M_IRQ_POW_SAV_JD1     0x4000
#define A1M_IRQ_MANUAL          0x0100
/* IRQ_1 */
#define IRQ1_EN_IRQ_JD1         0x0040      /* V1 */
#define IRQ1_EN_CB_JD           0x0008      /* V2 */
/* GPIO_1 / GPIO_2 / GPIO_3 */
#define GPIO1_GP1_PIN_IRQ       0x8000      /* also V1 GPIO1_TYPE_EN */
#define GPIO2_GP4_PIN_CONF_OUT  0x0020      /* V2 */
#define GPIO2_V1_GP1_OUT_SEL    0x0004      /* V1 GP1_PIN_CONF_OUTPUT | SEL_GPIO1_EN */
#define GPIO3_GP8_PIN_CONF_OUT  0x2000      /* V2 */
/* IL_CMD_2 / IL_CMD_5 */
#define IL2_PWR_MIC_DET         0x0001
#define IL5_4BTN_CLK_DEB_MASK   0x000c
#define IL5_4BTN_CLK_DEB_65MS   0x000c
/* EM_JACK_TYPE_1 (V1) */
#define EMJ1_CBJ_DET_EN         0x8000
#define EMJ1_EXT_JD_EN          0x0800
#define EMJ1_POL_EXT_JD_EN      0x0400
#define EMJ1_EM_JD_NOR          0x0080
/* DIG_MISC */
#define DIGM_GATE_CTRL_EN       0x0001
/* HP_LOGIC_2 */
#define HPL2_SIG_SRC1_MASK      0x0003
#define HPL2_SIG_SRC1_REG       0x0002
#define HPL2_SIG_SRC1_SILENCE   0x0003
/* RECMIX (0x10) */
#define RECMIX_V2_CBJ_DET_EN    0x8000
#define RECMIX_V2_DET_TYPE_QFN  0x1000
#define RECMIX_V2_VREF_BIAS_REG 0x0040
#define RECMIX_V1_BST1_OFF      0x0001
/* RECMIX1L (0x3c, V2): 1 = mute */
#define RM1L_BST1_CBJ           0x0080
#define RM1L_BST2               0x0010
/* HP_AMP_2 (V2) */
#define HPA2_EN_DAC_HPO         0x4000
/* CBJ_1 / CBJ_2 (V1) */
#define CBJ1_INBUF_CBJ_BST1_ON  0x0800
#define CBJ1_CBJ_SENSE_BST1_L   0x0400
#define CBJ2_GAIN_BST1_MASK     0x000f
/* AEC_BST (V2): GAIN_CBJ [11:8]; boost {0, +20, +24, +30, ...} dB */
#define AEC_GAIN_CBJ_MASK       0x0f00
#define BST_24DB                2
/* DUMMY_1 (V1) */
#define DUM1_EMB_CLK_EN         0x0200
#define DUM1_HPA_CPL_BIAS_MASK  0x01c0
#define DUM1_HPA_CPL_BIAS_1     0x0040
#define DUM1_HPA_CPR_BIAS_MASK  0x0038
#define DUM1_HPA_CPR_BIAS_1     0x0008
/* CHOP_DAC_L / CHOP_ADC: CKGEN_DAC1 / CKGEN_ADCC */
#define CHOP_CKGEN              0x1000
/* STO1_ADC_MIXER: [15] M_L1, [14] M_L2, [13] L1 src (V2 PGA), [12] L2 src (V2 PGA, inverted),
 * [11:10] L mux (0 = ADC L), [7] M_R1, [6] M_R2, [5] R1 src, [4] R2 src (inv), [3:2] R mux */
#define STO1_ADC_L1_SRC         0x2000
#define STO1_ADC_R1_SRC         0x0020
#define STO1_ADC_MIX_V2         0x5050      /* ADC1 on, ADC2 off, L / R mux = ADC L */
#define STO1_ADC_MIX_V1         0x40c0      /* MIXL ADC1 on (V1 has no R path) */
/* STO_DAC_MIXER (V2): DAC L1 -> STO L, DAC R1 -> STO R (1 = mute) */
#define STODAC_M_L1_STO_L       0x8000
#define STODAC_M_R1_STO_R       0x0020
/* AD_DA_MIXER: "DAC L/R Switch" of ADDA MIXL/R */
#define ADDA_M_DAC1_L           0x4000
#define ADDA_M_DAC1_R           0x0040
/* STO_DRE_9 / 10 (V1): HP gain, inverted, -24 dB + 1.5 dB steps (max 23): 7 = 0 dB */
#define DRE_GAIN_HP_MASK        0x001f
#define DRE_GAIN_HP_0DB         7

/* ---- private state ---- */
#define MIC_HS          0
#define MIC_IN2         1
#define MIC_HS_FORCED   2

#define CLK_BCLK_PLL    0
#define CLK_MCLK_PLL    1
#define CLK_MCLK        2

typedef struct _RT63_PRIV {
    BOOLEAN V2;                     /* CODEC_VER_1 */
    UCHAR   Mic;
    UCHAR   Clk;
    BOOLEAN Asrc;                   /* DAC / ADC filters track I2S1 */
    BOOLEAN I2sAsrc;                /* rt5663_i2s_use_asrc: ASRC and sysclk > 384 fs */
    BOOLEAN Hs;                     /* type detection found a headset */
    UCHAR   JackType;
    ULONG   Sysclk;
    ULONG   PllIn;
    USHORT  PllN, PllM, PllK;
    BOOLEAN PllMBp;
} RT63_PRIV;

typedef char RT63_PRIV_FITS[(sizeof(RT63_PRIV) <= sizeof(((SST_CODEC *)0)->Priv)) ? 1 : -1];

#define PRIV(c) ((RT63_PRIV *)(c)->Priv)

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

/* rl6231_pll_calc: Fout = Fin * (N + 2) / ((M + 2) * (K + 2)), M / K optionally bypassed */
static BOOLEAN RlPllCalc(ULONG fin, ULONG fout, RL_PLL *p)
{
    LONG k, n = 0, m = 0, kt, nt, mt, mink, maxk;
    BOOLEAN mbp = FALSE, kbp = FALSE;
    ULONG red, redt, pllout, in_t, out_t, div, fi, fo, i;

    if (fin > RL_PLL_INP_MAX || fin < RL_PLL_INP_MIN || !fout)
        return FALSE;

    for (i = 0; i < ARRAYSIZE(s_RlPreset); i++) {
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

static BOOLEAN RlPllExact(ULONG fin, ULONG fout, const RL_PLL *p)
{
    ULONGLONG num = (ULONGLONG)fin * (ULONGLONG)(p->N + 2);
    ULONGLONG den = (ULONGLONG)(p->MBp ? 1 : p->M + 2) * (ULONGLONG)(p->KBp ? 1 : p->K + 2);
    return num == (ULONGLONG)fout * den;
}

/* rl6231_get_clk_info: index of the pre-divider giving 256 * fs from sysclk */
static LONG RlClkInfo(ULONG sclk, ULONG rate)
{
    static const ULONG pd[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
    ULONG i;
    for (i = 0; i < ARRAYSIZE(pd); i++)
        if (sclk == (rate << 8) * pd[i])
            return (LONG)i;
    return -1;
}

/* channel length code 16 / 20 / 24 / 32 -> 0..3 (25-bit slots -> 24) */
static ULONG LenCode(ULONG bits)
{
    if (bits <= 16)
        return 0;
    if (bits <= 20)
        return 1;
    if (bits <= 25)
        return 2;
    return 3;
}

/* ------------------------------------------------------------------ */

static BOOLEAN Rt5663Probe(SST_CODEC *c)
{
    ULONG v;

    if (!CRd(c, R63_VENDOR_ID_2, &v))
        return FALSE;
    return v == RT5663_DEVICE_ID_2 || v == RT5663_DEVICE_ID_1;
}

/* rt5663_v2_calibrate */
static void R63CalibrateV2(SST_CODEC *c)
{
    CWr(c, R63_BIAS_CUR_8, 0xa402);
    CWr(c, R63_PWR_DIG_1, 0x0100);
    CWr(c, R63_RECMIX, 0x4040);
    CWr(c, R63_DIG_MISC, 0x0001);
    CWr(c, R63_RC_CLK, 0x0380);
    CWr(c, R63_GLB_CLK, 0x8000);
    CWr(c, R63_ADDA_CLK_1, 0x1000);
    CWr(c, R63_CHOP_DAC_L, 0x3030);
    CWr(c, R63_CALIB_ADC, 0x3c05);
    CWr(c, R63_PWR_ANLG_1, 0xa23e);
    CDelay(40);
    CWr(c, R63_PWR_ANLG_1, 0xf23e);
    CWr(c, R63_HP_CALIB_2, 0x0321);
    CWr(c, R63_HP_CALIB_1, 0xfc00);
    CDelay(500);
}

/* wait while (reg & bit) == busy, 10 ms polls; FALSE on timeout */
static BOOLEAN R63Wait(SST_CODEC *c, ULONG reg, ULONG bit, BOOLEAN busy, ULONG polls)
{
    ULONG n;
    for (n = 0; n <= polls; n++) {
        ULONG v = CGet(c, reg);
        if (v == 0xffffffffUL)
            return FALSE;
        if (((v & bit) != 0) != busy)
            return TRUE;
        CDelay(10);
    }
    return FALSE;
}

/* rt5663_calibrate (CODEC_VER_0) */
static void R63CalibrateV1(SST_CODEC *c)
{
    static const ULONG seq1[] = {
        R63_HP_IMP_SEN_19,      0x0000,
        R63_DEPOP_2,            0x3003,
        R63_DEPOP_1,            0x0038,
        R63_DEPOP_1,            0x003b,
        R63_PWR_DIG_2,          0x8400,
        R63_PWR_DIG_1,          0x8df8,
        R63_PWR_ANLG_2,         0x8003,
        R63_PWR_ANLG_3,         0x018c,
        R63_HP_CHARGE_PUMP_1,   0x1e32,
        R63_DUMMY_2,            0x8089,
        R63_DACREF_LDO,         0x3b0b,
    };
    static const ULONG seq2[] = {
        R63_STO_DAC_MIXER,      0x0000,
        R63_BYPASS_STO_DAC,     0x000c,
        R63_HP_BIAS,            0xafaa,
        R63_CHARGE_PUMP_1,      0x2224,
        0x0002 /* HP_OUT_EN */, 0x8088,
        R63_STO_DRE_9,          0x0017,
        R63_STO_DRE_10,         0x0017,
        R63_STO1_ADC_MIXER,     0x4040,
        R63_CHOP_ADC,           0x3000,
        R63_RECMIX,             0x0005,
        R63_ADDA_RST,           0xc000,
        R63_STO1_HPF_ADJ1,      0x3320,
        R63_HP_CALIB_2,         0x00c9,
        R63_DUMMY_1,            0x004c,
        R63_ANA_BIAS_CUR_1,     0x1111,
        R63_BIAS_CUR_8,         0x4402,
        R63_CHARGE_PUMP_2,      0x3311,
        R63_HP_CALIB_1,         0x0069,
        R63_HP_CALIB_3,         0x06ce,
        R63_HP_CALIB_1_1,       0x6800,
        R63_CHARGE_PUMP_2,      0x1100,
        R63_HP_CALIB_7,         0x0057,
        R63_HP_CALIB_1_1,       0xe800,
    };

    CWr(c, R63_RESET, 0x0000);
    CDelay(20);
    CWr(c, R63_ANA_BIAS_CUR_4, 0x00a1);
    CWr(c, R63_RC_CLK, 0x0380);
    CWr(c, R63_GLB_CLK, 0x8000);
    CWr(c, R63_ADDA_CLK_1, 0x1000);
    CWr(c, R63_VREF_RECMIX, 0x0032);
    CWr(c, R63_HP_IMP_SEN_19, 0x000c);
    CWr(c, R63_DUMMY_1, 0x0324);
    CWr(c, R63_DIG_MISC, 0x8001);
    CWr(c, R63_VREFADJ_OP, 0x0f28);
    CWr(c, R63_PWR_ANLG_1, 0xa23b);
    CDelay(30);
    CWr(c, R63_PWR_ANLG_1, 0xf23b);
    CWr(c, R63_PWR_ANLG_2, 0x8000);
    CWr(c, R63_PWR_ANLG_3, 0x0008);
    CWr(c, R63_PRE_DIV_GATING_1, 0xffff);
    CWr(c, R63_PRE_DIV_GATING_2, 0xffff);
    CWr(c, R63_CBJ_1, 0x8c10);
    CWr(c, R63_IL_CMD_2, 0x00c1);
    CWr(c, R63_EM_JACK_TYPE_1, 0xb880);
    CWr(c, R63_EM_JACK_TYPE_2, 0x4110);
    CWr(c, R63_EM_JACK_TYPE_2, 0x4118);
    R63Wait(c, R63_INT_ST_2, 0x80, FALSE, 200);     /* wait for INT_ST_2 bit 7 */

    CSeq(c, seq1, ARRAYSIZE(seq1) / 2);
    CDelay(40);
    CSeq(c, seq2, ARRAYSIZE(seq2) / 2);
    if (!R63Wait(c, R63_HP_CALIB_1_1, 0x8000, TRUE, 200)) {
        LOGE("RT5663: HP calibration (1) timeout\n");
        return;
    }
    CWr(c, R63_HP_CALIB_1_1, 0x6200);
    CWr(c, R63_HP_CALIB_7, 0x0059);
    CWr(c, R63_HP_CALIB_1_1, 0xe200);
    if (!R63Wait(c, R63_HP_CALIB_1_1, 0x8000, TRUE, 200)) {
        LOGE("RT5663: HP calibration (2) timeout\n");
        return;
    }
    CWr(c, R63_EM_JACK_TYPE_1, 0xb8e0);
    CDelay(10);
    CWr(c, R63_PWR_ANLG_1, 0x003b);
    CDelay(10);
    CWr(c, R63_PWR_DIG_1, 0x0000);
    CDelay(10);
    CWr(c, R63_DEPOP_1, 0x000b);
    CDelay(10);
    CWr(c, R63_DEPOP_1, 0x0008);
    CDelay(10);
    CWr(c, R63_PWR_ANLG_2, 0x0000);
    CDelay(10);
}

/* rt5663_set_bias_level STANDBY (from OFF or PREPARE) */
static void R63BiasStandby(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);

    if (p->V2)
        CUpd(c, R63_DIG_MISC, DIGM_GATE_CTRL_EN, 0);
    CUpd(c, R63_PWR_ANLG_1, PA1_VREF1 | PA1_VREF2 | PA1_FV1 | PA1_FV2 | PA1_MB, PA1_VREF1 | PA1_VREF2 | PA1_MB);
    CDelay(10);
    if (p->V2)
        CUpd(c, R63_SIG_CLK_DET, CLKDET_EN_ANA_AUTO | CLKDET_PWR, 0);
}

/* stream start: (V1: OFF -> STANDBY), STANDBY -> PREPARE; V2 idles in STANDBY (forced micbias) */
static void R63BiasPrepare(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);

    if (p->V2) {
        CUpd(c, R63_DIG_MISC, DIGM_GATE_CTRL_EN, DIGM_GATE_CTRL_EN);
        CUpd(c, R63_SIG_CLK_DET, CLKDET_EN_ANA_AUTO | CLKDET_PWR, CLKDET_EN_ANA_AUTO | CLKDET_PWR);
    } else {
        R63BiasStandby(c);
    }
}

/* PREPARE -> ON (after the widgets) */
static void R63BiasOn(SST_CODEC *c)
{
    CUpd(c, R63_PWR_ANLG_1, PA1_FV1 | PA1_FV2, PA1_FV1 | PA1_FV2);
}

/* stream stop: PREPARE -> STANDBY (-> OFF on V1, headset in: FV1 / FV2 on) */
static void R63BiasIdle(SST_CODEC *c)
{
    R63BiasStandby(c);
    if (!PRIV(c)->V2)
        CUpd(c, R63_PWR_ANLG_1, PA1_FV1 | PA1_FV2, PA1_FV1 | PA1_FV2);
}

/* "Pre Div Power" (V1, I2S supply) */
static void R63PreDiv(SST_CODEC *c, BOOLEAN on)
{
    CWr(c, R63_PRE_DIV_GATING_1, on ? 0xff00 : 0x0000);
    CWr(c, R63_PRE_DIV_GATING_2, on ? 0xfffc : 0x0000);
}

/* I2S, PLL, I2S ASRC: the supplies shared by both directions */
static ULONG R63AsrcShared(RT63_PRIV *p) { return p->I2sAsrc ? ASRC1_I2S1 : 0; }

static void R63PlayUp(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);
    BOOLEAN rec = c->RecOn;
    ULONG d1 = PD1_DAC_L1 | PD1_DAC_R1 | (p->V2 ? PD1_LDO_DACREF : 0) | (rec ? 0 : PD1_I2S1);
    ULONG a1 = (p->Asrc ? ASRC1_DAC_STO1 : 0) | (rec ? 0 : R63AsrcShared(p));

    if (!rec)
        R63BiasPrepare(c);
    if (!p->V2) {
        /* "HP Charge Pump" PRE_PMU, "Pre Div Power" POST_PMU (NOPM supplies) */
        CUpd(c, R63_DEPOP_1, 0x0030, 0x0030);
        CUpd(c, R63_DEPOP_1, 0x0003, 0x0003);
        if (!rec)
            R63PreDiv(c, TRUE);
    }
    CUpd(c, R63_PWR_DIG_1, d1, d1);
    CUpd(c, R63_PWR_DIG_2, PD2_DAC_S1F, PD2_DAC_S1F);
    if (!rec && p->Clk != CLK_MCLK)
        CUpd(c, R63_PWR_ANLG_3, PA3_PLL, PA3_PLL);
    if (a1)
        CUpd(c, R63_ASRC_1, a1, a1);
    if (p->V2)
        CUpd(c, R63_CHOP_DAC_L, CHOP_CKGEN, CHOP_CKGEN);    /* "DAC Clock" */
    /* "HP Amp" POST_PMU (rt5663_hp_event) */
    if (p->V2) {
        /* Linux passes RT5663_SEL_PM_HP_SHIFT (8) as the mask: the update clears bit 3 */
        CUpd(c, R63_HP_CHARGE_PUMP_1, 0x0008, 0);
        CUpd(c, R63_HP_LOGIC_2, HPL2_SIG_SRC1_MASK, HPL2_SIG_SRC1_SILENCE);
        CUpd(c, R63_HP_AMP_2, HPA2_EN_DAC_HPO, HPA2_EN_DAC_HPO); /* "HPO Playback" switch */
    } else {
        CUpd(c, R63_DACREF_LDO, 0x3e0e, 0x3a0a);
        CWr(c, R63_DEPOP_2, 0x3003);
        CUpd(c, R63_HP_CHARGE_PUMP_1, HCP_OVCD_HP_EN, 0);
        CWr(c, R63_HP_CHARGE_PUMP_2, 0x1371);
        CWr(c, R63_HP_BIAS, 0xabba);
        CWr(c, R63_CHARGE_PUMP_1, 0x2224);
        CWr(c, R63_ANA_BIAS_CUR_1, 0x7766);
        CWr(c, R63_HP_BIAS, 0xafaa);
        CWr(c, R63_CHARGE_PUMP_2, 0x7777);
        CUpd(c, R63_STO_DRE_1, 0x8000, 0x8000);
        CUpd(c, R63_DEPOP_1, 0x3000, 0x3000);
        CUpd(c, R63_DIG_VOL_ZCD, 0x00c0, 0x0080);
    }
    if (!rec)
        R63BiasOn(c);
}

static void R63PlayDown(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);
    BOOLEAN rec = c->RecOn;
    ULONG d1 = PD1_DAC_L1 | PD1_DAC_R1 | (p->V2 ? PD1_LDO_DACREF : 0) | (rec ? 0 : PD1_I2S1);
    ULONG a1 = (p->Asrc ? ASRC1_DAC_STO1 : 0) | (rec ? 0 : R63AsrcShared(p));

    /* "HP Amp" PRE_PMD */
    if (p->V2) {
        CUpd(c, R63_HP_AMP_2, HPA2_EN_DAC_HPO, 0);
        CUpd(c, R63_HP_LOGIC_2, HPL2_SIG_SRC1_MASK, HPL2_SIG_SRC1_REG);
    } else {
        CUpd(c, R63_DEPOP_1, 0x3000, 0);
        CUpd(c, R63_HP_CHARGE_PUMP_1, HCP_OVCD_HP_EN, HCP_OVCD_HP_EN);
        CUpd(c, R63_DACREF_LDO, 0x3e0e, 0);
        CUpd(c, R63_DIG_VOL_ZCD, 0x00c0, 0);
        /* "HP Charge Pump" POST_PMD, "Pre Div Power" PRE_PMD */
        CUpd(c, R63_DEPOP_1, 0x0003, 0);
        CUpd(c, R63_DEPOP_1, 0x0030, 0);
        if (!rec)
            R63PreDiv(c, FALSE);
    }
    CUpd(c, R63_PWR_DIG_1, d1, 0);
    CUpd(c, R63_PWR_DIG_2, PD2_DAC_S1F, 0);
    if (!rec && p->Clk != CLK_MCLK)
        CUpd(c, R63_PWR_ANLG_3, PA3_PLL, 0);
    if (a1)
        CUpd(c, R63_ASRC_1, a1, 0);
    if (p->V2)
        CUpd(c, R63_CHOP_DAC_L, CHOP_CKGEN, 0);
    if (!rec)
        R63BiasIdle(c);
}

static void R63RecUp(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);
    BOOLEAN play = c->PlayOn;
    ULONG d1 = PD1_ADC_L1 | (play ? 0 : PD1_I2S1);
    ULONG a1 = (p->Asrc ? ASRC1_ADC_STO1 : 0) | (play ? 0 : R63AsrcShared(p));

    if (!play)
        R63BiasPrepare(c);
    if (!p->V2 && !play)
        R63PreDiv(c, TRUE);
    if (p->V2 && p->Mic == MIC_IN2)     /* "BST2 Power" POST_PMU */
        CUpd(c, R63_PWR_ANLG_2, PA2_BST2 | PA2_BST2_OP, PA2_BST2 | PA2_BST2_OP);
    CUpd(c, R63_PWR_DIG_1, d1, d1);
    CUpd(c, R63_PWR_DIG_2, PD2_ADC_S1F, PD2_ADC_S1F);
    CUpd(c, R63_PWR_ANLG_2, PA2_RECMIX1, PA2_RECMIX1);
    if (!play && p->Clk != CLK_MCLK)
        CUpd(c, R63_PWR_ANLG_3, PA3_PLL, PA3_PLL);
    if (a1)
        CUpd(c, R63_ASRC_1, a1, a1);
    CUpd(c, R63_CHOP_ADC, CHOP_CKGEN, CHOP_CKGEN);          /* "ADC Clock" */
    if (p->V2)                                              /* PGAs "STO1 ADC L1" / "R1" */
        CUpd(c, R63_STO1_ADC_MIXER, STO1_ADC_L1_SRC | STO1_ADC_R1_SRC, STO1_ADC_L1_SRC | STO1_ADC_R1_SRC);
    if (!play)
        R63BiasOn(c);
}

static void R63RecDown(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);
    BOOLEAN play = c->PlayOn;
    ULONG d1 = PD1_ADC_L1 | (play ? 0 : PD1_I2S1);
    ULONG a1 = (p->Asrc ? ASRC1_ADC_STO1 : 0) | (play ? 0 : R63AsrcShared(p));

    if (p->V2)
        CUpd(c, R63_STO1_ADC_MIXER, STO1_ADC_L1_SRC | STO1_ADC_R1_SRC, 0);
    if (p->V2 && p->Mic == MIC_IN2)     /* "BST2 Power" PRE_PMD */
        CUpd(c, R63_PWR_ANLG_2, PA2_BST2 | PA2_BST2_OP, 0);
    if (!p->V2 && !play)
        R63PreDiv(c, FALSE);
    CUpd(c, R63_PWR_DIG_1, d1, 0);
    CUpd(c, R63_PWR_DIG_2, PD2_ADC_S1F, 0);
    CUpd(c, R63_PWR_ANLG_2, PA2_RECMIX1, 0);
    if (!play && p->Clk != CLK_MCLK)
        CUpd(c, R63_PWR_ANLG_3, PA3_PLL, 0);
    if (a1)
        CUpd(c, R63_ASRC_1, a1, 0);
    CUpd(c, R63_CHOP_ADC, CHOP_CKGEN, 0);
    if (!play)
        R63BiasIdle(c);
}

/*
 * Headset state.  V2: rt5663_v2_jack_detect(1): CBJ type setup, forced MICBIAS1/2 (+LDO2),
 * "Mic Det Power", "CBJ Power" (the DAPM sync takes the bias OFF -> STANDBY first), 1 MHz
 * clock, type FSM poll.  V1: rt5663_jack_detect(1).  A headphone / no-plug result keeps the
 * headset powering (forced headset mode) instead of the Linux headphone branch.
 */
static void R63JackIn(SST_CODEC *c)
{
    static const ULONG sleep_ms[5] = { 300, 150, 100, 50, 30 };
    RT63_PRIV *p = PRIV(c);
    BOOLEAN detect = p->Mic != MIC_HS_FORCED;
    ULONG v = 0, i;

    if (p->V2) {
        CWr(c, R63_CBJ_TYPE_2, 0x8040);
        CWr(c, R63_CBJ_TYPE_3, 0x1484);
        R63BiasStandby(c);
        CUpd(c, R63_PWR_ANLG_3, PA3_LDO2 | PA3_CBJ, PA3_LDO2 | PA3_CBJ);
        CUpd(c, R63_PWR_VOL, PVOL_V2_MIC_DET, PVOL_V2_MIC_DET);
        CUpd(c, R63_PWR_ANLG_2, PA2_MB1 | PA2_MB2, PA2_MB1 | PA2_MB2);
        CUpd(c, R63_RC_CLK, RC_DIG_1M_CLK_EN, RC_DIG_1M_CLK_EN);
        CUpd(c, R63_RECMIX, 0x0008, 0x0008);
        for (i = 0; detect && i < 5; i++) {
            CDelay(sleep_ms[i]);
            v = CGet(c, R63_CBJ_TYPE_2) & 0x0003;
            if (v)
                break;
        }
    } else {
        CUpd(c, R63_DIG_MISC, DIGM_GATE_CTRL_EN, DIGM_GATE_CTRL_EN);
        CUpd(c, R63_HP_CHARGE_PUMP_1, HCP_SI_HP | HCP_OSW_L | HCP_OSW_R, HCP_SI_HP);
        CUpd(c, R63_DUMMY_1, DUM1_EMB_CLK_EN | DUM1_HPA_CPL_BIAS_MASK | DUM1_HPA_CPR_BIAS_MASK,
             DUM1_EMB_CLK_EN | DUM1_HPA_CPL_BIAS_1 | DUM1_HPA_CPR_BIAS_1);
        CUpd(c, R63_CBJ_1, CBJ1_INBUF_CBJ_BST1_ON | CBJ1_CBJ_SENSE_BST1_L,
             CBJ1_INBUF_CBJ_BST1_ON | CBJ1_CBJ_SENSE_BST1_L);
        CUpd(c, R63_IL_CMD_2, IL2_PWR_MIC_DET, IL2_PWR_MIC_DET);
        CUpd(c, R63_PWR_ANLG_2, PA2_BST1, PA2_BST1);             /* BST1 power on for JD */
        CUpd(c, R63_EM_JACK_TYPE_1, EMJ1_CBJ_DET_EN | EMJ1_EXT_JD_EN | EMJ1_POL_EXT_JD_EN,
             EMJ1_CBJ_DET_EN | EMJ1_EXT_JD_EN | EMJ1_POL_EXT_JD_EN);
        CUpd(c, R63_PWR_ANLG_1, PA1_MB | PA1_LDO1_DVO_MASK | PA1_AMP_HP_MASK, PA1_MB | PA1_AMP_HP_3X);
        CUpd(c, R63_PWR_ANLG_1, PA1_VREF1 | PA1_VREF2 | PA1_FV1 | PA1_FV2, PA1_VREF1 | PA1_VREF2);
        CDelay(20);
        CUpd(c, R63_PWR_ANLG_1, PA1_FV1 | PA1_FV2, PA1_FV1 | PA1_FV2);
        CUpd(c, R63_AUTO_1MRC_CLK, A1M_IRQ_POW_SAV, A1M_IRQ_POW_SAV);
        CUpd(c, R63_IRQ_1, IRQ1_EN_IRQ_JD1, IRQ1_EN_IRQ_JD1);
        if (detect) {
            CUpd(c, R63_EM_JACK_TYPE_1, EMJ1_EM_JD_NOR, 0);      /* EM_JD reset */
            CUpd(c, R63_EM_JACK_TYPE_1, EMJ1_EM_JD_NOR, EMJ1_EM_JD_NOR);
            /* Linux polls INT_ST_2 bit 7 up to 2 s; 1 s here (no plug: no result anyway) */
            R63Wait(c, R63_INT_ST_2, 0x80, FALSE, 100);
            v = CGet(c, R63_EM_JACK_TYPE_2) & 0x0003;
        }
        CUpd(c, R63_HP_CHARGE_PUMP_1, HCP_OSW_L | HCP_OSW_R, HCP_OSW_L | HCP_OSW_R);
    }
    p->JackType = (UCHAR)v;
    p->Hs = (v == 1 || v == 2);
}

/* rt5663(_v2)_jack_detect(0) */
static void R63JackOut(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);

    if (p->V2) {
        CUpd(c, R63_RECMIX, 0x0008, 0);
        CUpd(c, R63_PWR_ANLG_2, PA2_MB1 | PA2_MB2, 0);
        CUpd(c, R63_PWR_ANLG_3, PA3_LDO2 | PA3_CBJ, 0);
        CUpd(c, R63_PWR_VOL, PVOL_V2_MIC_DET, 0);
    }
    /* bias OFF without a headset */
    CUpd(c, R63_PWR_ANLG_1, PA1_VREF1 | PA1_VREF2 | PA1_FV1 | PA1_FV2 | PA1_MB, 0);
}

/* sysclk = 512 fs from PLL1 (BCLK1 preferred) or MCLK; PD1, ASRC */
static BOOLEAN R63Clock(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);
    ULONG fs = c->Rate ? c->Rate : 48000, target = 512 * fs, glb;
    LONG pd;
    RL_PLL pll;
    BOOLEAN have = FALSE;

    p->Sysclk = target;
    p->Asrc = FALSE;
    if (c->Bclk >= RL_PLL_INP_MIN && c->Bclk <= 40000000 && RlPllCalc(c->Bclk, target, &pll) &&
        RlPllExact(c->Bclk, target, &pll)) {
        p->Clk = CLK_BCLK_PLL;
        p->PllIn = c->Bclk;
        have = TRUE;
    } else if (c->Mclk == target) {
        p->Clk = CLK_MCLK;                  /* avs_rt5663_hw_params: MCLK 24.576 MHz + ASRC */
        p->Asrc = TRUE;
    } else if (c->Mclk && c->Mclk <= 40000000 && RlPllCalc(c->Mclk, target, &pll)) {
        p->Clk = CLK_MCLK_PLL;
        p->PllIn = c->Mclk;
        p->Asrc = TRUE;
        have = TRUE;
    } else if (c->Bclk >= RL_PLL_INP_MIN && c->Bclk <= 40000000 && RlPllCalc(c->Bclk, target, &pll)) {
        p->Clk = CLK_BCLK_PLL;
        p->PllIn = c->Bclk;
        p->Asrc = TRUE;
        have = TRUE;
    } else {
        LOGE("RT5663: no clock for %u Hz sysclk (BCLK %u, MCLK %u)\n", target, c->Bclk, c->Mclk);
        return FALSE;
    }
    p->I2sAsrc = p->Asrc && target > 384 * fs;

    if (have) {
        p->PllN = (USHORT)pll.N;
        p->PllM = (USHORT)pll.M;
        p->PllK = (USHORT)pll.K;
        p->PllMBp = pll.MBp;
        /* rt5663_set_dai_pll: N [15:7], K [4:0]; M [15:12], M bypass [11] */
        CWr(c, R63_PLL_1, ((ULONG)pll.N << PLL1_N_SFT) | (ULONG)pll.K);
        CWr(c, R63_PLL_2, ((ULONG)(pll.MBp ? 0 : pll.M) << PLL2_M_SFT) | (pll.MBp ? PLL2_M_BP : 0));
        if (p->V2)
            glb = GLB_SCLK_SRC_PLL1 | (p->Clk == CLK_BCLK_PLL ? GLB_V2_PLL1_SRC_BCLK1 : 0);
        else
            glb = GLB_SCLK_SRC_PLL1 | (p->Clk == CLK_BCLK_PLL ? GLB_V1_PLL1_SRC_BCLK1 : 0);
    } else {
        glb = 0;
    }
    CUpd(c, R63_GLB_CLK, GLB_SCLK_SRC_MASK | (p->V2 ? GLB_V2_PLL1_SRC_MASK : GLB_V1_PLL1_SRC_MASK), glb);
    pd = RlClkInfo(target, fs);
    CUpd(c, R63_ADDA_CLK_1, ADDA_I2S_PD1_MASK, (ULONG)(pd < 0 ? 1 : pd) << ADDA_I2S_PD1_SFT);
    /* rt5663_sel_asrc_clk_src(DA | AD, I2S1_ASRC) or system clock */
    CUpd(c, R63_ASRC_2, ASRC2_DA_STO1_TRACK_MASK, p->Asrc ? ASRC2_DA_STO1_TRACK_I2S1 : 0);
    if (p->V2)
        CUpd(c, R63_ASRC_3, ASRC3_V2_AD_TRACK_MASK, p->Asrc ? ASRC3_V2_AD_TRACK_I2S1 : 0);
    else
        CUpd(c, R63_ASRC_2, ASRC2_AD_STO1_TRACK_MASK, p->Asrc ? ASRC2_AD_STO1_TRACK_I2S1 : 0);
    return TRUE;
}

/* I2S slave, normal polarity (rt5663_set_dai_fmt), word length (hw_params), slot length and
 * TDM (rt5663_set_tdm_slot / set_bclk_ratio) from c->Slots / c->Slot */
static void R63Format(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);
    ULONG len = LenCode(c->Slot), dl = c->Valid <= 16 ? 0 : c->Valid <= 20 ? 1 : 2, ch = 0;
    BOOLEAN tdm = c->Slots > 2;

    if (c->Slot != 16 && c->Slot != 20 && c->Slot != 24 && c->Slot != 32)
        LOGI("RT5663: %u-bit slots, slot length set to %u\n", c->Slot,
             len == 0 ? 16 : len == 1 ? 20 : len == 2 ? 24 : 32);
    if (tdm)
        ch = c->Slots >= 8 ? 3 : c->Slots >= 6 ? 2 : 1;
    CUpd(c, R63_I2S1_SDP, SDP_MS_SLAVE | SDP_BP_INV | SDP_DL_MASK | SDP_DF_MASK, SDP_MS_SLAVE | (dl << SDP_DL_SFT));
    CUpd(c, p->V2 ? R63_TDM_2 : R63_TDM_1,
         TDM_MODE_TDM | TDM_IN_CH_MASK | TDM_OUT_CH_MASK | TDM_IN_LEN_MASK | TDM_OUT_LEN_MASK,
         (tdm ? TDM_MODE_TDM : 0) | (ch << TDM_IN_CH_SFT) | (ch << TDM_OUT_CH_SFT) |
         (len << TDM_IN_LEN_SFT) | (len << TDM_OUT_LEN_SFT));
    CUpd(c, p->V2 ? R63_TDM_9 : R63_TDM_5, TDM_LENGTN_MASK, len);
}

static BOOLEAN Rt5663Init(SST_CODEC *c)
{
    RT63_PRIV *p = PRIV(c);
    ULONG e0 = c->Errors, id = 0, vid = 0, n;

    memset(p, 0, sizeof(*p));
    c->Caps = 0;
    if (!CRd(c, R63_VENDOR_ID_2, &id) || (id != RT5663_DEVICE_ID_2 && id != RT5663_DEVICE_ID_1)) {
        CDelay(100);                        /* rt5663_i2c_probe retries once */
        if (!CRd(c, R63_VENDOR_ID_2, &id) || (id != RT5663_DEVICE_ID_2 && id != RT5663_DEVICE_ID_1)) {
            LOGE("RT5663: device id %x\n", id);
            return FALSE;
        }
        e0 = c->Errors;
    }
    p->V2 = id == RT5663_DEVICE_ID_2;
    CRd(c, R63_VENDOR_ID, &vid);
    p->Mic = (UCHAR)(c->Mic <= MIC_HS_FORCED ? c->Mic : MIC_HS);
    if (p->Mic == MIC_IN2 && !p->V2) {
        LOGE("RT5663: no IN2 on this version, using the headset mic\n");
        p->Mic = MIC_HS;
    }

    /* reset and calibrate, reset again (calibration results survive) */
    CWr(c, R63_RESET, 0);
    if (p->V2)
        R63CalibrateV2(c);
    else
        R63CalibrateV1(c);
    CWr(c, R63_RESET, 0);
    if (!p->V2) {
        static const ULONG patch[] = {      /* rt5663_patch_list */
            R63_STO_DAC_MIXER, 0x8020,
            R63_ASRC_4,        0x0028,
            R63_DIG_VOL_ZCD,   0xa020,
            R63_VREFADJ_OP,    0x0f28,
            R63_DUMMY_2,       0x8089,
        };
        CSeq(c, patch, ARRAYSIZE(patch) / 2);
    }
    /* rt5663_i2c_probe: GPIO1 as IRQ, inline command debounce */
    CUpd(c, R63_GPIO_1, GPIO1_GP1_PIN_IRQ, GPIO1_GP1_PIN_IRQ);
    CUpd(c, R63_IL_CMD_5, IL5_4BTN_CLK_DEB_MASK, IL5_4BTN_CLK_DEB_65MS);
    if (p->V2) {
        CWr(c, R63_BIAS_CUR_8, 0xa402);
        CUpd(c, R63_AUTO_1MRC_CLK, A1M_IRQ_POW_SAV | A1M_IRQ_POW_SAV_JD1, A1M_IRQ_POW_SAV | A1M_IRQ_POW_SAV_JD1);
        CUpd(c, R63_PWR_ANLG_2, PA2_JD1, PA2_JD1);
        CUpd(c, R63_IRQ_1, IRQ1_EN_CB_JD, IRQ1_EN_CB_JD);
        CUpd(c, R63_HP_LOGIC_2, HPL2_SIG_SRC1_MASK, HPL2_SIG_SRC1_REG);
        CUpd(c, R63_RECMIX, RECMIX_V2_VREF_BIAS_REG | RECMIX_V2_CBJ_DET_EN | RECMIX_V2_DET_TYPE_QFN,
             RECMIX_V2_VREF_BIAS_REG | RECMIX_V2_CBJ_DET_EN | RECMIX_V2_DET_TYPE_QFN);
        CUpd(c, R63_GPIO_2, GPIO2_GP4_PIN_CONF_OUT, 0);        /* GPIO4 / GPIO8 inputs for the CBJ */
        CUpd(c, R63_GPIO_3, GPIO3_GP8_PIN_CONF_OUT, 0);
        CUpd(c, R63_PWR_ANLG_1, PA1_LDO1_DVO_MASK | PA1_AMP_HP_MASK, PA1_AMP_HP_3X);
    } else {
        CUpd(c, R63_DIG_MISC, DIGM_GATE_CTRL_EN, DIGM_GATE_CTRL_EN);
        CUpd(c, R63_AUTO_1MRC_CLK, A1M_IRQ_MANUAL, A1M_IRQ_MANUAL);
        CUpd(c, R63_IRQ_1, IRQ1_EN_IRQ_JD1, IRQ1_EN_IRQ_JD1);
        CUpd(c, R63_GPIO_1, GPIO1_GP1_PIN_IRQ, GPIO1_GP1_PIN_IRQ);  /* GPIO1_TYPE_EN */
        CWr(c, R63_VREF_RECMIX, 0x0032);
        CUpd(c, R63_GPIO_2, GPIO2_V1_GP1_OUT_SEL, GPIO2_V1_GP1_OUT_SEL);
        CUpd(c, R63_RECMIX, RECMIX_V1_BST1_OFF, 0);             /* RECMIX1 BST1 on */
        CUpd(c, R63_TDM_2, TDM2_SWAP_ADCDAT1_MASK, TDM2_SWAP_ADCDAT1_LL);
    }

    R63Format(c);
    if (!R63Clock(c))
        return FALSE;

    /* playback: IF1 -> ADDA MIXL/R (DAC switches on) -> DAC L1/R1 -> (V2: STO1 DAC MIX) */
    CWr(c, R63_STO1_DAC_DIG_VOL, 0xafaf);
    CUpd(c, R63_AD_DA_MIXER, ADDA_M_DAC1_L | ADDA_M_DAC1_R, 0);
    if (p->V2) {
        CUpd(c, R63_STO_DAC_MIXER, STODAC_M_L1_STO_L | STODAC_M_R1_STO_R, 0);
        CUpd(c, R63_HP_AMP_2, HPA2_EN_DAC_HPO, 0);
    } else {
        CUpd(c, R63_BYPASS_STO_DAC, 0x000c, 0);                 /* DAC L/R Mux = DAC L1/R1 */
        CUpd(c, R63_STO_DRE_9, DRE_GAIN_HP_MASK, DRE_GAIN_HP_0DB);
        CUpd(c, R63_STO_DRE_10, DRE_GAIN_HP_MASK, DRE_GAIN_HP_0DB);
    }
    /* capture */
    if (p->V2) {
        CWr(c, R63_STO1_ADC_MIXER, STO1_ADC_MIX_V2);
        CUpd(c, R63_RECMIX1L, RM1L_BST1_CBJ | RM1L_BST2, p->Mic == MIC_IN2 ? RM1L_BST1_CBJ : RM1L_BST2);
        CUpd(c, R63_AEC_BST, AEC_GAIN_CBJ_MASK, BST_24DB << 8);
    } else {
        CWr(c, R63_STO1_ADC_MIXER, STO1_ADC_MIX_V1);
        CUpd(c, R63_CBJ_2, CBJ2_GAIN_BST1_MASK, BST_24DB);
    }
    CWr(c, R63_STO1_BOOST, 0x0000);
    CWr(c, R63_STO1_ADC_DIG_VOL, 0x2f2f);                      /* 0 dB, unmuted */

    R63JackIn(c);

    n = c->Errors - e0;
    if (n) {
        LOGE("RT5663: %u I2C errors in init\n", n);
        return FALSE;
    }
    c->Caps = CC_PLAY | CC_REC;
    LOGI("RT5663 %s (vendor id %x): sysclk %u from %s %u, PLL1 N=%u M=%d K=%u%s, mic %u, CBJ type %u%s\n",
         p->V2 ? "V2 (6451)" : "V1 (6406)", vid, p->Sysclk, p->Clk == CLK_BCLK_PLL ? "BCLK1" : "MCLK",
         p->Clk == CLK_MCLK ? c->Mclk : p->PllIn, p->PllN, p->PllMBp ? -1 : (int)p->PllM, p->PllK,
         p->Asrc ? ", ASRC" : "", p->Mic, p->JackType,
         p->Mic == MIC_IN2 ? "" : p->Hs ? " (headset)" : ", headset mode forced");
    return TRUE;
}

static void Rt5663Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (on == (rec ? c->RecOn : c->PlayOn))
        return;
    if (on) {
        if (rec)
            R63RecUp(c);
        else
            R63PlayUp(c);
    } else {
        if (rec)
            R63RecDown(c);
        else
            R63PlayDown(c);
    }
}

/* STO1 DAC digital volume: 0xaf = 0 dB, 0.375 dB steps; mute in the ADDA MIXL/R DAC switches */
static void Rt5663Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    CWr(c, R63_STO1_DAC_DIG_VOL, (CVolSteps(left, 0xaf) << 8) | CVolSteps(right, 0xaf));
    CUpd(c, R63_AD_DA_MIXER, ADDA_M_DAC1_L | ADDA_M_DAC1_R, mute ? ADDA_M_DAC1_L | ADDA_M_DAC1_R : 0);
}

static void Rt5663Shutdown(SST_CODEC *c)
{
    if (c->PlayOn)
        R63PlayDown(c);
    if (c->RecOn)
        R63RecDown(c);
    R63JackOut(c);
    CWr(c, R63_RESET, 0);       /* rt5663_i2c_shutdown */
}

const CODEC_DRV g_DrvRt5663 = { "RT5663", "10EC5663\0", CODEC_HEADSET, 2, 2,
                                Rt5663Probe, Rt5663Init, Rt5663Stream, Rt5663Volume, Rt5663Shutdown };
