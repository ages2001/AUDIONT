#include "codec.h"
/*
 * AUDIONT - Dialog DA7219 headset codec (Intel Chromebooks: GLK/CML/JSL/KBL, ACPI "DLGS7219").
 * Reference: Linux sound/soc/codecs/da7219.c, da7219-aad.c, intel/boards/sof_da7219.c.
 *
 * Clocking: the DA7219 PLL only takes MCLK as its reference (DA7219_PLL_CTRL INDIV/MODE).  BCLK cannot
 * drive it; "SRM" mode tracks WCLK but still needs MCLK.  Like sof_da7219.c we run the PLL in SRM mode
 * (98.304 MHz) while a stream is up and put it in bypass otherwise; with MCLK = 12.288/24.576 MHz the
 * PLL is bypassed for good (DA7219_SYSCLK_MCLK).  Without MCLK (c->Mclk == 0) Init fails.
 *
 * No jack detection: AAD (accessory detect) stays disabled and the chip is put in the state
 * da7219_aad_btn_det_work() leaves after a 4-pole headset was found: HP_L/R_AMP_OE driven, ground switch
 * (reg 0xFB bit 0) open, MICBIAS forced on.
 *
 * c->Mic: 0 = headset mic on MIC, differential input (Linux default "dlg,mic-amp-in-sel" = diff),
 *         1 = single-ended MIC_P, 2 = single-ended MIC_N.
 */

#define R_MIC_1_SELECT          0x10
#define R_CIF_CTRL              0x13
#define R_SR                    0x17
#define R_PLL_CTRL              0x20
#define R_PLL_FRAC_TOP          0x22
#define R_PLL_FRAC_BOT          0x23
#define R_PLL_INTEGER           0x24
#define R_PLL_SRM_STS           0x25
#define R_DIG_ROUTING_DAI       0x2A
#define R_DAI_CLK_MODE          0x2B
#define R_DAI_CTRL              0x2C
#define R_DAI_TDM_CTRL          0x2D
#define R_DIG_ROUTING_DAC       0x2E
#define R_DAI_OFFSET_LOWER      0x30
#define R_DAI_OFFSET_UPPER      0x31
#define R_REFERENCES            0x32
#define R_MIXIN_L_SELECT        0x33
#define R_MIXIN_L_GAIN          0x34
#define R_ADC_L_GAIN            0x36
#define R_MIC_1_GAIN            0x39
#define R_DAC_L_GAIN            0x45
#define R_DAC_R_GAIN            0x46
#define R_CP_CTRL               0x47
#define R_HP_L_GAIN             0x48
#define R_HP_R_GAIN             0x49
#define R_MIXOUT_L_SELECT       0x4B
#define R_MIXOUT_R_SELECT       0x4C
#define R_SYSTEM_MODES_INPUT    0x50
#define R_SYSTEM_MODES_OUTPUT   0x51
#define R_MICBIAS_CTRL          0x62
#define R_MIC_1_CTRL            0x63
#define R_MIXIN_L_CTRL          0x65
#define R_ADC_L_CTRL            0x67
#define R_DAC_L_CTRL            0x69
#define R_DAC_R_CTRL            0x6A
#define R_HP_L_CTRL             0x6B
#define R_HP_R_CTRL             0x6C
#define R_MIXOUT_L_CTRL         0x6E
#define R_MIXOUT_R_CTRL         0x6F
#define R_CHIP_ID1              0x81
#define R_CHIP_ID2              0x82
#define R_CHIP_REVISION         0x83
#define R_IO_CTRL               0x91
#define R_GAIN_RAMP_CTRL        0x92
#define R_PC_COUNT              0x94
#define R_TONE_GEN_CYCLES       0xB6
#define R_ACCDET_STATUS_A       0xC0
#define R_ACCDET_IRQ_MASK_A     0xC4
#define R_ACCDET_IRQ_MASK_B     0xC5
#define R_ACCDET_CONFIG_1       0xC6
#define R_SYSTEM_STATUS         0xE0
#define R_GND_SWITCH            0xFB    /* undocumented: bit 0 = AAD ground switch (da7219-aad.c) */
#define R_SYSTEM_ACTIVE         0xFD

/* common bits of the *_CTRL registers */
#define AMP_EN                  0x80    /* *_AMP_EN / *_EN */
#define MUTE_EN                 0x40    /* *_MUTE_EN */
#define RAMP_EN                 0x20    /* *_RAMP_EN */
#define HP_AMP_OE               0x08    /* HP_x_AMP_OE */
#define HP_MIN_GAIN_EN          0x04    /* HP_x_AMP_MIN_GAIN_EN */
#define MIXIN_MIX_EN            0x08    /* MIXIN_L_MIX_EN ("Mixer In Supply") */

