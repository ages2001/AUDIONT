#include "codec.h"

/*
 * Realtek RT5682 (ALC5682I-VD / -VE, ACPI "10EC5682") and RT5682S (ALC5682I-VS, ACPI
 * "RTL5682") I2S headset codecs (I2C control) of the Intel SOF Chromebook / GLK / JSL /
 * CML / TGL / ADL boards.
 * Reference: Linux sound/soc/codecs/rt5682.c/.h, rt5682-i2c.c, rt5682s.c/.h, rl6231.c,
 * sound/soc/intel/boards/sof_rt5682.c, sof_realtek_common.c.
 *
 * HIDs: rt5682-i2c.c binds "10EC5682" only, rt5682s.c binds "RTL5682" only (the GLK / JSL
 * ACPI tables list both because a board ships with either chip).  The two chips differ in
 * PLL (RT5682: one rl6231 PLL1 + PLL2; RT5682S: table driven PLLA / PLLB), HP amp sequence,
 * SAR / micbias handling and register defaults, so they have separate descriptors.
 * DEVICE_ID (0xff): 0x6530 RT5682, 0x6749 RT5682S.
 *
 * I2C: 16-bit register address, 16-bit value, big endian.
 *
 * Clocking (I2S slave): sysclk = 512 * fs as sof_rt5682_hw_params (pll_out = rate * 512).
 *   RT5682:  PLL1 from BCLK1 (RT5682_PLL1_S_BCLK1, rl6231_pll_calc at run time); fallback
 *            MCLK direct (MCLK == 512 fs) or PLL1 from MCLK.
 *   RT5682S: PLLA from BCLK1 (plla_table: 1.536 / 3.072 MHz from Linux, 2.304 / 2.4 MHz rows
 *            added here with the same Fout = Fin * (N+2) / ((M+2)(K+2)) formula); fallback
 *            MCLK direct or PLLB (pllb_table) from MCLK.
 *   ASRC (filters tracking I2S1) only when the sysclk is not locked to the I2S clocks:
 *   inexact PLL result, or a 24 MHz MCLK as sof_rt5682_codec_init does.
 *
 * Output: HPOL/HPOR only (no speaker amp, no line out).  DAC1 -> HP amp with the Linux
 * rt5682_hp_event / rt5682s_hp_amp_event sequence; supplies in the DAPM power order.
 *
 * Capture, c->Mic:
 *   0  headset mic on the combo jack (IN1P -> BST1 CBJ -> RECMIX1L -> ADC1 L -> STO1 ADC,
 *      both ADC channels take ADC1 L).  Init puts the CBJ into the "jack inserted" state
 *      of rt5682(s)_headset_detect and lets its type FSM choose the headset pinout; when it
 *      does not report a headset (no plug at boot, 3-pole) the CBJ is forced to headset
 *      mode (MIC_CAP from register, MICBIAS1 path register controlled).  While no headset
 *      was found the type detection is re-run at capture start if playback is idle.
 *   1  DMIC1, data on GPIO2, clock on GPIO3 (Linux default platform data pins), 3.072 MHz.
 *   2  headset mic with the CBJ forced to headset mode, no type detection.
 *   CBJ boost +24 dB, ADC digital 0 dB, MICBIAS1 on while capturing.
 *
 * No jack detection: the JD1 / IRQ / button parts of rt5682_set_jack_detect are left out,
 * the CBJ stays in the jack-in state from Init to Shutdown (as Linux with a headset in).
 * "Fast off" (HP mute on jack removal) is not enabled: it is driven by the JD pin.
 *
 * GPIOs: Linux only uses an optional "realtek,ldo1-en" line (no ACPI _DSD mapping, falls
 * back to the first _CRS GPIO, often the jack GpioInt); its index in c->Gpio[] is not
 * known, so no line is driven here.
 */

#define RT5682_ID_VAL   0x6530
#define RT5682S_ID_VAL  0x6749
#define RT5682_ID_VE    0x6956      /* INT_DEVICE_ID of the ALC5682I-VE */

/* ------------------------------------------------------------------ */
/* RT5682 registers / bits (rt5682.h)                                  */
/* ------------------------------------------------------------------ */
#define R82_RESET               0x0000
#define R82_HP_CTRL_1           0x0002
#define R82_HP_CTRL_2           0x0003
#define R82_I2C_CTRL            0x0008
#define R82_CBJ_BST_CTRL        0x000b
#define R82_CBJ_CTRL_1          0x0010
#define R82_CBJ_CTRL_2          0x0011
#define R82_CBJ_CTRL_3          0x0012
#define R82_CBJ_CTRL_5          0x0014
#define R82_CBJ_CTRL_7          0x0016
#define R82_DAC1_DIG_VOL        0x0019
#define R82_STO1_ADC_DIG_VOL    0x001c
#define R82_STO1_ADC_BOOST      0x001f
#define R82_STO1_ADC_MIXER      0x0026
#define R82_AD_DA_MIXER         0x0029
#define R82_A_DAC1_MUX          0x002b
#define R82_REC_MIXER           0x003c
#define R82_CAL_REC             0x0044
#define R82_PWR_DIG_1           0x0061
#define R82_PWR_DIG_2           0x0062
#define R82_PWR_ANLG_1          0x0063
#define R82_PWR_ANLG_2          0x0064
#define R82_PWR_ANLG_3          0x0065
#define R82_CLK_DET             0x006b
#define R82_DMIC_CTRL_1         0x006e
#define R82_I2S1_SDP            0x0070
#define R82_ADDA_CLK_1          0x0073
#define R82_TDM_CTRL            0x0079
#define R82_TDM_ADDA_CTRL_2     0x007b
#define R82_TDM_TCON_CTRL       0x007e
#define R82_GLB_CLK             0x0080
#define R82_PLL_CTRL_1          0x0081
#define R82_PLL_CTRL_2          0x0082
#define R82_PLL_TRACK_1         0x0083
#define R82_PLL_TRACK_2         0x0084
#define R82_PLL_TRACK_3         0x0085
#define R82_DEPOP_1             0x008e
#define R82_HP_CHARGE_PUMP_1    0x0091
#define R82_MICBIAS_2           0x0094
#define R82_IRQ_CTRL_3          0x00b8
#define R82_GPIO_CTRL_1         0x00c0
#define R82_4BTN_IL_CMD_2       0x00e3
#define R82_INT_DEVICE_ID       0x00f9
#define R82_VERSION_ID          0x00fd
#define R82_VENDOR_ID           0x00fe
#define R82_DEVICE_ID           0x00ff
#define R82_DAC_ADC_DIG_VOL1    0x0100
#define R82_BIAS_CUR_CTRL_8     0x0111
#define R82_CHARGE_PUMP_1       0x0125
#define R82_CHOP_DAC            0x013a
#define R82_CHOP_ADC            0x013b
#define R82_CALIB_ADC_CTRL      0x013c
#define R82_TEST_MODE_CTRL_1    0x0145
#define R82_PLL2_INTERNAL       0x0156
#define R82_HP_IMP_SENS_CTRL_19 0x01c1
#define R82_HP_LOGIC_CTRL_2     0x01db
#define R82_HP_CALIB_CTRL_1     0x01de
#define R82_HP_CALIB_CTRL_2     0x01df
#define R82_HP_CALIB_CTRL_3     0x01e0
#define R82_HP_CALIB_STA_1      0x01ea
#define R82_SAR_IL_CMD_1        0x0210
#define R82_SAR_IL_CMD_2        0x0211
#define R82_SAR_IL_CMD_3        0x0212
#define R82_SAR_IL_CMD_6        0x0215
#define R82_SAR_IL_CMD_13       0x021c
#define R82_I2C_MODE            0xffff

/* PWR_DIG_1 (0x61) - same layout on the RT5682S */
#define PD1_I2S1                0x8000
#define PD1_DAC_L1              0x0800
#define PD1_DAC_R1              0x0400
#define PD1_LDO                 0x0100
#define PD1_ADC_L1              0x0010
#define PD1_DIG_GATE            0x0001
/* PWR_DIG_2 (0x62) */
#define PD2_ADC_S1F             0x8000
#define PD2_DAC_S1F             0x0400
#define PD2_DLDO_I_LIMIT        0x0080      /* RT5682S */
/* PWR_ANLG_1 (0x63) */
#define PA1_VREF1               0x8000
#define PA1_FV1                 0x4000
#define PA1_VREF2               0x2000
#define PA1_FV2                 0x1000
#define PA1_MB                  0x0200
#define PA1_BG                  0x0080
#define PA1_LDO1_DVO_MASK       0x0030
#define PA1_LDO1_DVO_12         0x0020
#define PA1_HP_DRIVER_MASK      0x000c
#define PA1_HP_DRIVER_5X        0x000c
#define PA1_HA_L                0x0002
#define PA1_HA_R                0x0001
/* PWR_ANLG_2 (0x64) */
#define PA2_MCLK0_WD            0x8000      /* RT5682S "MCLK0 DET PWR" */
#define PA2_MB1                 0x0800
#define PA2_RM1_L               0x0002      /* RT5682 "RECMIX1L Power" */
/* PWR_ANLG_3 (0x65) */
#define PA3_CBJ                 0x0200
#define PA3_PLL                 0x0040      /* RT5682 PLL1 */
/* CLK_DET (0x6b) */
#define CLKDET_SYS              0x8000
#define CLKDET_POW              0x0001
/* HP_CTRL_1 (0x02) / STO1_ADC_DIG_VOL (0x1c) */
#define LR_L_MUTE               0x8000
#define LR_R_MUTE               0x0080
/* HP_CTRL_2 (0x03) */
#define HPC2_DAC_AMP_MUTE       0x8000
#define HPC2_DAC_L_EN           0x4000
#define HPC2_DAC_R_EN           0x2000
/* DEPOP_1 (0x8e) */
#define DEP_PUMP_EN             0x0008
#define DEP_CAPLESS_EN          0x0001
/* HP_CHARGE_PUMP_1 (0x91) */
#define HCP_OSW_L               0x0800
#define HCP_OSW_R               0x0400
#define HCP_PM_HP_MASK          0x0300
#define HCP_PM_HP_HV            0x0200
/* CHARGE_PUMP_1 (0x125) */
#define CP_SW_SIZE_MASK         0x0700
#define CP_SW_SIZE_L            0x0400
#define CP_SW_SIZE_M            0x0200
#define CP_CLK_HP_MASK          0x0030
#define CP_CLK_HP_300KHZ        0x0020
/* BIAS_CUR_CTRL_8 (0x111) */
#define BC8_HPA_CP_BIAS_MASK    0x000c
#define BC8_HPA_CP_BIAS_3UA     0x0004
/* HP_LOGIC_CTRL_2 (0x1db) */
#define HPL2_SIG_SOUR2_REG      0x0010
/* CHOP_DAC / CHOP_ADC: CKGEN_DAC1 / CKGEN_ADC1 */
#define CHOP_CKGEN              0x1000
/* PLL_TRACK_1 (0x83) - same on the RT5682S */
#define TRK_DA_ASRC             0x2000
#define TRK_DAC_STO1_ASRC       0x1000
#define TRK_AD_ASRC             0x0100
#define TRK_DMIC_ASRC           0x0008
#define TRK_ADC_STO1_ASRC       0x0004
/* PLL_TRACK_2 / _3 (0x84 / 0x85) */
#define TRK_FILTER_CLK_SEL_MASK 0x7000
#define TRK_FILTER_SEL_I2S1     0x1000      /* RT5682_CLK_SEL_I2S1_ASRC */
#define TRK_FILTER_CLK_DIV_MASK 0x0f00
#define TRK_FILTER_CLK_DIV_SFT  8
/* ADDA_CLK_1 (0x73) */
#define ADDA_ADC_OSR_SFT        12
#define ADDA_ADC_OSR_MASK       0xf000
#define ADDA_DAC_OSR_MASK       0x000f
/* GLB_CLK (0x80) */
#define GLB_SCLK_SRC_MASK       0xe000
#define GLB_SCLK_SRC_SFT        13
#define GLB_PLL1_SRC_MASK       0x0300      /* RT5682S: PLL_SRC (PLLA / PLLB input) */
#define GLB_PLL1_SRC_MCLK       0x0000
#define GLB_PLL1_SRC_BCLK1      0x0100
#define SCLK_MCLK               0
#define SCLK_PLL1               1
#define SCLK_PLL2               2
/* PLL_CTRL_2 (0x82) */
#define PLL2C_M_SFT             12
#define PLL2C_M_BP              0x0800
#define PLL2C_RST               0x0002
/* I2S1_SDP (0x70) - same on the RT5682S */
#define SDP_SEL_ADCDAT_IN       0x8000      /* ADCDAT pin input; cleared = output (AIF1TX) */
#define SDP_TX_CHL_SFT          12
#define SDP_TX_CHL_MASK         0x7000
#define SDP_RX_CHL_SFT          8
#define SDP_RX_CHL_MASK         0x0700
#define SDP_MONO                0x0080
#define SDP_DL_SFT              4
#define SDP_DL_MASK             0x0070
#define SDP_DF_MASK             0x0007      /* 0 = I2S */
/* TDM_CTRL (0x79) */
#define TDM_TX_CH_SFT           12
#define TDM_TX_CH_MASK          0x3000
#define TDM_RX_CH_SFT           8
#define TDM_RX_CH_MASK          0x0300
#define TDM_ADC_DL_MASK         0x0003      /* RT5682S */
/* TDM_ADDA_CTRL_2 (0x7b) */
#define TDM_EN                  0x0080
/* TDM_TCON_CTRL (0x7e) */
#define TCON_S_BP_INV           0x8000
#define TCON_S_LP_INV           0x4000
#define TCON_DF_MASK            0x3800
#define TCON_CL_SFT             4
#define TCON_CL_MASK            0x0030
#define TCON_M_BP_INV           0x0004
#define TCON_M_LP_INV           0x0002
#define TCON_MS_MASTER          0x0001
/* CBJ_CTRL_1 (0x10) */
#define CBJ1_EMB_JD_EN          0x8000
#define CBJ1_DET_TYPE           0x1000
#define CBJ1_POL_FAST_OFF_HIGH  0x0100
#define CBJ1_FAST_OFF_EN        0x0080
#define CBJ1_VREF_POW_REG       0x0040
#define CBJ1_MB1_PATH_REG       0x0020
#define CBJ1_MB2_PATH_REG       0x0010
#define CBJ1_TRIG_JD            0x0008
#define CBJ1_MIC_CAP_HS         0x0002
#define CBJ1_MIC_CAP_SRC_REG    0x0001
/* CBJ_CTRL_2 (0x11) */
#define CBJ2_EXT_JD_SRC_MASK    0x0070
#define CBJ2_EXT_JD_SRC_MANUAL  0x0040
#define CBJ2_JACK_TYPE_MASK     0x0003
/* CBJ_CTRL_3 (0x12) */
#define CBJ3_IN_BUF_EN          0x0080
/* CBJ_BST_CTRL (0x0b): BST_CBJ [11:8] {0, +20, +24, +30, +35, +40, +44, +50, +52} dB */
#define CBJ_BST_MASK            0x0f00
#define CBJ_BST_24DB            0x0200
/* REC_MIXER (0x3c) */
#define REC_M_CBJ_RM1_L         0x0080      /* RECMIX1L "CBJ Switch", 1 = mute */
#define REC_G_CBJ_RM1_L_MASK    0x1c00
/* SAR_IL_CMD_1 (0x210) */
#define SAR1_BUTT_DET_EN        0x8000
#define SAR1_POW_EN             0x1000
/* SAR_IL_CMD_13 (0x21c) */
#define SAR13_SOUR_MASK         0x003f      /* 0 = type detection, 0x3f = buttons */
/* IRQ_CTRL_3 (0xb8) */
#define IRQ3_IL_IRQ_EN          0x0080
/* 4BTN_IL_CMD_2 (0xe3) */
#define BTN2_4BTN_IL_EN         0x8000
#define BTN2_4BTN_IL_NOR        0x4000
/* MICBIAS_2 (0x94) */
#define MB2_PWR_CLK25M          0x0200
#define MB2_PWR_CLK1M           0x0100
/* AD_DA_MIXER (0x29): "DAC1 Switch" of DAC1 MIXL / MIXR, 1 = mute */
#define ADDA_M_DAC1_L           0x4000
#define ADDA_M_DAC1_R           0x0040
/* GPIO_CTRL_1 (0xc0) */
#define GP2_PIN_MASK            0x3000
#define GP2_PIN_DMIC_SDA        0x2000
#define GP3_PIN_MASK            0x0c00
#define GP3_PIN_DMIC_CLK        0x0800
#define GP4_PIN_MASK            0x0300
#define GP4_PIN_ADCDAT1         0x0100
#define GP5_PIN_MASK            0x00c0
#define GP5_PIN_DACDAT1         0x0040
/* DMIC_CTRL_1 (0x6e) */
#define DMIC1_EN                0x8000
#define DMIC_FIFO_CLK_DIV_MASK  0x7000
#define DMIC_FIFO_CLK_DIV_2     0x1000
#define DMIC_DP_MASK            0x0030      /* 0 = data on GPIO2 */
#define DMIC_CLK_MASK           0x000f

