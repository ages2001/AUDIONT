#include "codec.h"
/*
 * AUDIONT - Nuvoton NAU88L24 / NAU8824 codec with class-D speaker amp (Cherry Trail / Braswell tablets,
 * ACPI "10508824").  Reference: Linux sound/soc/codecs/nau8824.c / nau8824.h,
 * intel/boards/cht_bsw_nau8824.c.  16-bit register addresses, 16-bit values.
 *
 * Clocking: the FLL can lock to BCLK (NAU8824_CLK_FLL_BLK), Fout = 256 * Fs, dividers from
 * nau8824_calc_fll_param() (FDCO 90..100 MHz).  cht_bsw_nau8824.c uses the frame clock
 * (NAU8824_CLK_FLL_FS); that is the fallback when BCLK is unusable.  The system clock is dropped
 * (NAU8824_CLK_DIS) after the last stream ("System Clock" POST_PMD, no jack reported).
 *
 * Outputs: headphone (HPOL/HPOR, charge pump, class G) and the class-D speaker (SPKOUTL/SPKOUTR) are
 * powered together while playing.  No jack detection (JD sleep mode, all interrupts disabled).
 * c->Mic: 0 = headset mic (HSMIC1 -> left ADC, HSMIC2 -> right ADC, as cht_bsw_nau8824.c routes both to
 *             "Headset Mic"), MICBIAS on while capturing;
 *         1 = internal analog mic on MIC1 (left ADC) / MIC2 (right ADC) ("Int Mic").
 * Both ADC channels go out in the two I2S slots (ADC CH0/CH1 TX).
 */

#define R_RESET                 0x00
#define R_ENA_CTRL              0x01
#define R_CLK_GATING_ENA        0x02
#define R_CLK_DIVIDER           0x03
#define R_FLL1                  0x04
#define R_FLL2                  0x05
#define R_FLL3                  0x06
#define R_FLL4                  0x07
#define R_FLL5                  0x08
#define R_FLL6                  0x09
#define R_JACK_DET_CTRL         0x0D
#define R_INTERRUPT_SETTING_1   0x0F
#define R_IRQ                   0x10
#define R_CLEAR_INT_REG         0x11
#define R_INTERRUPT_SETTING     0x12
#define R_SAR_ADC               0x13
#define R_VDET_COEFFICIENT      0x14
#define R_VDET_THRESHOLD_1      0x15
#define R_VDET_THRESHOLD_2      0x16
#define R_VDET_THRESHOLD_3      0x17
#define R_VDET_THRESHOLD_4      0x18
#define R_I2S_PCM_CTRL_1        0x1C
#define R_I2S_PCM_CTRL_2        0x1D
#define R_LEFT_TIME_SLOT        0x1E
#define R_TDM_CTRL              0x20
#define R_ADC_FILTER_CTRL       0x24
#define R_DAC_FILTER_CTRL_1     0x25
#define R_ADC_CH0_DGAIN_CTRL    0x2D
#define R_ADC_CH1_DGAIN_CTRL    0x2E
#define R_DAC_MUTE_CTRL         0x31
#define R_DAC_CH0_DGAIN_CTRL    0x32
#define R_DAC_CH1_DGAIN_CTRL    0x33
#define R_CLASSG                0x50
#define R_I2C_DEVICE_ID         0x58
#define R_BIAS_ADJ              0x66
#define R_TRIM_SETTINGS         0x68
#define R_ANALOG_CONTROL_1      0x69
#define R_ANALOG_CONTROL_2      0x6A
#define R_ENABLE_LO             0x6B
#define R_CLASSD_GAIN_1         0x6D
#define R_CLASSD_GAIN_2         0x6E
#define R_ANALOG_ADC_2          0x72
#define R_RDAC                  0x73
#define R_MIC_BIAS              0x74
#define R_BOOST                 0x76
#define R_FEPGA                 0x77
#define R_FEPGA_II              0x78
#define R_ATT_PORT0             0x7B
#define R_ATT_PORT1             0x7C
#define R_POWER_UP_CONTROL      0x7F
#define R_CHARGE_PUMP_CONTROL   0x80

