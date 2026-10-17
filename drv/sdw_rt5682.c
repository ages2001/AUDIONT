/*
 * AUDIONT - Realtek RT5682 (ALC5682I-VD) headset codec in SoundWire mode (SoundWire 1.1,
 * class 0, part 0x5682).  Linux sound/soc/codecs/rt5682-sdw.c (bus side, io_init, hw_params)
 * and rt5682.c (DAPM, calibration, headset detection); the I2S variant is cod_rt5682.c, whose
 * power sequences this file follows with the SoundWire clocking.
 *
 * Register access: the 16-bit codec registers through the indirect window of rt5682_sdw_read /
 * _write (SDW_CMD 0x3008, ADDR_H / _L 0x3001 / 0x3000, DATA_H / _L 0x3005 / 0x3004).
 *
 *   playback  DP1 (SDWRX) -> SOUND DAC L/R ("DAC L / R Mux" = SOUND) -> DAC1 MIXL/R -> DAC L1/R1
 *             -> HP amp (rt5682_hp_event) -> HPOL / HPOR
 *   capture   IN1P -> BST1 CBJ (+24 dB) -> RECMIX1L -> ADC1 L -> Stereo1 ADC MIX (both channels)
 *             -> IF1 ADC -> ADCDAT -> SDWTX -> DP2
 *   clocking  sysclk from PLL2 locked to the SoundWire clock (GLB_CLK SCLK_SRC_PLL2 /
 *             PLL2_SRC_SDW, the fixed PLL2 setup of rt5682_io_init), PLL2B / PLL2F powered with
 *             either stream; SDW_REF_CLK 48 kHz, DAC / ADC OSR /8 (hw_params); the bus clock
 *             code in 0xe0 / 0xf0 (rt5682_clock_config)
 *
 * No jack detection: the CBJ setup of rt5682_io_init without the JD1 interrupt, then the
 * "jack inserted" part of rt5682_headset_detect: the CBJ type FSM is triggered once; when it
 * does not report a headset (no plug at boot, 3-pole) the CBJ is forced to headset mode
 * (MIC_CAP from the register, MICBIAS1 register controlled), and the detection runs again
 * when capture starts while nothing plays.  Headphone volume DAC1 0 dB.
 */
#include "sdw_rtcom.h"

#define RT5682_ID_VAL           0x6530

/* SoundWire indirect access */
#define SDW_ADDR_L              0x3000
#define SDW_ADDR_H              0x3001
#define SDW_DATA_L              0x3004
#define SDW_DATA_H              0x3005
#define SDW_CMD                 0x3008

/* codec registers (rt5682.h) */
#define R82_RESET               0x0000
#define R82_HP_CTRL_1           0x0002
#define R82_HP_CTRL_2           0x0003
#define R82_I2C_CTRL            0x0008
#define R82_CBJ_BST_CTRL        0x000b
#define R82_CBJ_CTRL_1          0x0010
#define R82_CBJ_CTRL_2          0x0011
#define R82_CBJ_CTRL_3          0x0012
#define R82_CBJ_CTRL_5          0x0014
#define R82_CBJ_CTRL_7          0x0016
#define R82_DAC1_DIG_VOL        0x0019
#define R82_STO1_ADC_DIG_VOL    0x001c
#define R82_STO1_ADC_BOOST      0x001f
#define R82_STO1_ADC_MIXER      0x0026
#define R82_AD_DA_MIXER         0x0029
#define R82_A_DAC1_MUX          0x002b
#define R82_REC_MIXER           0x003c
#define R82_CAL_REC             0x0044
#define R82_PWR_DIG_1           0x0061
#define R82_PWR_DIG_2           0x0062
#define R82_PWR_ANLG_1          0x0063
#define R82_PWR_ANLG_2          0x0064
#define R82_PWR_ANLG_3          0x0065
#define R82_CLK_DET             0x006b
#define R82_ADDA_CLK_1          0x0073
#define R82_GLB_CLK             0x0080
#define R82_PLL_TRACK_2         0x0084
#define R82_PLL_TRACK_3         0x0085
#define R82_SDW_REF_CLK         0x008d
#define R82_DEPOP_1             0x008e
#define R82_HP_CHARGE_PUMP_1    0x0091
#define R82_MICBIAS_2           0x0094
#define R82_PLL2_CTRL_1         0x009b
#define R82_PLL2_CTRL_2         0x009c
#define R82_PLL2_CTRL_3         0x009d
#define R82_PLL2_CTRL_4         0x009e
#define R82_RC_CLK_CTRL         0x009f
#define R82_IRQ_CTRL_3          0x00b8
#define R82_4BTN_IL_CMD_2       0x00e3
#define R82_VERSION_ID          0x00fd
#define R82_VENDOR_ID           0x00fe
#define R82_DEVICE_ID           0x00ff
#define R82_DAC_ADC_DIG_VOL1    0x0100
#define R82_BIAS_CUR_CTRL_8     0x0111
#define R82_CHARGE_PUMP_1       0x0125
#define R82_CHOP_DAC            0x013a
#define R82_CHOP_ADC            0x013b
#define R82_CALIB_ADC_CTRL      0x013c
#define R82_TEST_MODE_CTRL_1    0x0145
#define R82_PLL2_INTERNAL       0x0156
#define R82_HP_IMP_SENS_CTRL_19 0x01c1
#define R82_HP_LOGIC_CTRL_2     0x01db
#define R82_HP_CALIB_CTRL_1     0x01de
#define R82_HP_CALIB_CTRL_2     0x01df
#define R82_HP_CALIB_CTRL_3     0x01e0
#define R82_HP_CALIB_STA_1      0x01ea
#define R82_SAR_IL_CMD_1        0x0210
#define R82_SAR_IL_CMD_2        0x0211
#define R82_SAR_IL_CMD_3        0x0212
#define R82_SAR_IL_CMD_6        0x0215
#define R82_SAR_IL_CMD_13       0x021c

