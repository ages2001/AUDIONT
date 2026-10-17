/*
 * AUDIONT - Realtek RT711-SDCA SoundWire jack codec (headphone + headset
 * microphone), Linux sound/soc/codecs/rt711-sdca.c / rt711-sdca-sdw.c.
 *
 *   playback   DP3 -> FU05 -> HP (PDE 28), OT1
 *   capture    MIC2 -> ADC 22 mux -> FU0F (PDE 29, boost FU44) -> DP2
 *              (DP4 / FU1E: the microphone-array path, PDE 2A, not used)
 *   functions  1 jack codec, 2 microphone array, 3 HID
 *
 * Vendor registers ("index" registers of the HDA-like NIDs) are 16-bit MBQ
 * quantities at (nid << 20) | index.  Init follows rt711_sdca_io_init (reset,
 * VD0 / VD1 tables, RC and headphone DC calibration, HP output enable) and
 * hw_params (CS01 / CS11 / CS1F at 48 kHz); jack detection stays off (HID
 * events disabled), the Group Entity is set from the detected jack mode when
 * a stream starts.  Volumes: DAC FU05 0 dB, ADC FU0F 0 dB, mic boost FU44 +20 dB.
 */
#include "sdw.h"

#define FN_JACK          0x01
#define FN_MIC           0x02
/* NIDs */
#define NID_VENDOR_REG   0x20
#define NID_VENDOR_CALI  0x58
#define NID_VENDOR_VAD   0x5e
#define NID_VENDOR_ANALOG 0x5f
#define NID_VENDOR_HDA   0x61
/* index registers */
#define JD_PRODUCT_NUM          0x00
#define DMIC_CTL1               0x06
#define PARA_VERB_CTL           0x1a
#define COMBO_JACK_AUTO_CTL3    0x47
#define DIGITAL_MISC_CTRL4      0x4a
#define JD_CTRL6                0x6a
#define GPIO_TEST_MODE_CTL2     0x6d
#define FSM_CTL                 0x6f
#define FILTER_SRC_SEL          0xb0
#define ADC27_VOL_SET           0xb7
#define DAC_DC_CALI_CTL1        0x00
#define VAD_SRAM_CTL1           0x10
#define MISC_POWER_CTL0         0x01
#define MISC_POWER_CTL4         0x05
#define HDA_LEGACY_MUX_CTL1     0x00
#define HDA_LEGACY_UNSOL_CTL    0x03
#define HDA_LEGACY_CONFIG_CTL   0x06
#define HDA_LEGACY_RESET_CTL    0x08
#define HDA_LEGACY_GPIO_CTL     0x0a
#define GE_MODE_RELATED_CTL     0x35
#define PUSH_BTN_INT_CTL0       0x36
#define RC_CAL_STATUS           0x320c
#define HIDDEN_REG_SW_RESET     (1U << 14)
#define DAC_DC_CALI_TRIGGER     (1U << 15)
#define DAC_DC_CALI_CLK_EN      (1U << 14)
#define DAC_DC_FORCE_CALI_RST   (1U << 3)
#define FSM_IMP_EN              (1U << 6)
/* SDCA entities / controls */
#define ENT_GE49    0x49
#define ENT_FU05    0x05
#define ENT_FU0F    0x0f
#define ENT_FU44    0x44
#define ENT_PDE28   0x28
#define ENT_PDE29   0x29
#define ENT_PDE2A   0x2a
#define ENT_CS01    0x01
#define ENT_CS11    0x11
#define ENT_CS1F    0x1f
#define ENT_OT1     0x06
#define CTL_VENDOR_DEF 0x30
#define RATE_48000  0x09

#define HW_VER(d)   ((d)->Priv[0])

static ULONG Idx(ULONG nid, ULONG reg) { return (nid << 20) | reg; }

static BOOLEAN IdxWr(SDW_DEV *d, ULONG nid, ULONG reg, ULONG v) { return SdwWriteMbq(d, Idx(nid, reg), 2, v); }

static BOOLEAN IdxRd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG *v) { return SdwReadMbq(d, Idx(nid, reg), 2, v); }

static BOOLEAN IdxUpd(SDW_DEV *d, ULONG nid, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;
    if (!IdxRd(d, nid, reg, &v)) return FALSE;
    return IdxWr(d, nid, reg, (v & ~mask) | (val & mask));
}

/* part 0x711 class 1 (SoundWire 1.2): RT711-SDCA; class 0 is the older HDA-verb RT711 (rt711.c), not handled */
static BOOLEAN Probe(SDW_DEV *d)
{
    if (d->Class != 1) return FALSE;
    d->Kind = SDW_KIND_HEADSET;
    d->Play.Num = 3; d->Play.Ch = 2; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = FALSE;   /* simple_ch_prep_sm */
    d->Rec.Num = 2; d->Rec.Ch = 2; d->Rec.Type = SDW_DP_FULL; d->Rec.CpSm = FALSE;
    d->Paging = TRUE;
    return TRUE;
}

