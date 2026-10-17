#include "codec.h"

/*
 * Realtek RT286 / RT288 / RT298 (ALC286 / ALC288 / ALC298, I2S variants) and RT274 (ALC274):
 * HD Audio codecs whose verbs are carried over I2C (Linux sound/soc/codecs/rt286.c, rt298.c,
 * rt274.c, rl6347a.c).  Ported from the Linux drivers plus the Intel machine drivers
 * (sound/soc/intel/boards/bdw_rt286.c, sound/soc/intel/avs/boards/rt286.c, rt298.c, rt274.c).
 *
 * Wire protocol (rl6347a_hw_write / rl6347a_hw_read): every access is one 32-bit HDA verb
 * sent big endian, a read is that 4-byte write followed by a 4-byte read (repeated start).
 *   - a "register" is the Linux regmap key VERB_CMD(V, N, D) = N << 20 | V << 8 | D; the
 *     payload is OR-ed into the low 16 bits (12-bit verbs carry 8 payload bits, 4-bit verbs
 *     (stream format, amp gain, coef) 16).
 *   - keys 0x00..0xff are the vendor index registers behind NID 0x20: SET_COEF_INDEX(idx),
 *     then SET_PROC_COEF(val) / GET_PROC_COEF.
 *   - a read turns the SET verb into the GET verb (| 0x80000).
 * The driver therefore uses CRaw only; RegBytes / ValBytes are 4 / 4 so CRd / CWr would emit
 * a plain 4-byte address + 4-byte value, which is NOT this protocol - never use them here.
 *
 * INT343A is listed by both rt286.c and rt298.c (Skylake / Kabylake / Broadwell / Broxton
 * match tables), so one descriptor (g_DrvRt286) serves RT286 / RT288 / RT298 and tells them
 * apart by the AFG vendor id (GET_PARAMETER(root, VENDOR_ID)).  RT274 is INT34C2 / 10EC0274
 * (Cannon Lake / Ice Lake RVPs) and has its own descriptor (g_DrvRt274).
 *
 * c->Mic:  0 = headset microphone (RT286/298: MIC1 NID 0x18 through the combo jack,
 *              RT274: MIC NID 0x19), MIC pin VREF80 bias, +20 dB pin boost;
 *          1 = codec digital microphone on the DMIC1 pins (NID 0x12).
 */

#define VERB(v, n, d)   (((ULONG)(n) << 20) | ((ULONG)(v) << 8) | (ULONG)(d))

/* HDA verbs (sound/hda_verbs.h) */
#define V_PARAMETERS        0xf00
#define V_STREAM_FORMAT     0x200
#define V_AMP_GAIN_MUTE     0x300
#define V_PROC_COEF         0x400
#define V_COEF_INDEX        0x500
#define V_CONNECT_SEL       0x701
#define V_POWER_STATE       0x705
#define V_CHANNEL_STREAMID  0x706
#define V_PIN_WIDGET_CTL    0x707
#define V_GET_PIN_SENSE     0xf09
#define V_EAPD_BTLENABLE    0x70c
#define V_GPIO_DATA         0x715
#define V_GPIO_MASK         0x716
#define V_GPIO_DIRECTION    0x717
#define V_CONFIG_DEFAULT_3  0x71f
#define V_CODEC_RESET       0x7ff

#define PAR_VENDOR_ID       0x00
#define PAR_AMP_OUT_CAP     0x12
#define PWR_D0              0
#define PWR_D1              1
#define PWR_D3              3

#define VENDOR_ID_REG       VERB(V_PARAMETERS, 0x00, PAR_VENDOR_ID)
#define COEF_INDEX_20       VERB(V_COEF_INDEX, 0x20, 0)         /* RL6347A_COEF_INDEX */
#define PROC_COEF_20        VERB(V_PROC_COEF, 0x20, 0)          /* RL6347A_PROC_COEF */

#define ID_RT274            0x10ec0274
#define ID_RT286            0x10ec0286
#define ID_RT288            0x10ec0288
#define ID_RT298            0x10ec0298

/* c->Priv[] */
#define P_CHIP      0       /* CH_xxx */
#define P_ID        1       /* AFG vendor id */
#define P_MCLKMODE  2       /* RT286/298: system clock taken from MCLK directly ("MCLK MODE" supply) */
#define P_CBJ       3       /* RT286/298: combo jack mode (pdata.cbj_en) */
#define P_CBJTYPE   4       /* RT286/298: CBJ_CTRL1[15:10] found by the headset detection, 0 = not yet */
#define P_PLAY      5       /* our own path state (independent of the core's PlayOn / RecOn) */
#define P_REC       6
#define P_HPGAIN    7       /* pin output amp gain for "0 dB" (from AMP_OUT_CAP offset) */
#define P_SPKGAIN   8       /* RT286/298 SPK_OUT, RT274 LINE3 */
#define P_VOLL      9       /* last DAC gain (bit 7 mute) */
#define P_VOLR      10

#define CH_RT286    0
#define CH_RT288    1
#define CH_RT298    2
#define CH_RT274    3

/* ------------------------------------------------------------------ */
/* rl6347a verb transport                                               */
/* ------------------------------------------------------------------ */
static BOOLEAN Hw4(SST_CODEC *c, ULONG reg, ULONG val)
{
    UCHAR b[4];

    b[0] = (UCHAR)(reg >> 24);
    b[1] = (UCHAR)(reg >> 16);
    /* 4-bit verbs have 16 payload bits, 12-bit verbs 8: the unused part of each is 0, so OR them */
    b[2] = (UCHAR)(((reg >> 8) | (val >> 8)) & 0xff);
    b[3] = (UCHAR)(val & 0xff);
    return CRaw(c, b, 4, NULL, 0);
}

/* rl6347a_hw_write */
static BOOLEAN HwWr(SST_CODEC *c, ULONG reg, ULONG val)
{
    if (reg <= 0xff) {
        if (!Hw4(c, COEF_INDEX_20, reg))
            return FALSE;
        reg = PROC_COEF_20;
    }
    return Hw4(c, reg, val);
}

/* rl6347a_hw_read: SET verb key -> GET verb, AMP_GAIN_MUTE index moved to the GET layout */
static BOOLEAN HwRd(SST_CODEC *c, ULONG reg, ULONG *val)
{
    UCHAR w[4], r[4];
    ULONG vid;

    *val = 0;
    if (reg <= 0xff) {
        if (!Hw4(c, COEF_INDEX_20, reg))
            return FALSE;
        reg = PROC_COEF_20;
    }
    reg |= 0x80000;
    vid = (reg >> 8) & 0xfff;
    if ((vid & 0xf00) == 0xb00)
        reg = (reg & ~0xf0fUL) | ((reg >> 8) & 0xf);
    w[0] = (UCHAR)(reg >> 24);
    w[1] = (UCHAR)(reg >> 16);
    w[2] = (UCHAR)(reg >> 8);
    w[3] = (UCHAR)reg;
    if (!CRaw(c, w, 4, r, 4))
        return FALSE;
    *val = ((ULONG)r[0] << 24) | ((ULONG)r[1] << 16) | ((ULONG)r[2] << 8) | r[3];
    return TRUE;
}

