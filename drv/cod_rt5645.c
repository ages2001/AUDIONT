#include "codec.h"

/*
 * Realtek RT5645 / RT5650 (and the ALC3270 package of the RT5645) I2S audio codec,
 * Cherry Trail / Braswell (cht_bsw_rt5645) and Broadwell (bdw-rt5650).
 * Reference: Linux sound/soc/codecs/rt5645.c/.h, sound/soc/codecs/rl6231.c,
 * sound/soc/intel/boards/cht_bsw_rt5645.c, bdw-rt5650.c.
 *
 * I2C: 8-bit register address, 16-bit big-endian value; private registers PR_xx through
 * RT5645_PRIV_INDEX (0x6a) / RT5645_PRIV_DATA (0x6c).  VENDOR_ID2 (0xff) = 0x6308 (RT5645,
 * ALC3270) or 0x6419 (RT5650).
 *
 * Clocking (slave): PLL1 from BCLK1 (AIF1) -> sysclk = 512 * fs, as cht_aif1_hw_params does
 * from MCLK (RT5645_PLL1_S_MCLK, 19.2 MHz -> rate * 512); I2S pre-divider 2 (probe default).
 * AIF1 only: the DAC1 source is a mux (IF1 or IF2), so a board wiring SSP0 to AIF2 (some
 * Bay Trail CR devices, CHT_RT5645_SSP0_AIF2) is not supported.
 * TDM: with c->Slots > 2 the TDM slot count / width fields are set (rt5645_set_tdm_slot) and
 * slots 0/1 carry left/right; framing stays I2S as configured by the SSP.
 *
 * Outputs (no jack detection, all on during playback):
 *   HPOL/HPOR:   DAC1 -> HPOVOL MIXL/R -> HPOVOL L/R -> HPO MIX (HPVOL) -> HP amp
 *   SPOL/SPOR:   DAC1 -> SPK MIXL/R -> SPKVOL L/R -> SPOL/SPOR MIX -> class-D "SPK amp"
 *   LOUTL/LOUTR: DAC1 -> LOUT MIX -> LOUT amp
 * Speakers use the cht/bdw stereo map (SPOL = left, SPOR = right).
 *
 * Capture, c->Mic:
 *   0  headset mic IN1P (combo jack, CBJ boost), MICBIAS1 - cht/bdw "Headset Mic".  The
 *      ALC3270 (10EC3270) has no headset mic pin (pdata.no_headset_mic), there 0 means 1.
 *   1  internal analog mic IN2P single-ended, MICBIAS1+2 (cht "Int Analog Mic", "cfg-mic:in2")
 *   2  internal analog mic IN2P/IN2N differential (pdata.in2_diff: GPD Pocket, Miix 310)
 *   3  DMIC1, data on IN2N, clock on GPIO2 (cht "Int Mic" DMIC L1/R1, Asus T100HA/T101HA)
 *   4  DMIC2, data on IN2P, clock on GPIO2 (Braswell pdata, cht_rt5650 map, bdw "DMIC Pair2")
 * Analog mics: boost +24 dB, RECMIXL and RECMIXR both take the boost stage (mono mic on both
 * ADC channels).  DMIC pairs are captured as stereo L/R.
 *
 * GPIOs: Linux only reads an "hp-detect" input (cht_rt5645_ef20_gpios, GpioIo 1) and an
 * optional DT "cbj-sleeve" line; no output line is driven here.
 */

/* ---- registers (rt5645.h) ---- */
#define RT5645_RESET            0x00
#define RT5645_SPK_VOL          0x01
#define RT5645_HP_VOL           0x02
#define RT5645_LOUT1            0x03
#define RT5645_IN1_CTRL1        0x0a
#define RT5645_IN1_CTRL2        0x0b
#define RT5645_IN2_CTRL         0x0d
#define RT5645_DAC1_DIG_VOL     0x19
#define RT5645_STO1_ADC_DIG_VOL 0x1c
#define RT5645_STO1_ADC_MIXER   0x27
#define RT5645_AD_DA_MIXER      0x29
#define RT5645_STO_DAC_MIXER    0x2a
#define RT5645_REC_L2_MIXER     0x3c
#define RT5645_REC_R2_MIXER     0x3e
#define RT5645_HPOMIXL_CTRL     0x40
#define RT5645_HPOMIXR_CTRL     0x42
#define RT5645_HPO_MIXER        0x45
#define RT5645_SPK_L_MIXER      0x46
#define RT5645_SPK_R_MIXER      0x47
#define RT5645_SPO_MIXER        0x48
#define RT5645_LOUT_MIXER       0x53
#define RT5645_PWR_DIG1         0x61
#define RT5645_PWR_DIG2         0x62
#define RT5645_PWR_ANLG1        0x63
#define RT5645_PWR_ANLG2        0x64
#define RT5645_PWR_MIXER        0x65
#define RT5645_PWR_VOL          0x66
#define RT5645_PRIV_INDEX       0x6a
#define RT5645_PRIV_DATA        0x6c
#define RT5645_I2S1_SDP         0x70
#define RT5645_ADDA_CLK1        0x73
#define RT5645_DMIC_CTRL1       0x75
#define RT5645_TDM_CTRL_1       0x77
#define RT5645_GLB_CLK          0x80
#define RT5645_PLL_CTRL1        0x81
#define RT5645_PLL_CTRL2        0x82
#define RT5645_ASRC_1           0x83
#define RT5645_ASRC_4           0x8a
#define RT5645_DEPOP_M1         0x8e
#define RT5645_DEPOP_M2         0x8f
#define RT5645_DEPOP_M3         0x90
#define RT5645_CHARGE_PUMP      0x91
#define RT5645_CLSD_OUT_CTRL    0xa0
#define RT5645_CLSD_OUT_CTRL1   0xa1
#define RT5645_EQ_CTRL2         0xb1
#define RT5645_GPIO_CTRL1       0xc0
#define RT5645_BASS_BACK        0xcf
#define RT5645_IL_CMD3          0xdd
#define RT5645_JD_CTRL3         0xf8
#define RT5645_GEN_CTRL1        0xfa
#define RT5645_GEN_CTRL2        0xfb
#define RT5645_GEN_CTRL3        0xfc
#define RT5645_VENDOR_ID        0xfd
#define RT5645_VENDOR_ID2       0xff
/* private registers */
#define RT5645_PR_MAMP_INT_REG2 0x37
#define RT5645_PR_HP_DCC_INT1   0x77