/* PWR_DIG_1 (0x61) */
#define PD1_DAC_L1              0x0800
#define PD1_DAC_R1              0x0400
#define PD1_LDO                 0x0100
#define PD1_ADC_L1              0x0010
#define PD1_DIG_GATE            0x0001
/* PWR_DIG_2 (0x62) */
#define PD2_ADC_S1F             0x8000
#define PD2_DAC_S1F             0x0400
/* PWR_ANLG_1 (0x63) */
#define PA1_VREF1               0x8000
#define PA1_FV1                 0x4000
#define PA1_VREF2               0x2000
#define PA1_FV2                 0x1000
#define PA1_MB                  0x0200
#define PA1_BG                  0x0080
#define PA1_LDO1_DVO_MASK       0x0030
#define PA1_LDO1_DVO_12         0x0020
#define PA1_HP_DRIVER_MASK      0x000c
#define PA1_HP_DRIVER_5X        0x000c
#define PA1_HA_L                0x0002
#define PA1_HA_R                0x0001
/* PWR_ANLG_2 (0x64) */
#define PA2_MB1                 0x0800
#define PA2_JDH                 0x0008
#define PA2_RM1_L               0x0002
/* PWR_ANLG_3 (0x65) */
#define PA3_CBJ                 0x0200
#define PA3_PLL2B               0x0020
#define PA3_PLL2F               0x0010
/* CLK_DET (0x6b) */
#define CLKDET_SYS              0x8000
#define CLKDET_POW              0x0001
/* HP_CTRL_1 (0x02) / STO1_ADC_DIG_VOL (0x1c) */
#define LR_L_MUTE               0x8000
#define LR_R_MUTE               0x0080
/* HP_CTRL_2 (0x03) */
#define HPC2_DAC_AMP_MUTE       0x8000
#define HPC2_DAC_L_EN           0x4000
#define HPC2_DAC_R_EN           0x2000
/* DEPOP_1 (0x8e) */
#define DEP_PUMP_EN             0x0008
#define DEP_CAPLESS_EN          0x0001
/* HP_CHARGE_PUMP_1 (0x91) */
#define HCP_OSW_L               0x0800
#define HCP_OSW_R               0x0400
#define HCP_PM_HP_MASK          0x0300
#define HCP_PM_HP_HV            0x0200
/* CHARGE_PUMP_1 (0x125) */
#define CP_SW_SIZE_MASK         0x0700
#define CP_SW_SIZE_L            0x0400
#define CP_SW_SIZE_M            0x0200
#define CP_CLK_HP_MASK          0x0030
#define CP_CLK_HP_300KHZ        0x0020
/* BIAS_CUR_CTRL_8 (0x111) */
#define BC8_HPA_CP_BIAS_MASK    0x000c
#define BC8_HPA_CP_BIAS_3UA     0x0004
/* HP_LOGIC_CTRL_2 (0x1db) */
#define HPL2_SIG_SOUR2_REG      0x0010
/* CHOP_DAC / CHOP_ADC: CKGEN_DAC1 / CKGEN_ADC1 */
#define CHOP_CKGEN              0x1000
/* ADDA_CLK_1 (0x73) */
#define ADDA_ADC_OSR_MASK       0xf000
#define ADDA_ADC_OSR_D_8        0x4000
#define ADDA_DAC_OSR_MASK       0x000f
#define ADDA_DAC_OSR_D_8        0x0004
/* GLB_CLK (0x80) */
#define GLB_SCLK_SRC_MASK       0xe000
#define GLB_SCLK_SRC_PLL2       0x4000
#define GLB_PLL2_SRC_MASK       0x0c00
#define GLB_PLL2_SRC_SDW        0x0800
/* SDW_REF_CLK (0x8d): [7:4] capture, [3:0] playback rate, 0 = 48 kHz */
#define SDW_REF_MASK            0x00ff
/* RC_CLK_CTRL (0x9f) */
#define RC_POW_IRQ              0x8000
#define RC_POW_JDH              0x4000
#define RC_POW_ANA              0x1000
/* AD_DA_MIXER (0x29): "DAC1 Switch" (1 = mute), "DAC L / R Mux" (1 = SOUND, the SoundWire DAC) */
#define ADDA_M_DAC1_L           0x4000
#define ADDA_DAC1_R_SEL         0x0400
#define ADDA_DAC1_L_SEL         0x0100
#define ADDA_M_DAC1_R           0x0040
/* CBJ_CTRL_1 (0x10) */
#define CBJ1_FAST_OFF_EN        0x0080
#define CBJ1_MB1_PATH_REG       0x0020
#define CBJ1_TRIG_JD            0x0008
#define CBJ1_MIC_CAP_HS         0x0002
#define CBJ1_MIC_CAP_SRC_REG    0x0001
/* CBJ_CTRL_2 (0x11) */
#define CBJ2_EXT_JD_SRC_MASK    0x0070
#define CBJ2_EXT_JD_SRC_MANUAL  0x0040
#define CBJ2_JACK_TYPE_MASK     0x0003
/* CBJ_CTRL_3 (0x12) */
#define CBJ3_IN_BUF_EN          0x0080
/* CBJ_BST_CTRL (0x0b): BST_CBJ [11:8] {0, +20, +24, +30, ..} dB */
#define CBJ_BST_MASK            0x0f00
#define CBJ_BST_24DB            0x0200
/* REC_MIXER (0x3c) */
#define REC_M_CBJ_RM1_L         0x0080
#define REC_G_CBJ_RM1_L_MASK    0x1c00
/* SAR_IL_CMD_1 (0x210) */
#define SAR1_BUTT_DET_EN        0x8000
#define SAR1_POW_EN             0x1000
#define SAR13_SOUR_MASK         0x003f
#define IRQ3_IL_IRQ_EN          0x0080
#define BTN2_4BTN_IL_EN         0x8000
#define BTN2_4BTN_IL_NOR        0x4000
/* MICBIAS_2 (0x94) */
#define MB2_PWR_CLK25M          0x0200
#define MB2_PWR_CLK1M           0x0100
/* STO1_ADC_MIXER (0x26): ADC1 L (CBJ) on both channels, ADC2 muted (see cod_rt5682.c) */
#define STO1_ADC_MIX_CBJ        0x6060