static BOOLEAN HwUpd(SST_CODEC *c, ULONG reg, ULONG mask, ULONG val)
{
    ULONG old, nv;

    if (!HwRd(c, reg, &old))
        return FALSE;
    nv = (old & ~mask) | (val & mask);
    if (nv == old)
        return TRUE;
    return HwWr(c, reg, nv);
}

/* output amp gain value for 0 dB: AMP_OUT_CAP offset when the amp has steps, else 0 (mute only) */
static ULONG AmpZero(SST_CODEC *c, ULONG nid)
{
    ULONG v;

    if (!HwRd(c, VERB(V_PARAMETERS, nid, PAR_AMP_OUT_CAP), &v) || v == 0xffffffff)
        return 0;
    return ((v >> 8) & 0x7f) ? (v & 0x7f) : 0;
}

static ULONG ChipOf(ULONG id)
{
    switch (id) {
    case ID_RT286: return CH_RT286;
    case ID_RT288: return CH_RT288;
    case ID_RT298: return CH_RT298;
    case ID_RT274: return CH_RT274;
    }
    return 0xff;
}

static const char *ChipName(ULONG ch)
{
    switch (ch) {
    case CH_RT286: return "RT286";
    case CH_RT288: return "RT288";
    case CH_RT298: return "RT298";
    }
    return "RT274";
}

/* word length codes from c->Valid */
static ULONG FmtBits(ULONG valid)         /* stream format BITS field [6:4] */
{
    return valid >= 32 ? 4 : valid >= 24 ? 3 : valid >= 20 ? 2 : 1;
}

static ULONG ILen(ULONG valid)            /* I2S_CTRL1 [4:3] data length: 16 / 20 / 24(32) */
{
    return valid >= 24 ? 2 : valid >= 20 ? 1 : 0;
}

/* DAC0 (NID 0x02) output amp: 0..0x7f = -63.5..0 dB in 0.5 dB steps (out_vol_tlv), bit 7 mute */
#define DACL_GAIN   VERB(V_AMP_GAIN_MUTE, 0x02, 0xa000)
#define DACR_GAIN   VERB(V_AMP_GAIN_MUTE, 0x02, 0x9000)
/* ADC0 (NID 0x09) input amp, same scale */
#define ADCL_GAIN   VERB(V_AMP_GAIN_MUTE, 0x09, 0x6000)
#define ADCR_GAIN   VERB(V_AMP_GAIN_MUTE, 0x09, 0x5000)
#define DAC_FORMAT  VERB(V_STREAM_FORMAT, 0x02, 0)
#define ADC_FORMAT  VERB(V_STREAM_FORMAT, 0x09, 0)
#define AFG_POWER   VERB(V_POWER_STATE, 0x01, 0)

#define MIC_BOOST   2       /* MIC pin input amp, 10 dB steps (mic_vol_tlv): +20 dB */

static void RtVolume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    ULONG m = mute ? 0x80 : 0;

    c->Priv[P_VOLL] = m | CVolSteps(left, 0x7f);
    c->Priv[P_VOLR] = m | CVolSteps(right, 0x7f);
    HwWr(c, DACL_GAIN, c->Priv[P_VOLL]);
    HwWr(c, DACR_GAIN, c->Priv[P_VOLR]);
}

/* ================================================================== */
/* RT286 / RT288 / RT298                                                */
/* ================================================================== */

/* index registers (rt286.h / rt298.h) */
#define R_A_BIAS_CTRL2  0x02
#define R_POWER_CTRL1   0x03
#define R_A_BIAS_CTRL3  0x04
#define R_D_FILTER_CTRL 0x05    /* RT298 */
#define R_POWER_CTRL2   0x08
#define R_I2S_CTRL1     0x09
#define R_I2S_CTRL2     0x0a
#define R_CLK_DIV       0x0b
#define R_DC_GAIN       0x0d
#define R_MIC1_DET_CTRL 0x19
#define R_MISC_CTRL1    0x20
#define R_GPIO_CTRL     0x29
#define R_WIND_FILTER   0x46    /* RT298 */
#define R_PLL_CTRL1     0x49
#define R_VAD_CTRL      0x4e    /* RT298 */
#define R_CBJ_CTRL1     0x4f
#define R_CBJ_CTRL2     0x50
#define R_PLL_CTRL      0x63
#define R_DEPOP_CTRL2   0x67
#define R_DEPOP_CTRL3   0x68
#define R_DEPOP_CTRL4   0x69

/* NIDs */
#define N_DAC_OUT1      0x02
#define N_DAC_OUT2      0x03
#define N_ADC_IN2       0x08
#define N_ADC_IN1       0x09
#define N_MIXER_OUT1    0x0c
#define N_DMIC1         0x12
#define N_DMIC2         0x13
#define N_SPK_OUT       0x14
#define N_MIC1          0x18
#define N_HP_OUT        0x21
#define N_MIXER_IN1     0x22
#define N_MIXER_IN2     0x23

#define R_SET_POWER(n)  VERB(V_POWER_STATE, n, 0)
#define R_SPK_MUX       VERB(V_CONNECT_SEL, N_SPK_OUT, 0)
#define R_HPO_MUX       VERB(V_CONNECT_SEL, N_HP_OUT, 0)
#define R_ADC0_MUX      VERB(V_CONNECT_SEL, N_MIXER_IN1, 0)
#define R_ADC1_MUX      VERB(V_CONNECT_SEL, N_MIXER_IN2, 0)
#define R_SET_MIC1      VERB(V_PIN_WIDGET_CTL, N_MIC1, 0)
#define R_SET_PIN_HPO   VERB(V_PIN_WIDGET_CTL, N_HP_OUT, 0)
#define R_SET_PIN_SPK   VERB(V_PIN_WIDGET_CTL, N_SPK_OUT, 0)
#define R_SET_PIN_DMIC1 VERB(V_PIN_WIDGET_CTL, N_DMIC1, 0)
#define R_SPK_EAPD      VERB(V_EAPD_BTLENABLE, N_SPK_OUT, 0)
#define R_GET_HP_SENSE  VERB(V_GET_PIN_SENSE, N_HP_OUT, 0)
#define R_DMIC2_DEFAULT VERB(V_CONFIG_DEFAULT_3, N_DMIC2, 0)
#define R_MIC_GAIN      VERB(V_AMP_GAIN_MUTE, N_MIC1, 0x7000)
#define R_SPOL_GAIN     VERB(V_AMP_GAIN_MUTE, N_SPK_OUT, 0xa000)
#define R_SPOR_GAIN     VERB(V_AMP_GAIN_MUTE, N_SPK_OUT, 0x9000)
#define R_HPOL_GAIN     VERB(V_AMP_GAIN_MUTE, N_HP_OUT, 0xa000)
#define R_HPOR_GAIN     VERB(V_AMP_GAIN_MUTE, N_HP_OUT, 0x9000)
#define R_F_DAC_SWITCH  VERB(V_AMP_GAIN_MUTE, N_MIXER_OUT1, 0x7000)
#define R_F_RECMIX_SW   VERB(V_AMP_GAIN_MUTE, N_MIXER_OUT1, 0x7100)
#define R_GPIO_MASK     VERB(V_GPIO_MASK, 0x01, 0)
#define R_GPIO_DIR      VERB(V_GPIO_DIRECTION, 0x01, 0)
#define R_GPIO_DATA     VERB(V_GPIO_DATA, 0x01, 0)