#define RT5645_DEVICE_ID        0x6308
#define RT5650_DEVICE_ID        0x6419

/* xxx_VOL */
#define RT5645_L_MUTE           (1 << 15)
#define RT5645_VOL_L_MUTE       (1 << 14)
#define RT5645_R_MUTE           (1 << 7)
#define RT5645_VOL_R_MUTE       (1 << 6)
#define RT5645_LR_MUTE          (RT5645_L_MUTE | RT5645_R_MUTE)
#define RT5645_OUT_0DB          0x0808      /* out_vol_tlv: field 8 = 0 dB (39 = -46.5 dB) */
/* IN1_CTRL1 / IN1_CTRL2 (combo jack) */
#define RT5645_BST_SFT1         12
#define RT5645_CBJ_BST1_EN      (1 << 2)
#define RT5645_CBJ_MN_JD        (1 << 12)
/* IN2_CTRL */
#define RT5645_BST_SFT2         8
#define RT5645_IN_DF2           (1 << 6)
#define RT5645_BST_24DB         2
/* PWR_DIG1 */
#define RT5645_PWR_I2S1         (1 << 15)
#define RT5645_PWR_DAC_L1       (1 << 12)
#define RT5645_PWR_DAC_R1       (1 << 11)
#define RT5645_PWR_CLS_D_R      (1 << 9)
#define RT5645_PWR_CLS_D_L      (1 << 8)
#define RT5645_PWR_ADC_L        (1 << 2)
#define RT5645_PWR_ADC_R        (1 << 1)
#define RT5645_PWR_CLS_D        (1 << 0)
/* PWR_DIG2 */
#define RT5645_PWR_ADC_S1F      (1 << 15)
#define RT5645_PWR_DAC_S1F      (1 << 11)
/* PWR_ANLG1 */
#define RT5645_PWR_VREF1        (1 << 15)
#define RT5645_PWR_FV1          (1 << 14)
#define RT5645_PWR_MB           (1 << 13)
#define RT5645_PWR_LM           (1 << 12)
#define RT5645_PWR_BG           (1 << 11)
#define RT5645_PWR_HP_L         (1 << 7)
#define RT5645_PWR_HP_R         (1 << 6)
#define RT5645_PWR_HA           (1 << 5)
#define RT5645_PWR_VREF2        (1 << 4)
#define RT5645_PWR_FV2          (1 << 3)
/* PWR_ANLG2 */
#define RT5645_PWR_BST1         (1 << 15)
#define RT5645_PWR_BST2         (1 << 14)
#define RT5645_PWR_MB1          (1 << 11)
#define RT5645_PWR_MB2          (1 << 10)
#define RT5645_PWR_PLL          (1 << 9)
#define RT5645_PWR_BST2_P       (1 << 5)
/* PWR_MIXER */
#define RT5645_PWR_SM_L         (1 << 13)
#define RT5645_PWR_SM_R         (1 << 12)
#define RT5645_PWR_RM_L         (1 << 11)
#define RT5645_PWR_RM_R         (1 << 10)
#define RT5645_PWR_HM_L         (1 << 7)
#define RT5645_PWR_HM_R         (1 << 6)
#define RT5645_PWR_LDO2         (1 << 1)
/* PWR_VOL */
#define RT5645_PWR_SV_L         (1 << 15)
#define RT5645_PWR_SV_R         (1 << 14)
#define RT5645_PWR_HV_L         (1 << 11)
#define RT5645_PWR_HV_R         (1 << 10)
#define RT5645_PWR_MIC_DET      (1 << 5)
/* STO1_ADC_MIXER */
#define RT5645_M_ADC_L1         (1 << 14)
#define RT5645_M_ADC_L2         (1 << 13)
#define RT5645_ADC_1_SRC_ADC    (1 << 12)
#define RT5645_ADC_2_SRC_DMIC   (1 << 11)
#define RT5645_DMIC_SRC_DMIC2   (1 << 8)
#define RT5645_M_ADC_R1         (1 << 6)
#define RT5645_M_ADC_R2         (1 << 5)
/* AD_DA_MIXER: Stereo ADC -> DAC1 mix 15/7, "DAC1 Switch" 14/6, DAC1 L/R source [9:8]/[11:10] (0 = IF1) */
#define RT5645_M_ADCMIX_L       (1 << 15)
#define RT5645_M_DAC1_L         (1 << 14)
#define RT5645_M_ADCMIX_R       (1 << 7)
#define RT5645_M_DAC1_R         (1 << 6)
/* STO_DAC_MIXER: DAC L1 14, DAC L2 12, DAC R1->L 9, DAC R1 6, DAC R2 4, DAC L1->R 1 (mutes) */
#define RT5645_M_DAC_L1         (1 << 14)
#define RT5645_M_DAC_R1         (1 << 6)
/* REC_x2_MIXER mutes: BST2 bit 2, BST1 bit 1 */
#define RT5645_M_BST2_RM        (1 << 2)
#define RT5645_M_BST1_RM        (1 << 1)
/* HPOMIXx_CTRL (HPOVOL MIX): DAC1 bit 0 */
#define RT5645_M_DAC1_HV        (1 << 0)
/* HPO_MIXER: DAC1 14, HPVOL 13 */
#define RT5645_M_DAC1_HM        (1 << 14)
#define RT5645_M_HPVOL_HM       (1 << 13)
/* SPK_x_MIXER: DAC x1 bit 1 */
#define RT5645_M_DAC_X1_SM      (1 << 1)
/* SPO_MIXER: SPOL: DAC L1 15, DAC R1 14, SPKVOL L 13, SPKVOL R 12, BST3 11; SPOR: DAC R1 2, BST3 1, SPKVOL R 0 */
#define RT5645_M_SV_L_SPM_L     (1 << 13)
#define RT5645_M_SV_R_SPM_R     (1 << 0)
/* LOUT_MIXER */
#define RT5645_M_DAC_L1_LM      (1 << 15)
#define RT5645_M_DAC_R1_LM      (1 << 14)
/* I2S1_SDP */
#define RT5645_I2S_MS_S         (1 << 15)
/* ADDA_CLK1 */
#define RT5645_I2S_PD1_SFT      12
#define RT5645_I2S_PD1_MASK     (7 << 12)
/* DMIC_CTRL1 */
#define RT5645_DMIC_1_EN        (1 << 15)
#define RT5645_DMIC_2_EN        (1 << 14)
#define RT5645_DMIC_2_DP_MASK   (3 << 10)
#define RT5645_DMIC_2_DP_IN2P   (3 << 10)
#define RT5645_DMIC_CLK_MASK    (7 << 5)
#define RT5645_DMIC_CLK_SFT     5
#define RT5645_DMIC_1_DP_MASK   (3 << 0)
#define RT5645_DMIC_1_DP_IN2N   (1 << 0)
/* GLB_CLK */
#define RT5645_SCLK_SRC_MASK    (3 << 14)
#define RT5645_SCLK_SRC_PLL1    (1 << 14)
#define RT5645_SCLK_SRC_RCCLK   (2 << 14)
#define RT5645_PLL1_SRC_MASK    (7 << 11)
#define RT5645_PLL1_SRC_MCLK    (0 << 11)
#define RT5645_PLL1_SRC_BCLK1   (1 << 11)
#define RT5645_PLL_N_SFT        7
#define RT5645_PLL_M_SFT        12
#define RT5645_PLL_M_BP         (1 << 11)
/* ASRC_1: "I2S1 ASRC" bit 11 (always routed to I2S1), "DMIC STO1 ASRC" bit 7 */
#define RT5645_I2S1_ASRC        (1 << 11)
#define RT5645_DMIC_STO1_ASRC   (1 << 7)
/* DEPOP_M1 */
#define RT5645_SMT_TRIG_EN      (1 << 15)
#define RT5645_HP_L_SMT_EN      (1 << 9)
#define RT5645_HP_R_SMT_EN      (1 << 8)
#define RT5645_RSTN_EN          (1 << 6)
#define RT5645_RSTP_EN          (1 << 5)
#define RT5645_HP_CO_EN         (1 << 4)
#define RT5645_HP_SG_EN         (1 << 2)
#define RT5645_SMT_LR           (RT5645_HP_L_SMT_EN | RT5645_HP_R_SMT_EN)
/* DEPOP_M2 */
#define RT5645_DEPOP_MASK       (1 << 13)
#define RT5645_DEPOP_MAN        (1 << 13)
/* DEPOP_M3: CP_FQ1 [10:8], CP_FQ2 [6:4], CP_FQ3 [2:0]: 7 192 kHz, 6 96 kHz, 3 12 kHz */
#define RT5645_CP_FQ_MASK       0x0777
#define RT5645_CP_FQ_ON         ((7 << 8) | (3 << 4) | 7)
#define RT5645_CP_FQ_OFF        ((6 << 8) | (3 << 4) | 6)
/* GPIO_CTRL1 */
#define RT5645_GP2_PIN_DMIC1_SCL (1 << 14)
/* GEN_CTRL1 / GEN_CTRL2 / GEN_CTRL3 */
#define RT5645_DIG_GATE_CTRL    (1 << 0)
#define RT5645_MICBIAS1_POW_CTRL_SEL_M (1 << 5)
#define RT5645_MICBIAS2_POW_CTRL_SEL_M (1 << 4)
#define RT5645_RING2_SLEEVE_GND (1 << 5)
/* BASS_BACK */
#define RT5645_G_BB_BST_MASK    0x3f
#define RT5645_G_BB_BST_25DB    0x14