typedef struct _R82_PRIV {
    BOOLEAN Hs;                     /* the CBJ type FSM reported a headset */
    UCHAR   JackType;
} R82_PRIV;
typedef char R82PrivFits[sizeof(R82_PRIV) <= sizeof(((SDW_DEV *)0)->Priv) ? 1 : -1];

static R82_PRIV *P(SDW_DEV *d) { return (R82_PRIV *)d->Priv; }

/* ------------------------------------------------------------------ */
/* register access                                                      */
/* ------------------------------------------------------------------ */
static BOOLEAN Wr(SDW_DEV *d, ULONG reg, ULONG val)
{
    BOOLEAN ok = SdwWrite(d, SDW_CMD, 1);
    ok &= SdwWrite(d, SDW_ADDR_H, (UCHAR)(reg >> 8));
    ok &= SdwWrite(d, SDW_ADDR_L, (UCHAR)reg);
    ok &= SdwWrite(d, SDW_DATA_H, (UCHAR)(val >> 8));
    ok &= SdwWrite(d, SDW_DATA_L, (UCHAR)val);
    return ok;
}

static BOOLEAN Rd(SDW_DEV *d, ULONG reg, ULONG *val)
{
    UCHAR h = 0, l = 0;
    BOOLEAN ok = SdwWrite(d, SDW_CMD, 0);
    ok &= SdwWrite(d, SDW_ADDR_H, (UCHAR)(reg >> 8));
    ok &= SdwWrite(d, SDW_ADDR_L, (UCHAR)reg);
    ok &= SdwRead(d, SDW_DATA_H, &h);
    ok &= SdwRead(d, SDW_DATA_L, &l);
    *val = ((ULONG)h << 8) | l;
    return ok;
}