/* ADC mux values (rt286_adc_values: Mic, RECMIX, Dmic) */
#define R_ADC_SEL_MIC   0
#define R_ADC_SEL_DMIC  5

/* rt286_index_def / rt298_index_def: index register reset values ("restore codec default") */
static const ULONG s_Rt286Idx[] = {
    0x01, 0xaaaa, 0x02, 0x8aaa, 0x03, 0x0002, 0x04, 0xaf01, 0x08, 0x000d, 0x09, 0xd810, 0x0a, 0x0120,
    0x0b, 0x0000, 0x0d, 0x2800, 0x0f, 0x0000, 0x19, 0x0a17, 0x20, 0x0020, 0x33, 0x0208, 0x49, 0x0004,
    0x4f, 0x50e9, 0x50, 0x2000, 0x63, 0x2902, 0x67, 0x1111, 0x68, 0x1016, 0x69, 0x273f,
};
static const ULONG s_Rt298Idx[] = {
    0x01, 0xa5a8, 0x02, 0x8e95, 0x03, 0x0002, 0x04, 0xaf67, 0x08, 0x200f, 0x09, 0xd010, 0x0a, 0x0100,
    0x0b, 0x0000, 0x0d, 0x2800, 0x0f, 0x0022, 0x19, 0x0217, 0x20, 0x0020, 0x33, 0x0208, 0x46, 0x0300,
    0x49, 0x4004, 0x4f, 0x50c9, 0x50, 0x3000, 0x63, 0x1b02, 0x67, 0x1111, 0x68, 0x1016, 0x69, 0x273f,
};
/* rt286_reg (identical in rt298.c): verb register defaults written at probe */
static const ULONG s_Rt286Reg[] = {
    0x00170500, 0x00000400, 0x00220000, 0x00000031, 0x00239000, 0x0000007f, 0x0023a000, 0x0000007f,
    0x00270500, 0x00000400, 0x00370500, 0x00000400, 0x00830000, 0x000000c3, 0x00870500, 0x00000400,
    0x00920000, 0x00000031, 0x00930000, 0x000000c3, 0x00935000, 0x000000c3, 0x00936000, 0x000000c3,
    0x00970500, 0x00000400, 0x00b37000, 0x00000097, 0x00b37200, 0x00000097, 0x00b37300, 0x00000097,
    0x00c37000, 0x00000000, 0x00c37100, 0x00000080, 0x01270500, 0x00000400, 0x01270700, 0x00000000,
    0x01370500, 0x00000400, 0x01371f00, 0x411111f0, 0x01439000, 0x00000080, 0x0143a000, 0x00000080,
    0x01470100, 0x00000000, 0x01470500, 0x00000400, 0x01470700, 0x00000000, 0x01470c00, 0x00000000,
    0x01837000, 0x00000000, 0x01870500, 0x00000400, 0x01870700, 0x00000020, 0x02050000, 0x00000000,
    0x02139000, 0x00000080, 0x0213a000, 0x00000080, 0x02170100, 0x00000000, 0x02170500, 0x00000400,
    0x02170700, 0x00000000, 0x02270100, 0x00000000, 0x02370100, 0x00000000,
};
/* rt286_support_power_controls: set to D1 at probe, DAPM moves DAC/ADC/DMIC1 to D0 */
static const UCHAR s_Rt286Pwr[] = {
    N_DAC_OUT1, N_DAC_OUT2, N_ADC_IN1, N_ADC_IN2, N_MIC1, N_DMIC1, N_DMIC2, N_SPK_OUT, N_HP_OUT
};

#define IS298(c)    ((c)->Priv[P_CHIP] == CH_RT298)

static BOOLEAN R286Probe(SST_CODEC *c)
{
    ULONG id;

    if (!HwRd(c, VENDOR_ID_REG, &id))
        return FALSE;
    if (id != ID_RT286 && id != ID_RT288 && id != ID_RT298)
        return FALSE;
    c->Priv[P_ID] = id;
    c->Priv[P_CHIP] = ChipOf(id);
    return TRUE;
}

/*
 * System clock (rt286/rt298_set_dai_sysclk as called by the machine drivers): the codec has no
 * PLL that takes BCLK in Linux; it runs either from its PLL fed by the 19.2 / 24 MHz platform
 * clock on MCLK (RT286_SCLK_S_PLL - bdw_rt286 / avs rt286: 24 MHz, avs rt298: 19.2 MHz except
 * KBL-R RVP 24 MHz) or directly from a 12.288 / 24.576 MHz MCLK (RT286_SCLK_S_MCLK).
 */
static void R286Clocks(SST_CODEC *c)
{
    ULONG mclk = c->Mclk;

    if (mclk == 12288000 || mclk == 24576000) {
        c->Priv[P_MCLKMODE] = 1;
        HwUpd(c, R_I2S_CTRL2, 0x0100, 0x0000);          /* sysclk = MCLK */
        if (mclk == 12288000) {
            HwUpd(c, R_I2S_CTRL2, 0x0008, 0x0000);
            HwUpd(c, R_CLK_DIV, 0xfc1e, 0x0004);
        } else {
            HwUpd(c, R_I2S_CTRL2, 0x0008, 0x0008);
            HwUpd(c, R_CLK_DIV, 0xfc1e, 0x5406);
        }
        /* PLL_CTRL1[5] "MCLK MODE" is a DAPM supply: switched in R286Stream */
        return;
    }
    c->Priv[P_MCLKMODE] = 0;
    HwUpd(c, R_I2S_CTRL2, 0x0100, 0x0100);              /* sysclk = PLL */
    if (!IS298(c))
        HwUpd(c, R_PLL_CTRL, 0x0004, 0x0004);
    HwUpd(c, R_PLL_CTRL1, 0x0020, 0x0000);
    /* I2S_CTRL2[6]: PLL reference 1 = 19.2 MHz, 0 = 24 MHz */
    HwUpd(c, R_I2S_CTRL2, 0x0040, mclk == 19200000 ? 0x0040 : 0x0000);
}

/* DAI: slave, I2S (DSP_A when the SSP runs more than 2 slots), word length, 50 fs flag */
static void R286Dai(SST_CODEC *c)
{
    ULONG fmt = c->Slots > 2 ? 0x2 : 0x0;      /* I2S_CTRL1[9:8]: 0 I2S, 1 LJ, 2 DSP_A, 3 DSP_B */
    ULONG f = (FmtBits(c->Valid) << 4) | 1;     /* 48 kHz base, 2 channels */

    HwUpd(c, R_I2S_CTRL1, 0x0800, 0x0000);                  /* [11] 0: slave (CBC_CFC) */
    HwUpd(c, R_I2S_CTRL1, 0x0300, fmt << 8);
    HwUpd(c, R_I2S_CTRL1, 0x1000, (ULONG)c->Slots * c->Slot == 50 ? 0x1000 : 0);  /* set_bclk_ratio */
    HwUpd(c, R_I2S_CTRL1, 0x0018, ILen(c->Valid) << 3);
    HwWr(c, DAC_FORMAT, f);                                 /* [15] 0: PCM, [14] 0: 48 kHz */
    HwWr(c, ADC_FORMAT, f);
}