/*
 * STO1_ADC_MIXER (0x26): [15] M_STO1_ADC_L1, [14] M_L2 (1 = mute), [13] ADC1L src (1 = ADC),
 * [12] ADC2L src (1 = DMIC), [11:10] ADCL src (0 = ADC1 L), [8] DMIC src (0 = DMIC1),
 * [7] M_R1, [6] M_R2, [5] ADC1R src, [4] ADC2R src, [3:2] ADCR src (0 = ADC1 L).
 */
#define STO1_ADC_MIX_CBJ        0x6060      /* ADC1 L (CBJ) on both channels, ADC2 muted */
#define STO1_ADC_MIX_DMIC       0x9090      /* DMIC1 L/R via ADC2, ADC1 muted */

/* ------------------------------------------------------------------ */
/* RT5682S registers / bits (rt5682s.h) where they differ               */
/* ------------------------------------------------------------------ */
#define R82S_CBJ_CTRL_6             0x0015
#define R82S_DEPOP_1                0x008e
#define R82S_HP_CHARGE_PUMP_2       0x0091
#define R82S_PLL_TRACK_11           0x008c
#define R82S_PLL_CTRL_1             0x0098
#define R82S_PLL_CTRL_2             0x0099
#define R82S_PLL_CTRL_3             0x009a
#define R82S_PLL_CTRL_4             0x009b
#define R82S_PLL_CTRL_6             0x009d
#define R82S_PLL_CTRL_7             0x009e
#define R82S_I2S2_M_CLK_CTRL_1      0x00a0
#define R82S_IL_CMD_1               0x00db
#define R82S_VREF_REC_OP_FB_CAP_1   0x0117
#define R82S_VREF_REC_OP_FB_CAP_2   0x0118
#define R82S_DIG_IN_CTRL_1          0x0132
#define R82S_CHOP_DAC_2             0x013a
#define R82S_HP_CALIB_CTRL_9        0x01e6
#define R82S_HP_CALIB_ST_1          0x01ea
#define R82S_BIAS_CUR_CTRL_11       0x0610
#define R82S_BIAS_CUR_CTRL_12       0x0611
#define R82S_HP_AMP_DET_CTL_1       0x3b00

/* PWR_ANLG_3 (0x65) */
#define SA3_LDO_PLLA                0x8000
#define SA3_LDO_PLLB                0x4000
#define SA3_BIAS_PLLA               0x2000
#define SA3_BIAS_PLLB               0x1000
#define SA3_CBJ                     0x0200
#define SA3_RSTB_PLLB               0x0080
#define SA3_RSTB_PLLA               0x0040
#define SA3_PLLB                    0x0020
#define SA3_PLLA                    0x0010
#define SA3_LDO_MB1                 0x0002
#define SA3_BGLDO                   0x0001
/* CAL_REC (0x44) */
#define SCAL_PWR_RM1_L              0x0001
/* DEPOP_1 (0x8e) */
#define SDEP_OUT_HP_L               0x0040
#define SDEP_OUT_HP_R               0x0020
#define SDEP_LDO_PUMP               0x0010
#define SDEP_PUMP                   0x0008
#define SDEP_CAPLESS_L              0x0002
#define SDEP_CAPLESS_R              0x0001
/* HP_CTRL_2 (0x03) */
#define SHPC2_HPO_L_PATH            0x4000
#define SHPC2_HPO_R_PATH            0x2000
#define SHPC2_IP_EN_GATING          0x0001  /* RT5682S_HPO_SEL_IP_EN_SW mask / IP_EN_GATING value */
/* HP_AMP_DET_CTL_1 (0x3b00) */
#define SAMP_CP_SW_SIZE_MASK        0x0070
#define SAMP_CP_SW_SIZE_L           0x0040
#define SAMP_CP_SW_SIZE_M           0x0020
#define SAMP_CP_SW_SIZE_S           0x0010
/* HP_LOGIC_CTRL_2 (0x1db) */
#define SHPL2_SIG_SRC_MASK          0x0003
#define SHPL2_SIG_SRC_1BIT          0x0003
/* CBJ_CTRL_1 (0x10) */
#define SCBJ1_POL_FAST_OFF_MASK     0x0100
#define SCBJ1_VREF_POW_MASK         0x0040
/* CBJ_CTRL_5 (0x14) */
#define SCBJ5_FAST_OFF_SRC_MASK     0x0700
#define SCBJ5_FAST_OFF_SRC_JDH      0x0600
/* SAR_IL_CMD_1 (0x210) */
#define SSAR1_BUTDET_EN             0x8000
#define SSAR1_BUTDET_POW_SAV        0x4000
#define SSAR1_POW_EN                0x1000
#define SSAR1_SEL_MB1_2_MASK        0x0300
#define SSAR1_SEL_MB1_2_SFT         8
#define SSAR1_SEL_MB1_2_AUTO        0x0020
/* SAR_IL_CMD_2 (0x211) */
#define SSAR2_ADC_PSV_ENTRY         0x0010
#define SSAR13_SOUR_BTN             0x003f
/* REC_MIXER (0x3c): BST_CBJ [13:8], -12 dB + 1.5 dB steps (max 35) */
#define SREC_BST_CBJ_MASK           0x3f00
#define SREC_BST_CBJ_24DB           (24 << 8)
/* PLL_CTRL_6 (0x9d) */
#define SPLL6_PLLB_SEL_PS           0x2000
#define SPLL6_PLLB_BYP_PS           0x1000
#define SPLL6_PLLB_M_BP             0x0800
#define SPLL6_PLLB_K_BP             0x0400
#define SPLL6_PLLA_M_BP             0x0080
#define SPLL6_PLLA_K_BP             0x0040
#define SPLL7_PLLB_SRC_DFIN         0x0001
/* PLL_TRACK_11 (0x8c) */
#define STRK11_ASRCIN_AUTO_CLKOUT   0x0020
/* ADDA_CLK_1 (0x73) / I2S2_M_CLK_CTRL_1 (0xa0): master clock source [6:4] */
#define SADDA_I2S_M_CLK_SRC_MASK    0x0070
#define SADDA_I2S_M_CLK_SRC_SFT     4

/* ------------------------------------------------------------------ */
/* private state                                                       */
/* ------------------------------------------------------------------ */
#define CHIP_5682       0
#define CHIP_5682S      1

#define MIC_HS          0           /* CBJ headset mic, type detection + forced fallback */
#define MIC_DMIC        1
#define MIC_HS_FORCED   2

#define CLK_BCLK_PLL    0           /* PLL (RT5682 PLL1, RT5682S PLLA) from BCLK1 */
#define CLK_MCLK_PLL    1
#define CLK_MCLK        2           /* sysclk = MCLK */

#define USE_PLLA        0           /* RT5682S PLL combinations (find_pll_inter_combination) */
#define USE_PLLB        1
#define USE_PLLAB       2

typedef struct _RT82_PRIV {
    UCHAR   Chip;
    UCHAR   Mic;
    UCHAR   Clk;
    UCHAR   Comb;                   /* RT5682S: USE_PLLx */
    BOOLEAN Asrc;
    BOOLEAN Hs;                     /* CBJ type FSM reported a headset (type 1 / 2) */
    BOOLEAN Ve;                     /* ALC5682I-VE */
    UCHAR   JackType;               /* CBJ_CTRL_2 JACK_TYPE of the last detection */
    ULONG   Sysclk;
    ULONG   PllIn;
    USHORT  PllN, PllM, PllK;
    BOOLEAN PllMBp, PllKBp;
} RT82_PRIV;

typedef char RT82_PRIV_FITS[(sizeof(RT82_PRIV) <= sizeof(((SST_CODEC *)0)->Priv)) ? 1 : -1];

#define PRIV(c) ((RT82_PRIV *)(c)->Priv)

/* ------------------------------------------------------------------ */
/* rl6231 helpers (sound/soc/codecs/rl6231.c), integer only            */
/* ------------------------------------------------------------------ */
#define RL_PLL_INP_MAX 50000000
#define RL_PLL_INP_MIN 256000
#define RL_PLL_N_MAX   0x1ff
#define RL_PLL_K_MAX   0x1f
#define RL_PLL_M_MAX   0xf

typedef struct { BOOLEAN MBp, KBp; LONG M, N, K; } RL_PLL;

static const struct { ULONG In, Out; LONG K, N, M; BOOLEAN MBp, KBp; } s_RlPreset[] = {
    { 19200000,  4096000, 23, 14, 1, FALSE, FALSE },
    { 19200000, 24576000,  3, 30, 3, FALSE, FALSE },
    { 48000000,  3840000, 23,  2, 0, FALSE, FALSE },
    {  3840000, 24576000,  3, 30, 0, TRUE,  FALSE },
    {  3840000, 22579200,  3,  5, 0, TRUE,  FALSE },
};

static ULONG RlDiff(ULONG a, ULONG b) { return a > b ? a - b : b - a; }

