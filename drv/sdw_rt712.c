/*
 * AUDIONT - Realtek RT712-SDCA / RT713-SDCA (RT716 / RT717) SoundWire jack codecs with a
 * speaker amplifier (not RT713) and, on the VB silicon, digital microphones; and the separate
 * RT712 / RT713 DMIC peripheral (parts 0x1712 ..).  Linux sound/soc/codecs/rt712-sdca.c,
 * rt712-sdca-sdw.c, rt712-sdca-dmic.c, machine side sdw_utils/soc_sdw_utils.c.
 *
 *   function 1 jack  DP1 -> FU05 -> HP (PDE 40);  MIC2 -> ADC 23 mux -> FU0F (PDE 12, boost FU44) -> DP4
 *   function 4 amp   DP3 -> FU06 -> OT23 class D (PDE 23)                     (not on the RT713)
 *   function 2 mic   DMIC1/2 -> ADC 0A/0B mux -> FU1E (PDE 11, boost FU15) -> DP8   (VB only)
 *   DMIC part        DMIC1/2 -> ADC 25/26 mux -> FU1E (PDE 11, boost FU15) -> DP2
 *
 * The bus core runs one playback and one capture port per peripheral.  Playback is the jack
 * port DP1; the amplifier port DP3 is a copy of it (same transport parameters, programmed by
 * Stream), so headphones and speakers play together - there is no jack detection to switch
 * between them.  Capture is the headset microphone (DP4), or the DMIC function (DP8) with the
 * registry value SdwDmic = 1 or when the BIOS describes no jack function.  Only functions the
 * ACPI DisCo description lists are used (Linux is_sdca_endpoint_present).
 *
 * Init follows rt712_sdca_io_init: reset, JD_PRODUCT_NUM (hw id 7:712 6:713 5:716 4:717, VA / VB),
 * rt712_sdca_va_io_init or rt712_sdca_vb_io_init (24.576 MHz clock patches, HP / class D DC
 * calibration), the jack detection left disabled (rt712_sdca_jack_init without a jack: HID1 and
 * detected-mode events off), SW_CONFIG1; hw_params at 48 kHz on CS01 / CS11 / CS31 / CS1F / CS1C.
 * The Group Entity GE49 gets the detected mode (headset when none) when a stream starts.
 * Volumes: DAC FU05 / FU06 0 dB, ADC FU0F / FU1E 0 dB, boosts FU44 / FU15 +20 dB.
 */
#include "sdw_rtcom.h"