/* the shared analog supplies HV / VREF / LDO1 / LDO2 (RT298: + BG_MBIAS / VREF1 / LV), DAPM order */
static void R286SupplyLate(SST_CODEC *c, BOOLEAN on)    /* DAPM subseq 1 and 2 */
{
    if (on) {
        HwUpd(c, R_POWER_CTRL1, 0x1000, 0);             /* "HV" (inverted) */
        if (IS298(c)) {
            HwUpd(c, R_POWER_CTRL2, 0x001e, 0x000e);    /* BG_MBIAS, LDO1, LDO2 on, VREF1 (inverted) on */
            HwUpd(c, R_POWER_CTRL1, 0x2000, 0);         /* "LV" */
        } else {
            HwUpd(c, R_POWER_CTRL2, 0x0004, 0x0004);    /* "LDO1" */
            HwUpd(c, R_POWER_CTRL1, 0x2000, 0);         /* "LDO2" (inverted) */
            HwUpd(c, R_POWER_CTRL2, 0x0038, 0x0008);    /* rt286_ldo2_event POST_PMU */
        }
    } else {
        if (IS298(c)) {
            HwUpd(c, R_POWER_CTRL1, 0x2000, 0x2000);
            HwUpd(c, R_POWER_CTRL1, 0x1000, 0x1000);
            HwUpd(c, R_POWER_CTRL2, 0x001e, 0x0010);
        } else {
            HwUpd(c, R_POWER_CTRL2, 0x0038, 0x0030);    /* rt286_ldo2_event PRE_PMD */
            HwUpd(c, R_POWER_CTRL1, 0x2000, 0x2000);
            HwUpd(c, R_POWER_CTRL1, 0x1000, 0x1000);
            HwUpd(c, R_POWER_CTRL2, 0x0004, 0x0000);
        }
    }
}

/* "MIC1 Input Buffer" supply event (analog mic bias network) */
static void R286MicBuf(SST_CODEC *c, BOOLEAN on)
{
    HwUpd(c, R_A_BIAS_CTRL3, 0xc000, on ? 0x8000 : 0);
    HwUpd(c, R_A_BIAS_CTRL2, 0xc000, on ? 0x8000 : 0);
}

/*
 * Combo jack type (rt286/rt298_jack_detect, cbj_en): with a plug in the jack, MIC1 VREF on,
 * try CBJ_CTRL1 0xd400 then 0xe400 (the two headset pinouts) and keep the one for which
 * CBJ_CTRL2[6:4] reports a microphone.  Run on capture start (supplies HV/VREF/LDO1 are up);
 * the result is kept until the plug is removed.  Costs 350..650 ms the first time.
 */
static void R286Headset(SST_CODEC *c)
{
    ULONG v = 0;

    if (!c->Priv[P_CBJ]) {
        HwWr(c, R_SET_MIC1, 0x24);          /* VREF80 */
        return;
    }
    if (HwRd(c, R_GET_HP_SENSE, &v) && !(v & 0x80000000)) {
        c->Priv[P_CBJTYPE] = 0;
        HwWr(c, R_SET_MIC1, 0x20);
        HwUpd(c, R_CBJ_CTRL1, 0x0400, 0x0000);
        return;
    }
    if (c->Priv[P_CBJTYPE]) {
        HwUpd(c, R_CBJ_CTRL1, 0xfcc0, c->Priv[P_CBJTYPE]);
        HwWr(c, R_SET_MIC1, 0x24);
        return;
    }
    HwUpd(c, R_DC_GAIN, 0x0200, 0x0200);
    if (IS298(c)) {
        HwUpd(c, R_POWER_CTRL1, 0x1001, 0);
        HwUpd(c, R_POWER_CTRL2, 0x0004, 0x0004);
    }
    HwWr(c, R_SET_MIC1, 0x24);
    CDelay(50);
    HwUpd(c, R_CBJ_CTRL1, 0xfcc0, 0xd400);
    CDelay(300);
    HwRd(c, R_CBJ_CTRL2, &v);
    if ((v & 0x0070) == 0x0070) {
        c->Priv[P_CBJTYPE] = 0xd400;
    } else {
        HwUpd(c, R_CBJ_CTRL1, 0xfcc0, 0xe400);
        CDelay(300);
        HwRd(c, R_CBJ_CTRL2, &v);
        if ((v & 0x0070) == 0x0070) {
            c->Priv[P_CBJTYPE] = 0xe400;
        } else {
            HwUpd(c, R_CBJ_CTRL1, 0xfcc0, 0xc400);      /* headphone without microphone */
            c->Priv[P_CBJTYPE] = 0;
        }
    }
    HwUpd(c, R_DC_GAIN, 0x0200, 0x0000);
    LOGI("%s: combo jack %s\n", ChipName(c->Priv[P_CHIP]),
         c->Priv[P_CBJTYPE] == 0xd400 ? "headset (d400)" : c->Priv[P_CBJTYPE] ? "headset (e400)" : "no mic");
}

static void R286AdcGain(SST_CODEC *c, BOOLEAN on)
{
    ULONG g = on ? 0x7f : 0xff;     /* 0 dB, bit 7 mute */

    HwWr(c, ADCL_GAIN, g);
    HwWr(c, ADCR_GAIN, g);
}