/* rt711_sdca_calibration: RC calibration, then the headphone DAC DC offset calibration (manual mode) */
static BOOLEAN Calibrate(SDW_DEV *d)
{
    UCHAR st = 0;
    ULONG v = DAC_DC_CALI_TRIGGER, i;
    BOOLEAN ok = TRUE;
    SdwRead(d, RC_CAL_STATUS, &st);
    if (!(st & 0x40)) IdxUpd(d, NID_VENDOR_ANALOG, MISC_POWER_CTL0, 0x0010, 0x0010);
    for (i = 0; i < 100 && !(st & 0x40); i++) {
        SdwDelay(10);
        if (!SdwRead(d, RC_CAL_STATUS, &st)) break;
    }
    if (!(st & 0x40)) { LOGE("sdw: %s: RC calibration timed out", d->Name); ok = FALSE; }
    IdxUpd(d, NID_VENDOR_REG, FSM_CTL, 0x2000, 0x2000);                    /* HP calibration by manual mode */
    IdxUpd(d, NID_VENDOR_REG, FSM_CTL, 0xf, 0);                            /* calibration control */
    IdxUpd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, DAC_DC_FORCE_CALI_RST, 0);
    IdxUpd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, DAC_DC_FORCE_CALI_RST, DAC_DC_FORCE_CALI_RST);
    if (HW_VER(d) == 0) IdxUpd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, DAC_DC_CALI_CLK_EN, DAC_DC_CALI_CLK_EN);
    IdxUpd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, DAC_DC_CALI_TRIGGER, DAC_DC_CALI_TRIGGER);
    IdxRd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, &v);
    for (i = 0; i < 100 && (v & DAC_DC_CALI_TRIGGER); i++) {
        SdwDelay(10);
        if (!IdxRd(d, NID_VENDOR_CALI, DAC_DC_CALI_CTL1, &v)) break;
    }
    if (v & DAC_DC_CALI_TRIGGER) { LOGE("sdw: %s: DC calibration timed out", d->Name); ok = FALSE; }
    IdxUpd(d, NID_VENDOR_REG, FSM_CTL, FSM_IMP_EN, FSM_IMP_EN);           /* impedance sense on */
    IdxWr(d, NID_VENDOR_REG, DIGITAL_MISC_CTRL4, 0x201b);                 /* release HP-JD, trigger the FSM */
    return ok;
}