/* ENA_CTRL */
#define JD_SLEEP_MODE           (1 << 10)
#define ENA_DAC_ADC_CH          0x3F    /* DAC_CH1/CH0_EN, ADC_CH3..0_EN */
/* CLK_GATING_ENA: ADC CH23/CH01, DAC CH1/CH0, I2S, GAIN, SAR, DMIC CH23 clocks */
#define CLK_GATING_ALL          0xF0AA
/* CLK_DIVIDER */
#define CLK_SRC_VCO             (1 << 15)
#define CLK_MCLK_SRC_MASK       0x000F
#define CLK_ADC_SRC_SFT         6
#define CLK_DAC_SRC_SFT         4
/* FLL */
#define FLL_RATIO_MASK          0x7F
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
/* misc */
#define TDM_MODE                (1 << 15)
#define TDM_OFFSET_EN           (1 << 14)
#define I2S_MS_MASTER           (1 << 3)
#define DAC_ZC_EN               (1 << 11)
#define VMID                    (1 << 6)
#define VMID_SEL_MASK           (3 << 4)
#define GLOBAL_BIAS_EN          (1 << 12)
#define PRECHARGE_DIS           (1 << 13)
#define HP_BOOST_DIS            (1 << 9)
#define HP_BOOST_G_DIS          (1 << 8)
#define SHORT_SHUTDOWN_EN       (1 << 6)
#define MICBIAS_POWERUP         (1 << 8)
#define DRV_CURR_INC            (1 << 15)
#define JAMNODCLOW              (1 << 15)
#define SPKR_PULL_DOWN          (1 << 13)
#define SPKL_PULL_DOWN          (1 << 12)
#define POWER_DOWN_DACR         (1 << 9)
#define POWER_DOWN_DACL         (1 << 8)
#define CHARGE_PUMP_EN          (1 << 5)
#define TEST_DAC_EN             (3 << 14)
#define DACL_HPL_EN             (1 << 0)
#define DACR_HPR_EN             (1 << 3)
#define CLASSD_EN               (1 << 7)
#define CLASSD_CLAMP_DIS        (1 << 3)
#define FEPGA_MODEL_SHORT_EN    (1 << 3)
#define FEPGA_MODER_SHORT_EN    (1 << 7)
#define FEPGA_MODEL_HSMIC_EN    (1 << 0)
#define FEPGA_MODEL_MIC1_EN     (1 << 1)
#define FEPGA_MODER_HSMIC_EN    (1 << 4)
#define FEPGA_MODER_MIC2_EN     (1 << 5)

#define NAU_FREF_MAX            13500000
#define NAU_FVCO_MAX            100000000
#define NAU_FVCO_MIN            90000000
#define DAC_VOL_0DB             0x100   /* DAC_CHx_DGAIN_CTRL[8:0]: 0x100 = 0 dB (reset value), 0.5 dB steps */
#define HP_ATT_0DB              0x1F    /* ATT_PORT0 fields: hp_vol_tlv -30..0 dB, 0x1F = 0 dB */
#define SPK_GAIN                6       /* CLASSD_GAIN fields: spk_vol_tlv 0..24 dB in 0x19 steps (~+6 dB) */
#define MIC_GAIN_24DB           12      /* FEPGA_II GAINL/GAINR: 0..36 dB in 2 dB steps */

/* Priv[] */
#define P_DAC0      0
#define P_DAC1      1

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