static void R286Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN play = c->Priv[P_PLAY] != 0, cap = c->Priv[P_REC] != 0;
    BOOLEAN dmic = c->Mic == 1;

    if (on) {
        BOOLEAN first = !play && !cap;

        if (rec ? cap : play)
            return;
        if (first) {
            R286Clocks(c);
            /* set_bias_level PREPARE (from STANDBY) */
            HwWr(c, AFG_POWER, PWR_D0);
            if (IS298(c)) {
                HwUpd(c, R_DC_GAIN, 0x0200, 0x0200);
                HwUpd(c, 0x52, 0x0080, 0x0000);
                CDelay(20);
                HwUpd(c, R_DC_GAIN, 0x0200, 0x0000);
                HwUpd(c, 0x52, 0x0080, 0x0080);
            } else {
                HwUpd(c, R_DC_GAIN, 0x0200, 0x0200);
            }
        }
        /* supplies, DAPM subseq 0: "MIC1 Input Buffer" (NOPM), "VREF", "MCLK MODE", "HP Power" */
        if (rec && !dmic)
            R286MicBuf(c, TRUE);
        if (first) {
            HwUpd(c, R_POWER_CTRL1, 0x0001, 0);         /* "VREF" (inverted) */
            if (c->Priv[P_MCLKMODE])
                HwUpd(c, R_PLL_CTRL1, 0x0020, 0x0020);
        }
        if (!rec)
            HwWr(c, R_SET_PIN_HPO, 0x40);               /* "HP Power": pin out enable */
        if (first)
            R286SupplyLate(c, TRUE);
        if (rec) {
            if (!dmic)
                R286Headset(c);
            HwWr(c, R_ADC0_MUX, dmic ? R_ADC_SEL_DMIC : R_ADC_SEL_MIC);
            HwWr(c, R_SET_POWER(N_ADC_IN1), PWR_D0);    /* "ADC 0 Mux" */
            if (IS298(c)) {
                ULONG v = 0x200;

                /* rt298_adc_event POST_PMU: no MCLK -> reset the AD filter */
                HwRd(c, R_VAD_CTRL, &v);
                if (!(v & 0x200)) {
                    HwUpd(c, R_D_FILTER_CTRL, 0x2, 0x2);
                    CDelay(10);
                    HwUpd(c, R_D_FILTER_CTRL, 0x2, 0x0);
                }
            }
            if (dmic) {
                HwWr(c, R_SET_POWER(N_DMIC1), PWR_D0);
                HwWr(c, R_SET_PIN_DMIC1, 0x20);         /* rt286_set_dmic1_event POST_PMU */
            }
            R286AdcGain(c, TRUE);
            c->Priv[P_REC] = 1;
        } else {
            HwWr(c, R_SET_PIN_SPK, 0x40);               /* "SPO" switch */
            HwWr(c, R_SPK_EAPD, 0x2);                   /* rt286_spk_event POST_PMU (NOPM, runs first) */
            HwWr(c, R_SET_POWER(N_DAC_OUT1), PWR_D0);   /* "Front" mixer */
            HwWr(c, R_SPOL_GAIN, c->Priv[P_SPKGAIN]);
            HwWr(c, R_SPOR_GAIN, c->Priv[P_SPKGAIN]);
            HwWr(c, R_HPOL_GAIN, c->Priv[P_HPGAIN]);    /* autodisable kcontrols: last */
            HwWr(c, R_HPOR_GAIN, c->Priv[P_HPGAIN]);
            c->Priv[P_PLAY] = 1;
        }
        if (first && !IS298(c)) {
            /* set_bias_level ON */
            CDelay(10);
            HwUpd(c, R_DC_GAIN, 0x0200, 0x0000);
        }
        return;
    }

    if (!(rec ? cap : play))
        return;
    if (rec) {
        R286AdcGain(c, FALSE);                          /* rt298_adc_event PRE_PMD */
        if (dmic) {
            HwWr(c, R_SET_PIN_DMIC1, 0);
            HwWr(c, R_SET_POWER(N_DMIC1), PWR_D1);
        }
        HwWr(c, R_SET_POWER(N_ADC_IN1), PWR_D1);
        if (!dmic)
            HwWr(c, R_SET_MIC1, 0x20);
        c->Priv[P_REC] = 0;
    } else {
        HwWr(c, R_HPOL_GAIN, 0x80);
        HwWr(c, R_HPOR_GAIN, 0x80);
        HwWr(c, R_SPOL_GAIN, 0x80);
        HwWr(c, R_SPOR_GAIN, 0x80);
        HwWr(c, R_SPK_EAPD, 0x0);                       /* rt286_spk_event PRE_PMD */
        HwWr(c, R_SET_POWER(N_DAC_OUT1), PWR_D1);
        HwWr(c, R_SET_PIN_SPK, 0x00);
        c->Priv[P_PLAY] = 0;
    }
    if (!c->Priv[P_PLAY] && !c->Priv[P_REC])
        R286SupplyLate(c, FALSE);
    if (rec && !dmic)
        R286MicBuf(c, FALSE);                           /* POST_PMD */
    if (!c->Priv[P_PLAY] && !c->Priv[P_REC]) {
        HwUpd(c, R_POWER_CTRL1, 0x0001, 0x0001);
        if (c->Priv[P_MCLKMODE])
            HwUpd(c, R_PLL_CTRL1, 0x0020, 0x0000);
    }
    if (!rec)
        HwWr(c, R_SET_PIN_HPO, 0x00);
    if (!c->Priv[P_PLAY] && !c->Priv[P_REC])
        HwWr(c, AFG_POWER, PWR_D3);                     /* set_bias_level STANDBY */
}