static ULONG RlBestDiv(ULONG in, ULONG max, ULONG div)
{
    ULONG d;
    if (in <= max)
        return 1;
    d = in / max;
    if (in % max)
        d++;
    while (div % d != 0)
        d++;
    return d;
}

/* rl6231_pll_calc: Fout = Fin * (N + 2) / ((M + 2) * (K + 2)), M / K optionally bypassed */
static BOOLEAN RlPllCalc(ULONG fin, ULONG fout, RL_PLL *p)
{
    LONG k, n = 0, m = 0, kt, nt, mt, mink, maxk;
    BOOLEAN mbp = FALSE, kbp = FALSE;
    ULONG red, redt, pllout, in_t, out_t, div, fi, fo, i;

    if (fin > RL_PLL_INP_MAX || fin < RL_PLL_INP_MIN || !fout)
        return FALSE;

    for (i = 0; i < ARRAYSIZE(s_RlPreset); i++) {
        if (fin == s_RlPreset[i].In && fout == s_RlPreset[i].Out) {
            k = s_RlPreset[i].K;
            m = s_RlPreset[i].M;
            n = s_RlPreset[i].N;
            mbp = s_RlPreset[i].MBp;
            kbp = s_RlPreset[i].KBp;
            goto found;
        }
    }

    redt = RlDiff(fout, fin);
    mink = (LONG)(80000000 / fout) - 2;
    maxk = (LONG)(150000000 / fout) - 2;
    if (maxk > RL_PLL_K_MAX)
        maxk = RL_PLL_K_MAX;
    if (mink > RL_PLL_K_MAX)
        mink = maxk = RL_PLL_K_MAX;
    div = RlBestDiv(fin, 0xffffffffUL / RL_PLL_N_MAX, CGcd(fin, fout));
    fi = fin / div;
    fo = fout / div;
    k = mink;
    if (mink < -1)
        mink = -1;
    for (kt = mink; kt <= maxk; kt++) {
        for (nt = 0; nt <= RL_PLL_N_MAX; nt++) {
            in_t = fi * (ULONG)(nt + 2);
            pllout = fo * (ULONG)(kt + 2);
            if (in_t == pllout) {
                mbp = TRUE;
                n = nt;
                k = kt;
                goto found;
            }
            out_t = in_t / (ULONG)(kt + 2);
            red = RlDiff(fo, out_t);
            if (red < redt) {
                mbp = TRUE;
                n = nt;
                m = 0;
                k = kt;
                if (red == 0)
                    goto found;
                redt = red;
            }
            for (mt = 0; mt <= RL_PLL_M_MAX; mt++) {
                out_t = in_t / ((ULONG)(mt + 2) * (ULONG)(kt + 2));
                red = RlDiff(fo, out_t);
                if (red < redt) {
                    mbp = FALSE;
                    n = nt;
                    m = mt;
                    k = kt;
                    if (red == 0)
                        goto found;
                    redt = red;
                }
            }
        }
    }
    /* only an approximation */

found:
    if (k == -1) {
        kbp = TRUE;
        k = 0;
    }
    p->MBp = mbp;
    p->KBp = kbp;
    p->M = m;
    p->N = n;
    p->K = k;
    return TRUE;
}

/* the PLL output is exactly fout (sysclk locked to the reference, no ASRC needed) */
static BOOLEAN RlPllExact(ULONG fin, ULONG fout, LONG m, LONG n, LONG k, BOOLEAN mbp, BOOLEAN kbp)
{
    ULONGLONG num = (ULONGLONG)fin * (ULONGLONG)(n + 2);
    ULONGLONG den = (ULONGLONG)(mbp ? 1 : m + 2) * (ULONGLONG)(kbp ? 1 : k + 2);
    return num == (ULONGLONG)fout * den;
}

/* rt5682_div_sel / rt5682s_div_sel: index of the divider giving target from sysclk */
static ULONG DivSel(ULONG sysclk, ULONG target, const ULONG *div, ULONG size)
{
    ULONG i;

    if (sysclk < target)
        return 0;
    for (i = 0; i < size - 1; i++) {
        if (target * div[i] == sysclk)
            return i;
        if (target * div[i + 1] > sysclk)
            return i;
    }
    return size - 1;
}

/* set_filter_clk (both chips): filter clock 256 fs, over sample rate from sysclk */
static void FilterClk(SST_CODEC *c, ULONG sysclk, ULONG fs)
{
    static const ULONG div_f[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48 };
    static const ULONG div_o[] = { 1, 2, 4, 6, 8, 12, 16, 24, 32, 48 };
    ULONG idx = DivSel(sysclk, 256 * fs, div_f, ARRAYSIZE(div_f));
    ULONG osr;

    CUpd(c, R82_PLL_TRACK_2, TRK_FILTER_CLK_DIV_MASK, idx << TRK_FILTER_CLK_DIV_SFT);
    CUpd(c, R82_PLL_TRACK_3, TRK_FILTER_CLK_DIV_MASK, idx << TRK_FILTER_CLK_DIV_SFT);
    for (osr = 0; osr < ARRAYSIZE(div_o) - 1; osr++)
        if (sysclk <= 12288000 * div_o[osr])
            break;
    CUpd(c, R82_ADDA_CLK_1, ADDA_ADC_OSR_MASK | ADDA_DAC_OSR_MASK, (osr << ADDA_ADC_OSR_SFT) | osr);
}

/* set_dmic_clk: DMIC clock closest to 3.072 MHz from sysclk */
static void DmicClk(SST_CODEC *c, ULONG sysclk)
{
    static const ULONG div[] = { 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128 };
    CUpd(c, R82_DMIC_CTRL_1, DMIC_CLK_MASK, DivSel(sysclk, 3072000, div, ARRAYSIZE(div)));
}

/* I2S channel length / data length code: 16, 20, 24, 32 bits -> 0..3 */
static ULONG LenCode(ULONG bits)
{
    if (bits <= 16)
        return 0;
    if (bits <= 20)
        return 1;
    if (bits <= 25)
        return 2;                   /* 25-bit slots (50 fs): closest is 24 */
    return 3;
}

/*
 * DAI format, both chips: I2S, normal polarity, slave (rt5682_set_dai_fmt CBC_CFC), word
 * length from c->Valid (hw_params), channel length as sof_rt5682 set_tdm_slot(0, 0, 2,
 * params_width) for I2S; with c->Slots > 2 TDM is enabled with c->Slot wide slots.
 */
static void DaiFormat(SST_CODEC *c)
{
    ULONG chl, cl, ch = 0;
    BOOLEAN tdm = c->Slots > 2;

    if (tdm) {
        cl = LenCode(c->Slot);
        if (c->Slot != 16 && c->Slot != 20 && c->Slot != 24 && c->Slot != 32)
            LOGE("%s: TDM slot width %u unsupported, using %u\n", c->Drv->Name, c->Slot,
                 cl == 0 ? 16 : cl == 1 ? 20 : cl == 2 ? 24 : 32);
        ch = c->Slots >= 8 ? 3 : c->Slots >= 6 ? 2 : 1;
    } else {
        cl = LenCode(c->Valid);
    }
    chl = (cl << SDP_TX_CHL_SFT) | (cl << SDP_RX_CHL_SFT);
    CUpd(c, R82_I2S1_SDP, SDP_SEL_ADCDAT_IN | SDP_TX_CHL_MASK | SDP_RX_CHL_MASK | SDP_MONO |
         SDP_DL_MASK | SDP_DF_MASK, SDP_SEL_ADCDAT_IN | chl | (LenCode(c->Valid) << SDP_DL_SFT));
    CUpd(c, R82_TDM_TCON_CTRL, TCON_S_BP_INV | TCON_S_LP_INV | TCON_DF_MASK | TCON_CL_MASK |
         TCON_M_BP_INV | TCON_M_LP_INV | TCON_MS_MASTER, cl << TCON_CL_SFT);
    CUpd(c, R82_TDM_CTRL, TDM_TX_CH_MASK | TDM_RX_CH_MASK | TDM_ADC_DL_MASK,
         (ch << TDM_TX_CH_SFT) | (ch << TDM_RX_CH_SFT));
    CUpd(c, R82_TDM_ADDA_CTRL_2, TDM_EN, tdm ? TDM_EN : 0);
}

static BOOLEAN BclkUsable(SST_CODEC *c)
{
    return c->Bclk >= RL_PLL_INP_MIN && c->Bclk <= 40000000;
}

static UCHAR MicMode(SST_CODEC *c)
{
    if (c->Mic <= MIC_HS_FORCED)
        return (UCHAR)c->Mic;
    LOGE("%s: SstCodecMic %u unknown, using the headset mic\n", c->Drv->Name, c->Mic);
    return MIC_HS;
}

/* ================================================================== */
/* RT5682                                                              */
/* ================================================================== */

static BOOLEAN Rt5682Probe(SST_CODEC *c)
{
    ULONG v;

    if (!CRd(c, R82_DEVICE_ID, &v) || v == RT5682S_ID_VAL)
        return FALSE;
    if (v != RT5682_ID_VAL) {
        /* rt5682_i2c_probe writes I2C_MODE = 1 before reading the id */
        CWr(c, R82_I2C_MODE, 1);
        CDelay(10);
        if (!CRd(c, R82_DEVICE_ID, &v))
            return FALSE;
    }
    return v == RT5682_ID_VAL;
}

/* rt5682_reset */
static void R82Reset(SST_CODEC *c)
{
    CWr(c, R82_RESET, 0);
    CWr(c, R82_I2C_MODE, 1);
}

/* rt5682_calibrate (HP DC offset calibration from the internal RC clock) */
static void R82Calibrate(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    ULONG n;

    R82Reset(c);
    CWr(c, R82_I2C_CTRL, 0x000f);
    CWr(c, R82_PWR_ANLG_1, 0xa2af);
    CDelay(20);
    CWr(c, R82_PWR_ANLG_1, 0xf2af);
    CWr(c, R82_MICBIAS_2, 0x0300);
    CWr(c, R82_GLB_CLK, 0x8000);
    CWr(c, R82_PWR_DIG_1, 0x0100);
    CWr(c, R82_HP_IMP_SENS_CTRL_19, 0x3800);
    CWr(c, R82_CHOP_DAC, 0x3000);
    CWr(c, p->Ve ? R82_CHOP_ADC : R82_CALIB_ADC_CTRL, 0x7005);
    CWr(c, R82_STO1_ADC_MIXER, 0x686c);
    CWr(c, R82_CAL_REC, 0x0d0d);
    CWr(c, R82_HP_CALIB_CTRL_2, 0x0321);
    CWr(c, R82_HP_LOGIC_CTRL_2, 0x0004);
    CWr(c, R82_HP_CALIB_CTRL_1, 0x7c00);
    CWr(c, R82_HP_CALIB_CTRL_3, 0x06a1);
    CWr(c, R82_A_DAC1_MUX, 0x0311);
    CWr(c, R82_HP_CALIB_CTRL_1, 0x7c00);
    CWr(c, R82_HP_CALIB_CTRL_1, 0xfc00);
    for (n = 0; n < 60; n++) {
        if (!(CGet(c, R82_HP_CALIB_STA_1) & 0x8000))
            break;
        CDelay(10);
    }
    if (n >= 60)
        LOGE("RT5682: HP calibration failure\n");
    /* restore settings (A_DAC1_MUX stays 0x0311: DAC L1/R1 source = DAC1 MIXL/R, as Linux) */
    CWr(c, R82_PWR_ANLG_1, 0x002f);
    CWr(c, R82_MICBIAS_2, 0x0080);
    CWr(c, R82_GLB_CLK, 0x0000);
    CWr(c, R82_PWR_DIG_1, 0x0000);
    CWr(c, R82_CHOP_DAC, 0x2000);
    CWr(c, p->Ve ? R82_CHOP_ADC : R82_CALIB_ADC_CTRL, 0x2005);
    CWr(c, R82_STO1_ADC_MIXER, 0xc0c4);
    CWr(c, R82_CAL_REC, 0x0c0c);
}

/* rt5682_enable_push_button_irq(false) (I2C variant) */
static void R82ButtonsOff(SST_CODEC *c)
{
    CUpd(c, R82_IRQ_CTRL_3, IRQ3_IL_IRQ_EN, 0);
    CUpd(c, R82_SAR_IL_CMD_1, SAR1_BUTT_DET_EN, 0);
    CUpd(c, R82_4BTN_IL_CMD_2, BTN2_4BTN_IL_EN, 0);
    CUpd(c, R82_4BTN_IL_CMD_2, BTN2_4BTN_IL_NOR, 0);
    CUpd(c, R82_SAR_IL_CMD_13, SAR13_SOUR_MASK, 0);
}

/*
 * Type detection part of rt5682_headset_detect(jack_insert = 1): output switches off,
 * trigger the CBJ type FSM, wait for JACK_TYPE (1 / 2 = headset, 3 = headphone), output
 * switches on.  Then either keep the FSM result or force headset mode.
 */