enum { MIC_HS_IN1, MIC_IN2, MIC_IN2_DIFF, MIC_DMIC1, MIC_DMIC2, MIC_MAX };

typedef struct {
    ULONG   Sysclk;
    USHORT  PllSrc;
    UCHAR   Is5650;
    UCHAR   Mic;
    UCHAR   VId;
    UCHAR   PdIdx;
    UCHAR   DmicClk;
    UCHAR   HpAmpCnt;       /* hp_amp_power() reference count (HP amp, LOUT amp) */
    UCHAR   Muted;
    UCHAR   PlayActive;
} RT5645_PRIV;

typedef char RT5645_PRIV_FITS[(sizeof(RT5645_PRIV) <= sizeof(((SST_CODEC *)0)->Priv)) ? 1 : -1];

#define PRIV(c) ((RT5645_PRIV *)(c)->Priv)

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
    BOOLEAN ok = CWr(c, RT5645_PRIV_INDEX, idx);
    ok &= CWr(c, RT5645_PRIV_DATA, val);
    return ok;
}

static ULONG PrRd(SST_CODEC *c, ULONG idx)
{
    ULONG v = 0xffff;
    if (!CWr(c, RT5645_PRIV_INDEX, idx) || !CRd(c, RT5645_PRIV_DATA, &v))
        return 0xffff;
    return v;
}

static BOOLEAN Rt5645Probe(SST_CODEC *c)
{
    ULONG v;

    if (!CRd(c, RT5645_VENDOR_ID2, &v))
        return FALSE;
    return v == RT5645_DEVICE_ID || v == RT5650_DEVICE_ID;
}

/* rt5645_set_bias_level(OFF -> STANDBY -> PREPARE) */
static void BiasOn(SST_CODEC *c)
{
    ULONG m = RT5645_PWR_VREF1 | RT5645_PWR_MB | RT5645_PWR_BG | RT5645_PWR_VREF2;
    ULONG fv = RT5645_PWR_FV1 | RT5645_PWR_FV2;

    /* STANDBY */
    CUpd(c, RT5645_PWR_ANLG1, m, m);
    CDelay(10);
    CUpd(c, RT5645_PWR_ANLG1, fv, fv);
    CWr(c, RT5645_DEPOP_M2, 0x1140);
    CDelay(40);
    /* PREPARE */
    CUpd(c, RT5645_PWR_ANLG1, m, m);
    CDelay(10);
    CUpd(c, RT5645_PWR_ANLG1, fv, fv);
    CUpd(c, RT5645_GEN_CTRL1, RT5645_DIG_GATE_CTRL, RT5645_DIG_GATE_CTRL);
}

