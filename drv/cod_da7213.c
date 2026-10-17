#include "codec.h"
/*
 * AUDIONT - Dialog DA7213 / DA7212 codec (Bay Trail / Cherry Trail tablets, ACPI "DLGS7213" / "DLGS7212").
 * Reference: Linux sound/soc/codecs/da7213.c / da7213.h, intel/boards/bytcht_da7213.c.
 *
 * Clocking: the DA7213 PLL only takes MCLK as reference (PLL_CTRL INDIV; 5..54 MHz), so BCLK cannot be
 * used.  bytcht_da7213.c feeds 19.2 MHz MCLK and runs the PLL in SRM mode, which locks the 94.3104 MHz
 * PLL output to WCLK (the driver forces DA7213_PLL_FREQ_OUT_94310400 for SRM).  As da7213_set_auto_pll()
 * does, harmonic MCLKs (12.288/24.576/49.152 MHz for 48 kHz) bypass the PLL.  The PLL is enabled from the
 * first stream start to the last stream stop (machine hw_params / hw_free).  Init fails without MCLK.
 *
 * The chip has no software-reset register in the Linux map; Init writes the Linux register defaults
 * (outputs first) to reach a known state.  There is no id register either: Probe checks reserved bits and
 * rejects a DA7219 (which answers at the same address with CHIP_ID 0x23 0x93).
 *
 * Outputs: HPL/HPR (charge pump) and the mono LINE output (fed by Mixout Right), all powered while
 * playing.  c->Mic: 0 = MIC1 (headset mic per bytcht_da7213.c), differential, MICBIAS1;
 *                   1 = MIC2 (on-board analog mic), differential, MICBIAS2;
 *                   2 = MIC1 single-ended (MIC_P), MICBIAS1.
 */

#define R_DIG_ROUTING_DAI       0x21
#define R_SR                    0x22
#define R_REFERENCES            0x23
#define R_PLL_FRAC_TOP          0x24
#define R_PLL_FRAC_BOT          0x25
#define R_PLL_INTEGER           0x26
#define R_PLL_CTRL              0x27
#define R_DAI_CLK_MODE          0x28
#define R_DAI_CTRL              0x29
#define R_DIG_ROUTING_DAC       0x2A
#define R_ALC_CTRL1             0x2B
#define R_MIXIN_L_SELECT        0x32
#define R_MIXIN_R_SELECT        0x33
#define R_MIXIN_L_GAIN          0x34
#define R_MIXIN_R_GAIN          0x35
#define R_ADC_L_GAIN            0x36
#define R_ADC_R_GAIN            0x37
#define R_MIC_1_GAIN            0x39
#define R_MIC_2_GAIN            0x3A
#define R_DAC_L_GAIN            0x45
#define R_DAC_R_GAIN            0x46
#define R_CP_CTRL               0x47
#define R_HP_L_GAIN             0x48
#define R_HP_R_GAIN             0x49
#define R_LINE_GAIN             0x4A
#define R_MIXOUT_L_SELECT       0x4B
#define R_MIXOUT_R_SELECT       0x4C
#define R_AUX_L_CTRL            0x60
#define R_AUX_R_CTRL            0x61
#define R_MICBIAS_CTRL          0x62
#define R_MIC_1_CTRL            0x63
#define R_MIC_2_CTRL            0x64
#define R_MIXIN_L_CTRL          0x65
#define R_MIXIN_R_CTRL          0x66
#define R_ADC_L_CTRL            0x67
#define R_ADC_R_CTRL            0x68
#define R_DAC_L_CTRL            0x69
#define R_DAC_R_CTRL            0x6A
#define R_HP_L_CTRL             0x6B
#define R_HP_R_CTRL             0x6C
#define R_LINE_CTRL             0x6D
#define R_MIXOUT_L_CTRL         0x6E
#define R_MIXOUT_R_CTRL         0x6F
#define R_MIC_CONFIG            0x93
#define R_PC_COUNT              0x94
#define R_DAI_OFFSET            0x98
#define R_TONE_GEN_CYCLES       0xB6
#define R_PLL_STATUS            0x03