#define FN_JACK         0x01
#define FN_MIC          0x02
#define FN_AMP          0x04
/* NIDs */
#define NID_VENDOR_REG  0x20
#define NID_CHARGE_PUMP 0x57
#define NID_VENDOR_CALI 0x58
#define NID_ULTRA_SOUND 0x59
#define NID_IMS_DRE     0x5b
#define NID_ANALOG_CTL  0x5f
#define NID_HDA_CTL     0x61
/* index registers, NID 20h */
#define JD_PRODUCT_NUM          0x00
#define ANALOG_BIAS_CTL3        0x04
#define JD_CTL1                 0x09
#define JD_CTL3                 0x0b
#define IO_CTL                  0x0c
#define LDO2_3_CTL1             0x0e
#define PARA_VERB_CTL           0x1a
#define CC_DET1                 0x24
#define CLASSD_AMP_CTL1         0x37
#define CLASSD_AMP_CTL6         0x3c
#define DIGITAL_MISC_CTRL4      0x4a
#define FSM_CTL                 0x67
#define SW_CONFIG1              0x8a
/* NID 57h / 58h / 59h / 5bh / 5fh */
#define HP_DET_CTL3             0x0c
#define DAC_DC_CALI_CTL1        0x00
#define ULTRA_SOUND_DETECTOR6   0x1e
#define IMS_DIGITAL_CTL1        0x00
#define IMS_DIGITAL_CTL5        0x05
#define SEL_VEE2_HP_CTL1        0x23
#define HP_DETECT_RLDET_CTL1    0x29
#define HP_DETECT_RLDET_CTL2    0x2a
#define MISC_POWER_CTL0         0x00
#define MISC_POWER_CTL7         0x08
/* NID 61h */
#define HDA_LEGACY_CONFIG_CTL0  0x06
#define HDA_LEGACY_RESET_CTL    0x08
#define HDA_LEGACY_GPIO_WAKE_EN_CTL 0x0e
#define DMIC_ENT_FLOAT_CTL      0x10
#define DMIC_GAIN_ENT_FLOAT_CTL0 0x11
#define DMIC_GAIN_ENT_FLOAT_CTL2 0x13
#define ADC_ENT_FLOAT_CTL       0x15
#define ADC_VOL_CH_FLOAT_CTL2   0x18
#define DAC03_HP_PDE_FLOAT_CTL  0x22
#define MIC2_LINE2_PDE_FLOAT_CTL 0x23
#define ADC0A_08_PDE_FLOAT_CTL  0x26
#define ADC0B_11_PDE_FLOAT_CTL  0x27
#define DMIC1_2_PDE_FLOAT_CTL   0x2b
#define AMP_PDE_FLOAT_CTL       0x2c
#define I2S_IN_OUT_PDE_FLOAT_CTL 0x2f
#define GE_RELATED_CTL1         0x45
#define MIXER_CTL1              0x53
#define EAPD_CTL                0x55
#define MISC_CTL_FOR_UAJ        0x72
#define ADC0A_CS_ADC0B_FU_FLOAT_CTL 0xa2
#define DMIC2_FU_IT_FLOAT_CTL   0xa6
#define ADC0B_FU_CH12_FLOAT_CTL 0xb0
#define DMIC2_FU_CH12_FLOAT_CTL 0xb1
#define HIDDEN_REG_SW_RESET     (1U << 14)
#define DAC_DC_CALI_TRIGGER     (1U << 15)
/* SoundWire registers */
#define SDW_ROOT_CLK            0x004d      /* SCP_BusClock_Base, written by the bus core */
#define PLL2_CONF1              0x2f3a
#define PLL2_CONF2              0x2f3d
#define PLL2_CONF3              0x2f41
#define RC_CAL                  0x3201
#define CLK_FREQ_24_576MHZ      3
/* SDCA entities / controls */
#define ENT0            0x00
#define ENT_GE49        0x49
#define ENT_FU05        0x05
#define ENT_FU06        0x06
#define ENT_FU0F        0x0f
#define ENT_FU1E        0x1e
#define ENT_FU15        0x15
#define ENT_FU44        0x44
#define ENT_PDE23       0x23
#define ENT_PDE40       0x40
#define ENT_PDE11       0x11
#define ENT_PDE12       0x12
#define ENT_CS01        0x01
#define ENT_CS11        0x11
#define ENT_CS1F        0x1f
#define ENT_CS1C        0x1c
#define ENT_CS31        0x31
#define ENT_OT23        0x42
#define ENT_IT26        0x26
#define ENT_IT09        0x09
#define CTL_VENDOR_DEF  0x30
#define CTL_FUNC_STATUS 0x10
#define FUNCTION_NEEDS_INITIALIZATION 0x20
#define RATE_48000      0x09
/* JD_PRODUCT_NUM */
#define HW_ID_713       0x6
#define VER_VA          0

typedef struct _RT712_PRIV {
    UCHAR   DmicPart;               /* the separate DMIC peripheral (0x1712 ..) */
    UCHAR   Fn;                     /* RT_FN_xxx in use */
    UCHAR   Amp;                    /* amplifier port mirrored from the jack port (3), 0: none */
    UCHAR   RecDmic;                /* capture from the DMIC function */
    UCHAR   HwId, Ver;
} RT712_PRIV;
typedef char Rt712PrivFits[sizeof(RT712_PRIV) <= sizeof(((SDW_DEV *)0)->Priv) ? 1 : -1];