static ULONG Get(SDW_DEV *d, ULONG reg)
{
    ULONG v = 0;
    Rd(d, reg, &v);
    return v;
}

static void Upd(SDW_DEV *d, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;
    if (!Rd(d, reg, &v)) return;
    if (((v & ~mask) | (val & mask)) != v) Wr(d, reg, (v & ~mask) | (val & mask));
}

/* ------------------------------------------------------------------ */
/* probe / init                                                         */
/* ------------------------------------------------------------------ */
static BOOLEAN Probe(SDW_DEV *d)
{
    d->Kind = SDW_KIND_HEADSET;
    d->Play.Num = 1; d->Play.Ch = 2; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = FALSE;
    d->Rec.Num = 2; d->Rec.Ch = 2; d->Rec.Type = SDW_DP_FULL; d->Rec.CpSm = FALSE;
    d->Paging = FALSE;
    return TRUE;
}

/* rt5682_calibrate (HP DC offset calibration from the internal RC clock); rt5682_reset of SoundWire
   mode: no I2C_MODE write; the VE silicon is not detected in SoundWire mode (CALIB_ADC_CTRL) */
static void Calibrate(SDW_DEV *d)
{
    ULONG n;
    Wr(d, R82_RESET, 0);
    Wr(d, R82_I2C_CTRL, 0x000f);
    Wr(d, R82_PWR_ANLG_1, 0xa2af);
    SdwDelay(20);
    Wr(d, R82_PWR_ANLG_1, 0xf2af);
    Wr(d, R82_MICBIAS_2, 0x0300);
    Wr(d, R82_GLB_CLK, 0x8000);
    Wr(d, R82_PWR_DIG_1, 0x0100);
    Wr(d, R82_HP_IMP_SENS_CTRL_19, 0x3800);
    Wr(d, R82_CHOP_DAC, 0x3000);
    Wr(d, R82_CALIB_ADC_CTRL, 0x7005);
    Wr(d, R82_STO1_ADC_MIXER, 0x686c);
    Wr(d, R82_CAL_REC, 0x0d0d);
    Wr(d, R82_HP_CALIB_CTRL_2, 0x0321);
    Wr(d, R82_HP_LOGIC_CTRL_2, 0x0004);
    Wr(d, R82_HP_CALIB_CTRL_1, 0x7c00);
    Wr(d, R82_HP_CALIB_CTRL_3, 0x06a1);
    Wr(d, R82_A_DAC1_MUX, 0x0311);
    Wr(d, R82_HP_CALIB_CTRL_1, 0x7c00);
    Wr(d, R82_HP_CALIB_CTRL_1, 0xfc00);
    for (n = 0; n < 60; n++) {
        if (!(Get(d, R82_HP_CALIB_STA_1) & 0x8000)) break;
        SdwDelay(10);
    }
    if (n >= 60) LOGE("sdw: %s: HP calibration failure", d->Name);
    /* restore (A_DAC1_MUX stays 0x0311: DAC L1 / R1 source = DAC1 MIXL / R) */
    Wr(d, R82_PWR_ANLG_1, 0x002f);
    Wr(d, R82_MICBIAS_2, 0x0080);
    Wr(d, R82_GLB_CLK, 0x0000);
    Wr(d, R82_PWR_DIG_1, 0x0000);
    Wr(d, R82_CHOP_DAC, 0x2000);
    Wr(d, R82_CALIB_ADC_CTRL, 0x2005);
    Wr(d, R82_STO1_ADC_MIXER, 0xc0c4);
    Wr(d, R82_CAL_REC, 0x0c0c);
}

/* rt5682_enable_push_button_irq(false) */
static void ButtonsOff(SDW_DEV *d)
{
    Upd(d, R82_IRQ_CTRL_3, IRQ3_IL_IRQ_EN, 0);
    Upd(d, R82_SAR_IL_CMD_1, SAR1_BUTT_DET_EN, 0);
    Upd(d, R82_4BTN_IL_CMD_2, BTN2_4BTN_IL_EN, 0);
    Upd(d, R82_4BTN_IL_CMD_2, BTN2_4BTN_IL_NOR, 0);
    Upd(d, R82_SAR_IL_CMD_13, SAR13_SOUR_MASK, 0);
}