/* rt5645_set_bias_level(SND_SOC_BIAS_OFF) */
static void BiasOff(SST_CODEC *c)
{
    CWr(c, RT5645_DEPOP_M2, 0x1100);
    CUpd(c, RT5645_GEN_CTRL1, RT5645_DIG_GATE_CTRL, 0);
    CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_VREF1 | RT5645_PWR_MB | RT5645_PWR_BG | RT5645_PWR_VREF2 |
         RT5645_PWR_FV1 | RT5645_PWR_FV2, 0);
}

/* "Platform Clock" + "PLL1" supply: sysclk from PLL1 (rt5645_set_dai_sysclk(PLL1)) */
static void ClockOn(SST_CODEC *c)
{
    CUpd(c, RT5645_GLB_CLK, RT5645_SCLK_SRC_MASK, RT5645_SCLK_SRC_PLL1);
    CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_PLL, RT5645_PWR_PLL);
}

/* platform_clock_control POST_PMD: sysclk to the RC clock, PLL1 supply off */
static void ClockOff(SST_CODEC *c)
{
    CUpd(c, RT5645_GLB_CLK, RT5645_SCLK_SRC_MASK, RT5645_SCLK_SRC_RCCLK);
    CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_PLL, 0);
}

/* hp_amp_power() with its reference count */
static void HpAmpPower(SST_CODEC *c, BOOLEAN on)
{
    RT5645_PRIV *p = PRIV(c);
    ULONG v, i;

    if (on) {
        if (p->HpAmpCnt++ != 0)
            return;
        if (p->Is5650) {
            CWr(c, RT5645_DEPOP_M2, 0x3100);
            CWr(c, RT5645_CHARGE_PUMP, 0x0e06);
            CWr(c, RT5645_DEPOP_M1, 0x000d);
            PrWr(c, RT5645_PR_HP_DCC_INT1, 0x9f01);
            for (i = 0; i < 20; i++) {          /* HP DC calibration done */
                CDelay(1);
                v = PrRd(c, RT5645_PR_HP_DCC_INT1);
                if (!(v & 0x8000))
                    break;
            }
            CUpd(c, RT5645_DEPOP_M1, RT5645_HP_CO_EN, RT5645_HP_CO_EN);
            PrWr(c, 0x3e, 0x7400);
            CWr(c, RT5645_DEPOP_M3, 0x0737);
            PrWr(c, RT5645_PR_MAMP_INT_REG2, 0xfc00);
            CWr(c, RT5645_DEPOP_M2, 0x1140);
            CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_HP_L | RT5645_PWR_HP_R, RT5645_PWR_HP_L | RT5645_PWR_HP_R);
            CDelay(90);
        } else {
            CUpd(c, RT5645_DEPOP_M2, RT5645_DEPOP_MASK, RT5645_DEPOP_MAN);
            CWr(c, RT5645_DEPOP_M1, 0x000d);
            PrWr(c, RT5645_PR_HP_DCC_INT1, 0x9f01);
            CDelay(150);
            CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_FV1 | RT5645_PWR_FV2, 0);
            CUpd(c, RT5645_PWR_VOL, RT5645_PWR_HV_L | RT5645_PWR_HV_R, RT5645_PWR_HV_L | RT5645_PWR_HV_R);
            CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_HP_L | RT5645_PWR_HP_R | RT5645_PWR_HA,
                 RT5645_PWR_HP_L | RT5645_PWR_HP_R | RT5645_PWR_HA);
            CDelay(5);
            CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_FV1 | RT5645_PWR_FV2, RT5645_PWR_FV1 | RT5645_PWR_FV2);
            CUpd(c, RT5645_DEPOP_M1, RT5645_HP_CO_EN | RT5645_HP_SG_EN, RT5645_HP_CO_EN | RT5645_HP_SG_EN);
            PrWr(c, 0x14, 0x1aaa);
            PrWr(c, 0x24, 0x0430);
        }
    } else {
        if (p->HpAmpCnt == 0 || --p->HpAmpCnt != 0)
            return;
        if (p->Is5650) {
            PrWr(c, 0x3e, 0x7400);
            CWr(c, RT5645_DEPOP_M3, 0x0737);
            PrWr(c, RT5645_PR_MAMP_INT_REG2, 0xfc00);
            CWr(c, RT5645_DEPOP_M2, 0x1140);
            CDelay(100);
            CWr(c, RT5645_DEPOP_M1, 0x0001);
            CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_HP_L | RT5645_PWR_HP_R, 0);
        } else {
            CUpd(c, RT5645_DEPOP_M1, RT5645_HP_SG_EN | RT5645_SMT_LR, 0);
            CWr(c, RT5645_DEPOP_M1, 0x0000);
            CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_HP_L | RT5645_PWR_HP_R | RT5645_PWR_HA, 0);
            CUpd(c, RT5645_DEPOP_M2, RT5645_DEPOP_MASK, 0);
        }
    }
}

/* rt5645_hp_event POST_PMU */
static void HpEventOn(SST_CODEC *c)
{
    HpAmpPower(c, TRUE);
    if (PRIV(c)->Is5650)
        return;
    /* headphone unmute sequence */
    CUpd(c, RT5645_DEPOP_M3, RT5645_CP_FQ_MASK, RT5645_CP_FQ_ON);
    PrWr(c, RT5645_PR_MAMP_INT_REG2, 0xfc00);
    CUpd(c, RT5645_DEPOP_M1, RT5645_SMT_TRIG_EN, RT5645_SMT_TRIG_EN);
    CUpd(c, RT5645_DEPOP_M1, RT5645_RSTN_EN, RT5645_RSTN_EN);
    CUpd(c, RT5645_DEPOP_M1, RT5645_RSTN_EN | RT5645_SMT_LR, RT5645_SMT_LR);
    CDelay(40);
    CUpd(c, RT5645_DEPOP_M1, RT5645_HP_SG_EN | RT5645_SMT_LR, 0);
}

