#include "codec.h"

/*
 * Everest ES8326 (ACPI ESSX8326, SOF platforms, machine driver sound/soc/intel/boards/sof_es8336.c).
 * Ported from Linux sound/soc/codecs/es8326.c/h.
 *
 * I2C: 8-bit register, 8-bit value.  Chip id 0x83/0x26 at 0xFD/0xFE (CHIP_ID1/2), version at 0xFF
 * (0 = first silicon, 3 = "version B"; later parts report more and get the MICBIAS/VMID tweaks).
 *
 * Clocking: no PLL; the internal clocks come from a divider/DLL table indexed by (MCLK, LRCK)
 * (coeff_div_v0 for version 0, coeff_div_v3 otherwise).  CLK_INV bit 3 (ES8326_BCLK_AS_MCLK) makes
 * BCLK the master clock, and the v3 table has rows for MCLK = 32/48/64/100/125 x fs, i.e. for BCLK.
 * Policy (as for the ES8316): MCLK when the SSP drives one the table knows (this is what Linux does:
 * sof_es8336 sets 19.2 MHz), else BCLK-as-MCLK when the table has a row for c->Bclk (32, 48, 64 fs on
 * v3, 64 fs on v0; there is no row for 50 fs).  Caveat of the BCLK mode: BCLK only runs while the SSP
 * runs, but the version-B calibration in Init and the first-unmute HP calibration of version 0 need
 * a running master clock; Linux never uses this bit, so the BCLK mode is untested.
 *
 * Outputs: HPOL/HPOR only; the speakers hang on the same pins behind an amplifier enabled through
 * the codec device's GpioIo lines, exactly as for the ES8316/ES8336 (see cod_es8316.c): GpioIo 0
 * (speakers-enable on the default sof_es8336 quirk) and, if present, GpioIo 1 (speakers- or
 * headphone-enable on the GPIO1 / HEADPHONE_GPIO quirk boards) are driven high 70 ms after the
 * playback path is up and low before it goes down.  HP_MISC is set to "HPVOL: HPL+HPR" so that
 * DACL_VOL / DACR_VOL give independent left / right volume (Linux default uses DACL_VOL for both).
 *
 * Microphone, c->Mic:
 *   0 (default): analog MIC1 (PGAGAIN MIC1_SEL, what es8326_init selects) = the headset mic.
 *                The codec is put in its "headset inserted" analog state while capturing
 *                (SYS_BIAS 0x1f, HP_DRIVER_REF[3:0] 0x0d, ADC1/2 from the analog PGA, MICBIAS1/2 on),
 *                as es8326_jack_detect_handler does when it finds a headset; jack type (CTIA/OMTP)
 *                stays in the automatic mode es8326_init leaves it in.
 *   1:           analog MIC2 (internal analog mic, same gains).
 *   2:           codec DMIC: data on SDINOUT2, clock on SDINOUT1 (ES8326_ADC_DMIC, ES8326_IO_DMIC_CLK).
 *   Gain: analog PGA +24 dB, ADC digital 0 dB; the mono analog PGA feeds ADC1 and ADC2 = left and
 *   right I2S channels.
 */