static BOOLEAN R286Init(SST_CODEC *c)
{
    ULONG e0 = c->Errors, i, id = 0;
    const ULONG *idx;
    ULONG nidx;

    if (!HwRd(c, VENDOR_ID_REG, &id) || ChipOf(id) > CH_RT298) {
        LOGE("RT286: vendor id %08x\n", id);
        return FALSE;
    }
    c->Priv[P_ID] = id;
    c->Priv[P_CHIP] = ChipOf(id);
    c->Priv[P_PLAY] = c->Priv[P_REC] = 0;
    c->Priv[P_CBJTYPE] = 0;
    /*
     * Combo jack mode: rt286.c enables it for RT288 on Dell and on the Wilson Beach / SKL / KBL
     * RVPs and ThinkPad Helix 2 (all RT286 boards Linux knows), rt298.c on BXT-P / GLK / KBL-R
     * RVPs: every known INT343A board, so it is always on here.
     */
    c->Priv[P_CBJ] = 1;

    /* restore codec default (the chip has no reset verb in Linux) */
    if (IS298(c)) {
        idx = s_Rt298Idx;
        nidx = sizeof(s_Rt298Idx) / sizeof(s_Rt298Idx[0]);
    } else {
        idx = s_Rt286Idx;
        nidx = sizeof(s_Rt286Idx) / sizeof(s_Rt286Idx[0]);
    }
    for (i = 0; i < nidx; i += 2)
        HwWr(c, idx[i], idx[i + 1]);
    for (i = 0; i < sizeof(s_Rt286Reg) / sizeof(s_Rt286Reg[0]); i += 2)
        HwWr(c, s_Rt286Reg[i], s_Rt286Reg[i + 1]);
    if (c->Errors != e0)
        return FALSE;

    if (IS298(c)) {
        HwUpd(c, R_A_BIAS_CTRL3, 0x0080, 0x0080);       /* VREF charging */
        HwUpd(c, 0x1b, 0x0860, 0x0860);
        HwUpd(c, R_POWER_CTRL2, 0x0020, 0x0020);        /* Vref2 */
    }
    HwWr(c, AFG_POWER, PWR_D3);
    for (i = 0; i < sizeof(s_Rt286Pwr); i++)
        HwWr(c, R_SET_POWER(s_Rt286Pwr[i]), PWR_D1);
    if (!c->Priv[P_CBJ]) {
        HwWr(c, R_CBJ_CTRL2, 0x0000);
        HwWr(c, R_MIC1_DET_CTRL, 0x0816);
        HwUpd(c, R_CBJ_CTRL1, 0xf000, 0xb000);
    } else {
        HwUpd(c, R_CBJ_CTRL1, 0xf000, 0x5000);
    }
    CDelay(10);
    HwWr(c, R_DMIC2_DEFAULT, 0x40);                     /* !pdata.gpio2_en */
    CDelay(10);
    HwWr(c, R_MISC_CTRL1, 0x0000);
    if (IS298(c)) {
        HwUpd(c, R_WIND_FILTER, 0x0082, 0x0082);
        /* unsolicited responses / IRQ_FLAG_CTRL (jack interrupt) not enabled: no jack detection */
    } else {
        HwUpd(c, R_POWER_CTRL2, 0x000c, 0x0000);        /* power down LDO, VREF */
        HwUpd(c, R_POWER_CTRL1, 0x1001, 0x1001);
        HwUpd(c, R_DEPOP_CTRL2, 0x403a, 0x401a);        /* depop parameters */
        HwUpd(c, R_DEPOP_CTRL3, 0xf777, 0x4737);
        HwUpd(c, R_DEPOP_CTRL4, 0x00ff, 0x003f);
    }
    if (c->Priv[P_CHIP] == CH_RT288) {
        /* rt286.c: RT288 on Dell - codec GPIO6 driven high (GPIO_CTRL[3:2] = 2); RT288 is only
           known in Dell XPS 13 9343 class machines, so done for every RT288 */
        HwUpd(c, R_GPIO_MASK, 0x40, 0x40);
        HwUpd(c, R_GPIO_DIR, 0x40, 0x40);
        HwUpd(c, R_GPIO_DATA, 0x40, 0x40);
        HwUpd(c, R_GPIO_CTRL, 0x000c, 0x0008);
    }

    /* static setup: clocks, DAI, routing, gains */
    R286Clocks(c);
    R286Dai(c);
    HwWr(c, R_HPO_MUX, 0);                  /* HP-OUT <- Front (DAC0) */
    HwWr(c, R_SPK_MUX, 0);                  /* SPK-OUT <- Front */
    HwWr(c, R_F_DAC_SWITCH, 0x00);          /* Front mixer: DAC0 unmuted */
    HwWr(c, R_F_RECMIX_SW, 0x80);           /* RECMIX -> Front muted (no mic loopback) */
    HwWr(c, R_ADC0_MUX, c->Mic == 1 ? R_ADC_SEL_DMIC : R_ADC_SEL_MIC);
    HwWr(c, R_ADC1_MUX, R_ADC_SEL_MIC);
    HwWr(c, R_MIC_GAIN, MIC_BOOST);
    R286AdcGain(c, FALSE);
    c->Priv[P_HPGAIN] = AmpZero(c, N_HP_OUT);
    c->Priv[P_SPKGAIN] = AmpZero(c, N_SPK_OUT);
    HwWr(c, R_HPOL_GAIN, 0x80);
    HwWr(c, R_HPOR_GAIN, 0x80);
    HwWr(c, R_SPOL_GAIN, 0x80);
    HwWr(c, R_SPOR_GAIN, 0x80);
    RtVolume(c, 65535, 65535, FALSE);

    /* idle state as DAPM leaves it: all supplies off (RT298 resets with them on), AFG D3 */
    HwWr(c, R_SET_PIN_HPO, 0x00);
    HwWr(c, R_SET_PIN_SPK, 0x00);
    HwWr(c, R_SPK_EAPD, 0x0);
    R286SupplyLate(c, FALSE);
    R286MicBuf(c, FALSE);
    HwUpd(c, R_POWER_CTRL1, 0x0001, 0x0001);
    HwUpd(c, R_PLL_CTRL1, 0x0020, 0x0000);
    HwWr(c, AFG_POWER, PWR_D3);

    c->Caps = CC_PLAY | CC_REC;
    LOGI("%s: id %08x, %s %u Hz, %s, %u slots x %u bit, %u valid, mic %s\n", ChipName(c->Priv[P_CHIP]), id,
         c->Priv[P_MCLKMODE] ? "MCLK" : "PLL from MCLK", c->Mclk ? c->Mclk : 24000000,
         c->Slots > 2 ? "DSP_A" : "I2S", c->Slots, c->Slot, c->Valid, c->Mic == 1 ? "DMIC1" : "MIC1 (CBJ)");
    if (!c->Priv[P_MCLKMODE] && c->Mclk != 19200000 && c->Mclk != 24000000)
        LOGE("%s: MCLK %u Hz unsupported (needs 19.2/24 MHz or 12.288/24.576 MHz), assuming 24 MHz\n",
             ChipName(c->Priv[P_CHIP]), c->Mclk);
    return c->Errors == e0;
}

static void R286Shutdown(SST_CODEC *c)
{
    if (c->Priv[P_PLAY])
        R286Stream(c, FALSE, FALSE);
    if (c->Priv[P_REC])
        R286Stream(c, TRUE, FALSE);
    HwWr(c, R_HPOL_GAIN, 0x80);
    HwWr(c, R_HPOR_GAIN, 0x80);
    HwWr(c, R_SPOL_GAIN, 0x80);
    HwWr(c, R_SPOR_GAIN, 0x80);
    HwWr(c, R_SPK_EAPD, 0x0);
    HwWr(c, R_SET_MIC1, 0x20);
    HwWr(c, AFG_POWER, PWR_D3);
}

const CODEC_DRV g_DrvRt286 = {
    "RT286", "INT343A\0" "10EC0286\0" "10EC0298\0", CODEC_HEADSET, 4, 4,
    R286Probe, R286Init, R286Stream, RtVolume, R286Shutdown
};

/* ================================================================== */
/* RT274                                                                */
/* ================================================================== */

/* index registers (rt274.h) */
#define T_EAPD_GPIO_IRQ 0x10
#define T_PAD_CTRL12    0x35
#define T_I2S_CTRL1     0x63
#define T_I2S_CTRL2     0x64
#define T_MCLK_CTRL     0x71
#define T_CLK_CTRL      0x72
#define T_PLL2_CTRL     0x7b

/* NIDs */
#define T_N_DAC_OUT0    0x02
#define T_N_ADC_IN1     0x09
#define T_N_DMIC1       0x12
#define T_N_LINE3       0x16
#define T_N_MIC         0x19
#define T_N_HP_OUT      0x21
#define T_N_MIXER_IN1   0x22

#define T_RESET         VERB(V_CODEC_RESET, 0x01, 0)
#define T_LOUT_MUX      VERB(V_CONNECT_SEL, T_N_LINE3, 0)
#define T_HPO_MUX       VERB(V_CONNECT_SEL, T_N_HP_OUT, 0)
#define T_ADC0_MUX      VERB(V_CONNECT_SEL, T_N_MIXER_IN1, 0)
#define T_SET_MIC       VERB(V_PIN_WIDGET_CTL, T_N_MIC, 0)
#define T_SET_PIN_LOUT3 VERB(V_PIN_WIDGET_CTL, T_N_LINE3, 0)
#define T_SET_PIN_HPO   VERB(V_PIN_WIDGET_CTL, T_N_HP_OUT, 0)
#define T_SET_PIN_DMIC1 VERB(V_PIN_WIDGET_CTL, T_N_DMIC1, 0)
#define T_MIC_GAIN      VERB(V_AMP_GAIN_MUTE, T_N_MIC, 0x7000)
#define T_LOUTL_GAIN    VERB(V_AMP_GAIN_MUTE, T_N_LINE3, 0xa000)
#define T_LOUTR_GAIN    VERB(V_AMP_GAIN_MUTE, T_N_LINE3, 0x9000)
#define T_HPOL_GAIN     VERB(V_AMP_GAIN_MUTE, T_N_HP_OUT, 0xa000)
#define T_HPOR_GAIN     VERB(V_AMP_GAIN_MUTE, T_N_HP_OUT, 0x9000)
#define T_COEF58_INDEX  VERB(V_COEF_INDEX, 0x58, 0)
#define T_COEF58_COEF   VERB(V_PROC_COEF, 0x58, 0)
#define T_COEF5B_INDEX  VERB(V_COEF_INDEX, 0x5b, 0)
#define T_COEF5B_COEF   VERB(V_PROC_COEF, 0x5b, 0)
#define T_STREAMID_DAC0 VERB(V_CHANNEL_STREAMID, T_N_DAC_OUT0, 0)
#define T_STREAMID_ADC1 VERB(V_CHANNEL_STREAMID, T_N_ADC_IN1, 0)

