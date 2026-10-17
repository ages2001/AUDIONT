/*
 * AUDIONT - Realtek RT700 and RT711 (non-SDCA, SoundWire 1.1, class 0) jack codecs: HDA
 * codec verbs carried over SoundWire registers.  Linux sound/soc/codecs/rt700.c / rt700-sdw.c,
 * rt711.c / rt711-sdw.c (the class 1 RT711-SDCA is sdw_rt711.c).
 *
 *   RT700  DP1 -> DAC Front (02h) -> HPO mux "Front" -> HP (21h)  and  -> SPK PGA -> SPK (14h, EAPD)
 *          MIC2 (19h, AMIC boost) -> ADC 22 mux (22h) -> ADC 09 -> DP2          (aif1)
 *   RT711  DP3 -> DAC Surround (03h) -> HP (21h)
 *          MIC2 (19h, AMIC boost) -> ADC 23 mux (23h) -> ADC 08 -> DP4          (aif1)
 *
 * No jack detection: unsolicited responses off (rt7xx_jack_init without a jack), the combo
 * jack auto switch of the codec selects the headset pinout by itself.  Headphones and the
 * RT700 speaker output play together.
 * Init: rt700_io_init / rt711_io_init (reset, pin widgets, configuration defaults, index
 * registers, D3; RT711 DC calibration), the bus clock code (rt7xx_clock_config), hw_params
 * (48 kHz, 2 channels, 24 bit converter formats), the muxes to MIC2 / Front; volumes DAC 0 dB,
 * ADC 0 dB, AMIC +20 dB.  Stream: set_bias_level (D0 / D3) and the DAPM events of the DAC /
 * ADC widgets (stream id 0x10) and of the HP / SPK output amplifiers (unmute, 50 ms on mute).
 */
#include "sdw_rtcom.h"

#define NID_AFG                 0x01
#define NID_DAC_OUT1            0x02        /* RT700 DAC Front */
#define NID_DAC_OUT2            0x03        /* DAC Surround */
#define NID_ADC_IN1             0x09
#define NID_ADC_IN2             0x08
#define NID_DMIC1               0x12
#define NID_DMIC2               0x13
#define NID_SPK_OUT             0x14
#define NID_MIC2                0x19
#define NID_LINE1               0x1a
#define NID_LINE2               0x1b
#define NID_VENDOR_REG          0x20
#define NID_HP_OUT              0x21
#define NID_MIXER_IN1           0x22
#define NID_MIXER_IN2           0x23
#define NID_INLINE_CMD          0x55
#define NID_VENDOR_CALI         0x58
/* index registers */
#define DAC_DC_CALI_CTL1        0x00
#define PARA_VERB_CTL           0x1a
#define COMBO_JACK_AUTO_CTL1    0x45
#define INLINE_CMD_CTL          0x48
#define DIGITAL_MISC_CTRL4      0x4a
#define VREFOUT_CTL             0x6b
#define FSM_CTL                 0x6f
#define TX_RX_MUX_CTL           0x91
#define HIDDEN_REG_SW_RESET     (1U << 14)
#define DAC_DC_CALI_TRIGGER     (1U << 15)
#define DEPOP_CTL               0x3
/* verbs */
#define SET_AUDIO_POWER_STATE   (RT_VERB_SET_POWER_STATE | NID_AFG)
#define AC_PWRST_D0             0
#define AC_PWRST_D3             3
#define EAPD_HIGH               0x2
#define FORMAT_48K_2CH_24BIT    0x31        /* [6:4] 24 bit, [3:0] channels - 1 */
#define DAC_0DB                 0x57        /* out_vol_tlv: -65.25 dB + 0.75 dB steps */
#define ADC_0DB                 0x17        /* in_vol_tlv: -17.25 dB + 0.75 dB steps */
#define AMIC_20DB               2           /* mic_vol_tlv: 10 dB steps */

#define IS_711(d)   ((d)->Part == 0x0711)

static ULONG Gain(ULONG nid) { return RT_SET_AMP_GAIN_MUTE_H | nid; }
static ULONG Sid(ULONG nid) { return RT_VERB_SET_CHANNEL_STREAMID | nid; }
static ULONG Unsol(ULONG nid) { return RT_VERB_SET_UNSOLICITED_ENABLE | nid; }

/* part 0x700, or 0x711 of class 0 (SoundWire 1.1: rt711-sdw.c); class 1 RT711-SDCA is sdw_rt711.c */
static BOOLEAN Probe(SDW_DEV *d)
{
    if (IS_711(d) && d->Class == 1) return FALSE;
    d->Kind = SDW_KIND_HEADSET;
    d->Play.Num = IS_711(d) ? 3 : 1; d->Play.Ch = 2; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = FALSE;
    d->Rec.Num = IS_711(d) ? 4 : 2; d->Rec.Ch = 2; d->Rec.Type = SDW_DP_FULL; d->Rec.CpSm = FALSE;
    d->Paging = FALSE;                                  /* paging_support = false */
    return TRUE;
}