/* nau8824_config_sysclk(FLL_*) + nau8824_calc_fll_param() + nau8824_fll_apply() + set_pll tail */
static BOOLEAN Fll(SST_CODEC *c, ULONG fin, ULONG src)
{
    static const ULONG pre[4] = { 1, 2, 4, 8 };
    static const ULONG ratio[7][2] = { { 512000, 0x01 }, { 256000, 0x02 }, { 128000, 0x04 }, { 64000, 0x08 },
                                      { 32000, 0x10 }, { 8000, 0x20 }, { 4000, 0x40 } };
    static const ULONG scal[10][2] = { { 1, 0x0 }, { 2, 0x2 }, { 4, 0x3 }, { 8, 0x4 }, { 16, 0x5 }, { 32, 0x6 },
                                      { 3, 0x7 }, { 6, 0xa }, { 12, 0xb }, { 24, 0xc } };
    ULONG fref = 0, i, rdiv, rat, msrc = 0, integ, frac;
    ULONGLONG fvco, fmax = 0, x;
    if (!fin) return FALSE;
    for (i = 0; i < 4; i++) { fref = fin / pre[i]; if (fref <= NAU_FREF_MAX) break; }
    if (i == 4) return FALSE;
    rdiv = i;
    for (i = 0; i < 7; i++) if (fref >= ratio[i][0]) break;
    if (i == 7) return FALSE;
    rat = ratio[i][1];
    for (i = 0; i < 10; i++) {
        fvco = (ULONGLONG)256 * c->Rate * 2 * scal[i][0];
        if (fvco > NAU_FVCO_MIN && fvco < NAU_FVCO_MAX && fmax < fvco) { fmax = fvco; msrc = scal[i][1]; }
    }
    if (!fmax) return FALSE;
    x = Div64(fmax << 16, fref * rat);
    integ = (ULONG)(x >> 16) & 0x3FF;
    frac = (ULONG)x & 0xFFFF;

    CUpd(c, R_FLL3, FLL_CLK_SRC_MASK, src);
    CUpd(c, R_CLK_DIVIDER, CLK_SRC_VCO | CLK_MCLK_SRC_MASK, msrc);
    CUpd(c, R_FLL1, FLL_RATIO_MASK, rat);
    CWr(c, R_FLL2, frac);
    CUpd(c, R_FLL3, FLL_INTEGER_MASK, integ);
    CUpd(c, R_FLL4, FLL_REF_DIV_MASK, rdiv << 10);
    CUpd(c, R_FLL5, FLL_CLK_SW_MASK, 0);
    CUpd(c, R_FLL6, DCO_EN, 0);
    if (frac) {
        CUpd(c, R_FLL5, FLL_PDB_DAC_EN | FLL_LOOP_FTR_EN | FLL_FTR_SW_MASK, FLL_PDB_DAC_EN | FLL_LOOP_FTR_EN);
        CUpd(c, R_FLL6, SDM_EN, SDM_EN);
    } else {
        CUpd(c, R_FLL5, FLL_PDB_DAC_EN | FLL_LOOP_FTR_EN | FLL_FTR_SW_MASK, FLL_FTR_SW_ACCU);
        CUpd(c, R_FLL6, SDM_EN, 0);
    }
    CDelay(2);
    CUpd(c, R_CLK_DIVIDER, CLK_SRC_VCO, CLK_SRC_VCO);
    LOGV("NAU8824: FLL in %u src %x: ref_div %u ratio %x int %u frac %x mclk_src %x\n",
         fin, src >> 10, rdiv, rat, integ, frac, msrc);
    return TRUE;
}

static void ClockOn(SST_CODEC *c)
{
    if (Fll(c, c->Bclk, FLL_CLK_SRC_BLK)) return;
    if (Fll(c, c->Rate, FLL_CLK_SRC_FS)) return;
    if (!c->Mclk || !Fll(c, c->Mclk, FLL_CLK_SRC_MCLK)) LOGE("NAU8824: no usable FLL reference\n");
}

static void ClockOff(SST_CODEC *c)                      /* NAU8824_CLK_DIS */
{
    CUpd(c, R_CLK_DIVIDER, CLK_SRC_VCO, 0);
    CUpd(c, R_FLL6, DCO_EN, 0);
}

static BOOLEAN Probe(SST_CODEC *c)
{
    ULONG id;
    if (!CRd(c, R_I2C_DEVICE_ID, &id)) return FALSE;    /* Linux only requires the read to succeed */
    return id != 0xFFFF && id != 0;
}

