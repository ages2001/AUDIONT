#include "codec.h"

/*
 * Realtek RT5651 I2S audio codec (Bay Trail / Cherry Trail tablets, bytcr_rt5651).
 * Reference: Linux sound/soc/codecs/rt5651.c/.h, sound/soc/codecs/rl6231.c,
 * sound/soc/intel/boards/bytcr_rt5651.c.
 *
 * I2C: 8-bit register address, 16-bit big-endian value; private registers PR_xx through
 * RT5651_PRIV_INDEX (0x6a) / RT5651_PRIV_DATA (0x6c).  RT5651_DEVICE_ID (0xff) = 0x6281.
 *
 * Clocking (slave): PLL1 from BCLK1 -> sysclk = 512 * fs (byt_rt5651_prepare_and_enable_pll1
 * without BYT_RT5651_MCLK_EN: RT5651_PLL1_S_BCLK1, rate * bclk_ratio -> rate * 512).
 * AIF: Bay Trail CR boards wire SSP0 to AIF1 or AIF2 (BIOS "CHAN" package).  On SSP port 0
 * both AIFs are set up identically, the stereo DAC mixer takes IF1 (DAC L1) and IF2 (DAC L2
 * mux = IF2) and IF2 ADC carries the stereo ADC; the PLL then runs from MCLK when there is one
 * (BYT_RT5651_MCLK_EN is in the default quirk), else from BCLK1.  Other ports: AIF1 only.
 *
 * Outputs (no jack detection): HPOL/HPOR (HPO MIX <- DAC1, depop as rt5651_amp_power_event /
 * rt5651_hp_event) and LOUTL/LOUTR (LOUT MIX <- DAC1).  The RT5651 has no class-D amplifier:
 * the bytcr map drives the speaker from LOUT through an external amplifier.  On Cherry Trail
 * its enable is the first GpioIo of the codec (cht_rt5651_gpios, ACPI_GPIO_QUIRK_ONLY_GPIOIO,
 * active high), switched like the "Ext Amp Power" supply.
 *
 * Capture, c->Mic:
 *   0  headset mic IN3P, micbias1 (bytcr default maps IN1/IN2/IN1_IN2: "IN3P" Headset Mic)
 *   1  internal analog mic IN2P (BYT_RT5651_IN2_MAP, the bytcr default internal mic)
 *   2  internal analog mic IN1P (BYT_RT5651_IN1_MAP)
 *   3  internal analog mics IN1P + IN2P (BYT_RT5651_IN1_IN2_MAP)
 *   4  DMIC1 (BYT_RT5651_DMIC_MAP + DMIC_EN: data on IN1P, clock on GPIO2)
 *   5  headset mic on IN2P (the headset mic of the DMIC map)
 *   6  IN2P/IN2N differential ("realtek,in2-differential")
 * Analog mics: boost +24 dB, both REC mixers take the boost stage(s) (mono mic on both ADC
 * channels).  The DMIC pair is captured as stereo L/R.
 */

/* ---- registers (rt5651.h) ---- */
#define RT5651_RESET            0x00
#define RT5651_HP_VOL           0x02
#define RT5651_LOUT_CTRL1       0x03
#define RT5651_IN1_IN2          0x0d
#define RT5651_IN3              0x0e
#define RT5651_DAC1_DIG_VOL     0x19
#define RT5651_DAC2_DIG_VOL     0x1a
#define RT5651_DAC2_CTRL        0x1b
#define RT5651_ADC_DIG_VOL      0x1c
#define RT5651_STO1_ADC_MIXER   0x27
#define RT5651_AD_DA_MIXER      0x29
#define RT5651_STO_DAC_MIXER    0x2a
#define RT5651_DIG_INF_DATA     0x2f
#define RT5651_REC_L2_MIXER     0x3c
#define RT5651_REC_R2_MIXER     0x3e
#define RT5651_HPO_MIXER        0x45
#define RT5651_LOUT_MIXER       0x53
#define RT5651_PWR_DIG1         0x61
#define RT5651_PWR_DIG2         0x62
#define RT5651_PWR_ANLG1        0x63
#define RT5651_PWR_ANLG2        0x64
#define RT5651_PWR_MIXER        0x65
#define RT5651_PWR_VOL          0x66
#define RT5651_PRIV_INDEX       0x6a
#define RT5651_PRIV_DATA        0x6c
#define RT5651_I2S1_SDP         0x70
#define RT5651_I2S2_SDP         0x71
#define RT5651_ADDA_CLK1        0x73
#define RT5651_DMIC             0x75
#define RT5651_GLB_CLK          0x80
#define RT5651_PLL_CTRL1        0x81
#define RT5651_PLL_CTRL2        0x82
#define RT5651_PLL_MODE_2       0x84
#define RT5651_DEPOP_M1         0x8e
#define RT5651_DEPOP_M2         0x8f
#define RT5651_DEPOP_M3         0x90
#define RT5651_CHARGE_PUMP      0x91
#define RT5651_GPIO_CTRL1       0xc0
#define RT5651_D_MISC           0xfa
#define RT5651_DEVICE_ID        0xff
/* private registers */
#define RT5651_PR_CHPUMP_INT_REG1 0x24
#define RT5651_PR_MAMP_INT_REG2   0x37
#define RT5651_PR_HP_DCC_INT1     0x77