static void R82TypeDetect(SST_CODEC *c, BOOLEAN detect)
{
    RT82_PRIV *p = PRIV(c);
    ULONG v = 0, n;

    CUpd(c, R82_HP_CHARGE_PUMP_1, HCP_OSW_L | HCP_OSW_R, 0);
    R82ButtonsOff(c);
    /* back to the FSM controlled CBJ of rt5682_set_jack_detect (CBJ_CTRL_1 = 0xd142) */
    CUpd(c, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG, CBJ1_MIC_CAP_HS);
    if (detect) {
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, 0);
        CDelay(60);
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, CBJ1_TRIG_JD);
        v = CGet(c, R82_CBJ_CTRL_2) & CBJ2_JACK_TYPE_MASK;
        for (n = 0; v == 0 && n < 50; n++) {
            CDelay(15);
            v = CGet(c, R82_CBJ_CTRL_2) & CBJ2_JACK_TYPE_MASK;
        }
    }
    p->JackType = (UCHAR)v;
    p->Hs = (v == 1 || v == 2);
    if (!p->Hs) {
        /* forced headset: MIC_CAP = headset from the register, MICBIAS1 register controlled */
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG,
             CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG);
    }
    CUpd(c, R82_HP_CHARGE_PUMP_1, HCP_OSW_L | HCP_OSW_R, HCP_OSW_L | HCP_OSW_R);
}

/* CBJ part of rt5682_set_jack_detect (JD1) and rt5682_headset_detect(jack_insert = 1) */
static void R82JackIn(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);

    CUpd(c, R82_CBJ_CTRL_5, 0x0700, 0x0600);
    CUpd(c, R82_CBJ_CTRL_2, CBJ2_EXT_JD_SRC_MASK, CBJ2_EXT_JD_SRC_MANUAL);
    CWr(c, R82_CBJ_CTRL_1, 0xd142);
    CUpd(c, R82_CBJ_CTRL_3, CBJ3_IN_BUF_EN, CBJ3_IN_BUF_EN);
    CUpd(c, R82_SAR_IL_CMD_1, SAR1_POW_EN, SAR1_POW_EN);

    CUpd(c, R82_PWR_ANLG_1, PA1_VREF2 | PA1_MB, PA1_VREF2 | PA1_MB);
    CUpd(c, R82_PWR_ANLG_1, PA1_FV2, 0);
    CDelay(20);
    CUpd(c, R82_PWR_ANLG_1, PA1_FV2, PA1_FV2);
    CUpd(c, R82_PWR_ANLG_3, PA3_CBJ, PA3_CBJ);
    R82TypeDetect(c, p->Mic == MIC_HS);
    /* headset: Linux also sets FAST_OFF_EN and the button IRQ (both need jack detection) */
    CUpd(c, R82_MICBIAS_2, MB2_PWR_CLK25M | MB2_PWR_CLK1M, MB2_PWR_CLK25M | MB2_PWR_CLK1M);
}

/* rt5682_headset_detect(jack_insert = 0) */
static void R82JackOut(SST_CODEC *c)
{
    R82ButtonsOff(c);
    CUpd(c, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, 0);
    CUpd(c, R82_PWR_ANLG_1, PA1_MB | PA1_VREF2, 0);
    CUpd(c, R82_PWR_ANLG_3, PA3_CBJ, 0);
    CUpd(c, R82_MICBIAS_2, MB2_PWR_CLK25M | MB2_PWR_CLK1M, 0);
    CUpd(c, R82_CBJ_CTRL_1, CBJ1_FAST_OFF_EN, 0);
}

/* rt5682_set_bias_level OFF -> STANDBY -> PREPARE */
static void R82BiasOn(SST_CODEC *c)
{
    CUpd(c, R82_PWR_DIG_1, PD1_DIG_GATE, PD1_DIG_GATE);
    CUpd(c, R82_PWR_ANLG_1, PA1_BG, PA1_BG);
    CUpd(c, R82_PWR_DIG_1, PD1_DIG_GATE | PD1_LDO, PD1_DIG_GATE | PD1_LDO);
}

/* PREPARE -> STANDBY -> OFF */
static void R82BiasOff(SST_CODEC *c)
{
    CUpd(c, R82_PWR_DIG_1, PD1_DIG_GATE | PD1_LDO, 0);
    CUpd(c, R82_PWR_ANLG_1, PA1_BG, 0);
}

/* "Vref1" supply: rt5682_set_verf PRE_PMU (FV1 low), power, POST_PMU (15 ms, FV1) */
static void R82Vref1On(SST_CODEC *c, ULONG more)
{
    CUpd(c, R82_PWR_ANLG_1, PA1_FV1, 0);
    CUpd(c, R82_PWR_ANLG_1, PA1_VREF1 | more, PA1_VREF1 | more);
    CDelay(20);
    CUpd(c, R82_PWR_ANLG_1, PA1_FV1, PA1_FV1);
}

static BOOLEAN R82RecVref(RT82_PRIV *p) { return p->Mic != MIC_DMIC; }  /* MICBIAS1 <- Vref1 */

static void R82PlayUp(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    BOOLEAN rec = c->RecOn;

    /* supplies, DAPM order: register ascending, then subsequences 1 / 2 / 3 */
    CUpd(c, R82_PWR_DIG_1, PD1_I2S1, PD1_I2S1);
    CUpd(c, R82_PWR_DIG_2, PD2_DAC_S1F, PD2_DAC_S1F);
    if (rec && R82RecVref(p))
        CUpd(c, R82_PWR_ANLG_1, PA1_HA_L | PA1_HA_R, PA1_HA_L | PA1_HA_R);
    else
        R82Vref1On(c, PA1_HA_L | PA1_HA_R);             /* Vref1 + HP Amp L/R in one write */
    if (p->Clk != CLK_MCLK)
        CUpd(c, R82_PWR_ANLG_3, PA3_PLL, PA3_PLL);
    CUpd(c, R82_CLK_DET, CLKDET_SYS | CLKDET_POW, CLKDET_SYS | CLKDET_POW);
    if (p->Asrc)
        CUpd(c, R82_PLL_TRACK_1, TRK_DAC_STO1_ASRC | TRK_DA_ASRC | TRK_AD_ASRC,
             TRK_DAC_STO1_ASRC | TRK_DA_ASRC | TRK_AD_ASRC);
    CUpd(c, R82_DEPOP_1, DEP_PUMP_EN, DEP_PUMP_EN);       /* "Charge Pump" */
    CUpd(c, R82_DEPOP_1, DEP_CAPLESS_EN, DEP_CAPLESS_EN); /* "Capless" */
    CUpd(c, R82_CHOP_DAC, CHOP_CKGEN, CHOP_CKGEN);        /* "DAC 1 Clock" */
    CUpd(c, R82_PWR_DIG_1, PD1_DAC_L1 | PD1_DAC_R1, PD1_DAC_L1 | PD1_DAC_R1);
    /* "HP Amp" PRE_PMU (rt5682_hp_event) */
    CUpd(c, R82_HP_CTRL_2, HPC2_DAC_AMP_MUTE, 0);
    CUpd(c, R82_HP_LOGIC_CTRL_2, HPL2_SIG_SOUR2_REG, HPL2_SIG_SOUR2_REG);
    CUpd(c, R82_DEPOP_1, 0x0060, 0x0060);
    CUpd(c, R82_DAC_ADC_DIG_VOL1, 0x00c0, 0x0080);
    CUpd(c, R82_HP_CTRL_2, HPC2_DAC_L_EN | HPC2_DAC_R_EN, HPC2_DAC_L_EN | HPC2_DAC_R_EN);
    CDelay(10);
    CUpd(c, R82_CHARGE_PUMP_1, CP_SW_SIZE_MASK, CP_SW_SIZE_L);
    /* HPOL/HPOR Playback switches (autodisable kcontrols, last) - legacy HPO path */
    CUpd(c, R82_HP_CTRL_1, LR_L_MUTE | LR_R_MUTE, 0);
}

static void R82PlayDown(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    BOOLEAN rec = c->RecOn;

    CUpd(c, R82_HP_CTRL_1, LR_L_MUTE | LR_R_MUTE, LR_L_MUTE | LR_R_MUTE);
    /* "HP Amp" POST_PMD */
    CUpd(c, R82_HP_CTRL_2, HPC2_DAC_L_EN | HPC2_DAC_R_EN, 0);
    CUpd(c, R82_CHARGE_PUMP_1, CP_SW_SIZE_MASK, CP_SW_SIZE_M);
    CUpd(c, R82_DEPOP_1, 0x0060, 0);
    CUpd(c, R82_DAC_ADC_DIG_VOL1, 0x00c0, 0);
    CUpd(c, R82_PWR_DIG_1, PD1_DAC_L1 | PD1_DAC_R1, 0);
    /* supplies: subsequence 3, 2, 1, then 0 by register */
    CUpd(c, R82_CHOP_DAC, CHOP_CKGEN, 0);
    CUpd(c, R82_DEPOP_1, DEP_CAPLESS_EN, 0);
    if (p->Asrc)
        CUpd(c, R82_PLL_TRACK_1, TRK_DAC_STO1_ASRC | (rec ? 0 : TRK_DA_ASRC | TRK_AD_ASRC), 0);
    CUpd(c, R82_DEPOP_1, DEP_PUMP_EN, 0);
    if (!rec)
        CUpd(c, R82_PWR_DIG_1, PD1_I2S1, 0);
    CUpd(c, R82_PWR_DIG_2, PD2_DAC_S1F, 0);
    CUpd(c, R82_PWR_ANLG_1, PA1_HA_L | PA1_HA_R | ((rec && R82RecVref(p)) ? 0 : PA1_VREF1), 0);
    if (!rec && p->Clk != CLK_MCLK)
        CUpd(c, R82_PWR_ANLG_3, PA3_PLL, 0);
    CUpd(c, R82_CLK_DET, CLKDET_SYS | ((rec && p->Asrc) ? 0 : CLKDET_POW), 0);
}

/* type detection again if no headset was found yet (not while playing: OSW toggles) */
static void R82Redetect(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);

    if (p->Mic != MIC_HS || p->Hs || c->PlayOn)
        return;
    R82TypeDetect(c, TRUE);
    LOGI("RT5682: CBJ type %u%s\n", p->JackType, p->Hs ? " (headset)" : ", headset mode forced");
}

static void R82RecUp(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    BOOLEAN play = c->PlayOn;
    ULONG trk = TRK_ADC_STO1_ASRC | TRK_AD_ASRC | TRK_DA_ASRC;

    R82Redetect(c);
    if (p->Mic == MIC_DMIC) {
        CUpd(c, R82_PWR_DIG_1, PD1_I2S1, PD1_I2S1);
        CUpd(c, R82_PWR_DIG_2, PD2_ADC_S1F, PD2_ADC_S1F);
        if (p->Clk != CLK_MCLK)
            CUpd(c, R82_PWR_ANLG_3, PA3_PLL, PA3_PLL);
        if (p->Asrc)
            CUpd(c, R82_CLK_DET, CLKDET_POW, CLKDET_POW);
        /* "DMIC1 Power" + set_dmic_power POST_PMU (Vref2 / MICBIAS with a PLL sysclk) */
        CUpd(c, R82_DMIC_CTRL_1, DMIC1_EN, DMIC1_EN);
        if (p->Clk != CLK_MCLK)
            CUpd(c, R82_PWR_ANLG_1, PA1_VREF2 | PA1_MB, PA1_VREF2 | PA1_MB);
        CDelay(50);
        CUpd(c, R82_PLL_TRACK_1, TRK_DMIC_ASRC | (p->Asrc ? trk : 0), TRK_DMIC_ASRC | (p->Asrc ? trk : 0));
    } else {
        CUpd(c, R82_PWR_DIG_1, PD1_I2S1 | PD1_ADC_L1, PD1_I2S1 | PD1_ADC_L1);
        CUpd(c, R82_PWR_DIG_2, PD2_ADC_S1F, PD2_ADC_S1F);
        if (!play)
            R82Vref1On(c, 0);
        CUpd(c, R82_PWR_ANLG_2, PA2_RM1_L | PA2_MB1, PA2_RM1_L | PA2_MB1);  /* RECMIX1L, MICBIAS1 */
        if (p->Clk != CLK_MCLK)
            CUpd(c, R82_PWR_ANLG_3, PA3_PLL, PA3_PLL);
        if (p->Asrc)
            CUpd(c, R82_CLK_DET, CLKDET_POW, CLKDET_POW);
        CUpd(c, R82_CHOP_ADC, CHOP_CKGEN, CHOP_CKGEN);    /* "ADC1 clock" */
        if (p->Asrc)
            CUpd(c, R82_PLL_TRACK_1, trk, trk);
    }
    CUpd(c, R82_I2S1_SDP, SDP_SEL_ADCDAT_IN, 0);              /* AIF1TX: ADCDAT1 output */
    CUpd(c, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | LR_R_MUTE, 0);  /* Stereo1 ADC MIXL / MIXR */
}

