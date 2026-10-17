#include "codec.h"
/*
 * AUDIONT - Nuvoton NAU88L25 / NAU8825 headset codec (Intel Chromebooks SKL/KBL/ADL, ACPI "10508825").
 * Reference: Linux sound/soc/codecs/nau8825.c / nau8825.h, intel/boards/sof_nau8825.c.
 * 16-bit register addresses, 16-bit values.
 *
 * Clocking: like sof_nau8825.c the FLL runs from BCLK (NAU8825_CLK_FLL_BLK) with Fout = 256 * Fs;
 * the dividers are computed with nau8825_calc_fll_param().  If BCLK is unusable the FLL falls back to
 * MCLK, then to the frame clock (NAU8825_CLK_FLL_FS).  The system clock is dropped (NAU8825_CLK_DIS)
 * after the last stream stops ("System Clock" POST_PMD with no jack reported).
 *
 * No jack detection: the chip is left in the state nau8825_jack_insert() programs for a CTIA headset
 * ("micgnd2" case): MICGND1 grounded / MICGND2 open, 2 kOhm MICBIAS resistor on JKSLV (sleeve), SAR
 * input on JKSLV, HSD auto mode off, MICBIAS forced on; HPOL/HPOR stay grounded until playback ungrounds
 * them ("HPOL/HPOR Pulldown" widgets).  The headset mic is the only input (c->Mic is ignored).
 * Jack-detect pin config uses the values Chromebook firmware passes in _DSD (jkdet input with pull-up,
 * active low) rather than the Linux no-property default (JKDET as output).
 */

#define R_RESET                 0x00
#define R_ENA_CTRL              0x01
#define R_IIC_ADDR_SET          0x02
#define R_CLK_DIVIDER           0x03
#define R_FLL1                  0x04
#define R_FLL2                  0x05
#define R_FLL3                  0x06
#define R_FLL4                  0x07
#define R_FLL5                  0x08
#define R_FLL6                  0x09
#define R_FLL_VCO_RSV           0x0A
#define R_HSD_CTRL              0x0C
#define R_JACK_DET_CTRL         0x0D
#define R_INTERRUPT_MASK        0x0F
#define R_INTERRUPT_DIS_CTRL    0x12
#define R_SAR_CTRL              0x13
#define R_KEYDET_CTRL           0x14
#define R_VDET_THRESHOLD_1      0x15
#define R_VDET_THRESHOLD_2      0x16
#define R_VDET_THRESHOLD_3      0x17
#define R_VDET_THRESHOLD_4      0x18
#define R_GPIO12_CTRL           0x1A
#define R_TDM_CTRL              0x1B
#define R_I2S_PCM_CTRL1         0x1C
#define R_I2S_PCM_CTRL2         0x1D
#define R_LEFT_TIME_SLOT        0x1E
#define R_ADC_RATE              0x2B
#define R_DAC_CTRL1             0x2C
#define R_DAC_DGAIN_CTRL        0x2F
#define R_ADC_DGAIN_CTRL        0x30
#define R_HSVOL_CTRL            0x32
#define R_DACL_CTRL             0x33
#define R_DACR_CTRL             0x34
#define R_CLASSG_CTRL           0x50
#define R_OPT_EFUSE_CTRL        0x51
#define R_I2C_DEVICE_ID         0x58
#define R_FLL2_LOWER            0x5A
#define R_FLL2_UPPER            0x5B
#define R_BIAS_ADJ              0x66
#define R_ANALOG_CONTROL_2      0x6A
#define R_ANALOG_ADC_2          0x72
#define R_RDAC                  0x73
#define R_MIC_BIAS              0x74
#define R_BOOST                 0x76
#define R_FEPGA                 0x77
#define R_POWER_UP_CONTROL      0x7F
#define R_CHARGE_PUMP           0x80