#define RT5651_DEVICE_ID_VALUE  0x6281

#define RT5651_L_MUTE           (1 << 15)
#define RT5651_VOL_L_MUTE       (1 << 14)
#define RT5651_R_MUTE           (1 << 7)
#define RT5651_VOL_R_MUTE       (1 << 6)
#define RT5651_LR_MUTE          (RT5651_L_MUTE | RT5651_R_MUTE)
#define RT5651_OUT_0DB          0x0808      /* out_vol_tlv field 8 = 0 dB */
#define RT5651_BST_SFT1         12
#define RT5651_BST_SFT2         8
#define RT5651_IN_DF2           (1 << 6)    /* IN1_IN2: IN2 differential */
#define RT5651_BST_24DB         2
/* PWR_DIG1 */
#define RT5651_PWR_I2S1         (1 << 15)
#define RT5651_PWR_I2S2         (1 << 14)
#define RT5651_PWR_DAC_L1       (1 << 12)
#define RT5651_PWR_DAC_R1       (1 << 11)
#define RT5651_PWR_ADC_L        (1 << 2)
#define RT5651_PWR_ADC_R        (1 << 1)
/* PWR_DIG2 */
#define RT5651_PWR_ADC_STO1_F   (1 << 15)
#define RT5651_PWR_DAC_STO1_F   (1 << 11)
#define RT5651_PWR_DAC_STO2_F   (1 << 10)
/* PWR_ANLG1 */
#define RT5651_PWR_VREF1        (1 << 15)
#define RT5651_PWR_FV1          (1 << 14)
#define RT5651_PWR_MB           (1 << 13)
#define RT5651_PWR_LM           (1 << 12)
#define RT5651_PWR_BG           (1 << 11)
#define RT5651_PWR_HP_L         (1 << 7)
#define RT5651_PWR_HP_R         (1 << 6)
#define RT5651_PWR_HA           (1 << 5)
#define RT5651_PWR_VREF2        (1 << 4)
#define RT5651_PWR_FV2          (1 << 3)
#define RT5651_PWR_LDO          (1 << 2)
#define RT5651_PWR_LDO_DVO_MASK (3 << 0)
#define RT5651_PWR_LDO_DVO_1_2V 2
/* PWR_ANLG2 */
#define RT5651_PWR_BST1         (1 << 15)
#define RT5651_PWR_BST2         (1 << 14)
#define RT5651_PWR_BST3         (1 << 13)
#define RT5651_PWR_MB1          (1 << 11)
#define RT5651_PWR_PLL          (1 << 9)
#define RT5651_PWR_BST1_OP2     (1 << 5)
#define RT5651_PWR_BST2_OP2     (1 << 4)
#define RT5651_PWR_BST3_OP2     (1 << 3)
#define RT5651_PWR_JD_M         (1 << 2)
/* PWR_MIXER */
#define RT5651_PWR_RM_L         (1 << 11)
#define RT5651_PWR_RM_R         (1 << 10)
/* STO1_ADC_MIXER */
#define RT5651_M_STO1_ADC_L1    (1 << 14)
#define RT5651_M_STO1_ADC_L2    (1 << 13)
#define RT5651_STO1_ADC_1_SRC_ADC (1 << 12)
#define RT5651_STO1_ADC_2_SRC_DACMIX (1 << 11)  /* 0 = DMIC */
#define RT5651_M_STO1_ADC_R1    (1 << 6)
#define RT5651_M_STO1_ADC_R2    (1 << 5)
/* AD_DA_MIXER */
#define RT5651_M_ADCMIX_L       (1 << 15)
#define RT5651_M_IF1_DAC_L      (1 << 14)
#define RT5651_M_ADCMIX_R       (1 << 7)
#define RT5651_M_IF1_DAC_R      (1 << 6)
/* STO_DAC_MIXER: DAC L1 14, DAC L2 12, DAC R1->L 9, DAC R1 6, DAC R2 4, DAC L1->R 1 (mutes) */
#define RT5651_M_DAC_L1_MIXL    (1 << 14)
#define RT5651_M_DAC_L2_MIXL    (1 << 12)
#define RT5651_M_DAC_R1_MIXR    (1 << 6)
#define RT5651_M_DAC_R2_MIXR    (1 << 4)
/* DAC2_CTRL */
#define RT5651_M_DAC_L2_VOL     (1 << 13)
#define RT5651_M_DAC_R2_VOL     (1 << 12)
#define RT5651_SEL_DAC_L2_IF2   (1 << 11)
#define RT5651_SEL_DAC_R2_IF2   (1 << 10)
/* REC_x2_MIXER mutes */
#define RT5651_M_BST3_RM        (1 << 3)
#define RT5651_M_BST2_RM        (1 << 2)
#define RT5651_M_BST1_RM        (1 << 1)
/* HPO_MIXER */
#define RT5651_M_DAC1_HM        (1 << 14)
#define RT5651_M_HPVOL_HM       (1 << 13)
/* LOUT_MIXER */
#define RT5651_M_DAC_L1_LM      (1 << 15)
#define RT5651_M_DAC_R1_LM      (1 << 14)
/* I2Sx_SDP */
#define RT5651_I2S_MS_S         (1 << 15)
#define RT5651_I2S_DL_16        (0 << 2)
#define RT5651_I2S_DL_24        (2 << 2)
/* ADDA_CLK1 */
#define RT5651_I2S_PD1_SFT      12
#define RT5651_I2S_BCLK_MS2     (1 << 11)
#define RT5651_I2S_PD2_SFT      8
/* DMIC */
#define RT5651_DMIC_1_EN        (1 << 15)
#define RT5651_DMIC_CLK_MASK    (7 << 5)
#define RT5651_DMIC_CLK_SFT     5
/* GLB_CLK */
#define RT5651_SCLK_SRC_MASK    (3 << 14)
#define RT5651_SCLK_SRC_PLL1    (1 << 14)
#define RT5651_SCLK_SRC_RCCLK   (2 << 14)
#define RT5651_PLL1_SRC_MASK    (3 << 12)
#define RT5651_PLL1_SRC_MCLK    (0 << 12)
#define RT5651_PLL1_SRC_BCLK1   (1 << 12)
#define RT5651_PLL_N_SFT        7
#define RT5651_PLL_M_SFT        12
#define RT5651_PLL_M_BP         (1 << 11)
/* PLL_MODE_2 ASRC supplies, routed unconditionally in rt5651_dapm_routes */
#define RT5651_I2S1_ASRC        (1 << 15)
#define RT5651_I2S2_ASRC        (1 << 14)
#define RT5651_STO1_DAC_ASRC    (1 << 13)
#define RT5651_STO2_DAC_ASRC    (1 << 12)
#define RT5651_ADC_ASRC         (1 << 11)
/* DEPOP */
#define RT5651_HP_CP_MASK       (1 << 3)
#define RT5651_HP_CP_PU         (1 << 3)
#define RT5651_HP_CP_PD         (0 << 3)
#define RT5651_HP_SG_MASK       (1 << 2)
#define RT5651_HP_SG_EN         (1 << 2)
#define RT5651_HP_SG_DIS        (0 << 2)
#define RT5651_HP_CB_MASK       (1 << 0)
#define RT5651_HP_CB_PU         (1 << 0)
#define RT5651_DEPOP_MASK       (1 << 13)
#define RT5651_DEPOP_AUTO       (0 << 13)
#define RT5651_DEPOP_MAN        (1 << 13)
#define RT5651_DIG_DP_MASK      (1 << 6)
#define RT5651_DIG_DP_EN        (1 << 6)
#define RT5651_CP_FQ_MASK       0x0777
#define RT5651_CP_FQ_VAL        ((7 << 8) | (3 << 4) | 7)   /* 192 kHz / 12 kHz / 192 kHz */
#define RT5651_PM_HP_MASK       (3 << 8)
#define RT5651_PM_HP_HV         (2 << 8)
/* GPIO_CTRL1 */
#define RT5651_GP2_PIN_DMIC1_SCL (1 << 14)