/* rt5645_hp_event PRE_PMD */
static void HpEventOff(SST_CODEC *c)
{
    if (!PRIV(c)->Is5650) {
        /* headphone mute sequence */
        CUpd(c, RT5645_DEPOP_M3, RT5645_CP_FQ_MASK, RT5645_CP_FQ_OFF);
        PrWr(c, RT5645_PR_MAMP_INT_REG2, 0xfc00);
        CUpd(c, RT5645_DEPOP_M1, RT5645_HP_SG_EN, RT5645_HP_SG_EN);
        CUpd(c, RT5645_DEPOP_M1, RT5645_RSTP_EN, RT5645_RSTP_EN);
        CUpd(c, RT5645_DEPOP_M1, RT5645_RSTP_EN | RT5645_SMT_LR, RT5645_SMT_LR);
        CDelay(30);
    }
    HpAmpPower(c, FALSE);
}

static BOOLEAN UsesBst1(RT5645_PRIV *p) { return p->Mic == MIC_HS_IN1; }
static BOOLEAN UsesDmic(RT5645_PRIV *p) { return p->Mic == MIC_DMIC1 || p->Mic == MIC_DMIC2; }

/* LDO2 / "Mic Det Power": supplies of the HP amp and of BST1 (headset mic) / IN2P */
static void MicDetSupply(SST_CODEC *c, BOOLEAN on)
{
    CUpd(c, RT5645_PWR_MIXER, RT5645_PWR_LDO2, on ? RT5645_PWR_LDO2 : 0);
    CUpd(c, RT5645_PWR_VOL, RT5645_PWR_MIC_DET, on ? RT5645_PWR_MIC_DET : 0);
}

static void PlayUp(SST_CODEC *c)
{
    RT5645_PRIV *p = PRIV(c);
    ULONG v;

    /* supplies: I2S1 (+ "I2S1 ASRC"), LDO2, Mic Det Power, HPOVOL MIX power, DAC stereo filter */
    CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_I2S1, RT5645_PWR_I2S1);
    MicDetSupply(c, TRUE);
    CUpd(c, RT5645_PWR_MIXER, RT5645_PWR_HM_L | RT5645_PWR_HM_R, RT5645_PWR_HM_L | RT5645_PWR_HM_R);
    CUpd(c, RT5645_ASRC_1, RT5645_I2S1_ASRC, RT5645_I2S1_ASRC);
    CUpd(c, RT5645_PWR_DIG2, RT5645_PWR_DAC_S1F, RT5645_PWR_DAC_S1F);
    /* DACs */
    CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_DAC_L1 | RT5645_PWR_DAC_R1, RT5645_PWR_DAC_L1 | RT5645_PWR_DAC_R1);
    /* mixers / switches with power bits: SPK MIX, HPOVOL MIX, SPKVOL */
    CUpd(c, RT5645_PWR_MIXER, RT5645_PWR_SM_L | RT5645_PWR_SM_R, RT5645_PWR_SM_L | RT5645_PWR_SM_R);
    v = RT5645_PWR_HV_L | RT5645_PWR_HV_R | RT5645_PWR_SV_L | RT5645_PWR_SV_R;
    CUpd(c, RT5645_PWR_VOL, v, v);
    /* PGAs: "HP amp" (subseq 1), "LOUT amp" (subseq 1), "SPK amp" (subseq 2) */
    HpEventOn(c);
    HpAmpPower(c, TRUE);
    CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_LM, RT5645_PWR_LM);
    CUpd(c, RT5645_LOUT1, RT5645_LR_MUTE, 0);
    v = RT5645_PWR_CLS_D | RT5645_PWR_CLS_D_R | RT5645_PWR_CLS_D_L;
    CUpd(c, RT5645_PWR_DIG1, v, v);
    /* autodisable kcontrols: "DAC1 Switch", SPKVOL L/R, HPOVOL L/R */
    CUpd(c, RT5645_AD_DA_MIXER, RT5645_M_DAC1_L | RT5645_M_DAC1_R,
         p->Muted ? RT5645_M_DAC1_L | RT5645_M_DAC1_R : 0);
    CUpd(c, RT5645_SPK_VOL, RT5645_LR_MUTE, 0);
    CUpd(c, RT5645_HP_VOL, RT5645_LR_MUTE, 0);
    p->PlayActive = TRUE;
}

static void PlayDown(SST_CODEC *c)
{
    RT5645_PRIV *p = PRIV(c);
    ULONG v;

    p->PlayActive = FALSE;
    /* kcontrols */
    CUpd(c, RT5645_HP_VOL, RT5645_LR_MUTE, RT5645_LR_MUTE);
    CUpd(c, RT5645_SPK_VOL, RT5645_LR_MUTE, RT5645_LR_MUTE);
    CUpd(c, RT5645_AD_DA_MIXER, RT5645_M_DAC1_L | RT5645_M_DAC1_R, RT5645_M_DAC1_L | RT5645_M_DAC1_R);
    /* PGAs, subseq descending: SPK amp, HP amp, LOUT amp */
    CWr(c, RT5645_EQ_CTRL2, 0);
    CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_CLS_D | RT5645_PWR_CLS_D_R | RT5645_PWR_CLS_D_L, 0);
    HpEventOff(c);
    CUpd(c, RT5645_LOUT1, RT5645_LR_MUTE, RT5645_LR_MUTE);
    CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_LM, 0);
    HpAmpPower(c, FALSE);
    /* mixers, DACs */
    v = RT5645_PWR_HV_L | RT5645_PWR_HV_R | RT5645_PWR_SV_L | RT5645_PWR_SV_R;
    CUpd(c, RT5645_PWR_VOL, v, 0);
    CUpd(c, RT5645_PWR_MIXER, RT5645_PWR_SM_L | RT5645_PWR_SM_R, 0);
    CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_DAC_L1 | RT5645_PWR_DAC_R1, 0);
    /* supplies */
    CUpd(c, RT5645_PWR_DIG2, RT5645_PWR_DAC_S1F, 0);
    CUpd(c, RT5645_PWR_MIXER, RT5645_PWR_HM_L | RT5645_PWR_HM_R, 0);
    if (!c->RecOn) {
        CUpd(c, RT5645_ASRC_1, RT5645_I2S1_ASRC, 0);
        CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_I2S1, 0);
    }
    if (!c->RecOn || UsesDmic(p))
        MicDetSupply(c, FALSE);
}