#define AMP_EN                  0x80    /* DA7213_AMP_EN / ADC_EN / DAC_EN / CP_EN (bit 7) */
#define MUTE_EN                 0x40    /* DA7213_MUTE_EN */
#define GAIN_RAMP_EN            0x20    /* DA7213_GAIN_RAMP_EN */
#define MIX_EN                  0x08    /* DA7213_MIXIN_MIX_EN / MIXOUT_MIX_EN */
#define AMP_OE                  0x08    /* DA7213_HP_AMP_OE / LINE_AMP_OE */
#define VMID_EN                 0x80
#define BIAS_EN                 0x08
#define MICBIAS1_EN             0x08
#define MICBIAS2_EN             0x80
#define PLL_INDIV_MASK          (3 << 2)
#define PLL_MCLK_SQR_EN         (1 << 4)
#define PLL_MODE_MASK           (7 << 5)
#define PLL_SRM_EN              (1 << 6)
#define PLL_EN                  (1 << 7)
#define PLL_SRM_LOCK            0x02    /* PLL_STATUS */
#define PLL_FREQ_OUT_94310400   94310400
#define DAI_EN                  0x80
#define PC_FREERUN              0x01
#define ALC_CALIB_MODE_MAN      0x04
#define DAC_GAIN_0DB            0x6F    /* register default; digital_gain_tlv 0.75 dB steps */
#define DAC_GAIN_MIN            0x08    /* 0x00..0x07 = mute */

/* Priv[] */
#define P_BYPASS    0
#define P_INDIV     1
#define P_MUTE      2

static ULONG MulDiv(ULONG a, ULONG b, ULONG c)
{
    ULONGLONG n = (ULONGLONG)a * b, r = 0;
    ULONG q = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        q <<= 1;
        if (r >= c) { r -= c; q |= 1; }
    }
    return q;
}

/* da7213_reg_defaults (Linux regmap), outputs and power bits first */
static const ULONG s_Defaults[] = {
    R_HP_L_CTRL, 0x41, R_HP_R_CTRL, 0x40, R_LINE_CTRL, 0x40, R_MIXOUT_L_CTRL, 0x10, R_MIXOUT_R_CTRL, 0x10,
    R_DAC_L_CTRL, 0x48, R_DAC_R_CTRL, 0x40, R_CP_CTRL, 0x61,
    R_ADC_L_CTRL, 0x40, R_ADC_R_CTRL, 0x40, R_MIXIN_L_CTRL, 0x40, R_MIXIN_R_CTRL, 0x40,
    R_MIC_1_CTRL, 0x40, R_MIC_2_CTRL, 0x40, R_AUX_L_CTRL, 0x44, R_AUX_R_CTRL, 0x44, R_MICBIAS_CTRL, 0x11,
    R_DAI_CTRL, 0x08, R_PLL_CTRL, 0x0C,
    R_DIG_ROUTING_DAI, 0x10, R_SR, 0x0A, R_PLL_FRAC_TOP, 0x00, R_PLL_FRAC_BOT, 0x00,
    R_PLL_INTEGER, 0x20, R_DAI_CLK_MODE, 0x01, R_DIG_ROUTING_DAC, 0x32,
    0x30, 0x35, 0x31, 0x35, R_MIXIN_L_SELECT, 0x00, R_MIXIN_R_SELECT, 0x00, R_MIXIN_L_GAIN, 0x03,
    R_MIXIN_R_GAIN, 0x03, R_ADC_L_GAIN, 0x6F, R_ADC_R_GAIN, 0x6F, 0x38, 0x80, R_MIC_1_GAIN, 0x01,
    R_MIC_2_GAIN, 0x01, 0x40, 0x00, 0x41, 0x88, 0x42, 0x88, 0x43, 0x08, 0x44, 0x80,
    R_DAC_L_GAIN, 0x6F, R_DAC_R_GAIN, 0x6F, R_HP_L_GAIN, 0x39, R_HP_R_GAIN, 0x39, R_LINE_GAIN, 0x30,
    R_MIXOUT_L_SELECT, 0x00, R_MIXOUT_R_SELECT, 0x00, 0x50, 0x00, 0x51, 0x00,
    0x90, 0x00, 0x91, 0x00, 0x92, 0x00, R_MIC_CONFIG, 0x00, R_PC_COUNT, 0x00, 0x95, 0x32, 0x96, 0x95,
    0x97, 0x00, R_DAI_OFFSET, 0x00, 0x99, 0x00, 0x9A, 0x00, 0x9B, 0x00, 0x9C, 0x3F, 0x9D, 0x3F,
    0x9E, 0x00, 0x9F, 0xFF, 0xA0, 0x71, 0xA1, 0x00, 0xA2, 0x00, 0xA6, 0x00, 0xA7, 0x00, 0xAB, 0x00,
    0xAC, 0x00, 0xAD, 0x00, 0xAF, 0x00, 0xB0, 0x00, 0xB1, 0x00, 0xB2, 0x00,
    R_REFERENCES, 0x80,
};