/* the type detection of rt5682_headset_detect(jack_insert = 1), forced headset mode without a result */
static void TypeDetect(SDW_DEV *d)
{
    R82_PRIV *p = P(d);
    ULONG v, n;
    Upd(d, R82_HP_CHARGE_PUMP_1, HCP_OSW_L | HCP_OSW_R, 0);
    ButtonsOff(d);
    Upd(d, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG, CBJ1_MIC_CAP_HS);
    Upd(d, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, 0);
    SdwDelay(60);
    Upd(d, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, CBJ1_TRIG_JD);
    v = Get(d, R82_CBJ_CTRL_2) & CBJ2_JACK_TYPE_MASK;
    for (n = 0; v == 0 && n < 50; n++) {
        SdwDelay(15);
        v = Get(d, R82_CBJ_CTRL_2) & CBJ2_JACK_TYPE_MASK;
    }
    p->JackType = (UCHAR)v;
    p->Hs = v == 1 || v == 2;
    if (!p->Hs)                                     /* MIC_CAP = headset from the register, MICBIAS1 by register */
        Upd(d, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG,
            CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG);
    Upd(d, R82_HP_CHARGE_PUMP_1, HCP_OSW_L | HCP_OSW_R, HCP_OSW_L | HCP_OSW_R);
}

/* rt5682_headset_detect(jack_insert = 1) */
static void JackIn(SDW_DEV *d)
{
    Upd(d, R82_PWR_ANLG_1, PA1_VREF2 | PA1_MB, PA1_VREF2 | PA1_MB);
    Upd(d, R82_PWR_ANLG_1, PA1_FV2, 0);
    SdwDelay(20);
    Upd(d, R82_PWR_ANLG_1, PA1_FV2, PA1_FV2);
    Upd(d, R82_PWR_ANLG_3, PA3_CBJ, PA3_CBJ);
    TypeDetect(d);
    Upd(d, R82_MICBIAS_2, MB2_PWR_CLK25M | MB2_PWR_CLK1M, MB2_PWR_CLK25M | MB2_PWR_CLK1M);
}

/* rt5682_headset_detect(jack_insert = 0) */
static void JackOut(SDW_DEV *d)
{
    ButtonsOff(d);
    Upd(d, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, 0);
    Upd(d, R82_PWR_ANLG_1, PA1_MB | PA1_VREF2, 0);
    Upd(d, R82_PWR_ANLG_3, PA3_CBJ, 0);
    Upd(d, R82_MICBIAS_2, MB2_PWR_CLK25M | MB2_PWR_CLK1M, 0);
    Upd(d, R82_CBJ_CTRL_1, CBJ1_FAST_OFF_EN, 0);
}

static const ULONG g_Patch[] = {                    /* rt5682_apply_patch_list */
    R82_HP_IMP_SENS_CTRL_19, 0x1000, R82_DAC_ADC_DIG_VOL1, 0xa020, R82_I2C_CTRL, 0x000f,
    R82_PLL2_INTERNAL, 0x8266, R82_SAR_IL_CMD_1, 0x22b7, R82_SAR_IL_CMD_3, 0x0365,
    R82_SAR_IL_CMD_6, 0x0110, R82_CHARGE_PUMP_1, 0x0210, R82_HP_LOGIC_CTRL_2, 0x0007,
    R82_SAR_IL_CMD_2, 0xac00, R82_CBJ_CTRL_7, 0x0104,
};

static const ULONG g_Pll2[] = {                     /* rt5682_io_init: "Soundwire" */
    R82_PLL2_INTERNAL, 0xa266, R82_PLL2_CTRL_1, 0x1700, R82_PLL2_CTRL_2, 0x0006,
    R82_PLL2_CTRL_3, 0x2600, R82_PLL2_CTRL_4, 0x0c8f, R82_PLL_TRACK_2, 0x3000, R82_PLL_TRACK_3, 0x4000,
};