/* ADC mux: Mic, Line1, Line2, Dmic */
#define T_ADC_SEL_MIC   0
#define T_ADC_SEL_DMIC  3

static BOOLEAN R274Probe(SST_CODEC *c)
{
    ULONG id;

    if (!HwRd(c, VENDOR_ID_REG, &id) || id != ID_RT274)
        return FALSE;
    c->Priv[P_ID] = id;
    c->Priv[P_CHIP] = CH_RT274;
    return TRUE;
}

/*
 * Clocks (avs rt274 machine: set_sysclk(PLL2, 24 MHz), set_bclk_ratio(100), set_pll(PLL2 from
 * BCLK)).  PLL2 takes BCLK; rt274_set_dai_pll only knows BCLK = 50 / 64 / 100 / 128 fs (table
 * rows 0x7a / 0x7b / 0x7c), so other ratios fall back to MCLK: 19.2 / 24 MHz through PLL1
 * (RT274_SCLK_S_PLL1), 12.288 / 24.576 MHz directly (RT274_SCLK_S_MCLK); with neither the
 * 100 fs row is used as Linux does ("assume 4.8M").  Returns a short description for the log.
 */
static const char *R274Clocks(SST_CODEC *c)
{
    ULONG fs = (ULONG)c->Slots * c->Slot, mclk = c->Mclk;
    BOOLEAN tab = fs == 50 || fs == 64 || fs == 100 || fs == 128;

    if (!tab && (mclk == 19200000 || mclk == 24000000)) {
        HwUpd(c, T_MCLK_CTRL, 0x4000, 0x0000);          /* MCLK_MODE_DIS */
        HwUpd(c, T_CLK_CTRL, 0x0038, 0x0000);           /* CLK_SRC_MCLK */
        HwUpd(c, T_I2S_CTRL2, 0x0040, mclk == 19200000 ? 0x0040 : 0x0000);
        HwUpd(c, T_I2S_CTRL1, 0x1000, fs < 50 ? 0x1000 : 0);
        return "PLL1 from MCLK";
    }
    if (!tab && (mclk == 12288000 || mclk == 24576000)) {
        HwUpd(c, T_MCLK_CTRL, 0x4000, 0x4000);          /* MCLK_MODE_EN */
        HwUpd(c, T_CLK_CTRL, 0x0038, 0x0000);
        HwUpd(c, T_MCLK_CTRL, 0x1fcf, mclk == 12288000 ? 0x0008 : 0x1543);
        HwUpd(c, T_I2S_CTRL1, 0x1000, fs < 50 ? 0x1000 : 0);
        return "MCLK";
    }
    /* rt274_set_dai_sysclk(RT274_SCLK_S_PLL2, 24000000) */
    HwUpd(c, T_MCLK_CTRL, 0x4000, 0x4000);
    HwUpd(c, T_CLK_CTRL, 0x0038, 0x0018);               /* CLK_SRC_PLL2 */
    HwUpd(c, T_I2S_CTRL2, 0x0040, 0x0000);              /* 24 MHz */
    /* rt274_set_bclk_ratio: [12] set when ratio / 50 == 0 */
    HwUpd(c, T_I2S_CTRL1, 0x1000, fs < 50 ? 0x1000 : 0);
    /* rt274_set_dai_pll(RT274_PLL2_S_BCLK) */
    HwUpd(c, T_PLL2_CTRL, 0x2000, 0x2000);              /* PLL2_SRC_BCLK */
    HwUpd(c, T_MCLK_CTRL, 0x3000, 0x3000);
    switch (fs) {
    case 50:
        HwWr(c, 0x7a, 0xaab6); HwWr(c, 0x7b, 0x0301); HwWr(c, 0x7c, 0x04fe);
        break;
    case 64:
        HwWr(c, 0x7a, 0xaa96); HwWr(c, 0x7b, 0x8003); HwWr(c, 0x7c, 0x081e);
        break;
    case 128:
        HwWr(c, 0x7a, 0xaa96); HwWr(c, 0x7b, 0x8003); HwWr(c, 0x7c, 0x080e);
        break;
    default:            /* 100 fs (4.8 MHz) */
        HwWr(c, 0x7a, 0xaab6); HwWr(c, 0x7b, 0x0301); HwWr(c, 0x7c, 0x047e);
        break;
    }
    return tab ? "PLL2 from BCLK" : "PLL2 from BCLK (ratio unsupported, 100 fs row)";
}

/* DAI: slave, I2S or DSP_A + TDM (2 / 4 slots), word / channel length, stream formats */
static void R274Dai(SST_CODEC *c)
{
    ULONG f = (FmtBits(c->Valid) << 4) | 1;
    ULONG clen = c->Slot >= 32 ? 3 : c->Slot >= 24 ? 2 : c->Slot >= 20 ? 1 : 0;

    HwUpd(c, T_I2S_CTRL1, 0x0800, 0x0000);              /* I2S_MODE_S */
    if (c->Slots > 2) {
        /* avs rt274: DSP_A, set_tdm_slot(0xf, 0xf, 4, 24): AIF1 = slots 0/1 -> DAC0 / ADC0 */
        HwUpd(c, T_I2S_CTRL1, 0x0300, 0x0200);          /* I2S_FMT_PCMA */
        HwUpd(c, T_I2S_CTRL1, 0x0480, c->Slots >= 4 ? 0x0480 : 0x0400);
    } else {
        HwUpd(c, T_I2S_CTRL1, 0x0300, 0x0000);          /* I2S_FMT_I2S */
        HwUpd(c, T_I2S_CTRL1, 0x0400, 0x0000);          /* TDM_DIS */
    }
    /* rt274_hw_params: [4:3] data length, [15:14] channel (slot) length */
    HwUpd(c, T_I2S_CTRL1, 0xc018, (ILen(c->Valid) << 3) | (clen << 14));
    HwWr(c, DAC_FORMAT, f);
    HwWr(c, ADC_FORMAT, f);
}