static RT712_PRIV *P(SDW_DEV *d) { return (RT712_PRIV *)d->Priv; }

static BOOLEAN IdxWr(SDW_DEV *d, ULONG nid, ULONG reg, ULONG v) { return SdwRtIdxWr(d, nid, reg, v); }

static void SetPort(SDW_DPN *p, ULONG num)
{
    p->Num = (UCHAR)num; p->Ch = 2; p->Type = SDW_DP_FULL; p->CpSm = FALSE;     /* simple_ch_prep_sm */
}

static BOOLEAN Probe(SDW_DEV *d)
{
    RT712_PRIV *x = P(d);
    ULONG fn;
    memset(x, 0, sizeof(*x));
    memset(&d->Play, 0, sizeof(d->Play));
    memset(&d->Rec, 0, sizeof(d->Rec));
    d->Paging = TRUE;
    if (d->Part & 0x1000) {                             /* rt712-sdca-dmic.c: DP2 capture only */
        x->DmicPart = TRUE;
        x->Fn = RT_FN_MIC;
        d->Kind = SDW_KIND_MIC;
        SetPort(&d->Rec, 2);
        return TRUE;
    }
    if (d->Class != 1) return FALSE;
    fn = SdwRtFuncs(d);
    if (d->Part == 0x0713) fn &= ~RT_FN_AMP;            /* one DAI (jack) on the RT713 */
    x->Fn = (UCHAR)fn;
    if (fn & RT_FN_JACK) {
        SetPort(&d->Play, 1);
        if (fn & RT_FN_AMP) x->Amp = 3;
    } else if (fn & RT_FN_AMP) {
        SetPort(&d->Play, 3);
    }
    if ((fn & RT_FN_MIC) && (!(fn & RT_FN_JACK) || SdwRtPreferDmic())) {
        SetPort(&d->Rec, 8);
        x->RecDmic = TRUE;
    } else if (fn & RT_FN_JACK) {
        SetPort(&d->Rec, 4);
    }
    d->Kind = d->Play.Num && d->Rec.Num ? SDW_KIND_HEADSET : d->Play.Num ? SDW_KIND_AMP : SDW_KIND_MIC;
    return d->Play.Num || d->Rec.Num;
}

/* rt712_sdca_clk_patch / _clk_patch2: PLL2 restart for a 24.576 MHz bus clock base */
static void ClkPatch(SDW_DEV *d)
{
    IdxWr(d, NID_VENDOR_REG, 0x65, 0x0000);
    SdwWrite(d, SDW_ROOT_CLK, 0x03);
    SdwDelay(1);
    SdwWrite(d, SDW_ROOT_CLK, 0x02);
    SdwDelay(1);
    SdwUpdate(d, PLL2_CONF2, 0x80, 0x00);
    SdwUpdate(d, PLL2_CONF2, 0x1f, 0x17);
    SdwUpdate(d, PLL2_CONF3, 0x10, 0x00);
    SdwUpdate(d, PLL2_CONF1, 0x81, 0x01);
    SdwWrite(d, SDW_ROOT_CLK, 0x03);
    SdwDelay(1);
    SdwUpdate(d, PLL2_CONF1, 0x81, 0x81);
    SdwUpdate(d, PLL2_CONF2, 0x80, 0x80);
    SdwUpdate(d, PLL2_CONF2, 0x1f, 0x00);
    SdwUpdate(d, PLL2_CONF3, 0x10, 0x10);
    SdwDelay(1);
    IdxWr(d, NID_VENDOR_REG, 0x65, 0x0081);
    /* patch 2 */
    SdwRtIdxUpd(d, NID_VENDOR_REG, 0x49, 0x0800, 0x0000);
    SdwRtIdxUpd(d, NID_VENDOR_REG, 0x49, 0xf000, 0x0000);
    IdxWr(d, NID_VENDOR_REG, 0x65, 0x0000);
    SdwRtIdxUpd(d, NID_ANALOG_CTL, 0x0c, 0xc000, 0xc000);
    SdwRtIdxUpd(d, NID_ANALOG_CTL, 0x00, 0xc000, 0xc000);
    IdxWr(d, NID_VENDOR_REG, 0x65, 0x0081);
    SdwWrite(d, SDW_ROOT_CLK, 0x02);
    SdwDelay(1);
    SdwWrite(d, SDW_ROOT_CLK, 0x03);
    SdwDelay(1);
    IdxWr(d, NID_VENDOR_REG, 0x65, 0x0000);
}