/* ENA_CTRL */
#define ENABLE_DACR             (1 << 10)
#define ENABLE_DACL             (1 << 9)
#define ENABLE_ADC              (1 << 8)
#define ENABLE_ADC_CLK          (1 << 7)
#define ENABLE_DAC_CLK          (1 << 6)
/* CLK_DIVIDER */
#define CLK_SRC_VCO             (1 << 15)
#define CLK_ADC_SRC_SFT         6
#define CLK_DAC_SRC_SFT         4
#define CLK_MCLK_SRC_MASK       0x000F
/* FLL */
#define ICTRL_LATCH_MASK        (7 << 10)
#define FLL_RATIO_MASK          0x7F
#define GAIN_ERR_MASK           (0xF << 12)
#define FLL_INTEGER_MASK        0x3FF
#define FLL_CLK_SRC_MASK        (3 << 10)
#define FLL_CLK_SRC_MCLK        (0 << 10)
#define FLL_CLK_SRC_BLK         (2 << 10)
#define FLL_CLK_SRC_FS          (3 << 10)
#define FLL_REF_DIV_MASK        (3 << 10)
#define FLL_PDB_DAC_EN          (1 << 15)
#define FLL_LOOP_FTR_EN         (1 << 14)
#define FLL_CLK_SW_MASK         (1 << 13)
#define FLL_FTR_SW_MASK         (1 << 12)
#define FLL_FTR_SW_ACCU         (1 << 12)
#define DCO_EN                  (1 << 15)
#define SDM_EN                  (1 << 14)
#define CUTOFF500               (1 << 13)
/* HSD_CTRL: bit 3 MICGND1 to GND, bit 2 MICGND2 to GND, bits 1/0 HPOR/HPOL 16 Ohm pull-downs */
#define HSD_AUTO_MODE           (1 << 6)
/* MIC_BIAS */
#define MICBIAS_JKSLV           (1 << 14)
#define MICBIAS_JKR2            (1 << 12)
#define MICBIAS_POWERUP         (1 << 8)
#define MICBIAS_VOLTAGE_MASK    0x7
/* SAR_CTRL */
#define SAR_INPUT_JKSLV         (1 << 11)
/* BOOST */
#define PRECHARGE_DIS           (1 << 13)
#define GLOBAL_BIAS_EN          (1 << 12)
#define DISCHRG_EN              (1 << 11)
#define HP_BOOST_DIS            (1 << 9)
#define HP_BOOST_G_DIS          (1 << 8)
#define SHORT_SHUTDOWN_EN       (1 << 6)
/* BIAS_ADJ */
#define BIAS_TESTDAC_EN         (3 << 8)
#define BIAS_VMID               (1 << 6)
#define BIAS_VMID_SEL_MASK      (3 << 4)
/* CHARGE_PUMP */
#define ADCOUT_DS_MASK          (1 << 12)
#define JAMNODCLOW              (1 << 10)
#define POWER_DOWN_DACR         (1 << 9)
#define POWER_DOWN_DACL         (1 << 8)
#define CHARGE_PUMP_EN          (1 << 5)
/* I2S */
#define I2S_TRISTATE            (1 << 15)
#define I2S_PCM_TS_EN           (1 << 10)
#define I2S_MS_MASTER           (1 << 3)
#define TDM_MODE                (1 << 15)
#define TDM_OFFSET_EN           (1 << 14)
#define DIS_FS_SHORT_DET        (1 << 13)
/* FEPGA */
#define ACDC_CTRL_MASK          (3 << 14)
#define ACDC_VREF_MICP_MICN     (3 << 14)

#define NAU_FREF_MAX            13500000
#define NAU_FVCO_MAX            124000000
#define NAU_FVCO_MIN            90000000
#define DAC_VOL_0DB             0xCF        /* DACL/DACR_CTRL[7:0]: 0.5 dB steps, 0 = mute */
#define FEPGA_GAIN_24DB         25          /* POWER_UP_CONTROL[13:8]: -1 dB + n dB */
#define ADC_DELAY_MS            125         /* "nuvoton,adc-delay-ms" default */

/* Priv[] */
#define P_SWID      0   /* NAU8825_SOFTWARE_ID: 0 NAU8825, 1 NAU8825C */
#define P_DACL      1   /* last volume register values (low byte) */
#define P_DACR      2

static ULONGLONG Div64(ULONGLONG n, ULONG d)
{
    ULONGLONG q = 0, r = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    return q;
}