/* registers (es8326.h) */
#define ES8326_RESET            0x00
#define ES8326_CLK_CTL          0x01
#define ES8326_CLK_INV          0x02
#define ES8326_CLK_RESAMPLE     0x03
#define ES8326_CLK_DIV1         0x04
#define ES8326_CLK_DIV2         0x05
#define ES8326_CLK_DLL          0x06
#define ES8326_CLK_MUX          0x07
#define ES8326_CLK_ADC_SEL      0x08
#define ES8326_CLK_DAC_SEL      0x09
#define ES8326_CLK_ADC_OSR      0x0a
#define ES8326_CLK_DAC_OSR      0x0b
#define ES8326_CLK_DIV_CPC      0x0c
#define ES8326_CLK_TRI          0x0e
#define ES8326_CLK_DIV_LRCK     0x0f
#define ES8326_CLK_VMIDS1       0x10
#define ES8326_CLK_VMIDS2       0x11
#define ES8326_CLK_CAL_TIME     0x12
#define ES8326_FMT              0x13
#define ES8326_DAC_MUTE         0x14
#define ES8326_ADC_MUTE         0x15
#define ES8326_ANA_PDN          0x16
#define ES8326_PGA_PDN          0x17
#define ES8326_VMIDSEL          0x18
#define ES8326_ANA_LP           0x19
#define ES8326_ANA_MICBIAS      0x1b
#define ES8326_ANA_VSEL         0x1c
#define ES8326_SYS_BIAS         0x1d
#define ES8326_VMIDLOW          0x22
#define ES8326_PGAGAIN          0x23
#define ES8326_HP_DRIVER        0x24
#define ES8326_DAC2HPMIX        0x25
#define ES8326_HP_VOL           0x26
#define ES8326_HP_CAL           0x27
#define ES8326_HP_DRIVER_REF    0x28
#define ES8326_ADC_SCALE        0x29
#define ES8326_ADC1_SRC         0x2a
#define ES8326_ADC2_SRC         0x2b
#define ES8326_ADC1_VOL         0x2c
#define ES8326_ADC2_VOL         0x2d
#define ES8326_ALC_RECOVERY     0x32
#define ES8326_HP_OFFSET_CAL    0x4a
#define ES8326_HPL_OFFSET_INI   0x4b
#define ES8326_HPR_OFFSET_INI   0x4c
#define ES8326_DAC_DSM          0x4d
#define ES8326_DAC_RAMPRATE     0x4e
#define ES8326_DAC_VPPSCALE     0x4f
#define ES8326_DACL_VOL         0x50
#define ES8326_DAC_CROSSTALK    0x55
#define ES8326_HPJACK_TIMER     0x56
#define ES8326_HPDET_TYPE       0x57
#define ES8326_INT_SOURCE       0x58
#define ES8326_INTOUT_IO        0x59
#define ES8326_SDINOUT1_IO      0x5a
#define ES8326_SDINOUT23_IO     0x5b
#define ES8326_DACR_VOL         0xf4
#define ES8326_HP_MISC          0xf7
#define ES8326_PULLUP_CTL       0xf9
#define ES8326_CSM_I2C_STA      0xfa
#define ES8326_CSM_MUTE_STA     0xfc
#define ES8326_CHIP_ID1         0xfd
#define ES8326_CHIP_ID2         0xfe
#define ES8326_CHIP_VERSION     0xff

#define ES8326_CSM_ON           0x80
#define ES8326_CLK_ON           0x7f
#define ES8326_BCLK_AS_MCLK     0x08    /* CLK_INV */
#define ES8326_S24_LE           (0 << 2)
#define ES8326_S16_LE           (3 << 2)
#define ES8326_DATA_LEN_MASK    (7 << 2)
#define ES8326_DAIFMT_MASK      ((1 << 5) | (3 << 0))
#define ES8326_DAIFMT_I2S       0
#define ES8326_MIC_SEL_MASK     (3 << 4)
#define ES8326_MIC1_SEL         (1 << 4)
#define ES8326_MIC2_SEL         (1 << 5)
#define ES8326_MUTE_MASK        3
#define ES8326_HP_OFF           0
#define ES8326_HP_FORCE_CAL     ((1 << 7) | (1 << 3))
#define ES8326_HP_ON            ((7 << 4) | (7 << 0))
#define ES8326_HP_DET_SRC_PIN9  (1 << 4)
#define ES8326_HP_TYPE_AUTO     (1 << 0)
#define ES8326_INT_SRC_PIN9     (1 << 3)
#define ES8326_IO_INPUT         0
#define ES8326_IO_DMIC_CLK      9
#define ES8326_SDINOUT1_SHIFT   4
#define ES8326_VERSION_B        3
#define ES8326_ADC_SRC_ANA      0x00    /* ADC1_SRC: analog for ADC1 [2:0] and ADC2 [6:4] */
#define ES8326_ADC_SRC_DMIC     0x44    /* SDIN2 DMIC for both (es8326.h ES8326_ADC_DMIC) */
#define ES8326_MICBIAS12        0x0c    /* ANA_MICBIAS bits 2/3: "MICBIAS1", "MICBIAS2" */