/* rt700_io_init after the reset */
static const ULONG g_Rt700[] = {
    SET_AUDIO_POWER_STATE, AC_PWRST_D0,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_HP_OUT, 0x40,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_SPK_OUT, 0x40,
    RT_VERB_SET_EAPD_BTLENABLE | NID_SPK_OUT, EAPD_HIGH,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_DMIC1, 0x20,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_DMIC2, 0x20,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_MIC2, 0x20,
    /* configuration default */
    0x4f12, 0x91, 0x4e12, 0xd6, 0x4d12, 0x11, 0x4c12, 0x20,
    0x4f13, 0x91, 0x4e13, 0xd6, 0x4d13, 0x11, 0x4c13, 0x21,
    0x4f19, 0x02, 0x4e19, 0xa1, 0x4d19, 0x90, 0x4c19, 0x80,
    /* enable LINE2 */
    0x371b, 0x40, 0x731b, 0xb0, 0x839b, 0x00,
    /* index registers */
    0x75204a, 0x201b, 0x752045, 0x5089, 0x75206b, 0x5064, 0x752048, 0xd249,
    SET_AUDIO_POWER_STATE, AC_PWRST_D3,
};

/* rt711_io_init after the reset */
static const ULONG g_Rt711[] = {
    SET_AUDIO_POWER_STATE, AC_PWRST_D0,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_MIC2, 0x25,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_HP_OUT, 0xc0,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_DMIC1, 0x20,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_DMIC2, 0x20,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_LINE1, 0x20,
    RT_VERB_SET_PIN_WIDGET_CONTROL | NID_LINE2, 0x20,
    /* mute HP / ADC1 / ADC2 */
    RT_SET_AMP_GAIN_MUTE_H | NID_HP_OUT, 0xa080, RT_SET_AMP_GAIN_MUTE_H | NID_HP_OUT, 0x9080,
    RT_SET_AMP_GAIN_MUTE_H | NID_ADC_IN2, 0x6080, RT_SET_AMP_GAIN_MUTE_H | NID_ADC_IN2, 0x5080,
    RT_SET_AMP_GAIN_MUTE_H | NID_ADC_IN1, 0x6080, RT_SET_AMP_GAIN_MUTE_H | NID_ADC_IN1, 0x5080,
    /* configuration default */
    0x4f12, 0x91, 0x4e12, 0xd6, 0x4d12, 0x11, 0x4c12, 0x20,
    0x4f13, 0x91, 0x4e13, 0xd6, 0x4d13, 0x11, 0x4c13, 0x21,
    0x4c21, 0xf0, 0x4d21, 0x11, 0x4e21, 0x11, 0x4f21, 0x01,
    /* data port arrangement, index registers */
    0x752000 | TX_RX_MUX_CTL, 0x0154,
    0x752000 | DIGITAL_MISC_CTRL4, 0x201b, 0x752000 | COMBO_JACK_AUTO_CTL1, 0x5089,
    0x752000 | VREFOUT_CTL, 0x5064, 0x752000 | INLINE_CMD_CTL, 0xd249,
    SET_AUDIO_POWER_STATE, AC_PWRST_D3,
};

/* rt711_calibration: HP DC offset calibration in manual mode, then the depop mode */
static void Rt711Calibrate(SDW_DEV *d)
{
    ULONG v = DAC_DC_CALI_TRIGGER, i;
    SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D0);
    SdwRtHdaIdxUpd(d, NID_VENDOR_REG, FSM_CTL, 0xf, 0x0);
    SdwRtHdaIdxUpd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, DAC_DC_CALI_TRIGGER, DAC_DC_CALI_TRIGGER);
    SdwRtHdaIdxRd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, &v);
    for (i = 0; i < 500 && (v & DAC_DC_CALI_TRIGGER); i++) {
        SdwDelay(10);
        if (!SdwRtHdaIdxRd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, &v)) break;
    }
    if (v & DAC_DC_CALI_TRIGGER) LOGE("sdw: %s: calibration timed out", d->Name);
    SdwRtHdaIdxUpd(d, NID_VENDOR_REG, FSM_CTL, 0xf, DEPOP_CTL);
    SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D3);
}