#define PLL_INDIV_MASK          (7 << 2)
#define PLL_MCLK_SQR_EN         (1 << 5)
#define PLL_MODE_MASK           (3 << 6)
#define PLL_MODE_BYPASS         (0 << 6)
#define PLL_MODE_SRM            (2 << 6)
#define PLL_SRM_STS_SRM_LOCK    0x80
#define PLL_FREQ_OUT_98304      98304000

#define DAI_EN                  0x80
#define DAI_OE                  0x40
#define DAI_TDM_MODE_EN         0x80
#define BIAS_EN                 0x08
#define MICBIAS1_EN             0x08
#define PC_FREERUN              0x01
#define GAIN_RAMP_NOMINAL       0x01
#define ACCDET_EN               0x01
#define BUTTON_CONFIG_MASK      (7 << 1)
#define MICBIAS_UP_STS          0x08
#define CIF_REG_SOFT_RESET      0x80

#define SETTLING_DELAY          40      /* DA7219_SETTLING_DELAY */
#define MIN_GAIN_DELAY          30      /* DA7219_MIN_GAIN_DELAY */
#define MICBIAS_2_2V            3       /* DA7219_MICBIAS_2_2V: Linux default "dlg,micbias-lvl" */
#define DAC_GAIN_0DB            0x6F    /* DA7219_DAC_DIGITAL_GAIN_0DB, 0.75 dB steps */
#define DAC_GAIN_MIN            0x08    /* -77.25 dB (0x00..0x07 = mute) */

/* Priv[] */
#define P_BYPASS    0   /* MCLK is 12.288/24.576 MHz: PLL bypassed */
#define P_INDIV     1   /* PLL_CTRL INDIV bits for c->Mclk */
#define P_MICEVT    2   /* micbias just enabled: first capture waits mic_pga_delay */
#define P_PGADELAY  3   /* mic_pga_delay (ms) */
#define P_RAMP      4   /* saved GAIN_RAMP_CTRL */
#define P_MUTE      5   /* DAC mute requested by Volume */

/* (a * b) / c with a 64-bit intermediate, by shift/subtract (no libgcc 64-bit division) */
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

/* DA7219_SR codes (da7219_set_sr) */
static ULONG SrCode(ULONG rate)
{
    static const ULONG tab[][2] = { { 8000, 0x01 }, { 11025, 0x02 }, { 12000, 0x03 }, { 16000, 0x05 },
        { 22050, 0x06 }, { 24000, 0x07 }, { 32000, 0x09 }, { 44100, 0x0A }, { 48000, 0x0B },
        { 88200, 0x0E }, { 96000, 0x0F } };
    ULONG i;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (tab[i][0] == rate) return tab[i][1];
    return 0x0B;
}

static BOOLEAN Probe(SST_CODEC *c)
{
    ULONG id1, id2;
    if (!CRd(c, R_CHIP_ID1, &id1) || !CRd(c, R_CHIP_ID2, &id2)) return FALSE;
    return id1 == 0x23 && id2 == 0x93;
}

/* DA7219_PLL_FREQ_OUT_98304 for the 48 kHz family, DA7219_PLL_FREQ_OUT_90316 for 44.1 kHz */
static ULONG PllOut(SST_CODEC *c)
{
    return (c->Rate % 11025) == 0 ? 90316800 : PLL_FREQ_OUT_98304;
}

/* da7219_set_pll(DA7219_SYSCLK_MCLK): PLL bypass (also the idle state when SRM is used) */
static void PllBypass(SST_CODEC *c)
{
    CUpd(c, R_PLL_CTRL, PLL_INDIV_MASK | PLL_MODE_MASK, c->Priv[P_INDIV] | PLL_MODE_BYPASS);
}