static void Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? c->PlayOn : c->RecOn;
    if (on) {
        if (!other) ClockOn(c);                         /* machine hw_params */
        if (!rec) {
            CUpd(c, R_CLK_DIVIDER, 3 << CLK_DAC_SRC_SFT, 2 << CLK_DAC_SRC_SFT);  /* OSR 64: SRC 1/4 */
            CUpd(c, R_RDAC, 3 << 8, 3 << 8);            /* supplies "DACL/DACR Clock" */
            CUpd(c, R_RDAC, 3 << 12, 3 << 12);          /* DACL/DACR */
            CUpd(c, R_CLASSG, 3 << 1, 3 << 1);          /* mixers "Left/Right Headphone" */
            /* PGAs, subseq 0 in register order: Class G, ClassD, HP boost, drivers, Output DACs */
            CUpd(c, R_CLASSG, 1, 1);
            CUpd(c, R_ANALOG_CONTROL_2, CLASSD_CLAMP_DIS, CLASSD_CLAMP_DIS);  /* nau8824_spk_event PRE_PMU */
            CUpd(c, R_CLASSD_GAIN_1, CLASSD_EN, CLASSD_EN);
            CUpd(c, R_BOOST, HP_BOOST_DIS, 0);
            CUpd(c, R_POWER_UP_CONTROL, 0xF, 0xF);      /* Output Driver L/R (3,2), Main Driver L/R (1,0) */
            CUpd(c, R_ENABLE_LO, TEST_DAC_EN, 0);       /* nau8824_output_dac_event PRE_PMU */
            CUpd(c, R_CHARGE_PUMP_CONTROL, POWER_DOWN_DACR | POWER_DOWN_DACL, 0);
            CUpd(c, R_CHARGE_PUMP_CONTROL, CHARGE_PUMP_EN, CHARGE_PUMP_EN);  /* subseq 1 */
            CDelay(10);                                 /* nau8824_pump_event POST_PMU */
            CUpd(c, R_CHARGE_PUMP_CONTROL, JAMNODCLOW, JAMNODCLOW);
        } else {
            CUpd(c, R_CLK_DIVIDER, 3 << CLK_ADC_SRC_SFT, 2 << CLK_ADC_SRC_SFT);  /* OSR 64: SRC 1/4 */
            CUpd(c, R_MIC_BIAS, MICBIAS_POWERUP, MICBIAS_POWERUP);   /* "MICBIAS" supply */
            CUpd(c, R_POWER_UP_CONTROL, 3 << 12, 3 << 12);           /* mixers "Left/Right ADC" */
            CUpd(c, R_ANALOG_ADC_2, 3 << 6, 3 << 6);                 /* ADCL / ADCR */
        }
    } else {
        if (!rec) {
            CUpd(c, R_CHARGE_PUMP_CONTROL, JAMNODCLOW, 0);           /* pump PRE_PMD, subseq 1 first */
            CUpd(c, R_CHARGE_PUMP_CONTROL, CHARGE_PUMP_EN, 0);
            CUpd(c, R_CLASSG, 1, 0);
            CUpd(c, R_CLASSD_GAIN_1, CLASSD_EN, 0);
            CUpd(c, R_ANALOG_CONTROL_2, CLASSD_CLAMP_DIS, 0);        /* spk POST_PMD */
            CUpd(c, R_BOOST, HP_BOOST_DIS, HP_BOOST_DIS);
            CUpd(c, R_POWER_UP_CONTROL, 0xF, 0);
            CUpd(c, R_CHARGE_PUMP_CONTROL, POWER_DOWN_DACR | POWER_DOWN_DACL, POWER_DOWN_DACR | POWER_DOWN_DACL);
            CUpd(c, R_ENABLE_LO, TEST_DAC_EN, TEST_DAC_EN);          /* output_dac POST_PMD */
            CUpd(c, R_CLASSG, 3 << 1, 0);
            CUpd(c, R_RDAC, 3 << 12, 0);
            CUpd(c, R_RDAC, 3 << 8, 0);
        } else {
            CUpd(c, R_ANALOG_ADC_2, 3 << 6, 0);
            CUpd(c, R_POWER_UP_CONTROL, 3 << 12, 0);
            CUpd(c, R_MIC_BIAS, MICBIAS_POWERUP, 0);
        }
        if (!other) ClockOff(c);                        /* "System Clock" POST_PMD */
    }
}

/* DAC CH0/CH1 digital gain [8:0]: 0x100 = 0 dB, 0.5 dB steps (dmic_vol_tlv scale), 0 = mute */
static void Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    ULONG l = mute ? 0 : 1 + CVolSteps(left, DAC_VOL_0DB - 1);
    ULONG r = mute ? 0 : 1 + CVolSteps(right, DAC_VOL_0DB - 1);
    CUpd(c, R_DAC_CH0_DGAIN_CTRL, 0x1FF, l);
    CUpd(c, R_DAC_CH1_DGAIN_CTRL, 0x1FF, r);
    c->Priv[P_DAC0] = l;
    c->Priv[P_DAC1] = r;
}