static BOOLEAN Probe(SST_CODEC *c)
{
    ULONG sr, pll, id1, id2, cp;
    if (!CRd(c, R_SR, &sr) || !CRd(c, R_PLL_CTRL, &pll) || !CRd(c, R_CP_CTRL, &cp)) return FALSE;
    if (CRd(c, 0x81, &id1) && CRd(c, 0x82, &id2) && id1 == 0x23 && id2 == 0x93) return FALSE;  /* DA7219 */
    if (sr & 0xF0) return FALSE;            /* SR[7:4] reserved */
    if (pll & 0x03) return FALSE;           /* PLL_CTRL[1:0] reserved */
    return TRUE;
}

static ULONG SrCode(ULONG rate)
{
    static const ULONG tab[][2] = { { 8000, 0x1 }, { 11025, 0x2 }, { 12000, 0x3 }, { 16000, 0x5 },
        { 22050, 0x6 }, { 32000, 0x9 }, { 44100, 0xA }, { 48000, 0xB }, { 88200, 0xE }, { 96000, 0xF } };
    ULONG i;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (tab[i][0] == rate) return tab[i][1];
    return 0xB;
}

/* _da7213_set_component_pll(): DA7213_SYSCLK_MCLK (PLL off) */
static void PllOff(SST_CODEC *c)
{
    CUpd(c, R_PLL_CTRL, PLL_INDIV_MASK | PLL_MODE_MASK, c->Priv[P_INDIV]);
}

/* _da7213_set_component_pll(DA7213_SYSCLK_PLL_SRM): fout forced to 94.3104 MHz, 13-bit fraction */
static void PllOn(SST_CODEC *c)
{
    static const ULONG div[] = { 2, 4, 8, 16 };         /* DA7213_PLL_INDIV_*_VAL */
    ULONG ref, integ, frac;
    if (c->Priv[P_BYPASS]) { PllOff(c); return; }
    ref = c->Mclk / div[c->Priv[P_INDIV] >> 2];
    integ = PLL_FREQ_OUT_94310400 / ref;
    frac = MulDiv(PLL_FREQ_OUT_94310400 % ref, 8192, ref);
    CWr(c, R_PLL_FRAC_TOP, (frac >> 8) & 0xFF);
    CWr(c, R_PLL_FRAC_BOT, frac & 0xFF);
    CWr(c, R_PLL_INTEGER, integ);
    CUpd(c, R_PLL_CTRL, PLL_INDIV_MASK | PLL_MODE_MASK, c->Priv[P_INDIV] | PLL_SRM_EN | PLL_EN);
}

/* "DAI" supply with da7213_dai_event() */
static void DaiOn(SST_CODEC *c)
{
    ULONG v, i;
    CUpd(c, R_PC_COUNT, PC_FREERUN, 0);
    if (CGet(c, R_PLL_CTRL) & PLL_SRM_EN) {
        for (i = 0; i < 8; i++) {                       /* DA7213_SRM_CHECK_RETRIES */
            if (CRd(c, R_PLL_STATUS, &v) && (v & PLL_SRM_LOCK)) break;
            CDelay(50);
        }
        if (i == 8) LOGV("DA7213: SRM not locked yet (WCLK not running?)\n");
    }
    CUpd(c, R_DAI_CTRL, DAI_EN, DAI_EN);
}

