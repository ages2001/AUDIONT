/*
 * AUDIONT - Realtek RT714 / RT715 SoundWire microphone array codecs (capture only).
 *   SoundWire 1.2, class 1: RT715-SDCA, Linux sound/soc/codecs/rt715-sdca.c / rt715-sdca-sdw.c
 *   SoundWire 1.1, class 0: RT715 (HDA verbs over SoundWire), Linux rt715.c / rt715-sdw.c
 * The Intel machine driver uses the "aif2" DAI of both: DP4 capture, 2 channels.
 *
 *   DMIC1..4 / MIC1 / MIC2 -> ADC 24 mux -> ADC 07 \
 *   DMIC1..4 / MIC1        -> ADC 25 mux -> ADC 27 / -> DP4   (SDCA: FU0A, power PDE 23_24)
 *   (ADC 22 / 23 muxes -> ADC 09 / 08 -> DP6: not used)
 *
 * The muxes select DMIC3 for ADC 24 and DMIC4 for ADC 25 (the ALSA UCM "ADC 24 Mux" 3 /
 * "ADC 25 Mux" 4 setting of the Dell / HP boards, the 3rd / 4th DMIC on GPIO 4/5/6 that
 * rt715_sdca_io_init enables).  Registry SdwRt715Mux overrides: bits 7:4 ADC 24, 3:0 ADC 25,
 * the hardware connection values (0 MIC2 / MIC1, 2 DMIC1, 3 DMIC2, 4 DMIC3, 5 DMIC4).
 * DMIC boost +20 dB, ADC 0 dB, unmuted while capturing.
 *
 * SDCA Init: rt715_sdca_io_init (clock selector external, DMIC GPIOs, DFLL calibration
 * trigger, VAD trigger mode, SMPU interrupt mask) and hw_params of aif2 (SDW_INPUT_SEL 0xaf00,
 * CS 01 at 48 kHz).  Legacy Init: rt715_io_init (function reset, ADC mutes, DMIC pins,
 * converter streams, configuration defaults, D3), the bus clock code (rt715_clock_config) and
 * hw_params of aif2 (SDW_INPUT_SEL 0xa000, 48 kHz / 2 ch / 24 bit on the four ADCs).
 * Stream: SDCA PDE 23_24 PS0 then FU0A unmuted; legacy D0 (400 ms), ADC 07 / 27 streams and
 * gains (Linux DAPM ADC widgets on the stream id bit 4, "Capture Switch").
 */
#include "sdw_rtcom.h"

/* --- SDCA --- */
#define FN_MIC                  0x02
#define NID_VENDOR_REG          0x20
#define NID_HDA_CTL             0x61
#define PRODUCT_NUM             0x00
#define AD_FUNC_EN              0x36
#define REV_1                   0x37
#define SDW_INPUT_SEL           0x39
#define DFLL_VAD                0x44
#define HDA_LEGACY_MUX_CTL1     0x00
#define INT_MASK                0x005e
#define ENT_CS_FREQ_IND_EN      0x01
#define ENT_FU_ADC7_27_VOL      0x0a        /* FU0A */
#define ENT_SMPU_TRIG_ST_EN     0x05
#define ENT_FU_DMIC_GAIN_EN     0x0e        /* FU0E */
#define ENT_CX_CLK_SEL_EN       0x10
#define ENT_CREQ_POW_EN         0x18        /* PDE 23_24 */
#define CTL_CX_CLK_SEL          0x01
#define CTL_FREQ_IND            0x10
#define CTL_SMPU_TRIG_EN        0x10
#define CTL_DMIC_GAIN           0x0b
#define RATE_48000              0x09
/* --- HDA verbs (rt715.h) --- */
#define NID_MIC_ADC             0x07
#define NID_LINE_ADC            0x08
#define NID_MIX_ADC             0x09
#define NID_DMIC1               0x12
#define NID_DMIC2               0x13
#define NID_DMIC3               0x1d
#define NID_DMIC4               0x29
#define NID_MUX_IN3             0x24
#define NID_MUX_IN4             0x25
#define NID_MIX_ADC2            0x27
#define VD_CLEAR_CTRL           0x01
#define CLEAR_HIDDEN_REG        (1U << 15)
#define SET_AUDIO_POWER_STATE   (RT_VERB_SET_POWER_STATE | 0x01)
#define AC_PWRST_D0             0
#define AC_PWRST_D3             3
#define POWER_UP_DELAY_MS       400
#define FORMAT_48K_2CH_24BIT    0x31        /* [14] 48 kHz, [6:4] 24 bit, [3:0] channels - 1 */
#define GAIN_0DB                0x17        /* in_vol_tlv: -17.25 dB + 0.75 dB steps */
#define BOOST_20DB              2           /* mic_vol_tlv: 10 dB steps */