static void R82RecDown(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    BOOLEAN play = c->PlayOn;
    ULONG trk = TRK_ADC_STO1_ASRC | (play ? 0 : TRK_AD_ASRC | TRK_DA_ASRC);

    CUpd(c, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | LR_R_MUTE, LR_L_MUTE | LR_R_MUTE);
    CUpd(c, R82_I2S1_SDP, SDP_SEL_ADCDAT_IN, SDP_SEL_ADCDAT_IN);
    if (p->Mic == MIC_DMIC) {
        CUpd(c, R82_PLL_TRACK_1, TRK_DMIC_ASRC | (p->Asrc ? trk : 0), 0);
        if (!play)
            CUpd(c, R82_PWR_DIG_1, PD1_I2S1, 0);
        CUpd(c, R82_PWR_DIG_2, PD2_ADC_S1F, 0);
        if (!play && p->Clk != CLK_MCLK)
            CUpd(c, R82_PWR_ANLG_3, PA3_PLL, 0);
        if (p->Asrc && !play)
            CUpd(c, R82_CLK_DET, CLKDET_POW, 0);
        CUpd(c, R82_DMIC_CTRL_1, DMIC1_EN, 0);    /* jack in: Vref2 / MICBIAS stay on */
    } else {
        if (p->Asrc)
            CUpd(c, R82_PLL_TRACK_1, trk, 0);
        CUpd(c, R82_PWR_DIG_1, PD1_ADC_L1 | (play ? 0 : PD1_I2S1), 0);
        CUpd(c, R82_PWR_DIG_2, PD2_ADC_S1F, 0);
        if (!play)
            CUpd(c, R82_PWR_ANLG_1, PA1_VREF1, 0);
        CUpd(c, R82_PWR_ANLG_2, PA2_RM1_L | PA2_MB1, 0);
        if (!play && p->Clk != CLK_MCLK)
            CUpd(c, R82_PWR_ANLG_3, PA3_PLL, 0);
        if (p->Asrc && !play)
            CUpd(c, R82_CLK_DET, CLKDET_POW, 0);
        CUpd(c, R82_CHOP_ADC, CHOP_CKGEN, 0);
    }
}

/* sysclk = 512 fs: PLL1 from BCLK1 (preferred), MCLK direct or PLL1 from MCLK */
static BOOLEAN R82Clock(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    ULONG fs = c->Rate ? c->Rate : 48000, target = 512 * fs, glb;
    RL_PLL pll;
    BOOLEAN exact = FALSE, have = FALSE;

    p->Sysclk = target;
    p->Asrc = FALSE;
    p->PllIn = 0;
    if (BclkUsable(c) && RlPllCalc(c->Bclk, target, &pll) &&
        RlPllExact(c->Bclk, target, pll.M, pll.N, pll.K, pll.MBp, pll.KBp)) {
        p->Clk = CLK_BCLK_PLL;
        p->PllIn = c->Bclk;
        have = exact = TRUE;
    } else if (c->Mclk == target) {
        p->Clk = CLK_MCLK;
    } else if (c->Mclk && c->Mclk <= 40000000 && RlPllCalc(c->Mclk, target, &pll)) {
        p->Clk = CLK_MCLK_PLL;
        p->PllIn = c->Mclk;
        have = TRUE;
        exact = RlPllExact(c->Mclk, target, pll.M, pll.N, pll.K, pll.MBp, pll.KBp);
        p->Asrc = !exact || c->Mclk == 24000000;     /* sof_rt5682_codec_init: ASRC for 24 MHz */
    } else if (BclkUsable(c) && RlPllCalc(c->Bclk, target, &pll)) {
        p->Clk = CLK_BCLK_PLL;
        p->PllIn = c->Bclk;
        have = TRUE;
        p->Asrc = TRUE;
    } else {
        LOGE("RT5682: no clock for %u Hz sysclk (BCLK %u, MCLK %u)\n", target, c->Bclk, c->Mclk);
        return FALSE;
    }

    if (have) {
        p->PllN = (USHORT)pll.N;
        p->PllM = (USHORT)pll.M;
        p->PllK = (USHORT)pll.K;
        p->PllMBp = pll.MBp;
        p->PllKBp = pll.KBp;
        /* rt5682_set_component_pll, PLL1: N [15:7], K [4:0]; M [15:12], M bypass, PLL reset */
        CWr(c, R82_PLL_CTRL_1, ((ULONG)pll.N << 7) | (ULONG)pll.K);
        CWr(c, R82_PLL_CTRL_2, ((ULONG)(pll.MBp ? 0 : pll.M) << PLL2C_M_SFT) |
            (pll.MBp ? PLL2C_M_BP : 0) | PLL2C_RST);
        glb = (SCLK_PLL1 << GLB_SCLK_SRC_SFT) |
              (p->Clk == CLK_BCLK_PLL ? GLB_PLL1_SRC_BCLK1 : GLB_PLL1_SRC_MCLK);
    } else {
        glb = SCLK_MCLK << GLB_SCLK_SRC_SFT;
    }
    CUpd(c, R82_GLB_CLK, GLB_SCLK_SRC_MASK | GLB_PLL1_SRC_MASK, glb);
    FilterClk(c, target, fs);
    DmicClk(c, target);
    /* rt5682_sel_asrc_clk_src(DA_STEREO1 | AD_STEREO1, I2S1_ASRC) or system clock */
    CUpd(c, R82_PLL_TRACK_2, TRK_FILTER_CLK_SEL_MASK, p->Asrc ? TRK_FILTER_SEL_I2S1 : 0);
    CUpd(c, R82_PLL_TRACK_3, TRK_FILTER_CLK_SEL_MASK, p->Asrc ? TRK_FILTER_SEL_I2S1 : 0);
    (void)exact;
    return TRUE;
}

static BOOLEAN Rt5682Init(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    ULONG e0 = c->Errors, id = 0, ver = 0, vid = 0, n;

    memset(p, 0, sizeof(*p));
    p->Chip = CHIP_5682;
    p->Mic = MicMode(c);
    c->Caps = 0;

    /* rt5682_i2c_probe */
    CWr(c, R82_I2C_MODE, 1);
    CDelay(10);
    if (!CRd(c, R82_DEVICE_ID, &id) || id != RT5682_ID_VAL) {
        LOGE("RT5682: device id %x\n", id);
        return FALSE;
    }
    CRd(c, R82_VENDOR_ID, &vid);
    CRd(c, R82_VERSION_ID, &ver);
    p->Ve = CGet(c, R82_INT_DEVICE_ID) == RT5682_ID_VE;
    R82Calibrate(c);
    {
        /* rt5682_apply_patch_list */
        static const ULONG patch[] = {
            R82_HP_IMP_SENS_CTRL_19, 0x1000,
            R82_DAC_ADC_DIG_VOL1,    0xa020,
            R82_I2C_CTRL,            0x000f,
            R82_PLL2_INTERNAL,       0x8266,
            R82_SAR_IL_CMD_1,        0x22b7,
            R82_SAR_IL_CMD_3,        0x0365,
            R82_SAR_IL_CMD_6,        0x0110,
            R82_CHARGE_PUMP_1,       0x0210,
            R82_HP_LOGIC_CTRL_2,     0x0007,    /* HPO signal: OneBit */
            R82_SAR_IL_CMD_2,        0xac00,
            R82_CBJ_CTRL_7,          0x0104,
        };
        CSeq(c, patch, ARRAYSIZE(patch) / 2);
    }
    CWr(c, R82_DEPOP_1, 0x0000);
    /* DMIC1 data on GPIO2 (shared with LRCK2), clock on GPIO3 (shared with BCLK2) */
    CUpd(c, R82_DMIC_CTRL_1, DMIC_DP_MASK, 0);
    CUpd(c, R82_GPIO_CTRL_1, GP2_PIN_MASK | GP3_PIN_MASK, GP2_PIN_DMIC_SDA | GP3_PIN_DMIC_CLK);
    CUpd(c, R82_PWR_ANLG_1, PA1_LDO1_DVO_MASK | PA1_HP_DRIVER_MASK, PA1_LDO1_DVO_12 | PA1_HP_DRIVER_5X);
    CWr(c, R82_MICBIAS_2, 0x0080);
    CUpd(c, R82_GPIO_CTRL_1, GP4_PIN_MASK | GP5_PIN_MASK, GP4_PIN_ADCDAT1 | GP5_PIN_DACDAT1);
    CWr(c, R82_TEST_MODE_CTRL_1, 0x0000);
    CUpd(c, R82_BIAS_CUR_CTRL_8, BC8_HPA_CP_BIAS_MASK, BC8_HPA_CP_BIAS_3UA);
    CUpd(c, R82_CHARGE_PUMP_1, CP_CLK_HP_MASK, CP_CLK_HP_300KHZ);
    CUpd(c, R82_HP_CHARGE_PUMP_1, HCP_PM_HP_MASK, HCP_PM_HP_HV);
    CUpd(c, R82_DMIC_CTRL_1, DMIC_FIFO_CLK_DIV_MASK, DMIC_FIFO_CLK_DIV_2);
    /* DAPM initial power-off of widgets whose reset value reads "on": HP Amp L/R */
    CUpd(c, R82_PWR_ANLG_1, PA1_HA_L | PA1_HA_R, 0);

    DaiFormat(c);
    if (!R82Clock(c))
        return FALSE;

    /* playback routing: IF1 -> DAC1 MIXL/R -> DAC L1/R1 (A_DAC1_MUX 0x0311), HP muted */
    CUpd(c, R82_HP_CTRL_1, LR_L_MUTE | LR_R_MUTE, LR_L_MUTE | LR_R_MUTE);
    CWr(c, R82_DAC1_DIG_VOL, 0xafaf);
    /* capture routing and gains */
    CWr(c, R82_STO1_ADC_MIXER, p->Mic == MIC_DMIC ? STO1_ADC_MIX_DMIC : STO1_ADC_MIX_CBJ);
    CUpd(c, R82_REC_MIXER, REC_M_CBJ_RM1_L | REC_G_CBJ_RM1_L_MASK, 0);   /* RECMIX1L CBJ Switch on */
    CUpd(c, R82_CBJ_BST_CTRL, CBJ_BST_MASK, CBJ_BST_24DB);
    CWr(c, R82_STO1_ADC_BOOST, 0x0000);
    CWr(c, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | 0x2f00 | LR_R_MUTE | 0x002f);  /* 0 dB, muted */

    R82JackIn(c);

    n = c->Errors - e0;
    if (n) {
        LOGE("RT5682: %u I2C errors in init\n", n);
        return FALSE;
    }
    c->Caps = CC_PLAY | CC_REC;
    LOGI("RT5682%s (vendor %x ver %x): sysclk %u from %s %u, PLL1 N=%u M=%d K=%u%s, mic %u, CBJ type %u%s\n",
         p->Ve ? " VE" : "", vid, ver, p->Sysclk,
         p->Clk == CLK_BCLK_PLL ? "BCLK1" : "MCLK", p->Clk == CLK_MCLK ? c->Mclk : p->PllIn,
         p->PllN, p->PllMBp ? -1 : (int)p->PllM, p->PllK, p->Asrc ? ", ASRC" : "",
         p->Mic, p->JackType, p->Mic == MIC_DMIC ? "" : p->Hs ? " (headset)" : ", headset mode forced");
    return TRUE;
}

static void Rt5682Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? c->PlayOn : c->RecOn;

    if (on == (rec ? c->RecOn : c->PlayOn))
        return;
    if (on) {
        if (!other)
            R82BiasOn(c);
        if (rec)
            R82RecUp(c);
        else
            R82PlayUp(c);
    } else {
        if (rec)
            R82RecDown(c);
        else
            R82PlayDown(c);
        if (!other)
            R82BiasOff(c);
    }
}

/* DAC1 digital volume: 0xaf = 0 dB in 0.375 dB steps (Linux writes [15:9] / [7:1] with the
 * LSB of the 0xafaf default); mute in the "DAC1 Switch" of DAC1 MIXL / MIXR */
static void Rt5682Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    CWr(c, R82_DAC1_DIG_VOL, (CVolSteps(left, 0xaf) << 8) | CVolSteps(right, 0xaf));
    CUpd(c, R82_AD_DA_MIXER, ADDA_M_DAC1_L | ADDA_M_DAC1_R, mute ? ADDA_M_DAC1_L | ADDA_M_DAC1_R : 0);
}

static void Rt5682Shutdown(SST_CODEC *c)
{
    if (c->PlayOn)
        R82PlayDown(c);
    if (c->RecOn)
        R82RecDown(c);
    if (c->PlayOn || c->RecOn)
        R82BiasOff(c);
    R82JackOut(c);
    R82Reset(c);                /* rt5682_i2c_shutdown */
}

const CODEC_DRV g_DrvRt5682 = { "RT5682", "10EC5682\0", CODEC_HEADSET, 2, 2,
                                Rt5682Probe, Rt5682Init, Rt5682Stream, Rt5682Volume, Rt5682Shutdown };

/* ================================================================== */
/* RT5682S                                                             */
/* ================================================================== */

