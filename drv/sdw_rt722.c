/*
 * AUDIONT - Realtek RT722-SDCA SoundWire codec: jack (headphone + headset microphone),
 * speaker amplifier and digital microphones in one chip.  Linux sound/soc/codecs/rt722-sdca.c,
 * rt722-sdca-sdw.c, machine side sdw_utils/soc_sdw_utils.c.
 *
 *   function 1 jack  DP1 -> FU05 ("FU 42") -> HP (PDE 40 / "PDE 47");
 *                    MIC2 -> ADC 22 mux -> FU0F ("FU 36", PDE 12, boost FU44) -> DP2
 *   function 4 amp   DP3 -> FU06 ("FU 21") -> class D (PDE 23)
 *   function 2 mic   DMIC1_2 / DMIC3_4 -> ADC 24 / 25 mux -> FU1E ("FU 113", PDE 2A, boost FU15) -> DP6
 *
 * One playback and one capture port per peripheral in the bus core: playback is the headphone
 * port DP1 and the speaker port DP3 is programmed as a copy of it in Stream (both outputs play,
 * no jack detection); capture is the headset microphone DP2, or the DMICs (DP6) with the
 * registry value SdwDmic = 1 or when the BIOS describes no jack function.  Functions missing
 * from the ACPI DisCo description are not used (Linux is_sdca_endpoint_present).
 *
 * Init follows rt722_sdca_io_init for a first init: JD_PRODUCT_NUM (VA / VB), reset,
 * rt722_sdca_dmic_preset, _amp_preset (class D DC calibration trigger), _jack_preset (HP DC
 * calibration, analog bias, PDE floating controls), the jack detection not enabled (no
 * rt722_sdca_jack_init); hw_params at 48 kHz on CS01 / CS11 / CS31 / CS1F.  The power domain
 * changes wait for ACTUAL_PS as rt722_pde_transition_delay.  The muxes keep their reset values
 * (ADC 22 MIC2, ADC 24 DMIC1_2, ADC 25 DMIC3_4).  The CAE (EQ firmware) is not loaded.
 * Volumes: DAC FU05 / FU06 0 dB, ADC FU0F / FU1E 0 dB, boosts FU44 / FU15 +20 dB.
 */
#include "sdw_rtcom.h"

#define FN_JACK         0x01
#define FN_MIC          0x02
#define FN_AMP          0x04
/* NIDs */
#define NID_VENDOR_REG  0x20
#define NID_VENDOR_CALI 0x58
#define NID_HDA_CTL     0x61
/* index registers, NID 20h */
#define JD_PRODUCT_NUM          0x00
#define ANALOG_BIAS_CTL3        0x04
#define JD_CTRL1                0x09
#define LDO1_CTL                0x1a
#define CLSD_CTRL6              0x3c
#define COMBO_JACK_AUTO_CTL1    0x45
#define DIGITAL_MISC_CTRL4      0x4a
#define VREFO_GAT               0x63
#define FSM_CTL                 0x67
/* NID 58h */
#define DAC_DC_CALI_CTL3        0x03
#define DC_CALIB_CTRL           0x10000
/* NID 61h */
#define HDA_LEGACY_CONFIG_CTL0  0x06
#define HDA_LEGACY_RESET_CTL    0x08
#define DMIC_ENT_FLOAT_CTL      0x10
#define DMIC_GAIN_ENT_FLOAT_CTL0 0x11
#define DMIC_GAIN_ENT_FLOAT_CTL2 0x13
#define ADC_ENT_FLOAT_CTL       0x15
#define ADC_VOL_CH_FLOAT_CTL    0x17
#define DAC03_HP_PDE_FLOAT_CTL  0x22
#define MIC2_LINE2_PDE_FLOAT_CTL 0x23
#define ET41_LINE2_PDE_FLOAT_CTL 0x24
#define ADC0A_08_PDE_FLOAT_CTL  0x25
#define ADC10_PDE_FLOAT_CTL     0x26
#define DMIC1_2_PDE_FLOAT_CTL   0x28
#define AMP_PDE_FLOAT_CTL       0x29
#define GE_RELATED_CTL2         0x46
#define EAPD_CTL                0x55
#define UMP_HID_CTL0            0x60
#define UMP_HID_CTL4            0x64
#define UMP_HID_CTL5            0x65
#define UMP_HID_CTL7            0x67
#define FLOAT_CTRL_1            0x70
#define ENT_FLOAT_CTRL_1        0x76
#define HIDDEN_REG_SW_RESET     (1U << 14)
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
#define ENT_PDE12       0x12
#define ENT_PDE2A       0x2a
#define ENT_CS01        0x01
#define ENT_CS11        0x11
#define ENT_CS1F        0x1f
#define ENT_CS31        0x31
#define ENT_OT23        0x42
#define ENT_IT26        0x26
#define CTL_VENDOR_DEF  0x30
#define CTL_FUNC_STATUS 0x10
#define FUNCTION_NEEDS_INITIALIZATION 0x20
#define RATE_48000      0x09
#define VER_VA          0
#define VER_VB          1
#define PDE_WAIT_MS     1000        /* rt722_pde_transition_delay: 1000 x 1 ms */