#define ES_GPIO_SPK             0
#define ES_GPIO_SPK2            1

/* c->Priv */
#define P_VER       0
#define P_CAL       1           /* HP offset calibrated */
#define P_MUTE      2           /* user mute */

/* coefficient rows for 48 kHz from es8326.c:
 * {ratio, MCLK, REG04 DIV1, REG05 DIV2, REG06 DLL, REG07 MUX, REG08 ADC_SEL, REG09 DAC_SEL, REG0A, REG0B} */
typedef struct { USHORT Fs; ULONG Mclk; UCHAR R[8]; } ES_COEFF;

static const ES_COEFF g_Coeff0[] = {            /* coeff_div_v0 */
    {  64,  3072000, { 0xE0, 0x00, 0x03, 0x2D, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 128,  6144000, { 0xE0, 0x01, 0x03, 0x2D, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 256, 12288000, { 0xE0, 0x00, 0x30, 0x2D, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 400, 19200000, { 0xE9, 0x04, 0x0F, 0x6d, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 500, 24000000, { 0xF8, 0x04, 0x3F, 0x6D, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 512, 24576000, { 0xE0, 0x00, 0x00, 0x2D, 0x4A, 0x0A, 0x1F, 0x1F } },
};

static const ES_COEFF g_Coeff3[] = {            /* coeff_div_v3 */
    {  32,  1536000, { 0x00, 0x00, 0x13, 0x2D, 0x8A, 0x0A, 0x1F, 0x1F } },
    {  48,  2304000, { 0x00, 0x02, 0x0D, 0x2D, 0x8A, 0x0A, 0x1F, 0x1F } },
    {  64,  3072000, { 0xE0, 0x00, 0x31, 0x2D, 0xCA, 0x0A, 0x1F, 0x1F } },
    { 100,  4800000, { 0x04, 0x04, 0x3F, 0x6D, 0xB8, 0x08, 0x4f, 0x1f } },
    { 125,  6000000, { 0x04, 0x04, 0x1F, 0x2D, 0x8A, 0x0A, 0x27, 0x27 } },
    { 128,  6144000, { 0xE0, 0x00, 0x01, 0x2D, 0x48, 0x08, 0x1F, 0x1F } },
    { 200,  9600000, { 0x04, 0x04, 0x0F, 0x2D, 0xCA, 0x0A, 0x1F, 0x1F } },
    { 250, 12000000, { 0x04, 0x04, 0x0F, 0x2D, 0xCA, 0x0A, 0x27, 0x27 } },
    { 256, 12288000, { 0xE0, 0x01, 0x01, 0x2D, 0x48, 0x08, 0x1F, 0x1F } },
    { 384, 18432000, { 0x00, 0x02, 0x01, 0x2D, 0x8A, 0x0A, 0x1F, 0x1F } },
    { 400, 19200000, { 0xE4, 0x04, 0x35, 0x6d, 0xCA, 0x0A, 0x1F, 0x1F } },
    { 500, 24000000, { 0xF8, 0x04, 0x3F, 0x6D, 0xCA, 0x0A, 0x1F, 0x1F } },
    { 512, 24576000, { 0xE0, 0x00, 0x00, 0x2D, 0x48, 0x08, 0x1F, 0x1F } },
    { 800, 38400000, { 0x00, 0x18, 0x13, 0x2D, 0x8A, 0x0A, 0x1F, 0x1F } },
};

static const ES_COEFF *Es26Coeff(ULONG ver, ULONG rate, ULONG mclk)
{
    const ES_COEFF *t = ver == 0 ? g_Coeff0 : g_Coeff3;
    ULONG n = ver == 0 ? sizeof(g_Coeff0) / sizeof(g_Coeff0[0]) : sizeof(g_Coeff3) / sizeof(g_Coeff3[0]), i;

    if (rate != 48000 || !mclk) return NULL;    /* only the 48 kHz rows are carried */
    for (i = 0; i < n; i++)
        if (t[i].Mclk == mclk) return &t[i];
    return NULL;
}

static BOOLEAN Es26Probe(SST_CODEC *c)
{
    ULONG id1, id2, ver;

    if (!CRd(c, ES8326_CHIP_ID1, &id1) || !CRd(c, ES8326_CHIP_ID2, &id2) || !CRd(c, ES8326_CHIP_VERSION, &ver))
        return FALSE;
    if (id1 != 0x83) return FALSE;              /* Everest */
    if (id2 != 0x26) LOGI("es8326: unexpected chip id %x%02x, treating it as an ES8326", id1, id2);
    return TRUE;
}

static void Es26Spk(SST_CODEC *c, BOOLEAN on)
{
    CGpio(c, ES_GPIO_SPK, on);
    if (c->NGpio > 1) CGpio(c, ES_GPIO_SPK2, on);
}

static void Es26MicBias(SST_CODEC *c, BOOLEAN on)
{
    CUpd(c, ES8326_ANA_MICBIAS, ES8326_MICBIAS12, on ? ES8326_MICBIAS12 : 0);
}

/* es8326_calibrate: HP offset calibration of version B and later */
static BOOLEAN Es26Calibrate(SST_CODEC *c)
{
    ULONG reg, l, r;
    BOOLEAN ok = TRUE;

    if (c->Priv[P_VER] < ES8326_VERSION_B || c->Priv[P_CAL]) return TRUE;
    ok &= CWr(c, ES8326_CLK_INV, 0xc0);
    ok &= CWr(c, ES8326_CLK_DIV1, 0x03);
    ok &= CWr(c, ES8326_CLK_DLL, 0x30);
    ok &= CWr(c, ES8326_CLK_MUX, 0xed);
    ok &= CWr(c, ES8326_CLK_DAC_SEL, 0x08);
    ok &= CWr(c, ES8326_CLK_TRI, 0xc1);
    ok &= CWr(c, ES8326_DAC_MUTE, 0x03);
    ok &= CWr(c, ES8326_ANA_VSEL, 0x7f);
    ok &= CWr(c, ES8326_VMIDLOW, 0x23);
    ok &= CWr(c, ES8326_DAC2HPMIX, 0x88);
    CDelay(20);
    ok &= CWr(c, ES8326_HP_OFFSET_CAL, 0x8c);
    CDelay(20);
    ok &= CWr(c, ES8326_RESET, 0xc0);
    CDelay(20);
    ok &= CWr(c, ES8326_HP_OFFSET_CAL, ES8326_HP_OFF);
    if (CRd(c, ES8326_CSM_MUTE_STA, &reg) && (reg & 0xf0) != 0x40) CDelay(50);
    ok &= CWr(c, ES8326_HP_CAL, 0xd4);
    CDelay(200);
    ok &= CWr(c, ES8326_HP_CAL, 0x4d);
    CDelay(200);
    ok &= CWr(c, ES8326_HP_CAL, ES8326_HP_OFF);
    ok &= CRd(c, ES8326_HPL_OFFSET_INI, &l) && CRd(c, ES8326_HPR_OFFSET_INI, &r);
    if (ok) {
        ok &= CWr(c, ES8326_HP_OFFSET_CAL, 0x8c);
        ok &= CWr(c, ES8326_HPL_OFFSET_INI, l);
        ok &= CWr(c, ES8326_HPR_OFFSET_INI, r);
    }
    ok &= CWr(c, ES8326_CLK_INV, 0x00);
    if (ok) c->Priv[P_CAL] = 1;
    return ok;
}

/* es8326_set_bias_level(SND_SOC_BIAS_ON) */
static void Es26BiasOn(SST_CODEC *c)
{
    CUpd(c, ES8326_RESET, 0x02, 0x02);
    CDelay(5);
    CWr(c, ES8326_INTOUT_IO, 0x00);                         /* everest,interrupt-clk default */
    CWr(c, ES8326_SDINOUT1_IO, ES8326_IO_DMIC_CLK << ES8326_SDINOUT1_SHIFT);
    CWr(c, ES8326_PGA_PDN, 0x40);
    CWr(c, ES8326_ANA_PDN, 0x00);
    CUpd(c, ES8326_CLK_CTL, 0x20, 0x20);
    CUpd(c, ES8326_RESET, 0x02, 0x00);
    if (c->Priv[P_VER] > ES8326_VERSION_B) {
        CUpd(c, ES8326_VMIDSEL, 0x40, 0x40);
        CUpd(c, ES8326_ANA_MICBIAS, 0x70, 0x30);
    }
}

/* es8326_set_bias_level(SND_SOC_BIAS_STANDBY) */
static void Es26BiasStandby(SST_CODEC *c)
{
    CWr(c, ES8326_ANA_PDN, 0x3b);
    CUpd(c, ES8326_CLK_CTL, 0x20, 0x00);
    CWr(c, ES8326_SDINOUT1_IO, ES8326_IO_INPUT);
    if (c->Priv[P_VER] > ES8326_VERSION_B) {
        CUpd(c, ES8326_VMIDSEL, 0x40, 0x40);
        CUpd(c, ES8326_ANA_MICBIAS, 0x70, 0x10);
    }
    CUpd(c, ES8326_CLK_INV, 0xc0, 0xc0);
    CUpd(c, ES8326_CLK_MUX, 0x80, 0x80);
}

/* first half of es8326_mute(unmute), common to both directions */
static void Es26Unmute(SST_CODEC *c)
{
    ULONG l, r;

    if (!c->Priv[P_CAL]) {
        CWr(c, ES8326_HP_CAL, ES8326_HP_FORCE_CAL);
        CDelay(30);
        CWr(c, ES8326_HP_CAL, ES8326_HP_OFF);
        if (CRd(c, ES8326_HPL_OFFSET_INI, &l) && CRd(c, ES8326_HPR_OFFSET_INI, &r)) {
            CWr(c, ES8326_HP_OFFSET_CAL, 0x8c);
            CWr(c, ES8326_HPL_OFFSET_INI, l);
            CWr(c, ES8326_HPR_OFFSET_INI, r);
            c->Priv[P_CAL] = 1;
        }
    }
    CUpd(c, ES8326_CLK_INV, 0xc0, 0x00);
    CUpd(c, ES8326_CLK_MUX, 0x80, 0x00);
}

static BOOLEAN Es26Init(SST_CODEC *c)
{
    const ES_COEFF *k = NULL;
    ULONG ver, clk = 0, i;
    BOOLEAN ok = TRUE, bclk = FALSE;

    Es26Spk(c, FALSE);
    if (!CRd(c, ES8326_CHIP_VERSION, &ver)) return FALSE;
    c->Priv[P_VER] = ver;
    c->Priv[P_CAL] = 0;
    c->Priv[P_MUTE] = 0;

    /* clock source: a known MCLK first (Linux), else BCLK as MCLK */
    if (c->Mclk && (k = Es26Coeff(ver, c->Rate, c->Mclk)) != NULL) clk = c->Mclk;
    else if ((k = Es26Coeff(ver, c->Rate, c->Bclk)) != NULL) { clk = c->Bclk; bclk = TRUE; }
    if (!k) {
        LOGE("es8326: no clock table row for %u Hz frames with MCLK %u / BCLK %u (version %u)",
             c->Rate, c->Mclk, c->Bclk, ver);
        return FALSE;
    }
    if (c->Slots != 2) LOGE("es8326: %u slots per frame, configured as 2-channel I2S", c->Slots);

    /* es8326_init */
    ok &= CWr(c, ES8326_RESET, 0x1f);
    ok &= CWr(c, ES8326_VMIDSEL, 0x3e);
    ok &= CWr(c, ES8326_ANA_LP, 0xf0);
    CDelay(10);
    if (!ok) return FALSE;
    ok &= CWr(c, ES8326_HPJACK_TIMER, 0xd9);
    ok &= CWr(c, ES8326_ANA_MICBIAS, 0xd8);
    ok &= CWr(c, ES8326_HPDET_TYPE, 0x83);                  /* default type and detect pin */
    ok &= CWr(c, ES8326_CLK_RESAMPLE, 0x05);
    ok &= CWr(c, ES8326_CLK_DIV_CPC, 0x89);                 /* internal oscillator clocks the HP charge pump */
    ok &= CWr(c, ES8326_CLK_CTL, ES8326_CLK_ON);
    ok &= CWr(c, ES8326_RESET, 0x17);                       /* clock manager reset release */
    ok &= CWr(c, ES8326_HP_MISC, 0x3d | 0x40);              /* half scan mode; HPVOL: HPL+HPR (see header) */
    ok &= CWr(c, ES8326_PULLUP_CTL, 0x00);
    ok &= CWr(c, ES8326_HP_VOL, 0xc4);
    ok &= CWr(c, ES8326_HP_DRIVER, 0xa7);                   /* enable headphone driver */
    CDelay(2);
    ok &= CWr(c, ES8326_HP_DRIVER_REF, 0x23);
    ok &= CWr(c, ES8326_HP_DRIVER_REF, 0x33);
    ok &= CWr(c, ES8326_HP_DRIVER, 0xa1);
    ok &= CWr(c, ES8326_CLK_INV, 0x00);
    ok &= CWr(c, ES8326_CLK_VMIDS1, 0xc4);
    ok &= CWr(c, ES8326_CLK_VMIDS2, 0x81);
    ok &= CWr(c, ES8326_CLK_CAL_TIME, 0x00);
    ok &= Es26Calibrate(c);
    ok &= CWr(c, ES8326_DAC_CROSSTALK, 0xaa);
    ok &= CWr(c, ES8326_DAC_RAMPRATE, 0x00);
    ok &= CWr(c, ES8326_HP_CAL, 0x00);                      /* headphone out off */
    ok &= CWr(c, ES8326_ANA_LP, 0xf0);                      /* ADC/DAC low power mode */
    ok &= CWr(c, ES8326_ANA_VSEL, 0x7f);
    ok &= CWr(c, ES8326_VMIDLOW, 0x03);                     /* vdda as micbias source */
    ok &= CWr(c, ES8326_DAC_DSM, 0x08);                     /* dsmclip */
    ok &= CWr(c, ES8326_DAC_VPPSCALE, 0x15);
    ok &= CWr(c, ES8326_HPDET_TYPE, 0x80 | ES8326_HP_DET_SRC_PIN9 | ES8326_HP_TYPE_AUTO |
              (ver >= ES8326_VERSION_B ? 0 : 0x04));
    CDelay(5);
    Es26MicBias(c, TRUE);
    CDelay(50);
    ok &= CUpd(c, ES8326_HPDET_TYPE, 0x03, 0x00);
    ok &= CWr(c, ES8326_INTOUT_IO, 0x00);
    ok &= CWr(c, ES8326_SDINOUT1_IO, ES8326_IO_INPUT);
    ok &= CWr(c, ES8326_SDINOUT23_IO, ES8326_IO_INPUT);
    ok &= CWr(c, ES8326_ANA_PDN, 0x00);
    ok &= CWr(c, ES8326_RESET, ES8326_CSM_ON);
    ok &= CUpd(c, ES8326_PGAGAIN, ES8326_MIC_SEL_MASK, c->Mic == 1 ? ES8326_MIC2_SEL : ES8326_MIC1_SEL);
    ok &= CUpd(c, ES8326_DAC_MUTE, ES8326_MUTE_MASK, ES8326_MUTE_MASK);
    ok &= CWr(c, ES8326_ADC_MUTE, 0x0f);
    ok &= CWr(c, ES8326_CLK_DIV_LRCK, 0xff);
    ok &= CWr(c, ES8326_ADC1_SRC, 0x44);                    /* ADC parked on the (absent) DMIC = silent */
    ok &= CWr(c, ES8326_ADC2_SRC, 0x66);
    Es26MicBias(c, FALSE);
    if (ver > ES8326_VERSION_B) {
        ok &= CUpd(c, ES8326_ANA_MICBIAS, 0x73, 0x10);
        ok &= CUpd(c, ES8326_VMIDSEL, 0x40, 0x40);
    }
    CDelay(200);
    ok &= CWr(c, ES8326_INT_SOURCE, ES8326_INT_SRC_PIN9);   /* jack IRQ source as Linux (nobody listens) */
    if (!ok) return FALSE;

    /* DAI: slave (RESET MASTER_MODE_EN clear), I2S, word length from c->Valid (set_fmt / hw_params) */
    ok &= CUpd(c, ES8326_FMT, ES8326_DAIFMT_MASK | ES8326_DATA_LEN_MASK,
               ES8326_DAIFMT_I2S | (c->Valid == 16 ? ES8326_S16_LE : ES8326_S24_LE));
    for (i = 0; i < 8; i++) ok &= CWr(c, ES8326_CLK_DIV1 + i, k->R[i]);    /* regs 0x04..0x0b */
    ok &= CUpd(c, ES8326_CLK_INV, ES8326_BCLK_AS_MCLK, bclk ? ES8326_BCLK_AS_MCLK : 0);

    /* gains: DAC 0 dB (0xbf, 0.5 dB steps from -95.5 dB), ADC digital 0 dB, analog PGA +24 dB */
    ok &= CWr(c, ES8326_DACL_VOL, 0xbf);
    ok &= CWr(c, ES8326_DACR_VOL, 0xbf);
    ok &= CWr(c, ES8326_ADC1_VOL, 0xbf);
    ok &= CWr(c, ES8326_ADC2_VOL, 0xbf);
    ok &= CWr(c, ES8326_ADC_SCALE, 0x00);
    ok &= CUpd(c, ES8326_PGAGAIN, 0x0f, 8);                 /* 8 x 3 dB */
    ok &= CUpd(c, ES8326_ALC_RECOVERY, 0x08, 0x00);         /* ALC off */
    ok &= CWr(c, ES8326_DAC2HPMIX, 0x00);                   /* LHPMIX/RHPMIX off until playback */

    /* idle: bias STANDBY state, everything quiet */
    Es26BiasStandby(c);
    if (!ok) return FALSE;

    c->Caps = CC_PLAY | CC_REC;
    LOGI("es8326: version %u, %s %u Hz (%u fs), %u-bit I2S, mic %s", ver, bclk ? "BCLK as MCLK" : "MCLK",
         clk, k->Fs, c->Valid == 16 ? 16 : 24, c->Mic == 2 ? "DMIC" : c->Mic == 1 ? "MIC2" : "MIC1");
    return TRUE;
}

static void Es26Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (!rec && on) {
        CUpd(c, ES8326_DAC2HPMIX, 0x88, 0x88);              /* LHPMIX / RHPMIX */
        CUpd(c, ES8326_ANA_PDN, 0x03, 0x00);                /* Left / Right DAC */
        if (!c->RecOn) Es26BiasOn(c);
        Es26Unmute(c);
        CUpd(c, ES8326_DAC_DSM, 0x01, 0x01);
        CDelay(2);
        CUpd(c, ES8326_DAC_DSM, 0x01, 0x00);
        CDelay(2);
        CUpd(c, ES8326_HP_DRIVER_REF, 0x30, 0x20);
        CUpd(c, ES8326_HP_DRIVER_REF, 0x30, 0x30);
        CWr(c, ES8326_HP_DRIVER, 0xa1);
        CWr(c, ES8326_HP_CAL, ES8326_HP_ON);
        if (!c->Priv[P_MUTE]) CUpd(c, ES8326_DAC_MUTE, ES8326_MUTE_MASK, 0);
        CDelay(70);                                         /* sof_es8336 pcm_pop_work */
        Es26Spk(c, TRUE);
    } else if (!rec) {
        Es26Spk(c, FALSE);
        CWr(c, ES8326_HP_CAL, ES8326_HP_OFF);
        CUpd(c, ES8326_DAC_MUTE, ES8326_MUTE_MASK, ES8326_MUTE_MASK);
        CUpd(c, ES8326_HP_DRIVER_REF, 0x30, 0x00);
        CUpd(c, ES8326_DAC2HPMIX, 0x88, 0x00);
        CUpd(c, ES8326_ANA_PDN, 0x03, 0x03);
        if (!c->RecOn) Es26BiasStandby(c);
    } else if (on) {
        if (!c->PlayOn) Es26BiasOn(c);
        if (c->Mic == 2) {
            CWr(c, ES8326_ADC1_SRC, ES8326_ADC_SRC_DMIC);
            CWr(c, ES8326_ADC2_SRC, 0x00);
        } else {
            /* the "headset detected" state of es8326_jack_detect_handler */
            CWr(c, ES8326_SYS_BIAS, 0x1f);
            CUpd(c, ES8326_HP_DRIVER_REF, 0x0f, 0x0d);
            Es26MicBias(c, TRUE);
            CUpd(c, ES8326_PGA_PDN, 0x08, 0x08);
            CWr(c, ES8326_ADC1_SRC, ES8326_ADC_SRC_ANA);
            CWr(c, ES8326_ADC2_SRC, 0x00);
            CUpd(c, ES8326_PGA_PDN, 0x08, 0x00);
            CDelay(10);
        }
        Es26Unmute(c);
        CDelay(300);                                        /* es8326_mute capture de-pop */
        if (c->Priv[P_VER] > ES8326_VERSION_B) {
            CUpd(c, ES8326_ANA_MICBIAS, 0x70, 0x70);
            CUpd(c, ES8326_VMIDSEL, 0x40, 0x00);
        }
        CDelay(40);                                         /* capture_pop_work */
        CUpd(c, ES8326_ADC_MUTE, 0x0f, 0x00);
    } else {
        CUpd(c, ES8326_ADC_MUTE, 0x0f, 0x0f);
        if (c->Priv[P_VER] > ES8326_VERSION_B) {
            CUpd(c, ES8326_VMIDSEL, 0x40, 0x40);
            CUpd(c, ES8326_ANA_MICBIAS, 0x70, 0x30);
        }
        /* back to the "no headset" state of the jack handler, ADC parked */
        CWr(c, ES8326_ADC1_SRC, 0x44);
        CWr(c, ES8326_ADC2_SRC, 0x66);
        if (c->Mic != 2) {
            Es26MicBias(c, FALSE);
            CWr(c, ES8326_SYS_BIAS, 0x0a);
            CUpd(c, ES8326_HP_DRIVER_REF, 0x0f, 0x03);
        }
        if (!c->PlayOn) Es26BiasStandby(c);
    }
}

static void Es26Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    /* DACL_VOL / DACR_VOL: 0xbf = 0 dB, 0.5 dB steps down to 0 = -95.5 dB */
    CWr(c, ES8326_DACL_VOL, CVolSteps(left, 0xbf));
    CWr(c, ES8326_DACR_VOL, CVolSteps(right, 0xbf));
    c->Priv[P_MUTE] = mute;
    if (mute || c->PlayOn) CUpd(c, ES8326_DAC_MUTE, ES8326_MUTE_MASK, mute ? ES8326_MUTE_MASK : 0);
}

static void Es26Shutdown(SST_CODEC *c)
{
    /* es8326_suspend + es8326_remove: mute, analog down, clocks off, registers back to defaults */
    Es26Spk(c, FALSE);
    CWr(c, ES8326_HP_CAL, ES8326_HP_OFF);
    CUpd(c, ES8326_DAC_MUTE, ES8326_MUTE_MASK, ES8326_MUTE_MASK);
    CWr(c, ES8326_ADC_MUTE, 0x0f);
    Es26MicBias(c, FALSE);
    CWr(c, ES8326_CLK_MUX, 0x2d);
    CWr(c, ES8326_DAC2HPMIX, 0x00);
    CWr(c, ES8326_ANA_PDN, 0x3b);
    CWr(c, ES8326_CLK_CTL, 0x00);
    CWr(c, ES8326_CSM_I2C_STA, 0x01);
    CDelay(2);
    CWr(c, ES8326_CSM_I2C_STA, 0x00);
    c->Priv[P_CAL] = 0;
}

const CODEC_DRV g_DrvEs8326 = { "ES8326", "ESSX8326\0", CODEC_HEADSET, 1, 1,
                                Es26Probe, Es26Init, Es26Stream, Es26Volume, Es26Shutdown };