static BOOLEAN Init(SDW_DEV *d)
{
    ULONG e0 = d->Errors, dac = IS_711(d) ? NID_DAC_OUT2 : NID_DAC_OUT1;
    ULONG adc = IS_711(d) ? NID_ADC_IN2 : NID_ADC_IN1, mux = IS_711(d) ? NID_MIXER_IN2 : NID_MIXER_IN1;
    if (!SdwRtClockConfig(d)) LOGE("sdw: %s: bus clock code not set", d->Name);   /* rt7xx_clock_config */
    /* reset */
    if (!SdwRtHdaWr(d, RT_FUNC_RESET, 0)) return FALSE;
    if (IS_711(d)) {
        SdwRtHdaIdxUpd(d, NID_VENDOR_REG, PARA_VERB_CTL, HIDDEN_REG_SW_RESET, HIDDEN_REG_SW_RESET);
        SdwRtHdaSeq(d, g_Rt711, ARRAYSIZE(g_Rt711) / 2);
        Rt711Calibrate(d);
    } else {
        SdwRtHdaWr(d, 0x7520, 0x001a);                  /* index 1ah (PARA_VERB_CTL) = 0xc003 */
        SdwRtHdaWr(d, 0x7420, 0xc003);
        SdwRtHdaSeq(d, g_Rt700, ARRAYSIZE(g_Rt700) / 2);
    }
    /* jack init without a jack: unsolicited responses off */
    SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D0);
    SdwRtHdaWr(d, Unsol(NID_MIC2), 0x00);
    SdwRtHdaWr(d, Unsol(NID_HP_OUT), 0x00);
    SdwRtHdaWr(d, Unsol(NID_INLINE_CMD), 0x00);
    /* hw_params: 48 kHz, 2 channels, 24 bit */
    SdwRtHdaWr(d, 0x7200 | dac, FORMAT_48K_2CH_24BIT);
    SdwRtHdaWr(d, 0x7200 | NID_ADC_IN1, FORMAT_48K_2CH_24BIT);
    if (IS_711(d)) SdwRtHdaWr(d, 0x7200 | NID_ADC_IN2, FORMAT_48K_2CH_24BIT);
    /* routing: ADC mux = MIC2 (index 0), RT700 HPO mux = Front; gains */
    SdwRtHdaWr(d, RT_VERB_SET_CONNECT_SEL | mux, 0);
    if (!IS_711(d)) SdwRtHdaWr(d, RT_VERB_SET_CONNECT_SEL | NID_HP_OUT, 0);
    SdwRtHdaWr(d, Gain(dac), RT_AMP_OUT_LR | DAC_0DB);
    SdwRtHdaWr(d, Gain(adc), RT_AMP_IN_LR | RT_AMP_MUTE | ADC_0DB);
    SdwRtHdaWr(d, Gain(NID_MIC2), RT_AMP_IN_LR | AMIC_20DB);
    SdwRtHdaWr(d, Gain(NID_HP_OUT), RT_AMP_OUT_LR | RT_AMP_MUTE);
    if (!IS_711(d)) SdwRtHdaWr(d, Gain(NID_SPK_OUT), RT_AMP_OUT_LR | RT_AMP_MUTE);
    SdwRtHdaWr(d, Sid(dac), 0x00);
    SdwRtHdaWr(d, Sid(adc), 0x00);
    SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D3);
    LOGI("sdw: %s: %s (HDA verbs), DP%u / DP%u, bus clock %u, %u bus errors in init", d->Name,
         IS_711(d) ? "RT711" : "RT700", d->Play.Num, d->Rec.Num, SdwRtBusClock(d), d->Errors - e0);
    return d->Errors - e0 < 16;
}

static void Output(SDW_DEV *d, BOOLEAN on)
{
    ULONG dac = IS_711(d) ? NID_DAC_OUT2 : NID_DAC_OUT1;
    if (on) {
        SdwRtHdaWr(d, Sid(dac), 0x10);                  /* DAC POST_PMU */
        SdwRtHdaWr(d, Gain(NID_HP_OUT), RT_AMP_OUT_LR); /* HPO mux / RT711 DAC Surround POST_PMU */
        if (!IS_711(d)) SdwRtHdaWr(d, Gain(NID_SPK_OUT), RT_AMP_OUT_LR);  /* SPK PGA POST_PMU */
    } else {
        if (!IS_711(d)) SdwRtHdaWr(d, Gain(NID_SPK_OUT), RT_AMP_OUT_LR | RT_AMP_MUTE);
        SdwRtHdaWr(d, Gain(NID_HP_OUT), RT_AMP_OUT_LR | RT_AMP_MUTE);
        SdwDelay(50);
        SdwRtHdaWr(d, Sid(dac), 0x00);
    }
}

static void Input(SDW_DEV *d, BOOLEAN on)
{
    ULONG adc = IS_711(d) ? NID_ADC_IN2 : NID_ADC_IN1;
    if (on) {
        SdwRtHdaWr(d, Sid(adc), 0x10);                  /* ADC POST_PMU */
        SdwRtHdaWr(d, Gain(adc), RT_AMP_IN_LR | ADC_0DB);   /* "ADC 0x Capture Switch" */
    } else {
        SdwRtHdaWr(d, Gain(adc), RT_AMP_IN_LR | RT_AMP_MUTE | ADC_0DB);
        SdwRtHdaWr(d, Sid(adc), 0x00);
    }
}

static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? d->PlayOn : d->RecOn;
    if (on) {
        if (!other) SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D0);   /* bias PREPARE */
        if (rec) Input(d, TRUE); else Output(d, TRUE);
    } else {
        if (rec) Input(d, FALSE); else Output(d, FALSE);
        if (!other) SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D3);   /* bias STANDBY */
    }
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    if (d->PlayOn) Output(d, FALSE);
    if (d->RecOn) Input(d, FALSE);
    SdwRtHdaWr(d, SET_AUDIO_POWER_STATE, AC_PWRST_D3);
}

static const USHORT g_Parts[] = { 0x0700, 0x0711, 0 };
const SDW_DRV g_SdwRt700 = { "RT700", 0x025D, g_Parts, SDW_KIND_HEADSET, Probe, Init, Stream, Shutdown };