typedef struct _RT722_PRIV {
    UCHAR   Fn;                     /* RT_FN_xxx in use */
    UCHAR   Amp;                    /* speaker port mirrored from the headphone port (3), 0: none */
    UCHAR   RecDmic;
    UCHAR   Ver;
} RT722_PRIV;
typedef char Rt722PrivFits[sizeof(RT722_PRIV) <= sizeof(((SDW_DEV *)0)->Priv) ? 1 : -1];

static RT722_PRIV *P(SDW_DEV *d) { return (RT722_PRIV *)d->Priv; }

static BOOLEAN IdxWr(SDW_DEV *d, ULONG nid, ULONG reg, ULONG v) { return SdwRtIdxWr(d, nid, reg, v); }

static void SetPort(SDW_DPN *p, ULONG num)
{
    p->Num = (UCHAR)num; p->Ch = 2; p->Type = SDW_DP_FULL; p->CpSm = FALSE;     /* simple_ch_prep_sm */
}

static BOOLEAN Probe(SDW_DEV *d)
{
    RT722_PRIV *x = P(d);
    ULONG fn;
    if (d->Class != 1) return FALSE;
    memset(x, 0, sizeof(*x));
    memset(&d->Play, 0, sizeof(d->Play));
    memset(&d->Rec, 0, sizeof(d->Rec));
    d->Paging = TRUE;
    fn = SdwRtFuncs(d);
    x->Fn = (UCHAR)fn;
    if (fn & RT_FN_JACK) {
        SetPort(&d->Play, 1);
        if (fn & RT_FN_AMP) x->Amp = 3;
    } else if (fn & RT_FN_AMP) {
        SetPort(&d->Play, 3);
    }
    if ((fn & RT_FN_MIC) && (!(fn & RT_FN_JACK) || SdwRtPreferDmic())) {
        SetPort(&d->Rec, 6);
        x->RecDmic = TRUE;
    } else if (fn & RT_FN_JACK) {
        SetPort(&d->Rec, 2);
    }
    d->Kind = d->Play.Num && d->Rec.Num ? SDW_KIND_HEADSET : d->Play.Num ? SDW_KIND_AMP : SDW_KIND_MIC;
    return d->Play.Num || d->Rec.Num;
}

static void ClearNeedsInit(SDW_DEV *d, ULONG fn)
{
    SdwWrite(d, SDW_SDCA_CTL(fn, ENT0, CTL_FUNC_STATUS, 0), FUNCTION_NEEDS_INITIALIZATION);
}