/* "Platform Clock" PRE_PMU of sof_da7219.c: da7219_set_pll(DA7219_SYSCLK_PLL_SRM, 98.304 MHz) */
static void PllSrm(SST_CODEC *c)
{
    static const ULONG div[] = { 1, 2, 4, 8, 16 };      /* DA7219_PLL_INDIV_*_VAL */
    ULONG ref, integ, frac;
    if (c->Priv[P_BYPASS]) return;
    ref = c->Mclk / div[c->Priv[P_INDIV] >> 2];
    integ = PllOut(c) / ref;
    frac = MulDiv(PllOut(c) % ref, 8192, ref);                /* 13-bit fraction */
    CWr(c, R_PLL_FRAC_TOP, (frac >> 8) & 0xFF);
    CWr(c, R_PLL_FRAC_BOT, frac & 0xFF);
    CWr(c, R_PLL_INTEGER, integ);
    CUpd(c, R_PLL_CTRL, PLL_INDIV_MASK | PLL_MODE_MASK, c->Priv[P_INDIV] | PLL_MODE_SRM);
}

/* "DAI" supply: da7219_dai_event(PRE_PMU) then DAI_EN */
static void DaiOn(SST_CODEC *c)
{
    ULONG v, i;
    CUpd(c, R_PC_COUNT, PC_FREERUN, 0);                 /* PC synchronised to DAI */
    if ((CGet(c, R_PLL_CTRL) & PLL_MODE_MASK) == PLL_MODE_SRM) {
        for (i = 0; i < 8; i++) {                       /* DA7219_SRM_CHECK_RETRIES */
            if (CRd(c, R_PLL_SRM_STS, &v) && (v & PLL_SRM_STS_SRM_LOCK)) break;
            CDelay(50);
        }
        if (i == 8) LOGV("DA7219: SRM not locked yet (WCLK not running?)\n");
    }
    CUpd(c, R_DAI_CTRL, DAI_EN, DAI_EN);
}

static void DaiOff(SST_CODEC *c)
{
    CUpd(c, R_DAI_CTRL, DAI_EN, 0);
    CUpd(c, R_PC_COUNT, PC_FREERUN, PC_FREERUN);        /* POST_PMD: PC free-running */
}

/* SND_SOC_DAPM_PRE / POST "Gain Ramp": nominal ramp rate during the sequence */
static void RampPre(SST_CODEC *c)
{
    c->Priv[P_RAMP] = CGet(c, R_GAIN_RAMP_CTRL) & 0xFF;
    CWr(c, R_GAIN_RAMP_CTRL, GAIN_RAMP_NOMINAL);
}

static void RampPost(SST_CODEC *c)
{
    CWr(c, R_GAIN_RAMP_CTRL, c->Priv[P_RAMP]);
}

static void Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? c->PlayOn : c->RecOn;
    RampPre(c);
    if (on) {
        if (!other) { PllSrm(c); DaiOn(c); }            /* supplies: Platform Clock (reg -1), DAI */
        if (!rec) {
            CUpd(c, R_CP_CTRL, AMP_EN, AMP_EN);         /* "Charge Pump" (CP_EN), subseq 0 */
            CDelay(SETTLING_DELAY);
            CUpd(c, R_HP_L_CTRL, AMP_EN, AMP_EN);       /* "Headphone Left/Right PGA", subseq 1 */
            CDelay(SETTLING_DELAY);
            CUpd(c, R_HP_R_CTRL, AMP_EN, AMP_EN);
            CDelay(SETTLING_DELAY);
            CUpd(c, R_DAC_L_CTRL, AMP_EN, AMP_EN);      /* DACL / DACR + settling */
            CDelay(SETTLING_DELAY);
            CUpd(c, R_DAC_R_CTRL, AMP_EN, AMP_EN);
            CDelay(SETTLING_DELAY);
            CUpd(c, R_MIXOUT_L_CTRL, AMP_EN, AMP_EN);   /* Mixout PGA, POST_PMU: remove HP min gain */
            CUpd(c, R_HP_L_CTRL, HP_MIN_GAIN_EN, 0);
            CUpd(c, R_MIXOUT_R_CTRL, AMP_EN, AMP_EN);
            CUpd(c, R_HP_R_CTRL, HP_MIN_GAIN_EN, 0);
        } else {
            CUpd(c, R_MIXIN_L_CTRL, MIXIN_MIX_EN, MIXIN_MIX_EN);  /* "Mixer In Supply" */
            CUpd(c, R_DAI_TDM_CTRL, DAI_OE, DAI_OE);              /* AIF_OUT "DAIOUT" */
            CUpd(c, R_MIC_1_CTRL, AMP_EN, AMP_EN);                /* "Mic PGA" */
            if (c->Priv[P_MICEVT]) {                    /* da7219_mic_pga_event: DC offset settling */
                c->Priv[P_MICEVT] = 0;
                CDelay(c->Priv[P_PGADELAY]);
            }
            CUpd(c, R_MIXIN_L_CTRL, AMP_EN, AMP_EN);    /* "Mixin PGA" + settling */
            CDelay(SETTLING_DELAY);
            CUpd(c, R_ADC_L_CTRL, AMP_EN, AMP_EN);      /* "ADC" */
        }
    } else {
        if (!rec) {
            CUpd(c, R_HP_L_CTRL, HP_MIN_GAIN_EN, HP_MIN_GAIN_EN);  /* Mixout PRE_PMD: HP min gain */
            CDelay(MIN_GAIN_DELAY);
            CUpd(c, R_MIXOUT_L_CTRL, AMP_EN, 0);
            CUpd(c, R_HP_R_CTRL, HP_MIN_GAIN_EN, HP_MIN_GAIN_EN);
            CDelay(MIN_GAIN_DELAY);
            CUpd(c, R_MIXOUT_R_CTRL, AMP_EN, 0);
            CUpd(c, R_DAC_L_CTRL, AMP_EN, 0);
            CDelay(SETTLING_DELAY);
            CUpd(c, R_DAC_R_CTRL, AMP_EN, 0);
            CDelay(SETTLING_DELAY);
            CUpd(c, R_HP_L_CTRL, AMP_EN, 0);            /* supplies, subseq 1 first */
            CDelay(SETTLING_DELAY);
            CUpd(c, R_HP_R_CTRL, AMP_EN, 0);
            CDelay(SETTLING_DELAY);
            if (!other) { PllBypass(c); DaiOff(c); }    /* subseq 0 in register order: clock, DAI, CP */
            CUpd(c, R_CP_CTRL, AMP_EN, 0);
        } else {
            CUpd(c, R_ADC_L_CTRL, AMP_EN, 0);
            CUpd(c, R_MIC_1_CTRL, AMP_EN, 0);
            CUpd(c, R_MIXIN_L_CTRL, AMP_EN, 0);
            CUpd(c, R_DAI_TDM_CTRL, DAI_OE, 0);
            if (!other) { PllBypass(c); DaiOff(c); }
            CUpd(c, R_MIXIN_L_CTRL, MIXIN_MIX_EN, 0);
        }
    }
    RampPost(c);
}