/* nau8825_calc_fll_param() + nau8825_fll_apply() + set_pll tail; FALSE if fin is unsupported */
static BOOLEAN Fll(SST_CODEC *c, ULONG fin, ULONG src)
{
    static const ULONG pre[4] = { 1, 2, 4, 8 };
    static const ULONG ratio[7][2] = { { 512000, 0x01 }, { 256000, 0x02 }, { 128000, 0x04 }, { 64000, 0x08 },
                                      { 32000, 0x10 }, { 8000, 0x20 }, { 4000, 0x40 } };
    static const ULONG scal[13][2] = { { 1, 0x0 }, { 2, 0x2 }, { 4, 0x3 }, { 8, 0x4 }, { 16, 0x5 }, { 32, 0x6 },
                                      { 3, 0x7 }, { 6, 0xa }, { 12, 0xb }, { 24, 0xc }, { 48, 0xd },
                                      { 96, 0xe }, { 5, 0xf } };
    ULONG fref = 0, i, rdiv, rat, msrc = 0, nfrac = c->Priv[P_SWID] ? 24 : 16, integ, frac;
    ULONGLONG fvco, fmax = 0, x;
    if (!fin) return FALSE;
    for (i = 0; i < 4; i++) { fref = fin / pre[i]; if (fref <= NAU_FREF_MAX) break; }
    if (i == 4) return FALSE;
    rdiv = i;
    for (i = 0; i < 7; i++) if (fref >= ratio[i][0]) break;
    if (i == 7) return FALSE;
    rat = ratio[i][1];
    for (i = 0; i < 13; i++) {
        fvco = (ULONGLONG)256 * c->Rate * 2 * scal[i][0];
        if (fvco > NAU_FVCO_MIN && fvco < NAU_FVCO_MAX && fmax < fvco) { fmax = fvco; msrc = scal[i][1]; }
    }
    if (!fmax) return FALSE;
    x = Div64(fmax << nfrac, fref * rat);
    integ = (ULONG)(x >> nfrac) & 0x3FF;
    frac = (ULONG)x & (nfrac == 16 ? 0xFFFF : 0xFFFFFF);

    /* nau8825_configure_sysclk(FLL_BLK / FLL_FS: gain error 0xf, FLL_MCLK: 0) */
    CUpd(c, R_FLL3, FLL_CLK_SRC_MASK | GAIN_ERR_MASK, src | (src == FLL_CLK_SRC_MCLK ? 0 : 0xF << 12));
    CUpd(c, R_CLK_DIVIDER, CLK_SRC_VCO | CLK_MCLK_SRC_MASK, msrc);
    CUpd(c, R_FLL1, FLL_RATIO_MASK | ICTRL_LATCH_MASK, rat | (6 << 10));
    if (nfrac == 16)
        CWr(c, R_FLL2, frac);
    else {
        CWr(c, R_FLL2_LOWER, frac & 0xFFFF);
        CWr(c, R_FLL2_UPPER, (frac >> 16) & 0xFF);
    }
    CUpd(c, R_FLL3, FLL_INTEGER_MASK, integ);
    CUpd(c, R_FLL4, FLL_REF_DIV_MASK, rdiv << 10);
    CUpd(c, R_FLL5, FLL_CLK_SW_MASK, 0);                /* FLL_CLK_SW_REF */
    CUpd(c, R_FLL6, DCO_EN, 0);                         /* not free-running */
    if (frac) {
        CUpd(c, R_FLL5, FLL_PDB_DAC_EN | FLL_LOOP_FTR_EN | FLL_FTR_SW_MASK, FLL_PDB_DAC_EN | FLL_LOOP_FTR_EN);
        CUpd(c, R_FLL6, SDM_EN | CUTOFF500, SDM_EN | CUTOFF500);
    } else {
        CUpd(c, R_FLL5, FLL_PDB_DAC_EN | FLL_LOOP_FTR_EN | FLL_FTR_SW_MASK, FLL_FTR_SW_ACCU);
        CUpd(c, R_FLL6, SDM_EN | CUTOFF500, 0);
    }
    CDelay(2);
    CUpd(c, R_CLK_DIVIDER, CLK_SRC_VCO, CLK_SRC_VCO);
    LOGV("NAU8825: FLL in %u src %x: ref_div %u ratio %x int %u frac %x mclk_src %x\n",
         fin, src >> 10, rdiv, rat, integ, frac, msrc);
    return TRUE;
}

static void ClockOn(SST_CODEC *c)
{
    if (Fll(c, c->Bclk, FLL_CLK_SRC_BLK)) return;
    if (c->Mclk && Fll(c, c->Mclk, FLL_CLK_SRC_MCLK)) return;
    if (!Fll(c, c->Rate, FLL_CLK_SRC_FS)) LOGE("NAU8825: no usable FLL reference (BCLK %u)\n", c->Bclk);
}