/* rt722_sdca_dmic_preset */
static void DmicPreset(SDW_DEV *d)
{
    IdxWr(d, NID_HDA_CTL, ADC0A_08_PDE_FLOAT_CTL, 0x2a29);     /* AD07 power entity floating control */
    IdxWr(d, NID_HDA_CTL, ADC10_PDE_FLOAT_CTL, 0x2a00);
    IdxWr(d, NID_HDA_CTL, DMIC1_2_PDE_FLOAT_CTL, 0x2a2a);
    IdxWr(d, NID_HDA_CTL, DMIC_ENT_FLOAT_CTL, 0x2626);         /* DMIC2 IT entity */
    IdxWr(d, NID_HDA_CTL, ADC_ENT_FLOAT_CTL, 0x1e00);          /* AD10 FU entity */
    IdxWr(d, NID_HDA_CTL, DMIC_GAIN_ENT_FLOAT_CTL0, 0x1515);    /* DMIC2 FU entity */
    IdxWr(d, NID_HDA_CTL, ADC_VOL_CH_FLOAT_CTL, 0x0304);
    IdxWr(d, NID_HDA_CTL, DMIC_GAIN_ENT_FLOAT_CTL2, 0x0304);
    IdxWr(d, NID_HDA_CTL, HDA_LEGACY_CONFIG_CTL0, 0x0000);
    SdwWrite(d, SDW_SDCA_CTL(FN_MIC, ENT_IT26, CTL_VENDOR_DEF, 0), 0x01);
    SdwWrite(d, 0x2f5c, 0x25);                                  /* PDE2A latency */
    SdwWrite(d, 0x2f03, 0x06);                                  /* PHY timing TDZ / TZD */
    if (P(d)->Ver == VER_VB) SdwWrite(d, 0x2f52, 0x00);
    ClearNeedsInit(d, FN_MIC);
}

/* rt722_sdca_amp_preset */
static void AmpPreset(SDW_DEV *d)
{
    IdxWr(d, NID_VENDOR_REG, CLSD_CTRL6, 0xc215);              /* DVQ = 01 */
    IdxWr(d, NID_VENDOR_CALI, DC_CALIB_CTRL, 0x702c);          /* reset dc_cal_top */
    IdxWr(d, NID_VENDOR_CALI, DC_CALIB_CTRL, 0xf02d);          /* W1C trigger */
    IdxWr(d, NID_HDA_CTL, AMP_PDE_FLOAT_CTL, 0x2323);
    IdxWr(d, NID_HDA_CTL, EAPD_CTL, 0x0002);                   /* EAPD high */
    SdwWrite(d, SDW_SDCA_CTL(FN_AMP, ENT_OT23, CTL_VENDOR_DEF, 8), 0x04);
    if (P(d)->Ver == VER_VB) SdwWrite(d, 0x2f54, 0x00);
    ClearNeedsInit(d, FN_AMP);
}