static void R274Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN play = c->Priv[P_PLAY] != 0, cap = c->Priv[P_REC] != 0;
    BOOLEAN dmic = c->Mic == 1;

    if (on) {
        if (rec ? cap : play)
            return;
        if (!play && !cap) {
            R274Clocks(c);                      /* "Platform Clock" PRE_PMU */
            HwWr(c, AFG_POWER, PWR_D0);         /* set_bias_level PREPARE */
        }
        if (rec) {
            if (!dmic)
                HwWr(c, T_SET_MIC, 0x24);       /* VREF80 */
            HwWr(c, T_STREAMID_ADC1, 0x10);     /* "ADC 0" */
            HwWr(c, ADCL_GAIN, 0x7f);
            HwWr(c, ADCR_GAIN, 0x7f);
            c->Priv[P_REC] = 1;
        } else {
            HwWr(c, T_SET_PIN_HPO, 0x40);       /* "HP Power" */
            HwWr(c, T_SET_PIN_LOUT3, 0x40);     /* "LOUT Power" */
            HwWr(c, T_STREAMID_DAC0, 0x10);     /* "DAC 0" */
            HwWr(c, T_LOUTL_GAIN, c->Priv[P_SPKGAIN]);  /* autodisable kcontrols: last */
            HwWr(c, T_LOUTR_GAIN, c->Priv[P_SPKGAIN]);
            HwWr(c, T_HPOL_GAIN, c->Priv[P_HPGAIN]);
            HwWr(c, T_HPOR_GAIN, c->Priv[P_HPGAIN]);
            c->Priv[P_PLAY] = 1;
        }
        return;
    }

    if (!(rec ? cap : play))
        return;
    if (rec) {
        HwWr(c, ADCL_GAIN, 0xff);
        HwWr(c, ADCR_GAIN, 0xff);
        HwWr(c, T_STREAMID_ADC1, 0x00);
        if (!dmic)
            HwWr(c, T_SET_MIC, 0x20);
        c->Priv[P_REC] = 0;
    } else {
        HwWr(c, T_HPOL_GAIN, 0x80);
        HwWr(c, T_HPOR_GAIN, 0x80);
        HwWr(c, T_LOUTL_GAIN, 0x80);
        HwWr(c, T_LOUTR_GAIN, 0x80);
        HwWr(c, T_STREAMID_DAC0, 0x00);
        HwWr(c, T_SET_PIN_HPO, 0x00);
        HwWr(c, T_SET_PIN_LOUT3, 0x00);
        c->Priv[P_PLAY] = 0;
    }
    if (!c->Priv[P_PLAY] && !c->Priv[P_REC])
        HwWr(c, AFG_POWER, PWR_D3);             /* set_bias_level STANDBY */
}

static BOOLEAN R274Init(SST_CODEC *c)
{
    ULONG e0 = c->Errors, id = 0;
    const char *clk;

    if (!HwRd(c, VENDOR_ID_REG, &id) || id != ID_RT274) {
        LOGE("RT274: vendor id %08x\n", id);
        return FALSE;
    }
    c->Priv[P_ID] = id;
    c->Priv[P_CHIP] = CH_RT274;
    c->Priv[P_PLAY] = c->Priv[P_REC] = 0;

    /* rt274_i2c_probe */
    HwWr(c, T_RESET, 0);
    HwUpd(c, 0x1a, 0x4000, 0x4000);
    if (c->Errors != e0)
        return FALSE;
    HwUpd(c, T_PAD_CTRL12, 0x0003, 0x0000);             /* pad PDB floating */
    HwWr(c, T_COEF5B_INDEX, 0x01);
    HwWr(c, T_COEF5B_COEF, 0x8540);
    HwUpd(c, 0x6f, 0x0100, 0x0100);
    HwWr(c, 0x4a, 0x201b);                              /* combo jack auto detect */
    HwUpd(c, 0x6f, 0x3000, 0x2000);                     /* aux mode off */
    HwUpd(c, 0x6f, 0x000f, 0x0000);                     /* HP DC calibration */
    HwWr(c, T_COEF58_INDEX, 0x00);
    HwWr(c, T_COEF58_COEF, 0xb888);
    CDelay(500);
    HwUpd(c, 0x6f, 0x000f, 0x000b);
    HwWr(c, T_COEF58_INDEX, 0x00);
    HwWr(c, T_COEF58_COEF, 0x3888);
    HwWr(c, T_SET_PIN_HPO, 0x40);
    HwWr(c, T_SET_PIN_LOUT3, 0x40);
    HwWr(c, T_SET_MIC, 0x20);
    HwWr(c, T_SET_PIN_DMIC1, 0x20);
    HwUpd(c, T_I2S_CTRL2, 0xc004, 0x4004);
    HwUpd(c, T_EAPD_GPIO_IRQ, 0x0180, 0x0100);          /* GPI2 = DMIC clock */
    /* unsolicited HP / MIC responses (jack IRQ) not enabled: no jack detection */

    /* static setup */
    clk = R274Clocks(c);
    R274Dai(c);
    HwWr(c, T_HPO_MUX, 0);                  /* HP <- DAC OUT0 */
    HwWr(c, T_LOUT_MUX, 0);                 /* LINE3 out <- DAC OUT0 */
    HwWr(c, T_ADC0_MUX, c->Mic == 1 ? T_ADC_SEL_DMIC : T_ADC_SEL_MIC);
    HwWr(c, T_MIC_GAIN, MIC_BOOST);
    HwWr(c, ADCL_GAIN, 0xff);
    HwWr(c, ADCR_GAIN, 0xff);
    c->Priv[P_HPGAIN] = AmpZero(c, T_N_HP_OUT);
    c->Priv[P_SPKGAIN] = AmpZero(c, T_N_LINE3);
    HwWr(c, T_HPOL_GAIN, 0x80);
    HwWr(c, T_HPOR_GAIN, 0x80);
    HwWr(c, T_LOUTL_GAIN, 0x80);
    HwWr(c, T_LOUTR_GAIN, 0x80);
    RtVolume(c, 65535, 65535, FALSE);

    /* idle state as DAPM leaves it: output pins and converters off, AFG D3 */
    HwWr(c, T_STREAMID_DAC0, 0x00);
    HwWr(c, T_STREAMID_ADC1, 0x00);
    HwWr(c, T_SET_PIN_HPO, 0x00);
    HwWr(c, T_SET_PIN_LOUT3, 0x00);
    HwWr(c, AFG_POWER, PWR_D3);

    c->Caps = CC_PLAY | CC_REC;
    LOGI("RT274: id %08x, %s (BCLK %u Hz, MCLK %u Hz), %s, %u slots x %u bit, %u valid, mic %s\n", id, clk,
         c->Bclk, c->Mclk, c->Slots > 2 ? "DSP_A TDM" : "I2S", c->Slots, c->Slot, c->Valid,
         c->Mic == 1 ? "DMIC1" : "MIC (0x19)");
    return c->Errors == e0;
}

static void R274Shutdown(SST_CODEC *c)
{
    if (c->Priv[P_PLAY])
        R274Stream(c, FALSE, FALSE);
    if (c->Priv[P_REC])
        R274Stream(c, TRUE, FALSE);
    HwWr(c, T_HPOL_GAIN, 0x80);
    HwWr(c, T_HPOR_GAIN, 0x80);
    HwWr(c, T_LOUTL_GAIN, 0x80);
    HwWr(c, T_LOUTR_GAIN, 0x80);
    HwWr(c, T_SET_MIC, 0x20);
    HwWr(c, AFG_POWER, PWR_D3);
}

const CODEC_DRV g_DrvRt274 = {
    "RT274", "INT34C2\0" "10EC0274\0", CODEC_HEADSET, 4, 4,
    R274Probe, R274Init, R274Stream, RtVolume, R274Shutdown
};