static void DaiOff(SST_CODEC *c)
{
    CUpd(c, R_DAI_CTRL, DAI_EN, 0);
    CUpd(c, R_PC_COUNT, PC_FREERUN, PC_FREERUN);
}

static void DacMute(SST_CODEC *c, BOOLEAN mute)      /* da7213_mute() */
{
    CUpd(c, R_DAC_L_CTRL, MUTE_EN, mute ? MUTE_EN : 0);
    CUpd(c, R_DAC_R_CTRL, MUTE_EN, mute ? MUTE_EN : 0);
}

static void Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? c->PlayOn : c->RecOn;
    BOOLEAN mic2 = c->Mic == 1;
    if (on) {
        if (!other) { PllOn(c); DaiOn(c); }             /* machine hw_params, then supplies (reg order) */
        if (!rec) {
            CUpd(c, R_CP_CTRL, AMP_EN, AMP_EN);         /* "Charge Pump" supply */
            CUpd(c, R_DAC_L_CTRL, AMP_EN, AMP_EN);      /* DACs */
            CUpd(c, R_DAC_R_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_HP_L_CTRL, AMP_EN, AMP_EN);       /* PGAs in register order */
            CUpd(c, R_HP_R_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_LINE_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_MIXOUT_L_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_MIXOUT_R_CTRL, AMP_EN, AMP_EN);
            DacMute(c, (BOOLEAN)c->Priv[P_MUTE]);       /* mute_stream(0) */
        } else {
            CUpd(c, R_MICBIAS_CTRL, mic2 ? MICBIAS2_EN : MICBIAS1_EN, mic2 ? MICBIAS2_EN : MICBIAS1_EN);
            CUpd(c, mic2 ? R_MIC_2_CTRL : R_MIC_1_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_MIXIN_L_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_MIXIN_R_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_ADC_L_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_ADC_R_CTRL, AMP_EN, AMP_EN);
        }
    } else {
        if (!rec) {
            DacMute(c, TRUE);                           /* mute_stream(1) */
            CUpd(c, R_HP_L_CTRL, AMP_EN, 0);
            CUpd(c, R_HP_R_CTRL, AMP_EN, 0);
            CUpd(c, R_LINE_CTRL, AMP_EN, 0);
            CUpd(c, R_MIXOUT_L_CTRL, AMP_EN, 0);
            CUpd(c, R_MIXOUT_R_CTRL, AMP_EN, 0);
            CUpd(c, R_DAC_L_CTRL, AMP_EN, 0);
            CUpd(c, R_DAC_R_CTRL, AMP_EN, 0);
            if (!other) DaiOff(c);
            CUpd(c, R_CP_CTRL, AMP_EN, 0);
        } else {
            CUpd(c, R_ADC_L_CTRL, AMP_EN, 0);
            CUpd(c, R_ADC_R_CTRL, AMP_EN, 0);
            CUpd(c, R_MIC_1_CTRL, AMP_EN, 0);
            CUpd(c, R_MIC_2_CTRL, AMP_EN, 0);
            CUpd(c, R_MIXIN_L_CTRL, AMP_EN, 0);
            CUpd(c, R_MIXIN_R_CTRL, AMP_EN, 0);
            if (!other) DaiOff(c);
            CUpd(c, R_MICBIAS_CTRL, MICBIAS1_EN | MICBIAS2_EN, 0);
        }
        if (!other) PllOff(c);                          /* machine hw_free */
    }
}

/* DAC digital gain: 0x6F (0 dB, reset default) down to 0x08 in 0.75 dB steps; mute = DAC_x_CTRL MUTE_EN */
static void Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    CWr(c, R_DAC_L_GAIN, DAC_GAIN_MIN + CVolSteps(left, DAC_GAIN_0DB - DAC_GAIN_MIN));
    CWr(c, R_DAC_R_GAIN, DAC_GAIN_MIN + CVolSteps(right, DAC_GAIN_0DB - DAC_GAIN_MIN));
    c->Priv[P_MUTE] = mute;
    if (c->PlayOn) DacMute(c, mute);
}