static BOOLEAN Init(SST_CODEC *c)
{
    ULONG e0 = c->Errors, id, dl, sel;
    c->PlayOn = c->RecOn = FALSE;
    if (!CRd(c, R_I2C_DEVICE_ID, &id)) return FALSE;
    CWr(c, R_RESET, 0);                                 /* nau8824_reset_chip() */
    CWr(c, R_RESET, 0);

    /* nau8824_init_regs(): vref-impedance 2, micbias-voltage 6 (Linux defaults) */
    CUpd(c, R_BIAS_ADJ, VMID | VMID_SEL_MASK, VMID | (2 << 4));
    CUpd(c, R_BOOST, GLOBAL_BIAS_EN, GLOBAL_BIAS_EN);
    CDelay(2);
    CUpd(c, R_MIC_BIAS, 0x7, 6);
    CUpd(c, R_BOOST, PRECHARGE_DIS | HP_BOOST_DIS | HP_BOOST_G_DIS | SHORT_SHUTDOWN_EN,
         PRECHARGE_DIS | HP_BOOST_DIS | HP_BOOST_G_DIS | SHORT_SHUTDOWN_EN);
    CUpd(c, R_CLK_DIVIDER, (3 << CLK_ADC_SRC_SFT) | (3 << CLK_DAC_SRC_SFT),
         (1 << CLK_ADC_SRC_SFT) | (1 << CLK_DAC_SRC_SFT));
    CUpd(c, R_DAC_MUTE_CTRL, DAC_ZC_EN, DAC_ZC_EN);
    CUpd(c, R_ENA_CTRL, ENA_DAC_ADC_CH, ENA_DAC_ADC_CH);
    CUpd(c, R_CLK_GATING_ENA, CLK_GATING_ALL, CLK_GATING_ALL);
    CUpd(c, R_CLASSG, 0x3F << 8, 0x20 << 8);            /* class G timer 64 ms */
    CUpd(c, R_TRIM_SETTINGS, DRV_CURR_INC, DRV_CURR_INC);
    CUpd(c, R_CHARGE_PUMP_CONTROL, SPKR_PULL_DOWN | SPKL_PULL_DOWN | POWER_DOWN_DACR | POWER_DOWN_DACL,
         SPKR_PULL_DOWN | SPKL_PULL_DOWN | POWER_DOWN_DACR | POWER_DOWN_DACL);
    CUpd(c, R_ENABLE_LO, TEST_DAC_EN, TEST_DAC_EN);     /* DAC inputs at '0' during power up */
    CUpd(c, R_DAC_CH0_DGAIN_CTRL, 1 << 9, 0);           /* CH0 <- I2S0 (left) */
    CUpd(c, R_DAC_CH1_DGAIN_CTRL, 1 << 9, 1 << 9);      /* CH1 <- I2S1 (right) */
    CUpd(c, R_ENABLE_LO, DACR_HPR_EN | DACL_HPL_EN, DACR_HPR_EN | DACL_HPL_EN);
    CUpd(c, R_ADC_FILTER_CTRL, 0x3, 1);                 /* ADC_SYNC_DOWN_64 */
    CUpd(c, R_DAC_FILTER_CTRL_1, (1 << 7) | 0x7, (1 << 7) | 0);  /* CICCLP off, OVERSAMPLE_64 */
    CUpd(c, R_RDAC, (7 << 4) | (3 << 2), (2 << 4) | (3 << 2));
    CUpd(c, R_FEPGA, FEPGA_MODEL_SHORT_EN | FEPGA_MODER_SHORT_EN, FEPGA_MODEL_SHORT_EN | FEPGA_MODER_SHORT_EN);
    CUpd(c, R_ANALOG_CONTROL_1, (1 << 3) | 0x7, (1 << 3) | 0x7);  /* DMIC clock drive / slew */
    CUpd(c, R_JACK_DET_CTRL, 1 << 1, 0);                /* jkdet-polarity 1 (active low) */
    CUpd(c, R_JACK_DET_CTRL, 3 << 2, 1 << 2);           /* jack-eject-debounce 1 */
    /* nau8824_setup_buttons() defaults */
    CUpd(c, R_SAR_ADC, (7 << 8) | (3 << 2) | 3, (6 << 8) | (1 << 2) | 1);
    CUpd(c, R_VDET_COEFFICIENT, (7 << 8) | 0xF | (3 << 12), 3 << 8);
    CWr(c, R_VDET_THRESHOLD_1, (0x0A << 8) | 0x14);
    CWr(c, R_VDET_THRESHOLD_2, (0x26 << 8) | 0x73);
    CWr(c, R_VDET_THRESHOLD_3, 0);
    CWr(c, R_VDET_THRESHOLD_4, 0);
    /* nau8824_setup_irq(): interrupts off, JD sleep mode; then BIAS_STANDBY: clock off */
    CUpd(c, R_ENA_CTRL, JD_SLEEP_MODE, JD_SLEEP_MODE);
    CUpd(c, R_INTERRUPT_SETTING, 0x3FF, 0x3FF);
    CUpd(c, R_INTERRUPT_SETTING_1, (1 << 9) | (1 << 8), 0);
    ClockOff(c);

    /* DAI: consumer, I2S, normal BCLK, data length */
    dl = c->Valid >= 32 ? 3 : c->Valid >= 24 ? 2 : c->Valid >= 20 ? 1 : 0;
    CUpd(c, R_I2S_PCM_CTRL_1, (3 << 2) | 3 | (1 << 7) | (1 << 6), (dl << 2) | 2);
    CUpd(c, R_I2S_PCM_CTRL_2, I2S_MS_MASTER, 0);
    if (c->Slots == 4) {
        /* nau8824_set_tdm_slot(tx 0x3, rx 0x1, 4 slots): ADC CH0/CH1 in slots 0/1, DACL slot 0, DACR slot 1 */
        CUpd(c, R_TDM_CTRL, TDM_MODE | TDM_OFFSET_EN | (3 << 6) | (3 << 4) | 0xF,
             TDM_MODE | TDM_OFFSET_EN | (1 << 4) | 0x3);
        CUpd(c, R_LEFT_TIME_SLOT, 0x3FF, 0);
    } else {
        if (c->Slots != 2) LOGI("NAU8824: %u slots unsupported, using I2S\n", c->Slots);
        CUpd(c, R_TDM_CTRL, TDM_MODE | TDM_OFFSET_EN | 0xF, 0x3);    /* "ADC CH0/CH1 TX Switch" */
    }

    /* playback gains: HP direct paths 0 dB, cross paths off (ENABLE_LO), speaker L<-DACL, R<-DACR */
    CUpd(c, R_ATT_PORT0, (0x1F << 8) | 0x1F, (HP_ATT_0DB << 8) | HP_ATT_0DB);
    CUpd(c, R_CLASSD_GAIN_2, 0x1F, SPK_GAIN);           /* "Speaker Left DACL Volume" */
    CUpd(c, R_CLASSD_GAIN_1, 0x1F << 8, SPK_GAIN << 8); /* "Speaker Right DACR Volume" */
    CUpd(c, R_CLASSD_GAIN_1, 0x1F, 0);                  /* "Speaker Left DACR Volume" off */
    CUpd(c, R_CLASSD_GAIN_2, 0x1F << 8, 0);             /* "Speaker Right DACL Volume" off */
    Volume(c, 65535, 65535, FALSE);

    /* capture: FEPGA input mixers, +24 dB analog, ADC digital 0 dB (0x100), CH0 <- input 0, CH1 <- input 1 */
    sel = c->Mic == 1 ? (FEPGA_MODEL_MIC1_EN | FEPGA_MODER_MIC2_EN) : (FEPGA_MODEL_HSMIC_EN | FEPGA_MODER_HSMIC_EN);
    CUpd(c, R_FEPGA, FEPGA_MODEL_MIC1_EN | FEPGA_MODEL_HSMIC_EN | FEPGA_MODER_MIC2_EN | FEPGA_MODER_HSMIC_EN, sel);
    CUpd(c, R_FEPGA_II, (0x1F << 5) | 0x1F, (MIC_GAIN_24DB << 5) | MIC_GAIN_24DB);
    CUpd(c, R_ADC_CH0_DGAIN_CTRL, (3 << 9) | 0x1FF, (0 << 9) | 0x100);
    CUpd(c, R_ADC_CH1_DGAIN_CTRL, (3 << 9) | 0x1FF, (c->Mic == 1 ? 1 << 9 : 0) | 0x100);

    if (c->Errors != e0) return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("NAU8824: id %04x, FLL ref BCLK %u, %u slots x %u bits, mic %s\n", id, c->Bclk, c->Slots, c->Slot,
         c->Mic == 1 ? "MIC1/MIC2" : "HSMIC");
    return TRUE;
}

static void Shutdown(SST_CODEC *c)
{
    if (c->PlayOn) Stream(c, FALSE, FALSE);
    c->PlayOn = FALSE;
    if (c->RecOn) Stream(c, TRUE, FALSE);
    c->RecOn = FALSE;
    /* SND_SOC_BIAS_OFF: interrupts off */
    CUpd(c, R_INTERRUPT_SETTING, 0x3FF, 0x3FF);
    CUpd(c, R_INTERRUPT_SETTING_1, (1 << 9) | (1 << 8), 0);
    CUpd(c, R_MIC_BIAS, MICBIAS_POWERUP, 0);
    ClockOff(c);
}

const CODEC_DRV g_DrvNau8824 = { "NAU8824", "10508824\0", CODEC_HEADSET, 2, 2,
                                 Probe, Init, Stream, Volume, Shutdown };