/* DAC digital gain 0x6F = 0 dB in 0.75 dB steps down to 0x08 (-77.25 dB); mute = DAC_x_MUTE_EN (ramped) */
static void Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    CWr(c, R_DAC_L_GAIN, DAC_GAIN_MIN + CVolSteps(left, DAC_GAIN_0DB - DAC_GAIN_MIN));
    CWr(c, R_DAC_R_GAIN, DAC_GAIN_MIN + CVolSteps(right, DAC_GAIN_0DB - DAC_GAIN_MIN));
    CUpd(c, R_DAC_L_CTRL, MUTE_EN, mute ? MUTE_EN : 0);
    CUpd(c, R_DAC_R_CTRL, MUTE_EN, mute ? MUTE_EN : 0);
    c->Priv[P_MUTE] = mute;
}

static BOOLEAN Init(SST_CODEC *c)
{
    ULONG e0 = c->Errors, v, rev, i, fmt, wl;
    if (!c->Mclk) {
        LOGE("DA7219: no MCLK on SSP%u - the DA7219 PLL cannot use BCLK as reference\n", c->Port);
        return FALSE;
    }
    if (c->Mclk < 2000000 || c->Mclk > 54000000) {
        LOGE("DA7219: MCLK %u out of the 2..54 MHz PLL input range\n", c->Mclk);
        return FALSE;
    }
    c->PlayOn = c->RecOn = FALSE;

    /* da7219_probe(): disable audio paths if still active from a previous start */
    if (!CRd(c, R_SYSTEM_ACTIVE, &v)) return FALSE;
    if (v) {
        CWr(c, R_GAIN_RAMP_CTRL, GAIN_RAMP_NOMINAL);
        CWr(c, R_SYSTEM_MODES_INPUT, 0x00);
        CWr(c, R_SYSTEM_MODES_OUTPUT, 0x01);
        for (i = 0; i < 6; i++) {                       /* DA7219_SYS_STAT_CHECK_RETRIES / _DELAY */
            if (!CRd(c, R_SYSTEM_STATUS, &v) || !v) break;
            CDelay(50);
        }
    }
    /* soft reset: AAD off, CIF register soft reset, toggle SYSTEM_ACTIVE */
    CUpd(c, R_ACCDET_CONFIG_1, ACCDET_EN, 0);
    v = CGet(c, R_CIF_CTRL);
    CWr(c, R_CIF_CTRL, (v | CIF_REG_SOFT_RESET) & 0xFF);
    CWr(c, R_SYSTEM_ACTIVE, 0);
    CWr(c, R_SYSTEM_ACTIVE, 1);
    if (c->Errors != e0) return FALSE;

    CWr(c, R_IO_CTRL, 0);           /* VDDIO unknown: Linux keeps the 2.5-3.6 V range (dummy regulator) */
    if (!CRd(c, R_CHIP_REVISION, &rev)) return FALSE;
    if ((rev & 0x0F) == 0) CWr(c, R_REFERENCES, 0x08);  /* da7219_rev_aa_patch */

    /* da7219_handle_pdata(): micbias level, mic input type */
    CWr(c, R_MICBIAS_CTRL, MICBIAS_2_2V);
    c->Priv[P_PGADELAY] = 100 + MICBIAS_2_2V * 40;     /* DA7219_MIC_PGA_BASE/OFFSET_DELAY */
    CWr(c, R_MIC_1_SELECT, c->Mic <= 2 ? c->Mic : 0);

    /* sysclk: DA7219_CLKSRC_MCLK (no squarer); PLL input divider for the MCLK range */
    CUpd(c, R_PLL_CTRL, PLL_MCLK_SQR_EN, 0);
    v = c->Mclk <= 4500000 ? 0 : c->Mclk <= 9000000 ? 1 : c->Mclk <= 18000000 ? 2 : c->Mclk <= 36000000 ? 3 : 4;
    c->Priv[P_INDIV] = v << 2;
    c->Priv[P_BYPASS] = (c->Mclk == 12288000 || c->Mclk == 24576000) && (c->Rate % 11025) != 0;
    PllBypass(c);

    CUpd(c, R_PC_COUNT, PC_FREERUN, PC_FREERUN);
    CUpd(c, R_MIXIN_L_CTRL, RAMP_EN, RAMP_EN);          /* default gain ramping */
    CUpd(c, R_ADC_L_CTRL, RAMP_EN, RAMP_EN);
    CUpd(c, R_DAC_L_CTRL, RAMP_EN, RAMP_EN);
    CUpd(c, R_DAC_R_CTRL, RAMP_EN, RAMP_EN);
    CUpd(c, R_HP_L_CTRL, RAMP_EN, RAMP_EN);
    CUpd(c, R_HP_R_CTRL, RAMP_EN, RAMP_EN);
    CUpd(c, R_HP_L_CTRL, HP_MIN_GAIN_EN, HP_MIN_GAIN_EN);  /* min gain on HP during sequencing */
    CUpd(c, R_HP_R_CTRL, HP_MIN_GAIN_EN, HP_MIN_GAIN_EN);
    CWr(c, R_TONE_GEN_CYCLES, 0x07);

    /* da7219_aad_init() without IRQ: button detection off, AAD IRQs masked, ACCDET stays disabled */
    CUpd(c, R_ACCDET_CONFIG_1, BUTTON_CONFIG_MASK | ACCDET_EN, 0);
    CWr(c, R_ACCDET_IRQ_MASK_A, 0xFF);
    CWr(c, R_ACCDET_IRQ_MASK_B, 0xFF);

    /* SND_SOC_BIAS_STANDBY (idle_bias_on): master bias */
    CUpd(c, R_REFERENCES, BIAS_EN, BIAS_EN);

    /* DAI: consumer (DAI_CLK_EN 0), I2S, normal polarity, word length, 2 channels */
    CUpd(c, R_DAI_CLK_MODE, 0x80 | 0x08 | 0x04, 0);     /* DAI_CLK_EN, WCLK_POL, CLK_POL */
    wl = c->Valid >= 24 ? (2 << 2) : (0 << 2);          /* DAI_WORD_LENGTH_S24_LE / S16_LE */
    fmt = 0 | wl | (2 << 4);                            /* DAI_FORMAT_I2S, DAI_CH_NUM 2 */
    CUpd(c, R_DAI_CTRL, 0x3F, fmt);
    if (c->Slots > 2) {                                 /* da7219_set_dai_tdm_slot(tx_mask 0x3, offset 0) */
        CWr(c, R_DAI_OFFSET_LOWER, 0);
        CWr(c, R_DAI_OFFSET_UPPER, 0);
        CUpd(c, R_DAI_TDM_CTRL, 0x03 | DAI_TDM_MODE_EN, 0x03 | DAI_TDM_MODE_EN);
    } else
        CUpd(c, R_DAI_TDM_CTRL, 0x03 | DAI_TDM_MODE_EN, 0);
    CUpd(c, R_DAI_TDM_CTRL, DAI_OE, 0);
    CWr(c, R_SR, SrCode(c->Rate));

    /* routing: DACL <- DAIL, DACR <- DAIR; DAI L/R <- ADC (mono mic on both channels) */
    CWr(c, R_DIG_ROUTING_DAC, 0x32);
    CWr(c, R_DIG_ROUTING_DAI, 0x00);
    CWr(c, R_MIXOUT_L_SELECT, 0x01);                    /* "Mixer Out FilterL DACL Switch" */
    CWr(c, R_MIXOUT_R_SELECT, 0x01);
    CWr(c, R_MIXIN_L_SELECT, 0x01);                     /* "Mixer In Mic Switch" */

    /* gains: HP 0 dB, mic PGA +24 dB, mixin 0 dB, ADC 0 dB; unmute the analog stages */
    CWr(c, R_HP_L_GAIN, 0x39);
    CWr(c, R_HP_R_GAIN, 0x39);
    CWr(c, R_MIC_1_GAIN, 0x05);                         /* -6 dB + 5 * 6 dB = +24 dB */
    CWr(c, R_MIXIN_L_GAIN, 0x03);                       /* -4.5 dB + 3 * 1.5 dB = 0 dB */
    CWr(c, R_ADC_L_GAIN, 0x6F);                         /* 0 dB */
    CUpd(c, R_MIC_1_CTRL, MUTE_EN, 0);
    CUpd(c, R_MIXIN_L_CTRL, MUTE_EN, 0);
    CUpd(c, R_ADC_L_CTRL, MUTE_EN, 0);
    CUpd(c, R_HP_L_CTRL, MUTE_EN, 0);
    CUpd(c, R_HP_R_CTRL, MUTE_EN, 0);
    Volume(c, 65535, 65535, FALSE);

    /* 4-pole headset state (da7219_aad_btn_det_work): ground switch open, drive HP, mic bias up */
    CUpd(c, R_GND_SWITCH, 0x01, 0);
    CUpd(c, R_HP_L_CTRL, HP_AMP_OE, HP_AMP_OE);
    CUpd(c, R_HP_R_CTRL, HP_AMP_OE, HP_AMP_OE);
    CUpd(c, R_MICBIAS_CTRL, MICBIAS1_EN, MICBIAS1_EN);
    for (i = 0; i < 5; i++) {                           /* DA7219_AAD_MICBIAS_CHK_RETRIES/_DELAY */
        if (CRd(c, R_ACCDET_STATUS_A, &v) && (v & MICBIAS_UP_STS)) break;
        CDelay(10);
    }
    c->Priv[P_MICEVT] = 1;

    if (c->Errors != e0) return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("DA7219: rev %u.%u, MCLK %u Hz, %s, mic %s, %u slots x %u bits\n", (rev >> 4) & 0xF, rev & 0xF,
         c->Mclk, c->Priv[P_BYPASS] ? "PLL bypass" : "PLL SRM 98.304 MHz",
         c->Mic == 1 ? "SE P" : c->Mic == 2 ? "SE N" : "diff", c->Slots, c->Slot);
    return TRUE;
}

static void Shutdown(SST_CODEC *c)
{
    if (c->PlayOn) Stream(c, FALSE, FALSE);
    c->PlayOn = FALSE;
    if (c->RecOn) Stream(c, TRUE, FALSE);
    c->RecOn = FALSE;
    /* da7219_aad_suspend + jack removal state: AAD off, ground switch open, HP undriven, micbias off */
    CUpd(c, R_ACCDET_CONFIG_1, ACCDET_EN | BUTTON_CONFIG_MASK, 0);
    CUpd(c, R_GND_SWITCH, 0x01, 0);
    CUpd(c, R_HP_R_CTRL, HP_AMP_OE, 0);
    CUpd(c, R_HP_L_CTRL, HP_AMP_OE, 0);
    CUpd(c, R_MICBIAS_CTRL, MICBIAS1_EN, 0);
    PllBypass(c);
    CUpd(c, R_REFERENCES, BIAS_EN, 0);                  /* SND_SOC_BIAS_OFF */
}

const CODEC_DRV g_DrvDa7219 = { "DA7219", "DLGS7219\0", CODEC_HEADSET, 1, 1,
                                Probe, Init, Stream, Volume, Shutdown };