/* W1C trigger of the HP + class D DC calibration, poll up to 1 s */
static BOOLEAN DcCal(SDW_DEV *d)
{
    ULONG v = DAC_DC_CALI_TRIGGER, i;
    IdxWr(d, NID_VENDOR_REG, FSM_CTL, 0x4100);         /* FSM: calibration manual mode */
    IdxWr(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, 0x7883);
    IdxWr(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, 0xf893);
    SdwRtIdxRd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, &v);
    for (i = 0; i < 100 && (v & DAC_DC_CALI_TRIGGER); i++) {
        SdwDelay(10);
        if (!SdwRtIdxRd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, &v)) return FALSE;
    }
    return !(v & DAC_DC_CALI_TRIGGER);
}

/* rt712_sdca_calibration */
static BOOLEAN Calibrate(SDW_DEV *d)
{
    RT712_PRIV *x = P(d);
    UCHAR base = 0;
    BOOLEAN ok;
    SdwRead(d, SDW_ROOT_CLK, &base);
    if (x->Ver == VER_VA) IdxWr(d, NID_VENDOR_REG, CC_DET1, 0x043a);   /* HP-JD source JD1 */
    ok = DcCal(d);
    if (!ok) {
        if (base == CLK_FREQ_24_576MHZ) ClkPatch(d);
        ok = DcCal(d);
        if (!ok) LOGE("sdw: %s: DC calibration timed out", d->Name);
    }
    IdxWr(d, NID_VENDOR_REG, FSM_CTL, 0x4500);         /* Rldet in the FSM */
    IdxWr(d, NID_IMS_DRE, IMS_DIGITAL_CTL1, 0x040f);   /* sensing L + R */
    IdxWr(d, NID_IMS_DRE, IMS_DIGITAL_CTL5, 0x0000);   /* sine generator path */
    IdxWr(d, NID_VENDOR_REG, DIGITAL_MISC_CTRL4, 0x0010);  /* release HP-JD, CBJ tie open */
    return ok;
}

static void ClearNeedsInit(SDW_DEV *d, ULONG fn)
{
    SdwWrite(d, SDW_SDCA_CTL(fn, ENT0, CTL_FUNC_STATUS, 0), FUNCTION_NEEDS_INITIALIZATION);
}