#define CHT_LPE_PCI_ID          0x22A8      /* Cherry Trail / Braswell LPE audio */

enum { MIC_HS_IN3, MIC_IN2, MIC_IN1, MIC_IN1_IN2, MIC_DMIC, MIC_HS_IN2, MIC_IN2_DIFF, MIC_MAX };

typedef struct {
    ULONG   Sysclk;
    USHORT  PllSrc;
    UCHAR   Aif2;
    UCHAR   Mic;
    UCHAR   PdIdx;
    UCHAR   DmicClk;
    UCHAR   ExtAmp;         /* drive GpioIo 0 as external speaker amplifier enable */
    UCHAR   Bst;            /* bit 0 BST1, 1 BST2, 2 BST3 */
} RT5651_PRIV;

typedef char RT5651_PRIV_FITS[(sizeof(RT5651_PRIV) <= sizeof(((SST_CODEC *)0)->Priv)) ? 1 : -1];

#define PRIV(c) ((RT5651_PRIV *)(c)->Priv)

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
    BOOLEAN ok = CWr(c, RT5651_PRIV_INDEX, idx);
    ok &= CWr(c, RT5651_PRIV_DATA, val);
    return ok;
}

static BOOLEAN PrUpd(SST_CODEC *c, ULONG idx, ULONG mask, ULONG val)
{
    ULONG v;
    if (!CWr(c, RT5651_PRIV_INDEX, idx) || !CRd(c, RT5651_PRIV_DATA, &v))
        return FALSE;
    return PrWr(c, idx, (v & ~mask) | (val & mask));
}