static BOOLEAN Init(SDW_DEV *d)
{
    ULONG v = 0;
    ULONG e0 = d->Errors;
    /* rt711_sdca_reset */
    IdxUpd(d, NID_VENDOR_REG, PARA_VERB_CTL, HIDDEN_REG_SW_RESET, HIDDEN_REG_SW_RESET);
    IdxUpd(d, NID_VENDOR_HDA, HDA_LEGACY_RESET_CTL, 0x1, 0x1);
    if (!IdxRd(d, NID_VENDOR_REG, JD_PRODUCT_NUM, &v)) return FALSE;
    HW_VER(d) = v & 0xf;
    if (HW_VER(d) == 0) {                                                  /* rt711_sdca_vd0_io_init */
        IdxWr(d, NID_VENDOR_REG, GPIO_TEST_MODE_CTL2, 0x0e00);
        IdxWr(d, NID_VENDOR_HDA, HDA_LEGACY_GPIO_CTL, 0x0008);
        SdwWrite(d, 0x2f5a, 0x01);
        IdxWr(d, NID_VENDOR_REG, ADC27_VOL_SET, 0x8728);
        IdxWr(d, NID_VENDOR_REG, COMBO_JACK_AUTO_CTL3, 0xa472);
        SdwWrite(d, 0x2f50, 0x02);
        IdxUpd(d, NID_VENDOR_ANALOG, MISC_POWER_CTL4, 0x6000, 0x6000);
        IdxUpd(d, NID_VENDOR_REG, COMBO_JACK_AUTO_CTL3, 0x000c, 0x000c);
        IdxWr(d, NID_VENDOR_HDA, HDA_LEGACY_CONFIG_CTL, 0x0000);
        IdxWr(d, NID_VENDOR_VAD, VAD_SRAM_CTL1, 0x0050);
    } else {                                                               /* rt711_sdca_vd1_io_init */
        IdxUpd(d, NID_VENDOR_HDA, HDA_LEGACY_UNSOL_CTL, 0x0300, 0x0000);
        IdxWr(d, NID_VENDOR_REG, COMBO_JACK_AUTO_CTL3, 0xa43e);
        SdwWrite(d, 0x2f5a, 0x05);
        IdxWr(d, NID_VENDOR_REG, JD_CTRL6, 0x0500);
        IdxWr(d, NID_VENDOR_REG, DMIC_CTL1, 0x6173);
        IdxWr(d, NID_VENDOR_HDA, HDA_LEGACY_CONFIG_CTL, 0x0000);
        IdxWr(d, NID_VENDOR_VAD, VAD_SRAM_CTL1, 0x0050);
    }
    IdxUpd(d, NID_VENDOR_REG, FILTER_SRC_SEL, 0x1800, 0x0800);           /* DP4 mux from 08_filter_Out_pri */
    IdxUpd(d, NID_VENDOR_HDA, PUSH_BTN_INT_CTL0, 0x20, 0x00);            /* ge_exclusive_inbox_en off */
    Calibrate(d);
    SdwWrite(d, SDW_SDCA_CTL(FN_JACK, ENT_OT1, CTL_VENDOR_DEF, 0), 0x4);  /* HP output enable */
    IdxUpd(d, NID_VENDOR_HDA, GE_MODE_RELATED_CTL, 0x0c00, 0x0000);      /* no jack: HID 1/2 events off */
    /* hw_params: 48 kHz on the jack and microphone clock sources */
    SdcaRate(d, FN_JACK, ENT_CS01, RATE_48000);
    SdcaRate(d, FN_JACK, ENT_CS11, RATE_48000);
    SdcaRate(d, FN_MIC, ENT_CS1F, RATE_48000);
    /* ADC 22 mux: MIC2 (the headset microphone); boost +20 dB, ADC and DAC 0 dB, all muted, powered off */
    IdxUpd(d, NID_VENDOR_HDA, HDA_LEGACY_MUX_CTL1, 0x7 << 10, 0 << 10);
    SdcaVolume(d, FN_JACK, ENT_FU44, SDCA_CTL_FU_GAIN, 3, 20 * 256);
    SdcaVolume(d, FN_JACK, ENT_FU0F, SDCA_CTL_FU_VOLUME, 3, 0);
    SdcaVolume(d, FN_JACK, ENT_FU05, SDCA_CTL_FU_VOLUME, 3, 0);
    SdcaMute(d, FN_JACK, ENT_FU05, 3, TRUE);
    SdcaMute(d, FN_JACK, ENT_FU0F, 3, TRUE);
    SdcaPower(d, FN_JACK, ENT_PDE28, SDCA_PS3, 0);
    SdcaPower(d, FN_JACK, ENT_PDE29, SDCA_PS3, 0);
    SdcaPower(d, FN_MIC, ENT_PDE2A, SDCA_PS3, 0);
    LOGI("sdw: %s: RT711-SDCA VD%u, %u bus errors in init", d->Name, HW_VER(d), d->Errors - e0);
    return d->Errors - e0 < 16;
}

/* Group Entity 49: the detected jack type (3 headphone, 5 headset) as the selected mode */
static void JackMode(SDW_DEV *d)
{
    UCHAR m = 0;
    if (SdwRead(d, SDW_SDCA_CTL(FN_JACK, ENT_GE49, SDCA_CTL_GE_DETECTED_MODE, 0), &m) && m)
        SdwWrite(d, SDW_SDCA_CTL(FN_JACK, ENT_GE49, SDCA_CTL_GE_SELECTED_MODE, 0), m);
}

/* the DAPM order of the Linux driver: supply (PDE) up, then the DAC / ADC feature unit unmuted; reverse off */
static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    ULONG pde = rec ? ENT_PDE29 : ENT_PDE28, fu = rec ? ENT_FU0F : ENT_FU05;
    if (on) {
        SdcaPower(d, FN_JACK, pde, SDCA_PS0, 0);
        JackMode(d);
        SdcaMute(d, FN_JACK, fu, 3, FALSE);
    } else {
        SdcaMute(d, FN_JACK, fu, 3, TRUE);
        SdcaPower(d, FN_JACK, pde, SDCA_PS3, 0);
    }
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    SdcaMute(d, FN_JACK, ENT_FU05, 3, TRUE);
    SdcaMute(d, FN_JACK, ENT_FU0F, 3, TRUE);
    SdcaPower(d, FN_JACK, ENT_PDE28, SDCA_PS3, 0);
    SdcaPower(d, FN_JACK, ENT_PDE29, SDCA_PS3, 0);
    SdcaPower(d, FN_MIC, ENT_PDE2A, SDCA_PS3, 0);
}

static const USHORT g_Parts[] = { 0x0711, 0 };
const SDW_DRV g_SdwRt711 = { "RT711", 0x025D, g_Parts, SDW_KIND_HEADSET, Probe, Init, Stream, Shutdown };