/* rt712_sdca_va_io_init */
static void VaInit(SDW_DEV *d)
{
    RT712_PRIV *x = P(d);
    ULONG hib = 0;
    IdxWr(d, NID_VENDOR_REG, ANALOG_BIAS_CTL3, 0xaa81);
    IdxWr(d, NID_VENDOR_REG, LDO2_3_CTL1, 0xa1e0);
    IdxWr(d, NID_IMS_DRE, HP_DETECT_RLDET_CTL1, 0x0000);
    IdxWr(d, NID_IMS_DRE, HP_DETECT_RLDET_CTL2, 0x0000);
    IdxWr(d, NID_ANALOG_CTL, MISC_POWER_CTL7, 0x0000);
    SdwWrite(d, RC_CAL, 0x23);
    SdwRtIdxRd(d, NID_VENDOR_REG, SW_CONFIG1, &hib);   /* hibernation flag: calibrated before */
    if (!hib) Calibrate(d);
    SdwRtIdxUpd(d, NID_HDA_CTL, MIXER_CTL1, 0x3000, 0x0000);
    IdxWr(d, NID_HDA_CTL, ADC0A_08_PDE_FLOAT_CTL, 0x1112);
    IdxWr(d, NID_HDA_CTL, MIC2_LINE2_PDE_FLOAT_CTL, 0x3412);
    IdxWr(d, NID_HDA_CTL, DAC03_HP_PDE_FLOAT_CTL, 0x4040);
    SdwRtIdxUpd(d, NID_HDA_CTL, HDA_LEGACY_GPIO_WAKE_EN_CTL, 0x0001, 0x0000);
    SdwWrite(d, 0x2f50, 0x00);
    SdwWrite(d, 0x2f54, 0x00);
    SdwWrite(d, SDW_SDCA_CTL(FN_JACK, ENT_IT09, CTL_VENDOR_DEF, 0), 0x01);
    if (x->HwId != HW_ID_713) {                          /* speaker settings */
        IdxWr(d, NID_HDA_CTL, AMP_PDE_FLOAT_CTL, 0x2323);
        IdxWr(d, NID_HDA_CTL, EAPD_CTL, 0x0002);
        SdwWrite(d, SDW_SDCA_CTL(FN_AMP, ENT_OT23, CTL_VENDOR_DEF, 0), 0x04);
    }
}

/* rt712_sdca_vb_io_init (first init: every function initialised) */
static void VbInit(SDW_DEV *d)
{
    RT712_PRIV *x = P(d);
    UCHAR base = 0;
    SdwRead(d, SDW_ROOT_CLK, &base);
    IdxWr(d, NID_VENDOR_REG, JD_CTL3, 0x7778);
    if (base == CLK_FREQ_24_576MHZ) ClkPatch(d);
    /* DMIC */
    IdxWr(d, NID_HDA_CTL, DMIC2_FU_IT_FLOAT_CTL, 0x1526);
    IdxWr(d, NID_HDA_CTL, DMIC2_FU_CH12_FLOAT_CTL, 0x0304);
    IdxWr(d, NID_HDA_CTL, ADC0A_CS_ADC0B_FU_FLOAT_CTL, 0x1f1e);
    IdxWr(d, NID_HDA_CTL, ADC0B_FU_CH12_FLOAT_CTL, 0x0304);
    IdxWr(d, NID_HDA_CTL, HDA_LEGACY_CONFIG_CTL0, 0x8010);
    SdwWrite(d, SDW_SDCA_CTL(FN_MIC, ENT_IT26, CTL_VENDOR_DEF, 0), 0x01);
    IdxWr(d, NID_ULTRA_SOUND, ULTRA_SOUND_DETECTOR6, 0x3200);
    SdwWrite(d, RC_CAL, 0x23);
    ClearNeedsInit(d, FN_MIC);
    /* jack */
    IdxWr(d, NID_IMS_DRE, SEL_VEE2_HP_CTL1, 0x042a);
    IdxWr(d, NID_CHARGE_PUMP, HP_DET_CTL3, 0x1fff);
    IdxWr(d, NID_VENDOR_REG, IO_CTL, 0xec67);
    IdxWr(d, NID_VENDOR_REG, ANALOG_BIAS_CTL3, 0xaa81);
    IdxWr(d, NID_VENDOR_REG, LDO2_3_CTL1, 0xa1e0);
    IdxWr(d, NID_IMS_DRE, HP_DETECT_RLDET_CTL1, 0x0000);
    IdxWr(d, NID_IMS_DRE, HP_DETECT_RLDET_CTL2, 0x0000);
    SdwWrite(d, RC_CAL, 0x23);
    IdxWr(d, NID_VENDOR_REG, JD_CTL1, 0x2802);
    IdxWr(d, NID_VENDOR_REG, CLASSD_AMP_CTL6, 0xf215);
    Calibrate(d);
    SdwRtIdxUpd(d, NID_HDA_CTL, MIXER_CTL1, 0x3000, 0x0000);
    SdwWrite(d, SDW_SDCA_CTL(FN_JACK, ENT_IT09, CTL_VENDOR_DEF, 0), 0x01);
    IdxWr(d, NID_HDA_CTL, MISC_CTL_FOR_UAJ, 0x0003);
    ClearNeedsInit(d, FN_JACK);
    /* speaker */
    if (x->HwId != HW_ID_713) {
        IdxWr(d, NID_VENDOR_REG, IO_CTL, 0xec63);
        IdxWr(d, NID_VENDOR_REG, CLASSD_AMP_CTL1, 0xfff5);
        IdxWr(d, NID_HDA_CTL, EAPD_CTL, 0x0002);
        SdwWrite(d, SDW_SDCA_CTL(FN_AMP, ENT_OT23, CTL_VENDOR_DEF, 0), 0x04);
    }
    ClearNeedsInit(d, FN_AMP);
}