static BOOLEAN Rt5651Probe(SST_CODEC *c)
{
    ULONG v;
    return CRd(c, RT5651_DEVICE_ID, &v) && v == RT5651_DEVICE_ID_VALUE;
}

/* rt5651_set_bias_level(SND_SOC_BIAS_OFF) */
static void BiasOff(SST_CODEC *c)
{
    CWr(c, RT5651_D_MISC, 0x0010);
    CWr(c, RT5651_PWR_DIG1, 0x0000);
    CWr(c, RT5651_PWR_DIG2, 0x0000);
    CWr(c, RT5651_PWR_VOL, 0x0000);
    CWr(c, RT5651_PWR_MIXER, 0x0000);
    CUpd(c, RT5651_PWR_ANLG1, 0xffff & ~RT5651_PWR_LDO_DVO_MASK, 0);   /* keep the LDO voltage */
    CUpd(c, RT5651_PWR_ANLG2, 0xffff & ~(RT5651_PWR_PLL | RT5651_PWR_JD_M), 0);
}

/* rt5651_set_bias_level(OFF -> STANDBY); PREPARE only touches D_MISC with ASRC (unused) */
static void BiasOn(SST_CODEC *c)
{
    ULONG m = RT5651_PWR_VREF1 | RT5651_PWR_MB | RT5651_PWR_BG | RT5651_PWR_VREF2;

    CUpd(c, RT5651_PWR_ANLG1, m, m);
    CDelay(10);
    CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_FV1 | RT5651_PWR_FV2, RT5651_PWR_FV1 | RT5651_PWR_FV2);
    CUpd(c, RT5651_D_MISC, 0x1, 0x1);
}

/* "Platform Clock" PRE_PMU: rt5651_set_dai_sysclk(PLL1) */
static void ClockOn(SST_CODEC *c)
{
    CUpd(c, RT5651_PWR_ANLG2, RT5651_PWR_PLL, RT5651_PWR_PLL);
    CUpd(c, RT5651_GLB_CLK, RT5651_SCLK_SRC_MASK, RT5651_SCLK_SRC_PLL1);
}

/* "Platform Clock" POST_PMD: sysclk to the RC clock */
static void ClockOff(SST_CODEC *c)
{
    CUpd(c, RT5651_PWR_ANLG2, RT5651_PWR_PLL, 0);
    CUpd(c, RT5651_GLB_CLK, RT5651_SCLK_SRC_MASK, RT5651_SCLK_SRC_RCCLK);
}

static ULONG I2sBits(SST_CODEC *c)
{
    return RT5651_PWR_I2S1 | (PRIV(c)->Aif2 ? RT5651_PWR_I2S2 : 0);
}

static ULONG I2sAsrc(SST_CODEC *c)
{
    return RT5651_I2S1_ASRC | (PRIV(c)->Aif2 ? RT5651_I2S2_ASRC : 0);
}

/* rt5651_amp_power_event POST_PMU ("Amp Power" = PWR_HA) */
static void AmpPowerOn(SST_CODEC *c)
{
    CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_HA, RT5651_PWR_HA);   /* the supply's own bit */
    PrUpd(c, RT5651_PR_CHPUMP_INT_REG1, 0x0700, 0x0200);
    CUpd(c, RT5651_DEPOP_M2, RT5651_DEPOP_MASK, RT5651_DEPOP_MAN);
    CUpd(c, RT5651_DEPOP_M1, RT5651_HP_CP_MASK | RT5651_HP_SG_MASK | RT5651_HP_CB_MASK,
         RT5651_HP_CP_PU | RT5651_HP_SG_DIS | RT5651_HP_CB_PU);
    PrWr(c, RT5651_PR_HP_DCC_INT1, 0x9f00);
    CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_FV1 | RT5651_PWR_FV2, 0);
    CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_HA, RT5651_PWR_HA);
    CDelay(10);
    CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_FV1 | RT5651_PWR_FV2, RT5651_PWR_FV1 | RT5651_PWR_FV2);
}