/* rt722_sdca_jack_preset */
static void JackPreset(SDW_DEV *d)
{
    ULONG st = 0x40, i;
    IdxWr(d, NID_VENDOR_REG, ANALOG_BIAS_CTL3, 0xa081);
    IdxWr(d, NID_HDA_CTL, GE_RELATED_CTL2, 0xa009);
    IdxWr(d, NID_HDA_CTL, UMP_HID_CTL4, 0xcf00);                /* buttons A..D bypass mode */
    IdxWr(d, NID_HDA_CTL, UMP_HID_CTL5, 0x000f);                /* HID1 slot enable */
    IdxWr(d, NID_HDA_CTL, UMP_HID_CTL0, 0x1100);                /* report id HID1 */
    IdxWr(d, NID_HDA_CTL, UMP_HID_CTL7, 0x0c12);
    IdxWr(d, NID_VENDOR_REG, JD_CTRL1, 0x7002);                 /* JD de-bounce clock */
    IdxWr(d, NID_VENDOR_REG, CLSD_CTRL6, 0xc215);
    IdxWr(d, NID_VENDOR_REG, FSM_CTL, 0x4100);                  /* FSM: calibration manual mode */
    IdxWr(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL3, 0x008d);        /* W1C trigger HP DC calibration */
    for (i = 0; i < 100; i++) {
        SdwDelay(10);
        if (SdwRtIdxRd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL3, &st) && !(st & 0x0040)) break;
    }
    if (i == 100) LOGI("sdw: %s: HP calibration timed out", d->Name);
    IdxWr(d, NID_HDA_CTL, ADC0A_08_PDE_FLOAT_CTL, 0x2a12);
    IdxWr(d, NID_HDA_CTL, MIC2_LINE2_PDE_FLOAT_CTL, 0x3429);
    IdxWr(d, NID_HDA_CTL, ET41_LINE2_PDE_FLOAT_CTL, 0x4112);
    IdxWr(d, NID_HDA_CTL, DAC03_HP_PDE_FLOAT_CTL, 0x4040);
    IdxWr(d, NID_HDA_CTL, ENT_FLOAT_CTRL_1, 0x4141);
    IdxWr(d, NID_HDA_CTL, FLOAT_CTRL_1, 0x0101);
    SdwWrite(d, 0x2f58, 0x07);                                  /* PDE40 latency */
    SdwWrite(d, 0x2f03, 0x06);
    SdwRtIdxUpd(d, NID_VENDOR_REG, COMBO_JACK_AUTO_CTL1, 0x0200, 0x0200);   /* MIC VRefo */
    SdwRtIdxUpd(d, NID_VENDOR_REG, VREFO_GAT, 0x4000, 0x4000);
    IdxWr(d, NID_VENDOR_REG, DIGITAL_MISC_CTRL4, 0x0010);       /* release HP-JD, CBJ tie open */
    if (P(d)->Ver == VER_VB) SdwWrite(d, 0x2f51, 0x00);
    ClearNeedsInit(d, FN_JACK);
}

static BOOLEAN Init(SDW_DEV *d)
{
    RT722_PRIV *x = P(d);
    ULONG v = 0, e0 = d->Errors;
    if (!SdwRtIdxRd(d, NID_VENDOR_REG, JD_PRODUCT_NUM, &v)) return FALSE;
    x->Ver = (UCHAR)((v >> 8) & 0xf);
    /* rt722_sdca_reset */
    SdwRtIdxUpd(d, NID_VENDOR_REG, LDO1_CTL, HIDDEN_REG_SW_RESET, HIDDEN_REG_SW_RESET);
    SdwRtIdxUpd(d, NID_HDA_CTL, HDA_LEGACY_RESET_CTL, 0x1, 0x1);
    if (x->Ver == VER_VA) IdxWr(d, NID_VENDOR_REG, LDO1_CTL, 0xb091);
    DmicPreset(d);
    AmpPreset(d);
    JackPreset(d);
    /* hw_params: 48 kHz; volumes; everything muted and in PS3 */
    SdcaRate(d, FN_JACK, ENT_CS01, RATE_48000);
    SdcaRate(d, FN_JACK, ENT_CS11, RATE_48000);
    SdcaRate(d, FN_AMP, ENT_CS31, RATE_48000);
    SdcaRate(d, FN_MIC, ENT_CS1F, RATE_48000);
    SdcaVolume(d, FN_JACK, ENT_FU05, SDCA_CTL_FU_VOLUME, 3, 0);
    SdcaVolume(d, FN_JACK, ENT_FU0F, SDCA_CTL_FU_VOLUME, 3, 0);
    SdcaVolume(d, FN_JACK, ENT_FU44, SDCA_CTL_FU_GAIN, 3, 20 * 256);
    SdcaVolume(d, FN_AMP, ENT_FU06, SDCA_CTL_FU_VOLUME, 3, 0);
    SdcaVolume(d, FN_MIC, ENT_FU1E, SDCA_CTL_FU_VOLUME, 0xF, 0);
    SdcaVolume(d, FN_MIC, ENT_FU15, SDCA_CTL_FU_GAIN, 0xF, 20 * 256);
    SdcaMute(d, FN_JACK, ENT_FU05, 3, TRUE);
    SdcaMute(d, FN_JACK, ENT_FU0F, 3, TRUE);
    SdcaMute(d, FN_AMP, ENT_FU06, 3, TRUE);
    SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, TRUE);
    SdcaPower(d, FN_JACK, ENT_PDE40, SDCA_PS3, 0);
    SdcaPower(d, FN_JACK, ENT_PDE12, SDCA_PS3, 0);
    SdcaPower(d, FN_AMP, ENT_PDE23, SDCA_PS3, 0);
    SdcaPower(d, FN_MIC, ENT_PDE2A, SDCA_PS3, 0);
    LOGI("sdw: %s: RT722-SDCA V%c, playback DP%u%s, capture DP%u (%s), %u bus errors in init", d->Name,
         'A' + x->Ver, d->Play.Num, x->Amp ? " + DP3 speakers" : "", d->Rec.Num,
         x->RecDmic ? "digital mics" : "headset mic", d->Errors - e0);
    return d->Errors - e0 < 16;
}