/* nau8825_configure_sysclk(NAU8825_CLK_DIS) = nau8825_configure_mclk_as_sysclk() */
static void ClockOff(SST_CODEC *c)
{
    CUpd(c, R_CLK_DIVIDER, CLK_SRC_VCO, 0);
    CUpd(c, R_FLL6, DCO_EN, 0);
    CUpd(c, R_FLL1, ICTRL_LATCH_MASK, 0);
}

static BOOLEAN Probe(SST_CODEC *c)
{
    ULONG id;
    if (!CRd(c, R_I2C_DEVICE_ID, &id)) return FALSE;
    return (id & 3) <= 1;                               /* NAU8825_SOFTWARE_ID_NAU8825 / _NAU8825C */
}

/* "Output DACL/R" event: TESTDAC off/on, DAC power-down bits (polarity differs on the C revision) */
static void OutputDac(SST_CODEC *c, BOOLEAN on)
{
    ULONG pd = POWER_DOWN_DACR | POWER_DOWN_DACL;
    BOOLEAN c_rev = c->Priv[P_SWID] != 0;
    CUpd(c, R_BIAS_ADJ, BIAS_TESTDAC_EN, on ? 0 : BIAS_TESTDAC_EN);
    CUpd(c, R_CHARGE_PUMP, pd, (on ? c_rev : !c_rev) ? pd : 0);
}

static void Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? c->PlayOn : c->RecOn;
    if (on) {
        if (!other) ClockOn(c);                         /* machine hw_params: FLL from BCLK */
        if (!rec) {
            CUpd(c, R_CLK_DIVIDER, 3 << CLK_DAC_SRC_SFT, 2 << CLK_DAC_SRC_SFT);  /* DAC OSR 64: SRC 1/4 */
            CUpd(c, R_ENA_CTRL, ENABLE_DAC_CLK, ENABLE_DAC_CLK);                 /* "DDAC Clock" */
            CUpd(c, R_ENA_CTRL, ENABLE_DACL | ENABLE_DACR, ENABLE_DACL | ENABLE_DACR);
            CUpd(c, R_CLASSG_CTRL, 3 << 1, 3 << 1);     /* subseq 0: HP amp L/R (CLASSG L/R DAC EN) */
            CUpd(c, R_CHARGE_PUMP, CHARGE_PUMP_EN, CHARGE_PUMP_EN);  /* subseq 1: charge pump */
            CDelay(10);                                 /* nau8825_pump_event: ramp before JAMNODCLOW */
            CUpd(c, R_CHARGE_PUMP, JAMNODCLOW, JAMNODCLOW);
            CUpd(c, R_RDAC, 3 << 12, 3 << 12);          /* subseq 2: ADACL/R */
            CUpd(c, R_RDAC, 3 << 8, 3 << 8);            /* subseq 3: ADAC clocks */
            CUpd(c, R_POWER_UP_CONTROL, 3 << 4, 3 << 4);    /* subseq 4: output driver stage 1 */
            CUpd(c, R_POWER_UP_CONTROL, 3 << 2, 3 << 2);    /* subseq 5: stage 2 */
            CUpd(c, R_POWER_UP_CONTROL, 3 << 0, 3 << 0);    /* subseq 6: stage 3 */
            OutputDac(c, TRUE);                         /* subseq 7: PRE_PMU of Output DACL/R */
            CUpd(c, R_HSD_CTRL, 3, 0);                  /* subseq 8: unground HPOL/HPOR */
            CUpd(c, R_BOOST, HP_BOOST_DIS, 0);          /* subseq 9: HP boost driver */
            CUpd(c, R_CLASSG_CTRL, 1, 1);               /* subseq 10: class G */
        } else {
            CUpd(c, R_CLK_DIVIDER, 3 << CLK_ADC_SRC_SFT, 2 << CLK_ADC_SRC_SFT);  /* ADC OSR 64: SRC 1/4 */
            CUpd(c, R_ENA_CTRL, ENABLE_ADC_CLK, ENABLE_ADC_CLK);                 /* "ADC Clock" */
            CUpd(c, R_ANALOG_ADC_2, 1 << 6, 1 << 6);    /* "ADC Power" (POWERUP_ADCL) */
            CUpd(c, R_MIC_BIAS, MICBIAS_POWERUP, MICBIAS_POWERUP);   /* forced on anyway (headset) */
            CUpd(c, R_I2S_PCM_CTRL2, I2S_TRISTATE, 0);  /* AIFTX */
            CUpd(c, R_POWER_UP_CONTROL, 1 << 14, 1 << 14);  /* "Frontend PGA" + nau8825_fepga_event */
            CUpd(c, R_FEPGA, ACDC_CTRL_MASK, ACDC_VREF_MICP_MICN);
            CUpd(c, R_BOOST, DISCHRG_EN, DISCHRG_EN);
            CDelay(40);
            CUpd(c, R_BOOST, DISCHRG_EN, 0);
            CUpd(c, R_FEPGA, ACDC_CTRL_MASK, 0);
            CDelay(ADC_DELAY_MS);                       /* nau8825_adc_event POST_PMU */
            CUpd(c, R_ENA_CTRL, ENABLE_ADC, ENABLE_ADC);
        }
    } else {
        if (!rec) {
            CUpd(c, R_CLASSG_CTRL, 1, 0);
            CUpd(c, R_BOOST, HP_BOOST_DIS, HP_BOOST_DIS);
            CUpd(c, R_HSD_CTRL, 3, 3);                  /* ground HPOL/HPOR again */
            OutputDac(c, FALSE);                        /* POST_PMD of Output DACL/R */
            CUpd(c, R_POWER_UP_CONTROL, 3 << 0, 0);
            CUpd(c, R_POWER_UP_CONTROL, 3 << 2, 0);
            CUpd(c, R_POWER_UP_CONTROL, 3 << 4, 0);
            CUpd(c, R_RDAC, 3 << 8, 0);
            CUpd(c, R_RDAC, 3 << 12, 0);
            CUpd(c, R_CHARGE_PUMP, JAMNODCLOW, 0);      /* pump PRE_PMD */
            CUpd(c, R_CHARGE_PUMP, CHARGE_PUMP_EN, 0);
            CUpd(c, R_CLASSG_CTRL, 3 << 1, 0);
            CUpd(c, R_ENA_CTRL, ENABLE_DACL | ENABLE_DACR, 0);
            CUpd(c, R_ENA_CTRL, ENABLE_DAC_CLK, 0);
        } else {
            CUpd(c, R_ENA_CTRL, ENABLE_ADC, 0);         /* adc_event POST_PMD (no IRQ) */
            CUpd(c, R_POWER_UP_CONTROL, 1 << 14, 0);
            CUpd(c, R_I2S_PCM_CTRL2, I2S_TRISTATE, I2S_TRISTATE);
            CUpd(c, R_ENA_CTRL, ENABLE_ADC_CLK, 0);
            CUpd(c, R_ANALOG_ADC_2, 1 << 6, 0);
        }
        if (!other) ClockOff(c);                        /* "System Clock" POST_PMD */
    }
}