/* rt5651_hp_event POST_PMU: headphone unmute sequence */
static void HpUnmuteSeq(SST_CODEC *c)
{
    CUpd(c, RT5651_DEPOP_M2, RT5651_DEPOP_MASK | RT5651_DIG_DP_MASK, RT5651_DEPOP_AUTO | RT5651_DIG_DP_EN);
    CUpd(c, RT5651_CHARGE_PUMP, RT5651_PM_HP_MASK, RT5651_PM_HP_HV);
    CUpd(c, RT5651_DEPOP_M3, RT5651_CP_FQ_MASK, RT5651_CP_FQ_VAL);
    PrWr(c, RT5651_PR_MAMP_INT_REG2, 0x1c00);
    CUpd(c, RT5651_DEPOP_M1, RT5651_HP_CP_MASK | RT5651_HP_SG_MASK, RT5651_HP_CP_PD | RT5651_HP_SG_EN);
    PrUpd(c, RT5651_PR_CHPUMP_INT_REG1, 0x0700, 0x0400);
}

static void PlayUp(SST_CODEC *c)
{
    RT5651_PRIV *p = PRIV(c);
    ULONG v;

    /* supplies: I2S, DAC filters (both feed the stereo DAC mixer), DAC power, HP L/R amp,
     * "Amp Power"; ASRC supplies (subseq 1); "Ext Amp Power" (machine) */
    CUpd(c, RT5651_PWR_DIG1, I2sBits(c), I2sBits(c));
    v = RT5651_PWR_DAC_STO1_F | RT5651_PWR_DAC_STO2_F;
    CUpd(c, RT5651_PWR_DIG2, v, v);
    v = RT5651_PWR_DAC_L1 | RT5651_PWR_DAC_R1;
    CUpd(c, RT5651_PWR_DIG1, v, v);
    v = RT5651_PWR_HP_L | RT5651_PWR_HP_R;
    CUpd(c, RT5651_PWR_ANLG1, v, v);
    AmpPowerOn(c);
    v = I2sAsrc(c) | RT5651_STO1_DAC_ASRC | RT5651_STO2_DAC_ASRC;
    CUpd(c, RT5651_PLL_MODE_2, v, v);
    if (p->ExtAmp)
        CGpio(c, 0, TRUE);
    /* mixer: LOUT MIX power */
    CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_LM, RT5651_PWR_LM);
    /* PGA: "HP Amp" */
    HpUnmuteSeq(c);
    /* autodisable kcontrols: "HPO L/R Playback", "LOUT L/R Playback" */
    CUpd(c, RT5651_HP_VOL, RT5651_LR_MUTE, 0);
    CUpd(c, RT5651_LOUT_CTRL1, RT5651_LR_MUTE, 0);
    CDelay(80);                                 /* "HP Post" */
}

static void PlayDown(SST_CODEC *c)
{
    RT5651_PRIV *p = PRIV(c);
    ULONG v;

    CUpd(c, RT5651_HP_VOL, RT5651_LR_MUTE, RT5651_LR_MUTE);
    CUpd(c, RT5651_LOUT_CTRL1, RT5651_LR_MUTE, RT5651_LR_MUTE);
    CDelay(70);                                 /* "HP Amp" PRE_PMD */
    CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_LM, 0);
    if (p->ExtAmp)
        CGpio(c, 0, FALSE);                     /* "Ext Amp Power" PRE_PMD */
    v = RT5651_PWR_HA | RT5651_PWR_HP_L | RT5651_PWR_HP_R;
    CUpd(c, RT5651_PWR_ANLG1, v, 0);
    CUpd(c, RT5651_PWR_DIG1, RT5651_PWR_DAC_L1 | RT5651_PWR_DAC_R1, 0);
    CUpd(c, RT5651_PWR_DIG2, RT5651_PWR_DAC_STO1_F | RT5651_PWR_DAC_STO2_F, 0);
    CUpd(c, RT5651_PLL_MODE_2, RT5651_STO1_DAC_ASRC | RT5651_STO2_DAC_ASRC, 0);
    if (!c->RecOn) {
        CUpd(c, RT5651_PLL_MODE_2, I2sAsrc(c), 0);
        CUpd(c, RT5651_PWR_DIG1, I2sBits(c), 0);
    }
}