#define MUX_DEFAULT             0x45        /* ADC 24 = DMIC3, ADC 25 = DMIC4 */

typedef struct _RT715_PRIV {
    UCHAR   Sdca;
    UCHAR   Mux24, Mux25;
    UCHAR   HwVer;
} RT715_PRIV;
typedef char Rt715PrivFits[sizeof(RT715_PRIV) <= sizeof(((SDW_DEV *)0)->Priv) ? 1 : -1];

static RT715_PRIV *P(SDW_DEV *d) { return (RT715_PRIV *)d->Priv; }

static BOOLEAN Probe(SDW_DEV *d)
{
    RT715_PRIV *x = P(d);
    ULONG mux = OsRegGetDword(L"SdwRt715Mux", MUX_DEFAULT);
    memset(x, 0, sizeof(*x));
    x->Sdca = d->Class == 1;                            /* Linux ids: (1.2, class 1) SDCA, (1.1, class 0) HDA verbs */
    x->Mux24 = (UCHAR)((mux >> 4) & 0xf);
    x->Mux25 = (UCHAR)(mux & 0xf);
    d->Kind = SDW_KIND_MIC;
    memset(&d->Play, 0, sizeof(d->Play));
    d->Rec.Num = 4; d->Rec.Ch = 2; d->Rec.Type = SDW_DP_FULL; d->Rec.CpSm = FALSE;     /* simple_ch_prep_sm */
    d->Paging = x->Sdca;                                /* rt715-sdw.c: paging_support = false */
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* RT715-SDCA                                                           */
/* ------------------------------------------------------------------ */
static BOOLEAN SdcaInit(SDW_DEV *d)
{
    RT715_PRIV *x = P(d);
    ULONG v = 0, e0 = d->Errors;
    if (!SdwRtIdxRd(d, NID_VENDOR_REG, PRODUCT_NUM, &v)) return FALSE;
    x->HwVer = (UCHAR)(v & 0xf);
    /* rt715_sdca_io_init */
    SdwWrite(d, SDW_SDCA_CTL(FN_MIC, ENT_CX_CLK_SEL_EN, CTL_CX_CLK_SEL, 0), 0x1);  /* clock selector external */
    if (x->HwVer == 0) {                                /* GPIO 4/5/6: 3rd / 4th DMIC */
        SdwRtIdxUpd(d, NID_VENDOR_REG, AD_FUNC_EN, 0x54, 0x54);
    } else if (x->HwVer == 1) {
        SdwRtIdxUpd(d, NID_VENDOR_REG, AD_FUNC_EN, 0x55, 0x55);
        SdwRtIdxUpd(d, NID_VENDOR_REG, REV_1, 0x40, 0x40);
    }
    SdwRtIdxUpd(d, NID_VENDOR_REG, DFLL_VAD, 0x1, 0x1);  /* DFLL calibration trigger */
    SdwWrite(d, SDW_SDCA_CTL(FN_MIC, ENT_SMPU_TRIG_ST_EN, CTL_SMPU_TRIG_EN, 0), 0x2);  /* trigger mode VAD */
    SdwUpdate(d, INT_MASK, 0x1, 0x1);                   /* SMPU-1 interrupt enable mask */
    /* hw_params of aif2 (DP4) at 48 kHz */
    SdwRtIdxWr(d, NID_VENDOR_REG, SDW_INPUT_SEL, 0xaf00);
    SdwWrite(d, SDW_SDCA_CTL(FN_MIC, ENT_CS_FREQ_IND_EN, CTL_FREQ_IND, 0), RATE_48000);
    /* ADC 24 / 25 muxes (HDA_LEGACY_MUX_CTL1 [7:4] / [3:0]), DMIC boost, ADC 0 dB muted, PS3 */
    SdwRtIdxUpd(d, NID_HDA_CTL, HDA_LEGACY_MUX_CTL1, 0xff, (ULONG)(x->Mux24 << 4) | x->Mux25);
    SdcaVolume(d, FN_MIC, ENT_FU_DMIC_GAIN_EN, CTL_DMIC_GAIN, 0xFF, 20 * 256);
    SdcaVolume(d, FN_MIC, ENT_FU_ADC7_27_VOL, SDCA_CTL_FU_VOLUME, 3, 0);
    SdcaMute(d, FN_MIC, ENT_FU_ADC7_27_VOL, 3, TRUE);
    SdcaPower(d, FN_MIC, ENT_CREQ_POW_EN, SDCA_PS3, 0);
    LOGI("sdw: %s: RT715-SDCA ver %u, DP4 from ADC 07 / 27 (mux %x / %x), %u bus errors in init", d->Name,
         x->HwVer, x->Mux24, x->Mux25, d->Errors - e0);
    return d->Errors - e0 < 8;
}

/* ------------------------------------------------------------------ */
/* RT715 (HDA verbs)                                                    */
/* ------------------------------------------------------------------ */
static ULONG Sid(ULONG nid) { return RT_VERB_SET_CHANNEL_STREAMID | nid; }
static ULONG Gain(ULONG nid) { return RT_SET_AMP_GAIN_MUTE_H | nid; }

static const ULONG g_Init[] = {
    /* mute nid 08h / 09h / 07h / 27h */
    0x7300 | NID_LINE_ADC, 0xb080, 0x7300 | NID_MIX_ADC, 0xb080,
    0x7300 | NID_MIC_ADC, 0xb080, 0x7300 | NID_MIX_ADC2, 0xb080,
    /* pin widgets */
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_DMIC1, 0x20, RT_VERB_SET_PIN_WIDGET_CONTROL | NID_DMIC2, 0x20,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_DMIC3, 0x20, RT_VERB_SET_PIN_WIDGET_CONTROL | NID_DMIC4, 0x20,
    /* converter streams */
    RT_VERB_SET_CHANNEL_STREAMID | NID_LINE_ADC, 0x10, RT_VERB_SET_CHANNEL_STREAMID | NID_MIX_ADC, 0x10,
    RT_VERB_SET_CHANNEL_STREAMID | NID_MIC_ADC, 0x10, RT_VERB_SET_CHANNEL_STREAMID | NID_MIX_ADC2, 0x10,
    /* configuration default (verbs 71c..71f: SET_CONFIG_DEFAULT1..4) */
    0x4c00 | NID_DMIC1, 0xd0, 0x4d00 | NID_DMIC1, 0x11, 0x4e00 | NID_DMIC1, 0xa1, 0x4f00 | NID_DMIC1, 0x81,
    0x4c00 | NID_DMIC2, 0xd1, 0x4d00 | NID_DMIC2, 0x11, 0x4e00 | NID_DMIC2, 0xa1, 0x4f00 | NID_DMIC2, 0x81,
    0x4c00 | NID_DMIC3, 0xd0, 0x4d00 | NID_DMIC3, 0x11, 0x4e00 | NID_DMIC3, 0xa1, 0x4f00 | NID_DMIC3, 0x81,
    0x4c00 | NID_DMIC4, 0xd1, 0x4d00 | NID_DMIC4, 0x11, 0x4e00 | NID_DMIC4, 0xa1, 0x4f00 | NID_DMIC4, 0x81,
    SET_AUDIO_POWER_STATE, AC_PWRST_D3,
};

static BOOLEAN HdaInit(SDW_DEV *d)
{
    RT715_PRIV *x = P(d);
    ULONG e0 = d->Errors;
    if (!SdwRtClockConfig(d)) LOGE("sdw: %s: bus clock code not set", d->Name);   /* rt715_clock_config */
    /* rt715_reset */
    if (!SdwRtHdaWr(d, RT_FUNC_RESET, 0)) return FALSE;
    SdwRtHdaIdxUpd(d, NID_VENDOR_REG, VD_CLEAR_CTRL, CLEAR_HIDDEN_REG, CLEAR_HIDDEN_REG);
    SdwRtHdaSeq(d, g_Init, ARRAYSIZE(g_Init) / 2);
    /* hw_params of aif2: DP4, 48 kHz, 2 channels, 24 bit on the four ADC converters */
    SdwRtHdaIdxWr(d, NID_VENDOR_REG, SDW_INPUT_SEL, 0xa000);
    SdwRtHdaWr(d, 0x7200 | NID_MIC_ADC, FORMAT_48K_2CH_24BIT);
    SdwRtHdaWr(d, 0x7200 | NID_LINE_ADC, FORMAT_48K_2CH_24BIT);
    SdwRtHdaWr(d, 0x7200 | NID_MIX_ADC, FORMAT_48K_2CH_24BIT);
    SdwRtHdaWr(d, 0x7200 | NID_MIX_ADC2, FORMAT_48K_2CH_24BIT);
    /* ADC 24 / 25 muxes (connection select of MUX_IN3 / MUX_IN4), DMIC boosts +20 dB (input amps) */
    SdwRtHdaWr(d, RT_VERB_SET_CONNECT_SEL | NID_MUX_IN3, x->Mux24);
    SdwRtHdaWr(d, RT_VERB_SET_CONNECT_SEL | NID_MUX_IN4, x->Mux25);
    SdwRtHdaWr(d, Gain(NID_DMIC1), RT_AMP_IN_LR | BOOST_20DB);
    SdwRtHdaWr(d, Gain(NID_DMIC2), RT_AMP_IN_LR | BOOST_20DB);
    SdwRtHdaWr(d, Gain(NID_DMIC3), RT_AMP_IN_LR | BOOST_20DB);
    SdwRtHdaWr(d, Gain(NID_DMIC4), RT_AMP_IN_LR | BOOST_20DB);
    /* DAPM: ADC widgets off (stream id bit 4) until capture starts */
    SdwRtHdaWr(d, Sid(NID_MIC_ADC), 0x00);
    SdwRtHdaWr(d, Sid(NID_MIX_ADC2), 0x00);
    LOGI("sdw: %s: RT715 (HDA verbs), DP4 from ADC 07 / 27 (mux %x / %x), bus clock %u, %u bus errors in init",
         d->Name, x->Mux24, x->Mux25, SdwRtBusClock(d), d->Errors - e0);
    return d->Errors - e0 < 8;
}

static BOOLEAN Init(SDW_DEV *d)
{
    return P(d)->Sdca ? SdcaInit(d) : HdaInit(d);
}

static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    if (!rec) return;
    if (P(d)->Sdca) {
        if (on) {
            SdcaPower(d, FN_MIC, ENT_CREQ_POW_EN, SDCA_PS0, 0);
            SdcaMute(d, FN_MIC, ENT_FU_ADC7_27_VOL, 3, FALSE);
        } else {
            SdcaMute(d, FN_MIC, ENT_FU_ADC7_27_VOL, 3, TRUE);
            SdcaPower(d, FN_MIC, ENT_CREQ_POW_EN, SDCA_PS3, 0);
        }
        return;
    }
    if (on) {                                           /* set_bias_level PREPARE, ADC widgets, Capture Switch */
        SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D0);
        SdwDelay(POWER_UP_DELAY_MS);
        SdwRtHdaWr(d, Sid(NID_MIC_ADC), 0x10);
        SdwRtHdaWr(d, Sid(NID_MIX_ADC2), 0x10);
        SdwRtHdaWr(d, Gain(NID_MIC_ADC), RT_AMP_IN_LR | GAIN_0DB);
        SdwRtHdaWr(d, Gain(NID_MIX_ADC2), RT_AMP_IN_LR | GAIN_0DB);
    } else {
        SdwRtHdaWr(d, Gain(NID_MIC_ADC), RT_AMP_IN_LR | RT_AMP_MUTE | GAIN_0DB);
        SdwRtHdaWr(d, Gain(NID_MIX_ADC2), RT_AMP_IN_LR | RT_AMP_MUTE | GAIN_0DB);
        SdwRtHdaWr(d, Sid(NID_MIC_ADC), 0x00);
        SdwRtHdaWr(d, Sid(NID_MIX_ADC2), 0x00);
        SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D3);
    }
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    if (d->RecOn) Stream(d, TRUE, FALSE);
    else if (P(d)->Sdca) SdcaPower(d, FN_MIC, ENT_CREQ_POW_EN, SDCA_PS3, 0);
    else SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D3);
}

static const USHORT g_Parts[] = { 0x0714, 0x0715, 0 };
const SDW_DRV g_SdwRt715 = { "RT715", 0x025D, g_Parts, SDW_KIND_MIC, Probe, Init, Stream, Shutdown };