static void RecUp(SST_CODEC *c)
{
    RT5645_PRIV *p = PRIV(c);
    ULONG v;

    /* supplies */
    CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_I2S1, RT5645_PWR_I2S1);
    CUpd(c, RT5645_ASRC_1, RT5645_I2S1_ASRC, RT5645_I2S1_ASRC);
    CUpd(c, RT5645_PWR_DIG2, RT5645_PWR_ADC_S1F, RT5645_PWR_ADC_S1F);
    if (UsesDmic(p)) {
        v = p->Mic == MIC_DMIC1 ? RT5645_DMIC_1_EN : RT5645_DMIC_2_EN;
        CUpd(c, RT5645_ASRC_1, RT5645_DMIC_STO1_ASRC, RT5645_DMIC_STO1_ASRC);
        CUpd(c, RT5645_DMIC_CTRL1, RT5645_DMIC_CLK_MASK, (ULONG)p->DmicClk << RT5645_DMIC_CLK_SFT);
        CUpd(c, RT5645_DMIC_CTRL1, v, v);
        return;
    }
    MicDetSupply(c, TRUE);
    /* micbias1 (PRE_PMU: GEN_CTRL2 manual power control), micbias2 for the internal mic */
    CUpd(c, RT5645_GEN_CTRL2, RT5645_MICBIAS1_POW_CTRL_SEL_M, RT5645_MICBIAS1_POW_CTRL_SEL_M);
    CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_MB1, RT5645_PWR_MB1);
    if (!UsesBst1(p)) {
        CUpd(c, RT5645_GEN_CTRL2, RT5645_MICBIAS2_POW_CTRL_SEL_M, RT5645_MICBIAS2_POW_CTRL_SEL_M);
        CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_MB2, RT5645_PWR_MB2);
    }
    /* ADC L/R power supplies */
    CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_ADC_L | RT5645_PWR_ADC_R, RT5645_PWR_ADC_L | RT5645_PWR_ADC_R);
    /* mixer: RECMIX; PGA: BST1 or BST2 (+ BST2_P POST_PMU) */
    CUpd(c, RT5645_PWR_MIXER, RT5645_PWR_RM_L | RT5645_PWR_RM_R, RT5645_PWR_RM_L | RT5645_PWR_RM_R);
    if (UsesBst1(p)) {
        CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_BST1, RT5645_PWR_BST1);
    } else {
        CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_BST2, RT5645_PWR_BST2);
        CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_BST2_P, RT5645_PWR_BST2_P);
    }
}

static void RecDown(SST_CODEC *c)
{
    RT5645_PRIV *p = PRIV(c);

    CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_BST2_P, 0);   /* bst2 PRE_PMD */
    CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_BST1 | RT5645_PWR_BST2, 0);
    CUpd(c, RT5645_PWR_MIXER, RT5645_PWR_RM_L | RT5645_PWR_RM_R, 0);
    CUpd(c, RT5645_DMIC_CTRL1, RT5645_DMIC_1_EN | RT5645_DMIC_2_EN, 0);
    CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_ADC_L | RT5645_PWR_ADC_R, 0);
    CUpd(c, RT5645_PWR_ANLG2, RT5645_PWR_MB1 | RT5645_PWR_MB2, 0);
    CUpd(c, RT5645_GEN_CTRL2, RT5645_MICBIAS1_POW_CTRL_SEL_M | RT5645_MICBIAS2_POW_CTRL_SEL_M, 0);
    CUpd(c, RT5645_ASRC_1, RT5645_DMIC_STO1_ASRC, 0);
    CUpd(c, RT5645_PWR_DIG2, RT5645_PWR_ADC_S1F, 0);
    if (!c->PlayOn) {
        CUpd(c, RT5645_ASRC_1, RT5645_I2S1_ASRC, 0);
        CUpd(c, RT5645_PWR_DIG1, RT5645_PWR_I2S1, 0);
        if (!UsesDmic(p))
            MicDetSupply(c, FALSE);
    }
}