static void RecUp(SST_CODEC *c)
{
    RT5651_PRIV *p = PRIV(c);
    ULONG v;

    /* supplies: I2S, stereo1 ADC filter, LDO, micbias1, ADC L/R power, DMIC CLK; ASRC */
    CUpd(c, RT5651_PWR_DIG1, I2sBits(c), I2sBits(c));
    CUpd(c, RT5651_PWR_DIG2, RT5651_PWR_ADC_STO1_F, RT5651_PWR_ADC_STO1_F);
    if (p->Mic == MIC_DMIC) {
        CUpd(c, RT5651_DMIC, RT5651_DMIC_CLK_MASK, (ULONG)p->DmicClk << RT5651_DMIC_CLK_SFT);
        CUpd(c, RT5651_DMIC, RT5651_DMIC_1_EN, RT5651_DMIC_1_EN);
    } else {
        CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_LDO, RT5651_PWR_LDO);
        CUpd(c, RT5651_PWR_ANLG2, RT5651_PWR_MB1, RT5651_PWR_MB1);
        v = RT5651_PWR_ADC_L | RT5651_PWR_ADC_R;
        CUpd(c, RT5651_PWR_DIG1, v, v);
    }
    v = I2sAsrc(c) | RT5651_ADC_ASRC;
    CUpd(c, RT5651_PLL_MODE_2, v, v);
    if (p->Mic == MIC_DMIC)
        return;
    /* mixer: RECMIX; PGA: boost stage(s), then their OP2 bit (rt5651_bstN_event POST_PMU) */
    v = RT5651_PWR_RM_L | RT5651_PWR_RM_R;
    CUpd(c, RT5651_PWR_MIXER, v, v);
    v = ((p->Bst & 1) ? RT5651_PWR_BST1 : 0) | ((p->Bst & 2) ? RT5651_PWR_BST2 : 0) |
        ((p->Bst & 4) ? RT5651_PWR_BST3 : 0);
    CUpd(c, RT5651_PWR_ANLG2, v, v);
    v = ((p->Bst & 1) ? RT5651_PWR_BST1_OP2 : 0) | ((p->Bst & 2) ? RT5651_PWR_BST2_OP2 : 0) |
        ((p->Bst & 4) ? RT5651_PWR_BST3_OP2 : 0);
    CUpd(c, RT5651_PWR_ANLG2, v, v);
}

static void RecDown(SST_CODEC *c)
{
    CUpd(c, RT5651_PWR_ANLG2, RT5651_PWR_BST1_OP2 | RT5651_PWR_BST2_OP2 | RT5651_PWR_BST3_OP2, 0);
    CUpd(c, RT5651_PWR_ANLG2, RT5651_PWR_BST1 | RT5651_PWR_BST2 | RT5651_PWR_BST3, 0);
    CUpd(c, RT5651_PWR_MIXER, RT5651_PWR_RM_L | RT5651_PWR_RM_R, 0);
    CUpd(c, RT5651_PWR_DIG1, RT5651_PWR_ADC_L | RT5651_PWR_ADC_R, 0);
    CUpd(c, RT5651_DMIC, RT5651_DMIC_1_EN, 0);
    CUpd(c, RT5651_PWR_ANLG2, RT5651_PWR_MB1, 0);
    CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_LDO, 0);
    CUpd(c, RT5651_PWR_DIG2, RT5651_PWR_ADC_STO1_F, 0);
    CUpd(c, RT5651_PLL_MODE_2, RT5651_ADC_ASRC, 0);
    if (!c->PlayOn) {
        CUpd(c, RT5651_PLL_MODE_2, I2sAsrc(c), 0);
        CUpd(c, RT5651_PWR_DIG1, I2sBits(c), 0);
    }
}