static BOOLEAN Init(SDW_DEV *d)
{
    R82_PRIV *p = P(d);
    ULONG e0, id = 0, i;
    memset(p, 0, sizeof(*p));
    if (!SdwRtClockConfig(d)) LOGE("sdw: %s: bus clock code not set", d->Name);   /* rt5682_clock_config */
    e0 = d->Errors;
    for (i = 0; i < 10; i++) {
        if (Rd(d, R82_DEVICE_ID, &id) && id == RT5682_ID_VAL) break;
        SdwDelay(30);
    }
    if (id != RT5682_ID_VAL) {
        LOGE("sdw: %s: device id %x is not an RT5682", d->Name, id);
        return FALSE;
    }
    Calibrate(d);
    for (i = 0; i < ARRAYSIZE(g_Patch); i += 2) Wr(d, g_Patch[i], g_Patch[i + 1]);
    Wr(d, R82_DEPOP_1, 0x0000);
    Upd(d, R82_PWR_ANLG_1, PA1_LDO1_DVO_MASK | PA1_HP_DRIVER_MASK, PA1_LDO1_DVO_12 | PA1_HP_DRIVER_5X);
    Wr(d, R82_MICBIAS_2, 0x0080);
    Wr(d, R82_TEST_MODE_CTRL_1, 0x0000);
    Upd(d, R82_BIAS_CUR_CTRL_8, BC8_HPA_CP_BIAS_MASK, BC8_HPA_CP_BIAS_3UA);
    Upd(d, R82_CHARGE_PUMP_1, CP_CLK_HP_MASK, CP_CLK_HP_300KHZ);
    Upd(d, R82_HP_CHARGE_PUMP_1, HCP_PM_HP_MASK, HCP_PM_HP_HV);
    for (i = 0; i < ARRAYSIZE(g_Pll2); i += 2) Wr(d, g_Pll2[i], g_Pll2[i + 1]);
    Upd(d, R82_GLB_CLK, GLB_SCLK_SRC_MASK | GLB_PLL2_SRC_MASK, GLB_SCLK_SRC_PLL2 | GLB_PLL2_SRC_SDW);
    /* CBJ of rt5682_io_init, JD1 interrupt (IRQ_CTRL_2) left off */
    Upd(d, R82_CBJ_CTRL_2, CBJ2_EXT_JD_SRC_MASK, CBJ2_EXT_JD_SRC_MANUAL);
    Wr(d, R82_CBJ_CTRL_1, 0xd142);
    Upd(d, R82_CBJ_CTRL_5, 0x0700, 0x0600);
    Upd(d, R82_CBJ_CTRL_3, CBJ3_IN_BUF_EN, CBJ3_IN_BUF_EN);
    Upd(d, R82_SAR_IL_CMD_1, SAR1_POW_EN, SAR1_POW_EN);
    Upd(d, R82_RC_CLK_CTRL, RC_POW_IRQ | RC_POW_JDH | RC_POW_ANA, RC_POW_IRQ | RC_POW_JDH | RC_POW_ANA);
    Upd(d, R82_PWR_ANLG_2, PA2_JDH, PA2_JDH);
    /* DAPM initial power-off of HP Amp L / R */
    Upd(d, R82_PWR_ANLG_1, PA1_HA_L | PA1_HA_R, 0);
    /* hw_params: 48 kHz on both SoundWire reference clocks, DAC / ADC over sample rate /8 */
    Upd(d, R82_SDW_REF_CLK, SDW_REF_MASK, 0);
    Upd(d, R82_ADDA_CLK_1, ADDA_ADC_OSR_MASK | ADDA_DAC_OSR_MASK, ADDA_ADC_OSR_D_8 | ADDA_DAC_OSR_D_8);
    /* playback routing: SOUND DAC -> DAC1 MIXL / R (DAC1 Switch on) -> DAC L1 / R1; HP muted, DAC1 0 dB */
    Upd(d, R82_AD_DA_MIXER, ADDA_M_DAC1_L | ADDA_M_DAC1_R | ADDA_DAC1_L_SEL | ADDA_DAC1_R_SEL,
        ADDA_DAC1_L_SEL | ADDA_DAC1_R_SEL);
    Upd(d, R82_HP_CTRL_1, LR_L_MUTE | LR_R_MUTE, LR_L_MUTE | LR_R_MUTE);
    Wr(d, R82_DAC1_DIG_VOL, 0xafaf);
    /* capture routing and gains */
    Wr(d, R82_STO1_ADC_MIXER, STO1_ADC_MIX_CBJ);
    Upd(d, R82_REC_MIXER, REC_M_CBJ_RM1_L | REC_G_CBJ_RM1_L_MASK, 0);       /* RECMIX1L CBJ Switch on */
    Upd(d, R82_CBJ_BST_CTRL, CBJ_BST_MASK, CBJ_BST_24DB);
    Wr(d, R82_STO1_ADC_BOOST, 0x0000);
    Wr(d, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | 0x2f00 | LR_R_MUTE | 0x002f);  /* 0 dB, muted */
    JackIn(d);
    LOGI("sdw: %s: RT5682 (vendor %x ver %x) SoundWire, PLL2 from the bus clock %u, CBJ type %u%s, "
         "%u bus errors in init", d->Name, Get(d, R82_VENDOR_ID), Get(d, R82_VERSION_ID), SdwRtBusClock(d),
         p->JackType, p->Hs ? " (headset)" : ", headset mode forced", d->Errors - e0);
    return d->Errors - e0 < 16;
}