static BOOLEAN Init(SST_CODEC *c)
{
    ULONG e0 = c->Errors, v, insel, wl;
    if (!c->Mclk) {
        LOGE("DA7213: no MCLK on SSP%u - the DA7213 PLL cannot use BCLK as reference\n", c->Port);
        return FALSE;
    }
    if (c->Mclk < 5000000 || c->Mclk > 54000000) {
        LOGE("DA7213: MCLK %u out of the 5..54 MHz PLL input range\n", c->Mclk);
        return FALSE;
    }
    c->PlayOn = c->RecOn = FALSE;
    if (!CSeq(c, s_Defaults, sizeof(s_Defaults) / sizeof(s_Defaults[0]) / 2)) return FALSE;

    /* da7213_probe() */
    CUpd(c, R_ALC_CTRL1, ALC_CALIB_MODE_MAN, 0);
    CUpd(c, R_PC_COUNT, PC_FREERUN, PC_FREERUN);
    CUpd(c, R_AUX_L_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_AUX_R_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_MIXIN_L_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_MIXIN_R_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_ADC_L_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_ADC_R_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_DAC_L_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_DAC_R_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_HP_L_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_HP_R_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_LINE_CTRL, GAIN_RAMP_EN, GAIN_RAMP_EN);
    CUpd(c, R_MIXIN_L_CTRL, MIX_EN, MIX_EN);
    CUpd(c, R_MIXIN_R_CTRL, MIX_EN, MIX_EN);
    CUpd(c, R_MIXOUT_L_CTRL, MIX_EN, MIX_EN);
    CUpd(c, R_MIXOUT_R_CTRL, MIX_EN, MIX_EN);
    CUpd(c, R_HP_L_CTRL, AMP_OE, AMP_OE);
    CUpd(c, R_HP_R_CTRL, AMP_OE, AMP_OE);
    CUpd(c, R_LINE_CTRL, AMP_OE, AMP_OE);
    CUpd(c, R_MICBIAS_CTRL, 0x33, 0x11);                /* MICBIAS1/2 2.2 V (Linux default) */
    CUpd(c, R_MIC_CONFIG, 0x07, 0x00);                  /* DMIC defaults: LRISE_RFALL, on edge, 3 MHz */
    CWr(c, R_TONE_GEN_CYCLES, 0x07);

    /* sysclk DA7213_CLKSRC_MCLK, input divider; harmonic MCLK -> PLL bypass (da7213_set_auto_pll) */
    CUpd(c, R_PLL_CTRL, PLL_MCLK_SQR_EN, 0);
    v = c->Mclk <= 9000000 ? 0 : c->Mclk <= 18000000 ? 1 : c->Mclk <= 36000000 ? 2 : 3;
    c->Priv[P_INDIV] = v << 2;
    c->Priv[P_BYPASS] = (c->Mclk == 12288000 || c->Mclk == 24576000 || c->Mclk == 49152000) &&
                        (c->Rate % 11025) != 0;
    PllOff(c);

    /* SND_SOC_BIAS_STANDBY (idle_bias_on): VMID reference and master bias */
    CUpd(c, R_REFERENCES, VMID_EN | BIAS_EN, VMID_EN | BIAS_EN);

    /* DAI: consumer, I2S, NB/NF, DAI_OFFSET 0; hw_params: word length, BCLKs/WCLK, stereo, SR */
    wl = c->Valid >= 24 ? (2 << 2) : (0 << 2);
    CUpd(c, R_DAI_CLK_MODE, 0x80 | 0x0F, c->Valid >= 24 ? 0x01 : 0x00);
    CUpd(c, R_DAI_CTRL, 0x1F, 0x00 | wl);
    CWr(c, R_DAI_OFFSET, 0);
    CWr(c, R_SR, SrCode(c->Rate));
    if (c->Slots != 2) LOGI("DA7213: %u slots - only the first two (I2S L/R) are used\n", c->Slots);

    /* playback routing: DAC L/R <- DAI L/R, Mixout L <- DAC L, Mixout R <- DAC R (LINE <- Mixout R) */
    CWr(c, R_DIG_ROUTING_DAC, 0x32);
    CWr(c, R_MIXOUT_L_SELECT, 1 << 3);                  /* "DAC Left Switch" */
    CWr(c, R_MIXOUT_R_SELECT, 1 << 3);                  /* "DAC Right Switch" */
    CWr(c, R_HP_L_GAIN, 0x39);                          /* 0 dB */
    CWr(c, R_HP_R_GAIN, 0x39);
    CWr(c, R_LINE_GAIN, 0x30);                          /* 0 dB */
    CUpd(c, R_HP_L_CTRL, MUTE_EN, 0);
    CUpd(c, R_HP_R_CTRL, MUTE_EN, 0);
    CUpd(c, R_LINE_CTRL, MUTE_EN, 0);
    DacMute(c, TRUE);                                   /* unmuted by Stream (mute_stream) */
    Volume(c, 65535, 65535, FALSE);

    /* capture: chosen mic PGA into both mixin channels, ADC L/R -> DAI L/R */
    insel = c->Mic == 2 ? 1 : 0;                        /* MIC_AMP_IN_SEL: 0 diff, 1 MIC_P */
    if (c->Mic == 1) {
        CWr(c, R_MIXIN_L_SELECT, 1 << 2);               /* "Mixin Left Mic 2 Switch" */
        CWr(c, R_MIXIN_R_SELECT, 1 << 1);               /* "Mixin Right Mic 2 Switch" */
        CUpd(c, R_MIC_2_CTRL, (3 << 2) | MUTE_EN, 0);
        CWr(c, R_MIC_2_GAIN, 0x05);                     /* -6 + 5 * 6 = +24 dB */
    } else {
        CWr(c, R_MIXIN_L_SELECT, 1 << 1);               /* "Mixin Left Mic 1 Switch" */
        CWr(c, R_MIXIN_R_SELECT, 1 << 2);               /* "Mixin Right Mic 1 Switch" */
        CUpd(c, R_MIC_1_CTRL, (3 << 2) | MUTE_EN, insel << 2);
        CWr(c, R_MIC_1_GAIN, 0x05);
    }
    CWr(c, R_MIXIN_L_GAIN, 0x03);                       /* 0 dB */
    CWr(c, R_MIXIN_R_GAIN, 0x03);
    CWr(c, R_ADC_L_GAIN, 0x6F);
    CWr(c, R_ADC_R_GAIN, 0x6F);
    CUpd(c, R_MIXIN_L_CTRL, MUTE_EN, 0);
    CUpd(c, R_MIXIN_R_CTRL, MUTE_EN, 0);
    CUpd(c, R_ADC_L_CTRL, MUTE_EN, 0);
    CUpd(c, R_ADC_R_CTRL, MUTE_EN, 0);
    CWr(c, R_DIG_ROUTING_DAI, 0x10);                    /* DAI L <- ADC L, DAI R <- ADC R */

    if (c->Errors != e0) return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("DA7213: %s, MCLK %u Hz, %s, mic %s\n", c->Hid, c->Mclk,
         c->Priv[P_BYPASS] ? "PLL bypass" : "PLL SRM 94.3104 MHz",
         c->Mic == 1 ? "MIC2 diff" : c->Mic == 2 ? "MIC1 SE" : "MIC1 diff");
    return TRUE;
}

static void Shutdown(SST_CODEC *c)
{
    if (c->PlayOn) Stream(c, FALSE, FALSE);
    c->PlayOn = FALSE;
    if (c->RecOn) Stream(c, TRUE, FALSE);
    c->RecOn = FALSE;
    PllOff(c);
    CUpd(c, R_REFERENCES, VMID_EN | BIAS_EN, 0);       /* SND_SOC_BIAS_OFF */
}

const CODEC_DRV g_DrvDa7213 = { "DA7213", "DLGS7213\0DLGS7212\0", CODEC_HEADSET, 1, 1,
                                Probe, Init, Stream, Volume, Shutdown };