/* rt712_sdca_dmic_io_init (the separate DMIC peripheral) */
static const ULONG g_DmicIdx[] = {
    ADC0A_08_PDE_FLOAT_CTL, 0x1112, ADC0B_11_PDE_FLOAT_CTL, 0x1111, DMIC1_2_PDE_FLOAT_CTL, 0x1111,
    I2S_IN_OUT_PDE_FLOAT_CTL, 0x1155, DMIC_ENT_FLOAT_CTL, 0x2626, ADC_ENT_FLOAT_CTL, 0x1e19,
    DMIC_GAIN_ENT_FLOAT_CTL0, 0x1515, ADC_VOL_CH_FLOAT_CTL2, 0x0304, DMIC_GAIN_ENT_FLOAT_CTL2, 0x0304,
    HDA_LEGACY_CONFIG_CTL0, 0x0050,
};

static BOOLEAN DmicInit(SDW_DEV *d)
{
    ULONG e0 = d->Errors, i;
    for (i = 0; i < ARRAYSIZE(g_DmicIdx); i += 2) IdxWr(d, NID_HDA_CTL, g_DmicIdx[i], g_DmicIdx[i + 1]);
    SdwWrite(d, SDW_SDCA_CTL(FN_MIC, ENT_IT26, CTL_VENDOR_DEF, 0), 0x01);
    IdxWr(d, NID_ULTRA_SOUND, ULTRA_SOUND_DETECTOR6, 0x3200);
    SdwWrite(d, RC_CAL, 0x23);
    SdwWrite(d, 0x2f52, 0x00);
    /* hw_params; ADC 25 / 26 muxes keep their DMIC1 / DMIC2 defaults */
    SdcaRate(d, FN_MIC, ENT_CS1F, RATE_48000);
    SdcaRate(d, FN_MIC, ENT_CS1C, RATE_48000);
    SdcaVolume(d, FN_MIC, ENT_FU15, SDCA_CTL_FU_GAIN, 0xF, 20 * 256);
    SdcaVolume(d, FN_MIC, ENT_FU1E, SDCA_CTL_FU_VOLUME, 0xF, 0);
    SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, TRUE);
    SdcaPower(d, FN_MIC, ENT_PDE11, SDCA_PS3, 0);
    LOGI("sdw: %s: RT712 DMIC, %u bus errors in init", d->Name, d->Errors - e0);
    return d->Errors - e0 < 8;
}