static BOOLEAN Rt5651Init(SST_CODEC *c)
{
    RT5651_PRIV *p = PRIV(c);
    BOOLEAN ok = TRUE;
    ULONG v, fin, sdp, recm, adcm;
    LONG pd, dm;
    RL_PLL pll;

    memset(p, 0, sizeof(*p));
    p->Mic = (UCHAR)(c->Mic < MIC_MAX ? c->Mic : MIC_HS_IN3);
    p->Aif2 = c->Port == 0;
    p->Sysclk = c->Rate * 512;
    p->ExtAmp = c->NGpio > 0 && c->Sst && c->Sst->P && c->Sst->P->Dev == CHT_LPE_PCI_ID;
    switch (p->Mic) {
    case MIC_HS_IN3:    p->Bst = 4; break;
    case MIC_IN1:       p->Bst = 1; break;
    case MIC_IN1_IN2:   p->Bst = 3; break;
    case MIC_DMIC:      p->Bst = 0; break;
    default:            p->Bst = 2; break;      /* IN2P (single-ended or differential) */
    }

    if (p->Aif2 && c->Mclk) {
        fin = c->Mclk;
        p->PllSrc = RT5651_PLL1_SRC_MCLK;
    } else if (c->Bclk >= RL_PLL_INP_MIN) {
        fin = c->Bclk;
        p->PllSrc = RT5651_PLL1_SRC_BCLK1;
    } else {
        fin = c->Mclk;
        p->PllSrc = RT5651_PLL1_SRC_MCLK;
    }
    if (!RlPllCalc(fin, p->Sysclk, &pll)) {
        LOGE("RT5651: no usable PLL reference (BCLK %u MCLK %u)\n", c->Bclk, c->Mclk);
        return FALSE;
    }
    pd = RlClkInfo(p->Sysclk, c->Rate);
    p->PdIdx = (UCHAR)(pd < 0 ? 1 : pd);
    dm = RlDmicClk(p->Sysclk / s_RlPd[p->PdIdx]);
    p->DmicClk = (UCHAR)(dm < 0 ? 4 : dm);

    if (p->ExtAmp)
        CGpio(c, 0, FALSE);                     /* GPIOD_OUT_LOW */

    /* rt5651_i2c_probe: reset, init_list */
    if (!CWr(c, RT5651_RESET, 0))
        return FALSE;
    CDelay(1);
    ok &= PrWr(c, 0x3d, 0x3e00);
    /* rt5651_probe: LDO 1.2 V, bias off, properties */
    ok &= CUpd(c, RT5651_PWR_ANLG1, RT5651_PWR_LDO_DVO_MASK, RT5651_PWR_LDO_DVO_1_2V);
    BiasOff(c);
    if (p->Mic == MIC_IN2_DIFF)
        ok &= CUpd(c, RT5651_IN1_IN2, RT5651_IN_DF2, RT5651_IN_DF2);
    if (p->Mic == MIC_DMIC)                     /* "realtek,dmic-en"; data pin IN1P is the default */
        ok &= CUpd(c, RT5651_GPIO_CTRL1, RT5651_GP2_PIN_DMIC1_SCL, RT5651_GP2_PIN_DMIC1_SCL);

    /* byt_rt5651_init: RC clock until a stream runs; PLL1 (rt5651_set_dai_pll) */
    ok &= CUpd(c, RT5651_GLB_CLK, RT5651_SCLK_SRC_MASK | RT5651_PLL1_SRC_MASK, RT5651_SCLK_SRC_RCCLK | p->PllSrc);
    ok &= CWr(c, RT5651_PLL_CTRL1, ((ULONG)pll.N << RT5651_PLL_N_SFT) | (ULONG)pll.K);
    ok &= CWr(c, RT5651_PLL_CTRL2, ((ULONG)(pll.MBp ? 0 : pll.M) << RT5651_PLL_M_SFT) |
              (pll.MBp ? RT5651_PLL_M_BP : 0));

    /* DAI: slave, I2S, normal polarity, word length; pre-dividers (rt5651_hw_params) */
    if (c->Slots != 2)
        LOGE("RT5651: %u slots per frame, the codec is set up for 2-slot I2S\n", c->Slots);
    sdp = RT5651_I2S_MS_S | (c->Valid > 16 ? RT5651_I2S_DL_24 : RT5651_I2S_DL_16);
    ok &= CWr(c, RT5651_I2S1_SDP, sdp);
    ok &= CWr(c, RT5651_I2S2_SDP, sdp);
    ok &= CUpd(c, RT5651_ADDA_CLK1, (7 << RT5651_I2S_PD1_SFT) | RT5651_I2S_BCLK_MS2 | (7 << RT5651_I2S_PD2_SFT),
               ((ULONG)p->PdIdx << RT5651_I2S_PD1_SFT) | ((ULONG)p->PdIdx << RT5651_I2S_PD2_SFT));

    /* playback routing: IF1 -> DAC MIX -> stereo DAC mixer (+ DAC L2/R2 = IF2 on SSP0) -> DAC1 */
    ok &= CWr(c, RT5651_AD_DA_MIXER, RT5651_M_ADCMIX_L | RT5651_M_ADCMIX_R);
    v = 0x5252 & ~(RT5651_M_DAC_L1_MIXL | RT5651_M_DAC_R1_MIXR);
    if (p->Aif2)
        v &= ~(RT5651_M_DAC_L2_MIXL | RT5651_M_DAC_R2_MIXR);
    ok &= CWr(c, RT5651_STO_DAC_MIXER, v);
    ok &= CWr(c, RT5651_DAC2_CTRL, RT5651_SEL_DAC_L2_IF2 | RT5651_SEL_DAC_R2_IF2);
    ok &= CWr(c, RT5651_DAC1_DIG_VOL, 0xafaf); /* 0 dB */
    ok &= CWr(c, RT5651_DAC2_DIG_VOL, 0xafaf);
    ok &= CWr(c, RT5651_HPO_MIXER, RT5651_M_HPVOL_HM);          /* HPO MIX <- DAC1 */
    ok &= CWr(c, RT5651_LOUT_MIXER, 0xf000 & ~(RT5651_M_DAC_L1_LM | RT5651_M_DAC_R1_LM));
    ok &= CWr(c, RT5651_HP_VOL, RT5651_LR_MUTE | RT5651_OUT_0DB);
    ok &= CWr(c, RT5651_LOUT_CTRL1, RT5651_LR_MUTE | RT5651_OUT_0DB);

    /* capture routing: boost +24 dB, RECMIX <- BSTx, stereo1 ADC (IF2 ADC = IF1 ADC1 default) */
    ok &= CUpd(c, RT5651_IN1_IN2, 0xff00, (RT5651_BST_24DB << RT5651_BST_SFT1) | (RT5651_BST_24DB << RT5651_BST_SFT2));
    ok &= CUpd(c, RT5651_IN3, 0xf << RT5651_BST_SFT1, RT5651_BST_24DB << RT5651_BST_SFT1);
    recm = 0x006f;
    if (p->Bst & 1)
        recm &= ~RT5651_M_BST1_RM;
    if (p->Bst & 2)
        recm &= ~RT5651_M_BST2_RM;
    if (p->Bst & 4)
        recm &= ~RT5651_M_BST3_RM;
    ok &= CWr(c, RT5651_REC_L2_MIXER, recm);
    ok &= CWr(c, RT5651_REC_R2_MIXER, recm);
    if (p->Mic == MIC_DMIC)
        adcm = RT5651_M_STO1_ADC_L1 | RT5651_M_STO1_ADC_R1 | RT5651_STO1_ADC_1_SRC_ADC;
    else
        adcm = RT5651_M_STO1_ADC_L2 | RT5651_M_STO1_ADC_R2 | RT5651_STO1_ADC_1_SRC_ADC |
               RT5651_STO1_ADC_2_SRC_DACMIX;
    ok &= CWr(c, RT5651_STO1_ADC_MIXER, adcm);
    ok &= CWr(c, RT5651_ADC_DIG_VOL, 0x2f2f);   /* 0 dB, unmuted */

    if (!ok)
        return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("RT5651: PLL %s %u -> %u N=%d K=%d M=%d%s, mic mode %u, AIF%s, ext amp %u\n",
         p->PllSrc == RT5651_PLL1_SRC_MCLK ? "MCLK" : "BCLK1", fin, p->Sysclk, pll.N, pll.K,
         pll.MBp ? -1 : pll.M, pll.KBp ? " (K bypass)" : "", p->Mic, p->Aif2 ? "1+2" : "1", p->ExtAmp);
    return TRUE;
}