/* ------------------------------------------------------------------ */
/* streams (DAPM order of rt5682.c with the SoundWire supplies)         */
/* ------------------------------------------------------------------ */
/* rt5682_set_bias_level OFF -> STANDBY -> PREPARE / back */
static void BiasOn(SDW_DEV *d)
{
    Upd(d, R82_PWR_DIG_1, PD1_DIG_GATE, PD1_DIG_GATE);
    Upd(d, R82_PWR_ANLG_1, PA1_BG, PA1_BG);
    Upd(d, R82_PWR_DIG_1, PD1_DIG_GATE | PD1_LDO, PD1_DIG_GATE | PD1_LDO);
}

static void BiasOff(SDW_DEV *d)
{
    Upd(d, R82_PWR_DIG_1, PD1_DIG_GATE | PD1_LDO, 0);
    Upd(d, R82_PWR_ANLG_1, PA1_BG, 0);
}

/* "Vref1" supply: rt5682_set_verf PRE_PMU (FV1 low), power, POST_PMU (15 ms, FV1) */
static void Vref1On(SDW_DEV *d, ULONG more)
{
    Upd(d, R82_PWR_ANLG_1, PA1_FV1, 0);
    Upd(d, R82_PWR_ANLG_1, PA1_VREF1 | more, PA1_VREF1 | more);
    SdwDelay(20);
    Upd(d, R82_PWR_ANLG_1, PA1_FV1, PA1_FV1);
}

static void PlayUp(SDW_DEV *d)
{
    BOOLEAN rec = d->RecOn;
    Upd(d, R82_PWR_DIG_2, PD2_DAC_S1F, PD2_DAC_S1F);   /* DAC Stereo1 Filter */
    if (rec) Upd(d, R82_PWR_ANLG_1, PA1_HA_L | PA1_HA_R, PA1_HA_L | PA1_HA_R);
    else Vref1On(d, PA1_HA_L | PA1_HA_R);
    Upd(d, R82_PWR_ANLG_3, PA3_PLL2B | PA3_PLL2F, PA3_PLL2B | PA3_PLL2F);
    Upd(d, R82_CLK_DET, CLKDET_SYS | CLKDET_POW, CLKDET_SYS | CLKDET_POW);
    Upd(d, R82_DEPOP_1, DEP_PUMP_EN, DEP_PUMP_EN);     /* "Charge Pump" */
    Upd(d, R82_DEPOP_1, DEP_CAPLESS_EN, DEP_CAPLESS_EN);   /* "Capless" */
    Upd(d, R82_CHOP_DAC, CHOP_CKGEN, CHOP_CKGEN);      /* "DAC 1 Clock" */
    Upd(d, R82_PWR_DIG_1, PD1_DAC_L1 | PD1_DAC_R1, PD1_DAC_L1 | PD1_DAC_R1);
    /* "HP Amp" PRE_PMU (rt5682_hp_event) */
    Upd(d, R82_HP_CTRL_2, HPC2_DAC_AMP_MUTE, 0);
    Upd(d, R82_HP_LOGIC_CTRL_2, HPL2_SIG_SOUR2_REG, HPL2_SIG_SOUR2_REG);
    Upd(d, R82_DEPOP_1, 0x0060, 0x0060);
    Upd(d, R82_DAC_ADC_DIG_VOL1, 0x00c0, 0x0080);
    Upd(d, R82_HP_CTRL_2, HPC2_DAC_L_EN | HPC2_DAC_R_EN, HPC2_DAC_L_EN | HPC2_DAC_R_EN);
    SdwDelay(10);
    Upd(d, R82_CHARGE_PUMP_1, CP_SW_SIZE_MASK, CP_SW_SIZE_L);
    Upd(d, R82_HP_CTRL_1, LR_L_MUTE | LR_R_MUTE, 0);  /* HPOL / HPOR Playback switches */
}