static BOOLEAN Init(SDW_DEV *d)
{
    RT712_PRIV *x = P(d);
    ULONG v = 0, e0 = d->Errors;
    if (x->DmicPart) return DmicInit(d);
    /* rt712_sdca_reset */
    SdwRtIdxUpd(d, NID_VENDOR_REG, PARA_VERB_CTL, HIDDEN_REG_SW_RESET, HIDDEN_REG_SW_RESET);
    SdwRtIdxUpd(d, NID_HDA_CTL, HDA_LEGACY_RESET_CTL, 0x1, 0x1);
    if (!SdwRtIdxRd(d, NID_VENDOR_REG, JD_PRODUCT_NUM, &v)) return FALSE;
    x->HwId = (UCHAR)((v >> 12) & 0xf);
    x->Ver = (UCHAR)((v >> 8) & 0xf);
    if (x->HwId == HW_ID_713 && x->Amp) {
        LOGI("sdw: %s: hw id RT713, no amplifier", d->Name);
        x->Amp = 0;
    }
    if (x->Ver == VER_VA) {
        if (x->RecDmic) {                               /* no DMIC function on the VA: headset mic port */
            LOGE("sdw: %s: RT712 VA but a SMART_MIC function in ACPI, capture from the headset mic", d->Name);
            x->RecDmic = FALSE;
            x->Fn = (UCHAR)((x->Fn & ~RT_FN_MIC) | RT_FN_JACK);   /* the VA jack function is always there */
            d->Rec.Num = 4;                             /* the stream exists: same channels, other port */
        }
        VaInit(d);
    } else {
        VbInit(d);
    }
    SdwRtIdxUpd(d, NID_HDA_CTL, GE_RELATED_CTL1, 0x0c00, 0x0000);  /* no jack: HID1 / detected-mode events off */
    IdxWr(d, NID_VENDOR_REG, SW_CONFIG1, 0x0001);
    /* hw_params: 48 kHz; volumes; everything muted and in PS3 */
    SdcaRate(d, FN_JACK, ENT_CS01, RATE_48000);
    SdcaRate(d, FN_JACK, ENT_CS11, RATE_48000);
    SdcaVolume(d, FN_JACK, ENT_FU05, SDCA_CTL_FU_VOLUME, 3, 0);
    SdcaVolume(d, FN_JACK, ENT_FU0F, SDCA_CTL_FU_VOLUME, 3, 0);
    SdcaVolume(d, FN_JACK, ENT_FU44, SDCA_CTL_FU_GAIN, 3, 20 * 256);
    SdcaMute(d, FN_JACK, ENT_FU05, 3, TRUE);
    SdcaMute(d, FN_JACK, ENT_FU0F, 3, TRUE);
    SdcaPower(d, FN_JACK, ENT_PDE40, SDCA_PS3, 0);
    SdcaPower(d, FN_JACK, ENT_PDE12, SDCA_PS3, 0);
    if (x->HwId != HW_ID_713) {
        SdcaRate(d, FN_AMP, ENT_CS31, RATE_48000);
        SdcaVolume(d, FN_AMP, ENT_FU06, SDCA_CTL_FU_VOLUME, 3, 0);
        SdcaMute(d, FN_AMP, ENT_FU06, 3, TRUE);
        SdcaPower(d, FN_AMP, ENT_PDE23, SDCA_PS3, 0);
    }
    if (x->Ver != VER_VA) {                             /* ADC 0A / 0B muxes keep DMIC1 / DMIC2 (0x04e4) */
        SdcaRate(d, FN_MIC, ENT_CS1F, RATE_48000);
        SdcaRate(d, FN_MIC, ENT_CS1C, RATE_48000);
        SdcaVolume(d, FN_MIC, ENT_FU15, SDCA_CTL_FU_GAIN, 0xF, 20 * 256);
        SdcaVolume(d, FN_MIC, ENT_FU1E, SDCA_CTL_FU_VOLUME, 0xF, 0);
        SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, TRUE);
        SdcaPower(d, FN_MIC, ENT_PDE11, SDCA_PS3, 0);
    }
    LOGI("sdw: %s: RT71%c-SDCA V%c%s%s, playback DP%u%s, capture DP%u (%s), %u bus errors in init", d->Name,
         x->HwId == HW_ID_713 ? '3' : x->HwId == 5 ? '6' : x->HwId == 4 ? '7' : '2', 'A' + x->Ver,
         (x->Fn & RT_FN_AMP) ? ", amp" : "", (x->Fn & RT_FN_MIC) ? ", dmic" : "", d->Play.Num,
         x->Amp ? " + DP3 speakers" : "", d->Rec.Num, x->RecDmic ? "digital mics" : "headset mic", d->Errors - e0);
    return d->Errors - e0 < 16;
}