static void Rt5651Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
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

/* DAC1/DAC2 digital volume: 0xaf = 0 dB, 0.375 dB steps; mute in the IF1 -> DAC MIX switch
 * (AD_DA_MIXER) and DAC2_CTRL */
static void Rt5651Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    ULONG v = (CVolSteps(left, 175) << 8) | CVolSteps(right, 175);

    CWr(c, RT5651_DAC1_DIG_VOL, v);
    CWr(c, RT5651_DAC2_DIG_VOL, v);
    CUpd(c, RT5651_AD_DA_MIXER, RT5651_M_IF1_DAC_L | RT5651_M_IF1_DAC_R,
         mute ? RT5651_M_IF1_DAC_L | RT5651_M_IF1_DAC_R : 0);
    CUpd(c, RT5651_DAC2_CTRL, RT5651_M_DAC_L2_VOL | RT5651_M_DAC_R2_VOL,
         mute ? RT5651_M_DAC_L2_VOL | RT5651_M_DAC_R2_VOL : 0);
}

static void Rt5651Shutdown(SST_CODEC *c)
{
    if (c->PlayOn)
        PlayDown(c);
    if (c->RecOn)
        RecDown(c);
    CUpd(c, RT5651_HP_VOL, RT5651_LR_MUTE, RT5651_LR_MUTE);
    CUpd(c, RT5651_LOUT_CTRL1, RT5651_LR_MUTE, RT5651_LR_MUTE);
    if (PRIV(c)->ExtAmp)
        CGpio(c, 0, FALSE);
    ClockOff(c);
    BiasOff(c);
    CWr(c, RT5651_RESET, 0);
}

const CODEC_DRV g_DrvRt5651 = { "RT5651", "10EC5651\0" "10EC5640\0", CODEC_HEADSET, 1, 2,
                                Rt5651Probe, Rt5651Init, Rt5651Stream, Rt5651Volume, Rt5651Shutdown };