/* DAC digital volume DACL/DACR_CTRL[7:0]: 0xCF = 0 dB, 0.5 dB steps, 0x00 = mute */
static void Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    ULONG l = mute ? 0 : 1 + CVolSteps(left, DAC_VOL_0DB - 1);
    ULONG r = mute ? 0 : 1 + CVolSteps(right, DAC_VOL_0DB - 1);
    CUpd(c, R_DACL_CTRL, 0xFF, l);
    CUpd(c, R_DACR_CTRL, 0xFF, r);
    c->Priv[P_DACL] = l;
    c->Priv[P_DACR] = r;
}

static BOOLEAN Init(SST_CODEC *c)
{
    static const ULONG patch_c[] = {                    /* nau8825_regmap_patch (Rev C) */
        R_FLL2, 0x0000, R_FLL4, 0x8010, R_FLL_VCO_RSV, 0x0bc0, R_INTERRUPT_MASK, 0x0800,
        R_DACL_CTRL, 0x00cf, R_DACR_CTRL, 0x02cf, R_OPT_EFUSE_CTRL, 0x0400, R_FLL2_LOWER, 0x26e9,
        R_FLL2_UPPER, 0x0031, R_ANALOG_CONTROL_2, 0x0020, R_ANALOG_ADC_2, 0x0220, R_MIC_BIAS, 0x0046,
    };
    ULONG e0 = c->Errors, id, dl, v;
    c->PlayOn = c->RecOn = FALSE;

    CWr(c, R_RESET, 0);                                 /* nau8825_reset_chip() */
    CWr(c, R_RESET, 0);
    if (!CRd(c, R_I2C_DEVICE_ID, &id) || (id & 3) > 1) return FALSE;
    c->Priv[P_SWID] = id & 3;
    if (c->Priv[P_SWID] && !CSeq(c, patch_c, sizeof(patch_c) / sizeof(patch_c[0]) / 2)) return FALSE;

    /* nau8825_init_regs() */
    CWr(c, R_IIC_ADDR_SET, 0x0001);                     /* latch IIC LSB */
    CUpd(c, R_BIAS_ADJ, BIAS_VMID, BIAS_VMID);
    CUpd(c, R_BOOST, GLOBAL_BIAS_EN, GLOBAL_BIAS_EN);
    CUpd(c, R_BIAS_ADJ, BIAS_VMID_SEL_MASK, 2 << 4);    /* vref-impedance 2 */
    CUpd(c, R_BOOST, PRECHARGE_DIS | HP_BOOST_DIS | HP_BOOST_G_DIS | SHORT_SHUTDOWN_EN,
         PRECHARGE_DIS | HP_BOOST_DIS | HP_BOOST_G_DIS | SHORT_SHUTDOWN_EN);
    CUpd(c, R_GPIO12_CTRL, (1 << 8) | (1 << 9) | (1 << 11), 1 << 11);  /* JKDET input, pull enabled, up */
    CUpd(c, R_JACK_DET_CTRL, 1 << 1, 0);                /* jkdet-polarity 1: active low */
    CUpd(c, R_JACK_DET_CTRL, (7 << 5) | (7 << 2), 7 << 5);   /* insert debounce 7, eject 0 */
    CUpd(c, R_INTERRUPT_MASK, (1 << 14) | (1 << 13), (1 << 14) | (1 << 13));  /* IRQ pin pull-up */
    CUpd(c, R_INTERRUPT_MASK, 0x7FF, 0x7FF);            /* everything masked (no IRQ handler) */
    CWr(c, R_INTERRUPT_DIS_CTRL, 0xFFFF);
    CUpd(c, R_MIC_BIAS, MICBIAS_VOLTAGE_MASK, 6);       /* micbias-voltage 6 */
    /* nau8825_setup_buttons() defaults (key interrupts stay masked) */
    CUpd(c, R_SAR_CTRL, (7 << 8) | (3 << 2) | 3, (6 << 8) | (1 << 2) | 1);
    CUpd(c, R_KEYDET_CTRL, (7 << 8) | 0xF | (3 << 12), (3 << 8) | 0 | (3 << 12));
    CWr(c, R_VDET_THRESHOLD_1, (0x08 << 8) | 0x12);
    CWr(c, R_VDET_THRESHOLD_2, (0x26 << 8) | 0x73);
    CWr(c, R_VDET_THRESHOLD_3, 0);
    CWr(c, R_VDET_THRESHOLD_4, 0);
    CUpd(c, R_ADC_RATE, 0x3 | (1 << 4), 1);             /* ADC_SYNC_DOWN_64, SINC4 off */
    CUpd(c, R_DAC_CTRL1, 0x7, 0);                       /* DAC_OVERSAMPLE_64 */
    if (!c->Priv[P_SWID])
        CUpd(c, R_CHARGE_PUMP, POWER_DOWN_DACR | POWER_DOWN_DACL, POWER_DOWN_DACR | POWER_DOWN_DACL);
    CUpd(c, R_BIAS_ADJ, BIAS_TESTDAC_EN, BIAS_TESTDAC_EN);  /* DAC inputs at '0' during power up */
    CUpd(c, R_DAC_CTRL1, 1 << 7, 1 << 7);               /* CICCLP off */
    CUpd(c, R_ANALOG_CONTROL_2, (1 << 12) | 3, (1 << 12) | 3);
    CUpd(c, R_CLASSG_CTRL, 0x3F << 8, 0x20 << 8);       /* class G timer 64 ms */
    CUpd(c, R_RDAC, (7 << 4) | (3 << 2), (2 << 4) | (3 << 2));
    CUpd(c, R_DACL_CTRL, 1 << 9, 0);                    /* DACL <- left */
    CUpd(c, R_DACR_CTRL, 1 << 9, 1 << 9);               /* DACR <- right */
    CUpd(c, R_LEFT_TIME_SLOT, DIS_FS_SHORT_DET, DIS_FS_SHORT_DET);
    CUpd(c, R_CHARGE_PUMP, ADCOUT_DS_MASK, 0);

    /* nau8825_enable_jack_detect(NULL) then SND_SOC_BIAS_STANDBY: clock off */
    CUpd(c, R_HSD_CTRL, HSD_AUTO_MODE | 3, 0);
    ClockOff(c);

    /* DAI: consumer, I2S, normal BCLK, data length from the valid bits */
    dl = c->Valid >= 32 ? 3 : c->Valid >= 24 ? 2 : c->Valid >= 20 ? 1 : 0;
    CUpd(c, R_I2S_PCM_CTRL1, (3 << 2) | 3 | (1 << 7) | (1 << 6), (dl << 2) | 2);
    CUpd(c, R_I2S_PCM_CTRL2, I2S_MS_MASTER, 0);
    CUpd(c, R_I2S_PCM_CTRL2, I2S_TRISTATE, I2S_TRISTATE);   /* AIFTX off until capture */
    if (c->Slots == 4 || c->Slots == 8) {
        /* nau8825_set_tdm_slot(tx 0x1, rx 0x3): DACL slot 0, DACR slot 1, ADC slot 0 */
        CUpd(c, R_TDM_CTRL, TDM_MODE | TDM_OFFSET_EN | (3 << 6) | (3 << 4) | 3, TDM_MODE | (1 << 4) | 0);
        CUpd(c, R_LEFT_TIME_SLOT, 0x3FF, 0);
    } else {
        if (c->Slots != 2) LOGI("NAU8825: %u slots unsupported, using I2S\n", c->Slots);
        CUpd(c, R_TDM_CTRL, TDM_MODE | TDM_OFFSET_EN, 0);
    }

    /* CTIA headset (nau8825_jack_insert, case 2), HP outputs grounded until playback */
    CUpd(c, R_HSD_CTRL, HSD_AUTO_MODE | 0xF, 0x8 | 0x3);
    CUpd(c, R_MIC_BIAS, MICBIAS_JKSLV | MICBIAS_JKR2, MICBIAS_JKSLV);
    CUpd(c, R_SAR_CTRL, SAR_INPUT_JKSLV, SAR_INPUT_JKSLV);
    CUpd(c, R_MIC_BIAS, MICBIAS_POWERUP, MICBIAS_POWERUP);
    CUpd(c, R_CLK_DIVIDER, CLK_MCLK_SRC_MASK, 0xF);

    /* gains: HP 0 dB unmuted, mic frontend PGA +24 dB, ADC digital 0 dB, sidetone off */
    CWr(c, R_HSVOL_CTRL, 0x0000);
    CUpd(c, R_POWER_UP_CONTROL, 0x3F << 8, FEPGA_GAIN_24DB << 8);
    CWr(c, R_ADC_DGAIN_CTRL, 0x00CF);
    Volume(c, 65535, 65535, FALSE);

    if (!CRd(c, R_I2C_DEVICE_ID, &v)) return FALSE;
    if (c->Errors != e0) return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("NAU8825: %s, id %04x, BCLK %u (FLL ref), %u slots x %u bits, CTIA headset config\n",
         c->Priv[P_SWID] ? "NAU8825C" : "NAU8825", id, c->Bclk, c->Slots, c->Slot);
    return TRUE;
}

static void Shutdown(SST_CODEC *c)
{
    if (c->PlayOn) Stream(c, FALSE, FALSE);
    c->PlayOn = FALSE;
    if (c->RecOn) Stream(c, TRUE, FALSE);
    c->RecOn = FALSE;
    /* nau8825_suspend: MICBIAS/SAR off; SND_SOC_BIAS_OFF: detach 2k resistors, ground HP and MICGNDs */
    CUpd(c, R_MIC_BIAS, MICBIAS_POWERUP, 0);
    CUpd(c, R_SAR_CTRL, 1 << 12, 0);
    CUpd(c, R_MIC_BIAS, MICBIAS_JKSLV | MICBIAS_JKR2, 0);
    CUpd(c, R_HSD_CTRL, 0xF, 0xF);
    CWr(c, R_INTERRUPT_DIS_CTRL, 0xFFFF);
    CUpd(c, R_ENA_CTRL, ENABLE_ADC, 0);
    ClockOff(c);
}

const CODEC_DRV g_DrvNau8825 = { "NAU8825", "10508825\0", CODEC_HEADSET, 2, 2,
                                 Probe, Init, Stream, Volume, Shutdown };