static BOOLEAN Rt5645Init(SST_CODEC *c)
{
    RT5645_PRIV *p = PRIV(c);
    BOOLEAN ok = TRUE;
    ULONG v, fin, sdp, recm, adcm;
    LONG pd, dm;
    RL_PLL pll;

    memset(p, 0, sizeof(*p));
    if (!CRd(c, RT5645_VENDOR_ID2, &v))
        return FALSE;
    p->Is5650 = v == RT5650_DEVICE_ID;
    p->Mic = (UCHAR)(c->Mic < MIC_MAX ? c->Mic : MIC_HS_IN1);
    if (p->Mic == MIC_HS_IN1 && !memcmp(c->Hid, "10EC3270", 9))
        p->Mic = MIC_IN2;                       /* ALC3270: headset-mic pin not connected */
    p->Sysclk = c->Rate * 512;

    if (c->Bclk >= RL_PLL_INP_MIN) {
        fin = c->Bclk;
        p->PllSrc = RT5645_PLL1_SRC_BCLK1;
    } else {
        fin = c->Mclk;
        p->PllSrc = RT5645_PLL1_SRC_MCLK;
    }
    if (!RlPllCalc(fin, p->Sysclk, &pll)) {
        LOGE("RT5645: no usable PLL reference (BCLK %u MCLK %u)\n", c->Bclk, c->Mclk);
        return FALSE;
    }
    pd = RlClkInfo(p->Sysclk, c->Rate);
    p->PdIdx = (UCHAR)(pd < 0 ? 1 : pd);
    dm = RlDmicClk(p->Sysclk / s_RlPd[p->PdIdx]);
    p->DmicClk = (UCHAR)(dm < 0 ? 4 : dm);

    /* rt5645_i2c_probe */
    if (!CWr(c, RT5645_RESET, 0))
        return FALSE;
    CDelay(1);
    ok &= CRd(c, RT5645_VENDOR_ID, &v);
    p->VId = (UCHAR)v;
    ok &= CWr(c, RT5645_AD_DA_MIXER, 0x8080);
    ok &= PrWr(c, 0x3d, 0x3600);                /* init_list */
    ok &= PrWr(c, 0x1c, 0xfd70);
    ok &= PrWr(c, 0x20, 0x611f);
    ok &= PrWr(c, 0x21, 0x4040);
    ok &= PrWr(c, 0x23, 0x0004);
    ok &= CWr(c, RT5645_ASRC_4, 0x0120);
    if (p->Is5650) {                            /* rt5650_init_list */
        ok &= CWr(c, 0xf6, 0x0100);
        ok &= CWr(c, RT5645_PWR_ANLG1, 0x02);
        ok &= CWr(c, RT5645_IL_CMD3, 0x6728);
        ok &= PrWr(c, 0x3a, 0x0000);
        ok &= CWr(c, RT5645_CLSD_OUT_CTRL1, 0x4059);
        ok &= CWr(c, RT5645_GEN_CTRL3, 0x0200);
    }
    ok &= CUpd(c, RT5645_CLSD_OUT_CTRL, 0xc0, 0xc0);
    if (p->Mic == MIC_IN2_DIFF)
        ok &= CUpd(c, RT5645_IN2_CTRL, RT5645_IN_DF2, RT5645_IN_DF2);
    if (UsesDmic(p)) {
        ok &= CUpd(c, RT5645_GPIO_CTRL1, RT5645_GP2_PIN_DMIC1_SCL, RT5645_GP2_PIN_DMIC1_SCL);
        if (p->Mic == MIC_DMIC1)
            ok &= CUpd(c, RT5645_DMIC_CTRL1, RT5645_DMIC_1_DP_MASK, RT5645_DMIC_1_DP_IN2N);
        else
            ok &= CUpd(c, RT5645_DMIC_CTRL1, RT5645_DMIC_2_DP_MASK, RT5645_DMIC_2_DP_IN2P);
    }
    ok &= CUpd(c, RT5645_ADDA_CLK1, RT5645_I2S_PD1_MASK, (ULONG)p->PdIdx << RT5645_I2S_PD1_SFT);
    BiasOff(c);                                 /* rt5645_probe: force bias off */

    /* combo jack in headset mode (rt5645_jack_detect(insert) before the card exists):
     * powers MB/VREF2, LDO2, Mic Det Power for the sequence */
    if (UsesBst1(p)) {
        ok &= CWr(c, RT5645_CHARGE_PUMP, 0x0206);
        ok &= CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_MB | RT5645_PWR_VREF2, RT5645_PWR_MB | RT5645_PWR_VREF2);
        MicDetSupply(c, TRUE);
        ok &= CWr(c, RT5645_JD_CTRL3, 0x00f0);
        ok &= CUpd(c, RT5645_IN1_CTRL2, RT5645_CBJ_MN_JD, RT5645_CBJ_MN_JD);
        ok &= CUpd(c, RT5645_IN1_CTRL1, RT5645_CBJ_BST1_EN, RT5645_CBJ_BST1_EN);
        CDelay(100);
        ok &= CUpd(c, RT5645_IN1_CTRL2, RT5645_CBJ_MN_JD, 0);
        ok &= CWr(c, RT5645_CHARGE_PUMP, 0x0e06);
        MicDetSupply(c, FALSE);
        ok &= CUpd(c, RT5645_PWR_ANLG1, RT5645_PWR_MB | RT5645_PWR_VREF2, 0);
    }

    /* PLL1 (rt5645_set_dai_pll), sysclk stays on the RC clock until a stream runs */
    ok &= CUpd(c, RT5645_GLB_CLK, RT5645_SCLK_SRC_MASK | RT5645_PLL1_SRC_MASK, RT5645_SCLK_SRC_RCCLK | p->PllSrc);
    ok &= CWr(c, RT5645_PLL_CTRL1, ((ULONG)pll.N << RT5645_PLL_N_SFT) | (ULONG)pll.K);
    ok &= CWr(c, RT5645_PLL_CTRL2, ((ULONG)(pll.MBp ? 0 : pll.M) << RT5645_PLL_M_SFT) |
              (pll.MBp ? RT5645_PLL_M_BP : 0));

    /* DAI: slave, I2S, normal BCLK polarity, word length (DL at [3:2], RT5650 [5:4]) */
    v = c->Valid > 16 ? 2 : 0;
    sdp = RT5645_I2S_MS_S | (v << (p->Is5650 ? 4 : 2));
    ok &= CWr(c, RT5645_I2S1_SDP, sdp);
    if (c->Slots > 2) {
        /* rt5645_set_tdm_slot: enable, slot count 4/6/8, slot width 16/20/24/32 */
        ULONG sl = c->Slots >= 8 ? 3 : c->Slots >= 6 ? 2 : 1;
        ULONG wd = c->Slot >= 32 ? 3 : c->Slot >= 24 ? 2 : c->Slot >= 20 ? 1 : 0;
        if (p->Is5650)
            ok &= CUpd(c, RT5645_TDM_CTRL_1, 0x8ff0, (1 << 15) | (sl << 10) | (sl << 8) | (wd << 6) | (wd << 4));
        else {
            ok &= CUpd(c, RT5645_BASS_BACK, RT5645_G_BB_BST_MASK, RT5645_G_BB_BST_25DB);
            ok &= CUpd(c, RT5645_TDM_CTRL_1, 0x7c00, (1 << 14) | (sl << 12) | (wd << 10));
        }
    }

    /* playback routing: IF1 -> DAC1 MIX -> stereo DAC mixer -> DAC1 (RT5650 "A DAC1": DAC1 MIX) */
    ok &= CWr(c, RT5645_AD_DA_MIXER, RT5645_M_ADCMIX_L | RT5645_M_ADCMIX_R | RT5645_M_DAC1_L | RT5645_M_DAC1_R);
    ok &= CWr(c, RT5645_STO_DAC_MIXER, 0x5656 & ~(RT5645_M_DAC_L1 | RT5645_M_DAC_R1));
    ok &= CWr(c, RT5645_DAC1_DIG_VOL, 0xafaf); /* 0 dB */
    ok &= CWr(c, RT5645_HPOMIXL_CTRL, 0x001f & ~RT5645_M_DAC1_HV);
    ok &= CWr(c, RT5645_HPOMIXR_CTRL, 0x001f & ~RT5645_M_DAC1_HV);
    ok &= CUpd(c, RT5645_HPO_MIXER, RT5645_M_DAC1_HM | RT5645_M_HPVOL_HM, RT5645_M_DAC1_HM);
    ok &= CWr(c, RT5645_SPK_L_MIXER, 0x003e & ~RT5645_M_DAC_X1_SM);
    ok &= CWr(c, RT5645_SPK_R_MIXER, 0x003e & ~RT5645_M_DAC_X1_SM);
    ok &= CWr(c, RT5645_SPO_MIXER, 0xf807 & ~(RT5645_M_SV_L_SPM_L | RT5645_M_SV_R_SPM_R));
    ok &= CWr(c, RT5645_LOUT_MIXER, 0xf000 & ~(RT5645_M_DAC_L1_LM | RT5645_M_DAC_R1_LM));
    /* analog volumes 0 dB, channel switches on, output switches off until playback */
    ok &= CWr(c, RT5645_SPK_VOL, RT5645_LR_MUTE | RT5645_OUT_0DB);
    ok &= CWr(c, RT5645_HP_VOL, RT5645_LR_MUTE | RT5645_OUT_0DB);
    ok &= CWr(c, RT5645_LOUT1, RT5645_LR_MUTE | RT5645_OUT_0DB);

    /* capture routing */
    ok &= CUpd(c, RT5645_IN1_CTRL1, 0xf << RT5645_BST_SFT1, RT5645_BST_24DB << RT5645_BST_SFT1);
    ok &= CUpd(c, RT5645_IN2_CTRL, 0xf << RT5645_BST_SFT2, RT5645_BST_24DB << RT5645_BST_SFT2);
    recm = 0x007f & ~(UsesBst1(p) ? RT5645_M_BST1_RM : UsesDmic(p) ? 0 : RT5645_M_BST2_RM);
    ok &= CWr(c, RT5645_REC_L2_MIXER, recm);
    ok &= CWr(c, RT5645_REC_R2_MIXER, recm);
    if (UsesDmic(p))
        adcm = RT5645_M_ADC_L1 | RT5645_M_ADC_R1 | RT5645_ADC_1_SRC_ADC | RT5645_ADC_2_SRC_DMIC |
               (p->Mic == MIC_DMIC2 ? RT5645_DMIC_SRC_DMIC2 : 0);
    else
        adcm = RT5645_M_ADC_L2 | RT5645_M_ADC_R2 | RT5645_ADC_1_SRC_ADC;
    ok &= CWr(c, RT5645_STO1_ADC_MIXER, adcm);
    ok &= CWr(c, RT5645_STO1_ADC_DIG_VOL, 0x2f2f);  /* 0 dB, unmuted */

    if (!ok)
        return FALSE;
    p->HpAmpCnt = 0;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("%s (vid %u): PLL %s %u -> %u N=%d K=%d M=%d%s, mic mode %u, %u slots\n",
         p->Is5650 ? "RT5650" : "RT5645", p->VId, p->PllSrc == RT5645_PLL1_SRC_MCLK ? "MCLK" : "BCLK1",
         fin, p->Sysclk, pll.N, pll.K, pll.MBp ? -1 : pll.M, pll.KBp ? " (K bypass)" : "", p->Mic, c->Slots);
    return TRUE;
}