typedef struct { ULONG In, Out; UCHAR M, K; USHORT N; BOOLEAN MBp, KBp, BypPs, SelPs; } RS_PLL;

/*
 * rt5682s plla_table, plus the BCLK = 48 fs (2.304 MHz) and 50 fs (2.4 MHz) rows added here:
 * 2.304 MHz / 3 * 128 / 4 (VCO 98.304 MHz like the Linux rows), 2.4 MHz / 5 * 256 / 5 (VCO
 * 122.88 MHz; 2.4 MHz cannot reach 98.304 MHz with N <= 511).  Fout = Fin*(N+2)/((M+2)(K+2)).
 */
static const RS_PLL s_PllA[] = {
    {  2048000, 24576000,  0,  2,  46, TRUE,  FALSE, FALSE, FALSE },
    {   256000, 24576000,  0,  2, 382, TRUE,  FALSE, FALSE, FALSE },
    {   512000, 24576000,  0,  2, 190, TRUE,  FALSE, FALSE, FALSE },
    {  4096000, 24576000,  0,  2,  22, TRUE,  FALSE, FALSE, FALSE },
    {  1024000, 24576000,  0,  2,  94, TRUE,  FALSE, FALSE, FALSE },
    { 11289600, 22579200,  1,  2,  22, FALSE, FALSE, FALSE, FALSE },
    {  1411200, 22579200,  0,  2,  62, TRUE,  FALSE, FALSE, FALSE },
    {  2822400, 22579200,  0,  2,  30, TRUE,  FALSE, FALSE, FALSE },
    { 12288000, 24576000,  1,  2,  22, FALSE, FALSE, FALSE, FALSE },
    {  1536000, 24576000,  0,  2,  62, TRUE,  FALSE, FALSE, FALSE },
    {  3072000, 24576000,  0,  2,  30, TRUE,  FALSE, FALSE, FALSE },
    { 24576000, 49152000,  4,  0,  22, FALSE, FALSE, FALSE, FALSE },
    {  3072000, 49152000,  0,  0,  30, TRUE,  FALSE, FALSE, FALSE },
    {  6144000, 49152000,  0,  0,  30, FALSE, FALSE, FALSE, FALSE },
    { 49152000, 98304000, 10,  0,  22, FALSE, TRUE,  FALSE, FALSE },
    {  6144000, 98304000,  0,  0,  30, FALSE, TRUE,  FALSE, FALSE },
    { 12288000, 98304000,  1,  0,  22, FALSE, TRUE,  FALSE, FALSE },
    { 48000000,  3840000, 10, 23,  22, FALSE, FALSE, FALSE, FALSE },
    { 24000000,  3840000,  4, 23,  22, FALSE, FALSE, FALSE, FALSE },
    { 19200000,  3840000,  3, 23,  23, FALSE, FALSE, FALSE, FALSE },
    { 38400000,  3840000,  8, 23,  23, FALSE, FALSE, FALSE, FALSE },
    /* added (not in Linux): BCLK 48 fs / 50 fs at 48 kHz */
    {  2304000, 24576000,  1,  2, 126, FALSE, FALSE, FALSE, FALSE },
    {  2400000, 24576000,  3,  3, 254, FALSE, FALSE, FALSE, FALSE },
};

/* rt5682s pllb_table (MCLK 19.2 / 24 / 38.4 / 48 MHz -> 24.576 / 22.5792 MHz) */
static const RS_PLL s_PllB[] = {
    { 48000000, 24576000,  8,  3,  6, FALSE, FALSE, FALSE, FALSE },
    { 48000000, 22579200, 23,  3, 12, FALSE, FALSE, FALSE, TRUE  },
    { 24000000, 24576000,  3,  3,  6, FALSE, FALSE, FALSE, FALSE },
    { 24000000, 22579200, 23,  3, 26, FALSE, FALSE, FALSE, TRUE  },
    { 19200000, 24576000,  2,  3,  6, FALSE, FALSE, FALSE, FALSE },
    { 19200000, 22579200,  3,  3,  5, FALSE, FALSE, FALSE, TRUE  },
    { 38400000, 24576000,  6,  3,  6, FALSE, FALSE, FALSE, FALSE },
    { 38400000, 22579200,  8,  3,  5, FALSE, FALSE, FALSE, TRUE  },
    {  3840000, 49152000,  0,  0,  6, TRUE,  FALSE, FALSE, FALSE },
};

/* find_pll_inter_combination: PLLA alone, PLLB alone, or PLLA -> 3.84 MHz -> PLLB */
static LONG RsPllFind(ULONG fin, ULONG fout, const RS_PLL **a, const RS_PLL **b)
{
    LONG i, j;

    *a = *b = NULL;
    for (i = 0; i < (LONG)ARRAYSIZE(s_PllA); i++)
        if (s_PllA[i].In == fin && s_PllA[i].Out == fout) {
            *a = &s_PllA[i];
            return USE_PLLA;
        }
    for (i = 0; i < (LONG)ARRAYSIZE(s_PllB); i++)
        if (s_PllB[i].In == fin && s_PllB[i].Out == fout) {
            *b = &s_PllB[i];
            return USE_PLLB;
        }
    for (i = (LONG)ARRAYSIZE(s_PllA) - 1; i >= 0; i--) {
        if (s_PllA[i].In != fin || s_PllA[i].Out != 3840000)
            continue;
        for (j = (LONG)ARRAYSIZE(s_PllB) - 1; j >= 0; j--)
            if (s_PllB[j].In == 3840000 && s_PllB[j].Out == fout) {
                *a = &s_PllA[i];
                *b = &s_PllB[j];
                return USE_PLLAB;
            }
    }
    return -1;
}

static BOOLEAN Rt5682sProbe(SST_CODEC *c)
{
    ULONG v;
    return CRd(c, R82_DEVICE_ID, &v) && v == RT5682S_ID_VAL;
}

static BOOLEAN RsUsesPllA(RT82_PRIV *p) { return p->Clk != CLK_MCLK && p->Comb != USE_PLLB; }
static BOOLEAN RsUsesPllB(RT82_PRIV *p) { return p->Clk != CLK_MCLK && p->Comb != USE_PLLA; }

/* rt5682s_calibrate */
static void RsCalibrate(SST_CODEC *c)
{
    ULONG n;

    CWr(c, R82_PWR_ANLG_1, 0xaa80);
    CDelay(20);
    CWr(c, R82_PWR_ANLG_1, 0xfa80);
    CWr(c, R82_PWR_DIG_1, 0x01c0);
    CWr(c, R82_MICBIAS_2, 0x0380);
    CWr(c, R82_GLB_CLK, 0x8000);
    CWr(c, R82_ADDA_CLK_1, 0x1001);
    CWr(c, R82S_CHOP_DAC_2, 0x3030);
    CWr(c, R82_CHOP_ADC, 0xb000);
    CWr(c, R82_STO1_ADC_MIXER, 0x686c);
    CWr(c, R82_CAL_REC, 0x5151);
    CWr(c, R82_HP_CALIB_CTRL_2, 0x0321);
    CWr(c, R82_HP_LOGIC_CTRL_2, 0x0004);
    CWr(c, R82_HP_CALIB_CTRL_1, 0x7c00);
    CWr(c, R82_HP_CALIB_CTRL_1, 0xfc00);
    for (n = 0; n < 60; n++) {
        if (!(CGet(c, R82S_HP_CALIB_ST_1) & 0x8000))
            break;
        CDelay(10);
    }
    if (n >= 60)
        LOGE("RT5682S: HP calibration failure\n");
    CWr(c, R82_MICBIAS_2, 0x0180);
    CWr(c, R82_CAL_REC, 0x5858);
    CWr(c, R82_STO1_ADC_MIXER, 0xc0c4);
    CWr(c, R82_HP_CALIB_CTRL_2, 0x0320);
    CWr(c, R82_PWR_DIG_1, 0x00c0);
    CWr(c, R82_PWR_ANLG_1, 0x0800);
    CWr(c, R82_GLB_CLK, 0x0000);
}

/* rt5682s_sar_power_mode */
#define SAR_PWR_OFF     0
#define SAR_PWR_NORMAL  1
#define SAR_PWR_SAVING  2

static void RsSarMode(SST_CODEC *c, ULONG mode)
{
    ULONG sar1 = SSAR1_BUTDET_EN | SSAR1_BUTDET_POW_SAV | SSAR1_SEL_MB1_2_AUTO;

    switch (mode) {
    case SAR_PWR_SAVING:
        CUpd(c, R82_CBJ_CTRL_3, CBJ3_IN_BUF_EN, CBJ3_IN_BUF_EN);
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MB2_PATH_REG | SCBJ1_VREF_POW_MASK, 0);
        CUpd(c, R82_SAR_IL_CMD_1, sar1, 0);
        CDelay(6);
        CUpd(c, R82_SAR_IL_CMD_1, SSAR1_BUTDET_EN, SSAR1_BUTDET_EN);
        CDelay(6);
        CUpd(c, R82_SAR_IL_CMD_2, SSAR2_ADC_PSV_ENTRY, SSAR2_ADC_PSV_ENTRY);
        break;
    case SAR_PWR_NORMAL:
        CUpd(c, R82_CBJ_CTRL_3, CBJ3_IN_BUF_EN, CBJ3_IN_BUF_EN);
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MB2_PATH_REG, 0);
        CUpd(c, R82_SAR_IL_CMD_1, SSAR1_SEL_MB1_2_AUTO, SSAR1_SEL_MB1_2_AUTO);
        CDelay(6);
        CUpd(c, R82_SAR_IL_CMD_1, SSAR1_BUTDET_EN | SSAR1_BUTDET_POW_SAV, SSAR1_BUTDET_EN);
        break;
    default:
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MB2_PATH_REG, 0);
        CUpd(c, R82_SAR_IL_CMD_1, sar1, 0);
        break;
    }
}

/* rt5682s_disable_push_button_irq */
static void RsButtonsOff(SST_CODEC *c)
{
    CUpd(c, R82_IRQ_CTRL_3, IRQ3_IL_IRQ_EN, 0);
    CUpd(c, R82_4BTN_IL_CMD_2, BTN2_4BTN_IL_EN, 0);
    CUpd(c, R82_SAR_IL_CMD_13, SAR13_SOUR_MASK, 0);
    CUpd(c, R82_SAR_IL_CMD_1, SSAR1_BUTDET_EN | SSAR1_BUTDET_POW_SAV | SSAR1_SEL_MB1_2_AUTO, 0);
}

/*
 * rt5682s_headset_detect(jack_insert = 1); detect FALSE skips the type FSM (forced mode).
 * Headset: the SAR / inline-command setup of rt5682s_enable_push_button_irq without the
 * IRQ enable, then SAR power saving.  Otherwise the CBJ is forced to headset mode.
 */
static void RsTypeDetect(SST_CODEC *c, BOOLEAN detect)
{
    RT82_PRIV *p = PRIV(c);
    ULONG v = 0, n;

    RsButtonsOff(c);
    CUpd(c, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG, CBJ1_MIC_CAP_HS);
    CUpd(c, R82_PWR_ANLG_1, PA1_VREF1 | PA1_VREF2 | PA1_MB, PA1_VREF1 | PA1_VREF2 | PA1_MB);
    CUpd(c, R82_PWR_ANLG_1, PA1_FV1 | PA1_FV2, 0);
    CDelay(20);
    CUpd(c, R82_PWR_ANLG_1, PA1_FV1 | PA1_FV2, PA1_FV1 | PA1_FV2);
    CUpd(c, R82_PWR_ANLG_3, SA3_CBJ, SA3_CBJ);
    CWr(c, R82_SAR_IL_CMD_3, 0x0365);
    CUpd(c, R82S_HP_CHARGE_PUMP_2, HCP_OSW_L | HCP_OSW_R, 0);
    CUpd(c, R82_SAR_IL_CMD_13, SAR13_SOUR_MASK, 0);
    CUpd(c, R82_CBJ_CTRL_3, CBJ3_IN_BUF_EN, CBJ3_IN_BUF_EN);
    if (detect) {
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, 0);
        CDelay(50);
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, CBJ1_TRIG_JD);
        n = 0;
        do {
            CDelay(15);
            v = CGet(c, R82_CBJ_CTRL_2) & CBJ2_JACK_TYPE_MASK;
            n++;
        } while (v == 0 && n < 50);
    }
    p->JackType = (UCHAR)v;
    p->Hs = (v == 1 || v == 2);
    if (p->Hs) {
        CWr(c, R82_SAR_IL_CMD_3, 0x024c);
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_FAST_OFF_EN, 0);
        CUpd(c, R82_SAR_IL_CMD_1, SSAR1_SEL_MB1_2_MASK, v << SSAR1_SEL_MB1_2_SFT);
        CUpd(c, R82_SAR_IL_CMD_13, SAR13_SOUR_MASK, SSAR13_SOUR_BTN);
        CUpd(c, R82_SAR_IL_CMD_1, SSAR1_BUTDET_EN | SSAR1_BUTDET_POW_SAV | SSAR1_SEL_MB1_2_AUTO,
             SSAR1_BUTDET_EN | SSAR1_SEL_MB1_2_AUTO);
        CWr(c, R82S_IL_CMD_1, 0x0040);
        CUpd(c, R82_4BTN_IL_CMD_2, BTN2_4BTN_IL_EN | BTN2_4BTN_IL_NOR, BTN2_4BTN_IL_EN | BTN2_4BTN_IL_NOR);
        RsSarMode(c, SAR_PWR_SAVING);
    } else {
        CUpd(c, R82_CBJ_CTRL_1, CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG,
             CBJ1_MB1_PATH_REG | CBJ1_MIC_CAP_HS | CBJ1_MIC_CAP_SRC_REG);
    }
    CUpd(c, R82S_HP_CHARGE_PUMP_2, HCP_OSW_L | HCP_OSW_R, HCP_OSW_L | HCP_OSW_R);
    CDelay(40);
}