static void PlayDown(SDW_DEV *d)
{
    BOOLEAN rec = d->RecOn;
    Upd(d, R82_HP_CTRL_1, LR_L_MUTE | LR_R_MUTE, LR_L_MUTE | LR_R_MUTE);
    /* "HP Amp" POST_PMD */
    Upd(d, R82_HP_CTRL_2, HPC2_DAC_L_EN | HPC2_DAC_R_EN, 0);
    Upd(d, R82_CHARGE_PUMP_1, CP_SW_SIZE_MASK, CP_SW_SIZE_M);
    Upd(d, R82_DEPOP_1, 0x0060, 0);
    Upd(d, R82_DAC_ADC_DIG_VOL1, 0x00c0, 0);
    Upd(d, R82_PWR_DIG_1, PD1_DAC_L1 | PD1_DAC_R1, 0);
    Upd(d, R82_CHOP_DAC, CHOP_CKGEN, 0);
    Upd(d, R82_DEPOP_1, DEP_CAPLESS_EN, 0);
    Upd(d, R82_DEPOP_1, DEP_PUMP_EN, 0);
    Upd(d, R82_PWR_DIG_2, PD2_DAC_S1F, 0);
    Upd(d, R82_PWR_ANLG_1, PA1_HA_L | PA1_HA_R | (rec ? 0 : PA1_VREF1), 0);
    if (!rec) Upd(d, R82_PWR_ANLG_3, PA3_PLL2B | PA3_PLL2F, 0);
    Upd(d, R82_CLK_DET, CLKDET_SYS | (rec ? 0 : CLKDET_POW), 0);
}

static void RecUp(SDW_DEV *d)
{
    R82_PRIV *p = P(d);
    BOOLEAN play = d->PlayOn;
    if (!p->Hs && !play) {                              /* no headset found yet: detect again */
        TypeDetect(d);
        LOGI("sdw: %s: CBJ type %u%s", d->Name, p->JackType, p->Hs ? " (headset)" : ", headset mode forced");
    }
    Upd(d, R82_PWR_DIG_1, PD1_ADC_L1, PD1_ADC_L1);
    Upd(d, R82_PWR_DIG_2, PD2_ADC_S1F, PD2_ADC_S1F);   /* ADC Stereo1 Filter */
    if (!play) Vref1On(d, 0);
    Upd(d, R82_PWR_ANLG_2, PA2_RM1_L | PA2_MB1, PA2_RM1_L | PA2_MB1);  /* RECMIX1L, MICBIAS1 */
    Upd(d, R82_PWR_ANLG_3, PA3_PLL2B | PA3_PLL2F, PA3_PLL2B | PA3_PLL2F);
    Upd(d, R82_CHOP_ADC, CHOP_CKGEN, CHOP_CKGEN);      /* "ADC1 clock" */
    Upd(d, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | LR_R_MUTE, 0);   /* Stereo1 ADC MIXL / MIXR */
}

static void RecDown(SDW_DEV *d)
{
    BOOLEAN play = d->PlayOn;
    Upd(d, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | LR_R_MUTE, LR_L_MUTE | LR_R_MUTE);
    Upd(d, R82_PWR_DIG_1, PD1_ADC_L1, 0);
    Upd(d, R82_PWR_DIG_2, PD2_ADC_S1F, 0);
    if (!play) Upd(d, R82_PWR_ANLG_1, PA1_VREF1, 0);
    Upd(d, R82_PWR_ANLG_2, PA2_RM1_L | PA2_MB1, 0);
    if (!play) Upd(d, R82_PWR_ANLG_3, PA3_PLL2B | PA3_PLL2F, 0);
    Upd(d, R82_CHOP_ADC, CHOP_CKGEN, 0);
}

static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? d->PlayOn : d->RecOn;
    if (on) {
        if (!other) BiasOn(d);
        if (rec) RecUp(d); else PlayUp(d);
    } else {
        if (rec) RecDown(d); else PlayDown(d);
        if (!other) BiasOff(d);
    }
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    if (d->PlayOn) PlayDown(d);
    if (d->RecOn) RecDown(d);
    if (d->PlayOn || d->RecOn) BiasOff(d);
    JackOut(d);
    Wr(d, R82_RESET, 0);
}

static const USHORT g_Parts[] = { 0x5682, 0 };
const SDW_DRV g_SdwRt5682 = { "RT5682", 0x025D, g_Parts, SDW_KIND_HEADSET, Probe, Init, Stream, Shutdown };