static void Rt5645Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
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

/* DAC1 digital volume: 0xaf = 0 dB in 0.375 dB steps (Linux uses bits [15:9]/[7:1]); mute in
 * the "DAC1 Switch" of DAC1 MIXL/R (AD_DA_MIXER) */
static void Rt5645Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    RT5645_PRIV *p = PRIV(c);

    CWr(c, RT5645_DAC1_DIG_VOL, (CVolSteps(left, 175) << 8) | CVolSteps(right, 175));
    p->Muted = mute ? TRUE : FALSE;
    if (p->PlayActive)
        CUpd(c, RT5645_AD_DA_MIXER, RT5645_M_DAC1_L | RT5645_M_DAC1_R,
             mute ? RT5645_M_DAC1_L | RT5645_M_DAC1_R : 0);
}

static void Rt5645Shutdown(SST_CODEC *c)
{
    if (c->PlayOn)
        PlayDown(c);
    if (c->RecOn)
        RecDown(c);
    ClockOff(c);
    BiasOff(c);
    /* jack-out state, then rt5645_i2c_shutdown */
    CUpd(c, RT5645_HP_VOL, RT5645_LR_MUTE, RT5645_LR_MUTE);
    CUpd(c, RT5645_GEN_CTRL3, RT5645_RING2_SLEEVE_GND, RT5645_RING2_SLEEVE_GND);
    CUpd(c, RT5645_IN1_CTRL2, RT5645_CBJ_MN_JD, RT5645_CBJ_MN_JD);
    CUpd(c, RT5645_IN1_CTRL1, RT5645_CBJ_BST1_EN, 0);
    CDelay(20);
    CWr(c, RT5645_RESET, 0);
    PRIV(c)->HpAmpCnt = 0;
}

const CODEC_DRV g_DrvRt5645 = { "RT5645", "10EC5645\0" "10EC5648\0" "10EC5650\0" "10EC3270\0" "10EC5640\0",
                                CODEC_HEADSET, 1, 2,
                                Rt5645Probe, Rt5645Init, Rt5645Stream, Rt5645Volume, Rt5645Shutdown };