/* CBJ part of rt5682s_set_jack_detect (JD1), then the jack-in state */
static void RsJackIn(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);

    CUpd(c, R82_CBJ_CTRL_5, SCBJ5_FAST_OFF_SRC_MASK, SCBJ5_FAST_OFF_SRC_JDH);
    CUpd(c, R82_CBJ_CTRL_2, CBJ2_EXT_JD_SRC_MASK, CBJ2_EXT_JD_SRC_MANUAL);
    CUpd(c, R82_CBJ_CTRL_1, CBJ1_EMB_JD_EN | CBJ1_DET_TYPE | SCBJ1_POL_FAST_OFF_MASK | CBJ1_MIC_CAP_HS,
         CBJ1_EMB_JD_EN | CBJ1_DET_TYPE | CBJ1_MIC_CAP_HS);
    CUpd(c, R82_SAR_IL_CMD_1, SSAR1_POW_EN, SSAR1_POW_EN);
    CUpd(c, R82_PWR_ANLG_3, SA3_BGLDO, SA3_BGLDO);
    RsTypeDetect(c, p->Mic == MIC_HS);
}

/* rt5682s_headset_detect(jack_insert = 0) */
static void RsJackOut(SST_CODEC *c)
{
    RsSarMode(c, SAR_PWR_OFF);
    RsButtonsOff(c);
    CUpd(c, R82_CBJ_CTRL_1, CBJ1_TRIG_JD, 0);
    CUpd(c, R82_PWR_ANLG_1, PA1_VREF2 | PA1_MB, 0);
    CUpd(c, R82_PWR_ANLG_3, SA3_CBJ, 0);
    CUpd(c, R82_CBJ_CTRL_1, CBJ1_FAST_OFF_EN, 0);
    CUpd(c, R82_CBJ_CTRL_3, CBJ3_IN_BUF_EN, 0);
}

/* rt5682s_set_bias_level OFF -> STANDBY (DIG_GATE) -> PREPARE (LDO) */
static void RsBiasOn(SST_CODEC *c)
{
    CUpd(c, R82_PWR_DIG_1, PD1_DIG_GATE, PD1_DIG_GATE);
    CUpd(c, R82_PWR_DIG_1, PD1_LDO, PD1_LDO);
}

static void RsBiasOff(SST_CODEC *c)
{
    CUpd(c, R82_PWR_DIG_1, PD1_LDO, 0);
    CUpd(c, R82_PWR_DIG_1, PD1_DIG_GATE, 0);
}

/* rt5682s_set_pllb_power */
static void RsPllB(SST_CODEC *c, BOOLEAN on)
{
    if (on) {
        CUpd(c, R82_PWR_ANLG_3, SA3_LDO_PLLB | SA3_BIAS_PLLB | SA3_PLLB, SA3_LDO_PLLB | SA3_BIAS_PLLB | SA3_PLLB);
        CUpd(c, R82_PWR_ANLG_3, SA3_RSTB_PLLB, SA3_RSTB_PLLB);
    } else {
        CUpd(c, R82_PWR_ANLG_3, SA3_LDO_PLLB | SA3_BIAS_PLLB | SA3_RSTB_PLLB | SA3_PLLB, 0);
    }
}

#define SA3_PLLA_PWR (SA3_LDO_PLLA | SA3_BIAS_PLLA | SA3_PLLA)

/* "SAR" supply (HP Amp and BST1 CBJ): sar_power_event, only with a detected headset */
static BOOLEAN RsRecSar(RT82_PRIV *p) { return p->Mic != MIC_DMIC; }

static void RsPlayUp(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    BOOLEAN rec = c->RecOn;

    /* NOPM supplies in widget order: PLLB, I2S1, SAR */
    if (!rec) {
        if (RsUsesPllB(p))
            RsPllB(c, TRUE);
        CUpd(c, R82_PWR_DIG_1, PD1_I2S1, PD1_I2S1);
    }
    if (p->Hs && !(rec && RsRecSar(p)))
        RsSarMode(c, SAR_PWR_NORMAL);
    CUpd(c, R82_PWR_DIG_2, PD2_DAC_S1F, PD2_DAC_S1F);
    CUpd(c, R82_PWR_ANLG_2, PA2_MCLK0_WD, PA2_MCLK0_WD);    /* "MCLK0 DET PWR" */
    if (!rec && RsUsesPllA(p))
        CUpd(c, R82_PWR_ANLG_3, SA3_PLLA_PWR, SA3_PLLA_PWR);
    CUpd(c, R82_CLK_DET, CLKDET_SYS, CLKDET_SYS);
    /* subsequence 1 */
    if (!rec && RsUsesPllA(p))
        CUpd(c, R82_PWR_ANLG_3, SA3_RSTB_PLLA, SA3_RSTB_PLLA);
    if (p->Asrc)
        CUpd(c, R82_PLL_TRACK_1, TRK_DAC_STO1_ASRC | TRK_DA_ASRC | TRK_AD_ASRC,
             TRK_DAC_STO1_ASRC | TRK_DA_ASRC | TRK_AD_ASRC);
    CUpd(c, R82_PWR_DIG_1, PD1_DAC_L1 | PD1_DAC_R1, PD1_DAC_L1 | PD1_DAC_R1);
    /* "HP Amp" POST_PMU (rt5682s_hp_amp_event) */
    CUpd(c, R82S_DEPOP_1, SDEP_OUT_HP_L | SDEP_OUT_HP_R, SDEP_OUT_HP_L | SDEP_OUT_HP_R);
    CDelay(20);
    CUpd(c, R82S_DEPOP_1, SDEP_LDO_PUMP | SDEP_PUMP | SDEP_CAPLESS_L | SDEP_CAPLESS_R,
         SDEP_LDO_PUMP | SDEP_PUMP | SDEP_CAPLESS_L | SDEP_CAPLESS_R);
    CWr(c, R82S_BIAS_CUR_CTRL_11, 0x6666);
    CWr(c, R82S_BIAS_CUR_CTRL_12, 0xa82a);
    CUpd(c, R82_HP_CTRL_2, SHPC2_HPO_L_PATH | SHPC2_HPO_R_PATH | SHPC2_IP_EN_GATING,
         SHPC2_HPO_L_PATH | SHPC2_HPO_R_PATH | SHPC2_IP_EN_GATING);
    CDelay(10);
    CUpd(c, R82S_HP_AMP_DET_CTL_1, SAMP_CP_SW_SIZE_MASK, SAMP_CP_SW_SIZE_L | SAMP_CP_SW_SIZE_S);
}

static void RsPlayDown(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    BOOLEAN rec = c->RecOn;

    /* "HP Amp" POST_PMD */
    CUpd(c, R82_HP_CTRL_2, SHPC2_HPO_L_PATH | SHPC2_HPO_R_PATH | SHPC2_IP_EN_GATING, 0);
    CUpd(c, R82S_HP_AMP_DET_CTL_1, SAMP_CP_SW_SIZE_MASK, SAMP_CP_SW_SIZE_M);
    CUpd(c, R82S_DEPOP_1, SDEP_LDO_PUMP | SDEP_PUMP | SDEP_CAPLESS_L | SDEP_CAPLESS_R, 0);
    CUpd(c, R82S_DEPOP_1, SDEP_OUT_HP_L | SDEP_OUT_HP_R, 0);
    CUpd(c, R82_PWR_DIG_1, PD1_DAC_L1 | PD1_DAC_R1, 0);
    /* subsequence 1, then 0 */
    if (!rec && RsUsesPllA(p))
        CUpd(c, R82_PWR_ANLG_3, SA3_RSTB_PLLA, 0);
    if (p->Asrc)
        CUpd(c, R82_PLL_TRACK_1, TRK_DAC_STO1_ASRC | (rec ? 0 : TRK_DA_ASRC | TRK_AD_ASRC), 0);
    if (!rec) {
        if (RsUsesPllB(p))
            RsPllB(c, FALSE);
        CUpd(c, R82_PWR_DIG_1, PD1_I2S1, 0);
    }
    if (p->Hs && !(rec && RsRecSar(p)))
        RsSarMode(c, SAR_PWR_SAVING);
    CUpd(c, R82_PWR_DIG_2, PD2_DAC_S1F, 0);
    CUpd(c, R82_PWR_ANLG_2, PA2_MCLK0_WD, 0);
    if (!rec && RsUsesPllA(p))
        CUpd(c, R82_PWR_ANLG_3, SA3_PLLA_PWR, 0);
    CUpd(c, R82_CLK_DET, CLKDET_SYS, 0);
}

static void RsRedetect(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);

    if (p->Mic != MIC_HS || p->Hs || c->PlayOn)
        return;
    RsTypeDetect(c, TRUE);
    LOGI("RT5682S: CBJ type %u%s\n", p->JackType, p->Hs ? " (headset)" : ", headset mode forced");
}

static void RsRecUp(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    BOOLEAN play = c->PlayOn, plla = !play && RsUsesPllA(p);
    ULONG trk = p->Asrc ? TRK_ADC_STO1_ASRC | TRK_AD_ASRC | TRK_DA_ASRC : 0;

    RsRedetect(c);
    if (!play) {
        if (RsUsesPllB(p))
            RsPllB(c, TRUE);
        CUpd(c, R82_PWR_DIG_1, PD1_I2S1, PD1_I2S1);
    }
    if (p->Mic == MIC_DMIC) {
        CUpd(c, R82_PWR_DIG_2, PD2_ADC_S1F, PD2_ADC_S1F);
        if (plla)
            CUpd(c, R82_PWR_ANLG_3, SA3_PLLA_PWR, SA3_PLLA_PWR);
        CUpd(c, R82_DMIC_CTRL_1, DMIC1_EN, DMIC1_EN);   /* + set_dmic_power POST_PMU */
        if (p->Clk != CLK_MCLK)
            CUpd(c, R82_PWR_ANLG_1, PA1_VREF2 | PA1_MB, PA1_VREF2 | PA1_MB);
        CDelay(50);
        if (plla)
            CUpd(c, R82_PWR_ANLG_3, SA3_RSTB_PLLA, SA3_RSTB_PLLA);
        CUpd(c, R82_PLL_TRACK_1, TRK_DMIC_ASRC | trk, TRK_DMIC_ASRC | trk);
    } else {
        if (p->Hs && !play)
            RsSarMode(c, SAR_PWR_NORMAL);
        CUpd(c, R82_CAL_REC, SCAL_PWR_RM1_L, SCAL_PWR_RM1_L);    /* "RECMIX1L Power" */
        CUpd(c, R82_PWR_DIG_1, PD1_ADC_L1, PD1_ADC_L1);
        CUpd(c, R82_PWR_DIG_2, PD2_ADC_S1F, PD2_ADC_S1F);
        CUpd(c, R82_PWR_ANLG_2, PA2_MB1, PA2_MB1);                /* MICBIAS1 */
        CUpd(c, R82_PWR_ANLG_3, SA3_LDO_MB1 | (plla ? SA3_PLLA_PWR : 0), SA3_LDO_MB1 | (plla ? SA3_PLLA_PWR : 0));
        CUpd(c, R82_CHOP_ADC, CHOP_CKGEN, CHOP_CKGEN);            /* "ADC1 clock" */
        if (plla)
            CUpd(c, R82_PWR_ANLG_3, SA3_RSTB_PLLA, SA3_RSTB_PLLA);
        if (trk)
            CUpd(c, R82_PLL_TRACK_1, trk, trk);
    }
    CUpd(c, R82_I2S1_SDP, SDP_SEL_ADCDAT_IN, 0);
    CUpd(c, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | LR_R_MUTE, 0);  /* MIXL POST_PMU event, MIXR */
}