static void Speaker(SDW_DEV *d, BOOLEAN on)
{
    if (on) {
        SdcaPower(d, FN_AMP, ENT_PDE23, SDCA_PS0, 0);
        SdcaMute(d, FN_AMP, ENT_FU06, 3, FALSE);        /* "OT23 L / R" switches */
    } else {
        SdcaMute(d, FN_AMP, ENT_FU06, 3, TRUE);
        SdcaPower(d, FN_AMP, ENT_PDE23, SDCA_PS3, 0);
    }
}

/* the DAPM order: supplies (PDE) up, then the feature units unmuted; off in reverse */
static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    RT712_PRIV *x = P(d);
    if (x->DmicPart || (rec && x->RecDmic)) {
        if (!rec) return;
        if (on) {
            SdcaPower(d, FN_MIC, ENT_PDE11, SDCA_PS0, 0);
            SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, FALSE);
        } else {
            SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, TRUE);
            SdcaPower(d, FN_MIC, ENT_PDE11, SDCA_PS3, 0);
        }
        return;
    }
    if (!rec && !(x->Fn & RT_FN_JACK)) {                /* amplifier only: DP3 */
        Speaker(d, on);
        return;
    }
    if (on) {
        if (!rec && x->Amp) {
            SdwRtMirror(d, &d->Play, x->Amp);
            Speaker(d, TRUE);
        }
        SdcaPower(d, FN_JACK, rec ? ENT_PDE12 : ENT_PDE40, SDCA_PS0, 0);
        SdwRtJackMode(d, FN_JACK, ENT_GE49);
        SdcaMute(d, FN_JACK, rec ? ENT_FU0F : ENT_FU05, 3, FALSE);
    } else {
        SdcaMute(d, FN_JACK, rec ? ENT_FU0F : ENT_FU05, 3, TRUE);
        SdcaPower(d, FN_JACK, rec ? ENT_PDE12 : ENT_PDE40, SDCA_PS3, 0);
        if (!rec && x->Amp) Speaker(d, FALSE);
    }
}

static void Shutdown(SDW_DEV *d)
{
    RT712_PRIV *x = P(d);
    if (!d->Ready) return;
    if (x->DmicPart || x->Ver != VER_VA) {
        SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, TRUE);
        SdcaPower(d, FN_MIC, ENT_PDE11, SDCA_PS3, 0);
    }
    if (x->DmicPart) return;
    SdcaMute(d, FN_JACK, ENT_FU05, 3, TRUE);
    SdcaMute(d, FN_JACK, ENT_FU0F, 3, TRUE);
    SdcaPower(d, FN_JACK, ENT_PDE40, SDCA_PS3, 0);
    SdcaPower(d, FN_JACK, ENT_PDE12, SDCA_PS3, 0);
    if (x->HwId != HW_ID_713) {
        SdcaMute(d, FN_AMP, ENT_FU06, 3, TRUE);
        SdcaPower(d, FN_AMP, ENT_PDE23, SDCA_PS3, 0);
    }
    SdwRtMirrorOff(d, x->Amp);
}

static const USHORT g_Parts[] = { 0x0712, 0x0713, 0x0716, 0x0717, 0x1712, 0x1713, 0x1716, 0x1717, 0 };
const SDW_DRV g_SdwRt712 = { "RT712", 0x025D, g_Parts, SDW_KIND_HEADSET, Probe, Init, Stream, Shutdown };