static void Pde(SDW_DEV *d, ULONG fn, ULONG pde, ULONG ps)
{
    SdcaPower(d, fn, pde, ps, PDE_WAIT_MS);
}

static void Speaker(SDW_DEV *d, BOOLEAN on)
{
    if (on) {
        Pde(d, FN_AMP, ENT_PDE23, SDCA_PS0);
        SdcaMute(d, FN_AMP, ENT_FU06, 3, FALSE);        /* "FU 21" POST_PMU */
    } else {
        SdcaMute(d, FN_AMP, ENT_FU06, 3, TRUE);
        Pde(d, FN_AMP, ENT_PDE23, SDCA_PS3);
    }
}

/* the DAPM order: supplies (PDE) up, then the feature units unmuted; off in reverse */
static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    RT722_PRIV *x = P(d);
    if (rec && x->RecDmic) {
        if (on) {
            Pde(d, FN_MIC, ENT_PDE2A, SDCA_PS0);        /* "PDE 11" */
            SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, FALSE);  /* "FU 113" POST_PMU, then 150 ms */
            SdwDelay(150);
        } else {
            SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, TRUE);
            Pde(d, FN_MIC, ENT_PDE2A, SDCA_PS3);
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
        Pde(d, FN_JACK, rec ? ENT_PDE12 : ENT_PDE40, SDCA_PS0);
        SdwRtJackMode(d, FN_JACK, ENT_GE49);
        SdcaMute(d, FN_JACK, rec ? ENT_FU0F : ENT_FU05, 3, FALSE);
    } else {
        SdcaMute(d, FN_JACK, rec ? ENT_FU0F : ENT_FU05, 3, TRUE);
        Pde(d, FN_JACK, rec ? ENT_PDE12 : ENT_PDE40, SDCA_PS3);
        if (!rec && x->Amp) Speaker(d, FALSE);
    }
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    SdcaMute(d, FN_JACK, ENT_FU05, 3, TRUE);
    SdcaMute(d, FN_JACK, ENT_FU0F, 3, TRUE);
    SdcaMute(d, FN_AMP, ENT_FU06, 3, TRUE);
    SdcaMute(d, FN_MIC, ENT_FU1E, 0xF, TRUE);
    SdcaPower(d, FN_JACK, ENT_PDE40, SDCA_PS3, 0);
    SdcaPower(d, FN_JACK, ENT_PDE12, SDCA_PS3, 0);
    SdcaPower(d, FN_AMP, ENT_PDE23, SDCA_PS3, 0);
    SdcaPower(d, FN_MIC, ENT_PDE2A, SDCA_PS3, 0);
    SdwRtMirrorOff(d, P(d)->Amp);
}

static const USHORT g_Parts[] = { 0x0722, 0 };
const SDW_DRV g_SdwRt722 = { "RT722", 0x025D, g_Parts, SDW_KIND_HEADSET, Probe, Init, Stream, Shutdown };