static void RsRecDown(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    BOOLEAN play = c->PlayOn, plla = !play && RsUsesPllA(p);
    ULONG trk = p->Asrc ? TRK_ADC_STO1_ASRC | (play ? 0 : TRK_AD_ASRC | TRK_DA_ASRC) : 0;

    CUpd(c, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | LR_R_MUTE, LR_L_MUTE | LR_R_MUTE);
    CUpd(c, R82_I2S1_SDP, SDP_SEL_ADCDAT_IN, SDP_SEL_ADCDAT_IN);
    if (plla)
        CUpd(c, R82_PWR_ANLG_3, SA3_RSTB_PLLA, 0);
    if (p->Mic == MIC_DMIC)
        CUpd(c, R82_PLL_TRACK_1, TRK_DMIC_ASRC | trk, 0);
    else if (trk)
        CUpd(c, R82_PLL_TRACK_1, trk, 0);
    if (!play) {
        if (RsUsesPllB(p))
            RsPllB(c, FALSE);
        CUpd(c, R82_PWR_DIG_1, PD1_I2S1, 0);
    }
    if (p->Mic == MIC_DMIC) {
        CUpd(c, R82_PWR_DIG_2, PD2_ADC_S1F, 0);
        if (plla)
            CUpd(c, R82_PWR_ANLG_3, SA3_PLLA_PWR, 0);
        CUpd(c, R82_DMIC_CTRL_1, DMIC1_EN, 0);
    } else {
        if (p->Hs && !play)
            RsSarMode(c, SAR_PWR_SAVING);
        CUpd(c, R82_CAL_REC, SCAL_PWR_RM1_L, 0);
        CUpd(c, R82_PWR_DIG_1, PD1_ADC_L1, 0);
        CUpd(c, R82_PWR_DIG_2, PD2_ADC_S1F, 0);
        CUpd(c, R82_PWR_ANLG_2, PA2_MB1, 0);
        CUpd(c, R82_PWR_ANLG_3, SA3_LDO_MB1 | (plla ? SA3_PLLA_PWR : 0), 0);
        CUpd(c, R82_CHOP_ADC, CHOP_CKGEN, 0);
    }
}

/* sysclk = 512 fs: PLLA from BCLK1 (preferred), MCLK direct, or PLLA / PLLB from MCLK */
static BOOLEAN RsClock(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    ULONG fs = c->Rate ? c->Rate : 48000, target = 512 * fs, src;
    const RS_PLL *a = NULL, *b = NULL;
    LONG comb = -1;

    p->Sysclk = target;
    p->Asrc = FALSE;
    p->PllIn = 0;
    if (BclkUsable(c) && (comb = RsPllFind(c->Bclk, target, &a, &b)) >= 0) {
        p->Clk = CLK_BCLK_PLL;
        p->PllIn = c->Bclk;
    } else if (c->Mclk == target) {
        p->Clk = CLK_MCLK;
    } else if (c->Mclk && (comb = RsPllFind(c->Mclk, target, &a, &b)) >= 0) {
        p->Clk = CLK_MCLK_PLL;
        p->PllIn = c->Mclk;
        p->Asrc = c->Mclk == 24000000;      /* sof_rt5682_codec_init */
    } else {
        LOGE("RT5682S: no PLL table entry for %u Hz sysclk (BCLK %u, MCLK %u)\n", target, c->Bclk, c->Mclk);
        return FALSE;
    }

    src = SCLK_MCLK;
    if (p->Clk != CLK_MCLK) {
        p->Comb = (UCHAR)comb;
        CUpd(c, R82_GLB_CLK, GLB_PLL1_SRC_MASK, p->Clk == CLK_BCLK_PLL ? GLB_PLL1_SRC_BCLK1 : GLB_PLL1_SRC_MCLK);
        if (a) {
            CUpd(c, R82S_PLL_CTRL_1, 0x01ff, a->N);
            CUpd(c, R82S_PLL_CTRL_2, 0x1f1f, ((ULONG)a->M << 8) | a->K);
            CUpd(c, R82S_PLL_CTRL_6, SPLL6_PLLA_M_BP | SPLL6_PLLA_K_BP,
                 (a->MBp ? SPLL6_PLLA_M_BP : 0) | (a->KBp ? SPLL6_PLLA_K_BP : 0));
            p->PllN = a->N;
            p->PllM = a->M;
            p->PllK = a->K;
            p->PllMBp = a->MBp;
            p->PllKBp = a->KBp;
        }
        if (b) {
            CUpd(c, R82S_PLL_CTRL_3, 0x03ff, b->N);
            CUpd(c, R82S_PLL_CTRL_4, 0x1f1f, ((ULONG)b->M << 8) | b->K);
            CUpd(c, R82S_PLL_CTRL_6, SPLL6_PLLB_SEL_PS | SPLL6_PLLB_BYP_PS | SPLL6_PLLB_M_BP | SPLL6_PLLB_K_BP,
                 (b->SelPs ? SPLL6_PLLB_SEL_PS : 0) | (b->BypPs ? SPLL6_PLLB_BYP_PS : 0) |
                 (b->MBp ? SPLL6_PLLB_M_BP : 0) | (b->KBp ? SPLL6_PLLB_K_BP : 0));
            if (!a) {
                p->PllN = b->N;
                p->PllM = b->M;
                p->PllK = b->K;
                p->PllMBp = b->MBp;
                p->PllKBp = b->KBp;
            }
        }
        if (comb == USE_PLLB)
            CUpd(c, R82S_PLL_CTRL_7, SPLL7_PLLB_SRC_DFIN, SPLL7_PLLB_SRC_DFIN);
        src = comb == USE_PLLA ? SCLK_PLL1 : SCLK_PLL2;
    }
    /* rt5682s_set_component_sysclk */
    CUpd(c, R82_GLB_CLK, GLB_SCLK_SRC_MASK, src << GLB_SCLK_SRC_SFT);
    CUpd(c, R82_ADDA_CLK_1, SADDA_I2S_M_CLK_SRC_MASK, src << SADDA_I2S_M_CLK_SRC_SFT);
    CUpd(c, R82S_I2S2_M_CLK_CTRL_1, SADDA_I2S_M_CLK_SRC_MASK, src << SADDA_I2S_M_CLK_SRC_SFT);
    FilterClk(c, target, fs);
    DmicClk(c, target);
    CUpd(c, R82_PLL_TRACK_2, TRK_FILTER_CLK_SEL_MASK, p->Asrc ? TRK_FILTER_SEL_I2S1 : 0);
    CUpd(c, R82_PLL_TRACK_3, TRK_FILTER_CLK_SEL_MASK, p->Asrc ? TRK_FILTER_SEL_I2S1 : 0);
    if (p->Asrc)
        CUpd(c, R82S_PLL_TRACK_11, STRK11_ASRCIN_AUTO_CLKOUT, STRK11_ASRCIN_AUTO_CLKOUT);
    return TRUE;
}

static BOOLEAN Rt5682sInit(SST_CODEC *c)
{
    RT82_PRIV *p = PRIV(c);
    ULONG e0 = c->Errors, id = 0, ver = 0, n;

    memset(p, 0, sizeof(*p));
    p->Chip = CHIP_5682S;
    p->Mic = MicMode(c);
    c->Caps = 0;

    /* rt5682s_i2c_probe */
    if (!CRd(c, R82_DEVICE_ID, &id) || id != RT5682S_ID_VAL) {
        LOGE("RT5682S: device id %x\n", id);
        return FALSE;
    }
    CRd(c, R82_VERSION_ID, &ver);
    CWr(c, R82_RESET, 0);
    {
        /* rt5682s patch_list */
        static const ULONG patch[] = {
            R82_I2C_CTRL,               0x0007,
            R82S_DIG_IN_CTRL_1,         0x0000,
            R82S_CHOP_DAC_2,            0x2020,
            R82S_VREF_REC_OP_FB_CAP_2,  0x0101,
            R82S_VREF_REC_OP_FB_CAP_1,  0x80c0,
            R82S_HP_CALIB_CTRL_9,       0x0002,
            R82S_DEPOP_1,               0x0000,
            R82S_HP_CHARGE_PUMP_2,      0x3c15,
            R82_DAC1_DIG_VOL,           0xfefe,
            R82_SAR_IL_CMD_2,           0xac00,
            R82_SAR_IL_CMD_3,           0x024c,
            R82S_CBJ_CTRL_6,            0x0804,
        };
        CSeq(c, patch, ARRAYSIZE(patch) / 2);
    }
    CUpd(c, R82_PWR_DIG_2, PD2_DLDO_I_LIMIT, 0);
    CDelay(25);
    RsCalibrate(c);
    CUpd(c, R82_MICBIAS_2, MB2_PWR_CLK25M | MB2_PWR_CLK1M, MB2_PWR_CLK1M);
    CUpd(c, R82_PWR_ANLG_1, PA1_BG, PA1_BG);
    CUpd(c, R82_HP_LOGIC_CTRL_2, SHPL2_SIG_SRC_MASK, SHPL2_SIG_SRC_1BIT);
    CUpd(c, R82S_HP_CHARGE_PUMP_2, HCP_PM_HP_MASK, HCP_PM_HP_HV);
    CUpd(c, R82S_HP_AMP_DET_CTL_1, SAMP_CP_SW_SIZE_MASK, SAMP_CP_SW_SIZE_M);
    /* DMIC1 data on GPIO2, clock on GPIO3 (default platform data) */
    CUpd(c, R82_DMIC_CTRL_1, DMIC_DP_MASK, 0);
    CUpd(c, R82_GPIO_CTRL_1, GP2_PIN_MASK | GP3_PIN_MASK, GP2_PIN_DMIC_SDA | GP3_PIN_DMIC_CLK);

    DaiFormat(c);               /* also the DAPM initial AIF1TX power-off (SEL_ADCDAT = input) */
    if (!RsClock(c))
        return FALSE;

    /* playback: IF1 -> DAC1 MIXL/R -> Stereo1 DAC mixer (reset default routing) -> HP */
    CWr(c, R82_DAC1_DIG_VOL, 0xfefe);
    /* capture: ADC1 L on both channels, RECMIX1L CBJ Switch on, CBJ boost +24 dB */
    CWr(c, R82_STO1_ADC_MIXER, p->Mic == MIC_DMIC ? STO1_ADC_MIX_DMIC : STO1_ADC_MIX_CBJ);
    CUpd(c, R82_REC_MIXER, SREC_BST_CBJ_MASK | REC_M_CBJ_RM1_L, SREC_BST_CBJ_24DB);
    CWr(c, R82_STO1_ADC_BOOST, 0x0000);
    CWr(c, R82_STO1_ADC_DIG_VOL, LR_L_MUTE | 0x2f00 | LR_R_MUTE | 0x002f);

    RsJackIn(c);

    n = c->Errors - e0;
    if (n) {
        LOGE("RT5682S: %u I2C errors in init\n", n);
        return FALSE;
    }
    c->Caps = CC_PLAY | CC_REC;
    LOGI("RT5682S (ver %x): sysclk %u from %s %u, %s N=%u M=%d K=%d, mic %u, CBJ type %u%s\n",
         ver, p->Sysclk, p->Clk == CLK_BCLK_PLL ? "BCLK1" : "MCLK", p->Clk == CLK_MCLK ? c->Mclk : p->PllIn,
         p->Clk == CLK_MCLK ? "no PLL" : p->Comb == USE_PLLA ? "PLLA" : p->Comb == USE_PLLB ? "PLLB" : "PLLA+B",
         p->PllN, p->PllMBp ? -1 : (int)p->PllM, p->PllKBp ? -1 : (int)p->PllK, p->Mic, p->JackType,
         p->Mic == MIC_DMIC ? "" : p->Hs ? " (headset)" : ", headset mode forced");
    if (p->Asrc)
        LOGI("RT5682S: ASRC on\n");
    return TRUE;
}

static void Rt5682sStream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    BOOLEAN other = rec ? c->PlayOn : c->RecOn;

    if (on == (rec ? c->RecOn : c->PlayOn))
        return;
    if (on) {
        if (!other)
            RsBiasOn(c);
        if (rec)
            RsRecUp(c);
        else
            RsPlayUp(c);
    } else {
        if (rec)
            RsRecDown(c);
        else
            RsPlayDown(c);
        if (!other)
            RsBiasOff(c);
    }
}

/* DAC1 digital volume 0.375 dB steps, top 0xfe (the patch default, -0.375 dB per the Linux
 * TLV; Linux never sets the LSB); mute in the DAC1 Switch of DAC1 MIXL / MIXR */
static void Rt5682sVolume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    CWr(c, R82_DAC1_DIG_VOL, (CVolSteps(left, 0xfe) << 8) | CVolSteps(right, 0xfe));
    CUpd(c, R82_AD_DA_MIXER, ADDA_M_DAC1_L | ADDA_M_DAC1_R, mute ? ADDA_M_DAC1_L | ADDA_M_DAC1_R : 0);
}

static void Rt5682sShutdown(SST_CODEC *c)
{
    if (c->PlayOn)
        RsPlayDown(c);
    if (c->RecOn)
        RsRecDown(c);
    if (c->PlayOn || c->RecOn)
        RsBiasOff(c);
    RsJackOut(c);
    CWr(c, R82_RESET, 0);       /* rt5682s_i2c_shutdown */
}

const CODEC_DRV g_DrvRt5682s = { "RT5682S", "RTL5682\0", CODEC_HEADSET, 2, 2,
                                 Rt5682sProbe, Rt5682sInit, Rt5682sStream, Rt5682sVolume, Rt5682sShutdown };
