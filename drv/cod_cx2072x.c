#include "codec.h"

/*
 * Conexant CX20721 / CX20723 (ACPI 14F10720, Bay Trail / Cherry Trail, machine driver
 * sound/soc/intel/boards/bytcht_cx2072x.c).  Ported from Linux sound/soc/codecs/cx2072x.c/h.
 *
 * I2C: 16-bit register address (big endian, the HDA NID/verb layout: NID << 10 | verb), values
 * LITTLE endian with a per-register width (cx2072x_register_size): most registers are 1 byte, the
 * PLL / test / converter-format registers 2 bytes, the I2S-PCM controls, ids and BIOS test registers
 * 4 bytes.  The core accessors (big-endian, fixed width) do not fit, so every access goes through
 * CRaw here; the descriptor's 2/4 widths are nominal.  As in Linux, a write is truncated to the
 * register's width (e.g. CODEC_TESTXX / CODEC_TEST24 are 1-byte registers).
 *
 * Clocking: the codec PLL (ANALOG_TEST4/6/7/8, pre-divider + integer/fractional feedback, PLL
 * tracking in DIGITAL_TEST16..20) is referenced to MCLK; Linux documents no BCLK reference for it
 * (DIGITAL_BIOS_TEST2 has pll_source / pll_ref_clock bits whose meaning is not public), so MCLK is
 * used.  With c->Mclk == 0 we assume 19.2 MHz: bytcht_cx2072x.c programs exactly that
 * (pmc_plt_clk_3, the only platform this codec ships on).  PLL output = 3072 * fs (147.456 MHz).
 *
 * DAI: I2S slave; the frame length is c->Slots * c->Slot (Bay Trail SSP2 uses 50 = 2 x 25 bit,
 * set_bclk_ratio(50) in the machine driver), handled with the "pause" cycles of I2SPCM_CONTROL6.
 * Bit fields Linux leaves uninitialised in its I2SPCM_CONTROL2/3 unions (slot 3/4, tri_n,
 * neg_bclk) are 0 here; only I2S channels 1/2 (ADC1 L/R out, DAC1 L/R in) are enabled.
 *
 * Outputs: PortA = headphone (HP amp, pin ctrl 0xc0), PortG = class-D speaker, both from DAC1;
 * both are powered whenever playback runs.  PortE / PortM are not wired on the Intel designs and
 * stay off.  No GPIO is driven: the device's only GpioIo/GpioInt is the jack-detect line
 * ("headset-gpios" in bytcht_cx2072x.c).
 *
 * Microphone (ADC1, stereo converter, mono pin on both channels), c->Mic:
 *   0 (default): PortD, the headset mic ("Headset Mic" in bytcht_cx2072x.c), Headset Bias
 *                (ANALOG_TEST11 bit 1) and PortD VRef (pin ctrl bit 2) on while capturing.
 *   1:           PortC, the internal mic ("Int Mic").
 *   Gain: port boost +24 dB (PORTx_GAIN 2 x 12 dB), ADC1 amp 0 dB (0x4a), HPF 30 Hz (reg init).
 *
 * Amplifier gain registers follow the HDA format: bit 7 = mute, bits 6:0 = gain, 0x4a = 0 dB,
 * 1 dB per step down to 0 = -74 dB.
 */

#define CX_VENDOR_ID                0x0200
#define CX_REVISION_ID              0x0208
#define CX_AFG_POWER_STATE          0x0414
#define CX_UM_RESPONSE              0x0420
#define CX_GPIO_ENABLE              0x0458
#define CX_GPIO_UM_ENABLE           0x0464
#define CX_DAC1_AMP_GAIN_RIGHT      0x41c0
#define CX_DAC1_AMP_GAIN_LEFT       0x41e0
#define CX_DAC1_POWER_STATE         0x4014
#define CX_DAC1_CONVERTER_FORMAT    0x43c8
#define CX_DAC2_CONVERTER_FORMAT    0x47c8
#define CX_DAC2_POWER_STATE         0x4414
#define CX_ADC1_AMP_GAIN_RIGHT_0    0x4d80      /* + 4 * input index */
#define CX_ADC1_AMP_GAIN_LEFT_0     0x4da0
#define CX_ADC1_CONNECTION_SELECT   0x4c04
#define CX_ADC1_POWER_STATE         0x4c14
#define CX_ADC1_CONVERTER_FORMAT    0x4fc8
#define CX_ADC2_CONVERTER_FORMAT    0x53c8
#define CX_ADC2_POWER_STATE         0x5014
#define CX_PORTA_CONNECTION_SELECT  0x5804
#define CX_PORTA_POWER_STATE        0x5814
#define CX_PORTA_PIN_CTRL           0x581c
#define CX_PORTA_PIN_SENSE          0x5824
#define CX_PORTB_PIN_SENSE          0x6024
#define CX_PORTC_POWER_STATE        0x6814
#define CX_PORTC_PIN_CTRL           0x681c
#define CX_PORTC_GAIN_RIGHT         0x6980
#define CX_PORTC_GAIN_LEFT          0x69a0
#define CX_PORTD_POWER_STATE        0x6414
#define CX_PORTD_PIN_CTRL           0x641c
#define CX_PORTD_PIN_SENSE          0x6424
#define CX_PORTD_GAIN_RIGHT         0x6580
#define CX_PORTD_GAIN_LEFT          0x65a0
#define CX_PORTE_PIN_SENSE          0x7424
#define CX_PORTF_PIN_SENSE          0x7824
#define CX_PORTG_POWER_STATE        0x5c14
#define CX_PORTG_PIN_CTRL           0x5c1c
#define CX_PORTG_CONNECTION_SELECT  0x5c04
#define CX_MIXER_POWER_STATE        0x5414
#define CX_EQ_ENABLE_BYPASS         0x6d00
#define CX_EQ_B0_COEFF              0x6d02
#define CX_EQ_B1_COEFF              0x6d04
#define CX_EQ_B2_COEFF              0x6d06
#define CX_EQ_A1_COEFF              0x6d08
#define CX_EQ_A2_COEFF              0x6d0a
#define CX_EQ_G_COEFF               0x6d0c
#define CX_SPKR_DRC_CONTROL         0x6d14
#define CX_SPKR_DRC_TEST            0x6d18
#define CX_DIGITAL_BIOS_TEST0       0x6d80
#define CX_DIGITAL_BIOS_TEST2       0x6d84
#define CX_I2SPCM_CONTROL1          0x6e00
#define CX_I2SPCM_CONTROL2          0x6e04
#define CX_I2SPCM_CONTROL3          0x6e08
#define CX_I2SPCM_CONTROL4          0x6e0c
#define CX_I2SPCM_CONTROL5          0x6e10
#define CX_UM_INTERRUPT_CRTL_E      0x6e14
#define CX_I2SPCM_CONTROL6          0x6e18
#define CX_CODEC_TEST2              0x7108
#define CX_CODEC_TEST9              0x7124
#define CX_CODEC_TESTXX             0x7290
#define CX_CODEC_TEST20             0x7310
#define CX_CODEC_TEST24             0x731c
#define CX_CODEC_TEST26             0x7328
#define CX_ANALOG_TEST3             0x718c
#define CX_ANALOG_TEST4             0x7190
#define CX_ANALOG_TEST5             0x7194
#define CX_ANALOG_TEST6             0x7198
#define CX_ANALOG_TEST7             0x719c
#define CX_ANALOG_TEST8             0x71a0
#define CX_ANALOG_TEST9             0x71a4
#define CX_ANALOG_TEST10            0x71a8
#define CX_ANALOG_TEST11            0x71ac
#define CX_ANALOG_TEST12            0x71b0
#define CX_ANALOG_TEST13            0x71b4
#define CX_DIGITAL_TEST0            0x7200
#define CX_DIGITAL_TEST1            0x7204
#define CX_DIGITAL_TEST11           0x722c
#define CX_DIGITAL_TEST12           0x7230
#define CX_DIGITAL_TEST15           0x723c
#define CX_DIGITAL_TEST16           0x7080
#define CX_DIGITAL_TEST17           0x7084
#define CX_DIGITAL_TEST18           0x7088
#define CX_DIGITAL_TEST19           0x708c
#define CX_DIGITAL_TEST20           0x7090

#define CX_PS_D0        0x00            /* power state writes: target D0 / D3 (bits 7:4 = actual, r/o) */
#define CX_PS_D3        0x03
#define CX_AMP_MUTE     0x80
#define CX_AMP_0DB      0x4a
#define CX_PIN_HP       0x80
#define CX_PIN_OUT      0x40
#define CX_PIN_IN       0x20
#define CX_PIN_VREF     0x04            /* "PortD Mic Bias" */
#define CX_HS_BIAS      0x02            /* ANALOG_TEST11: "Headset Bias" */

#define BITS_PER_SLOT   8

/* c->Priv */
#define P_MUTE      0
#define P_PORT      1                   /* mic port power-state register */
#define P_MCLK      2

/* cx2072x_register_size */
static ULONG CxSize(ULONG reg)
{
    switch (reg) {
    case CX_VENDOR_ID: case CX_REVISION_ID:
    case CX_PORTA_PIN_SENSE: case CX_PORTB_PIN_SENSE: case CX_PORTD_PIN_SENSE:
    case CX_PORTE_PIN_SENSE: case CX_PORTF_PIN_SENSE:
    case CX_I2SPCM_CONTROL1: case CX_I2SPCM_CONTROL2: case CX_I2SPCM_CONTROL3:
    case CX_I2SPCM_CONTROL4: case CX_I2SPCM_CONTROL5: case CX_I2SPCM_CONTROL6:
    case CX_UM_INTERRUPT_CRTL_E: case CX_EQ_G_COEFF:
    case CX_SPKR_DRC_CONTROL: case CX_SPKR_DRC_TEST:
    case CX_DIGITAL_BIOS_TEST0: case CX_DIGITAL_BIOS_TEST2:
        return 4;
    case CX_EQ_ENABLE_BYPASS: case CX_EQ_B0_COEFF: case CX_EQ_B1_COEFF: case CX_EQ_B2_COEFF:
    case CX_EQ_A1_COEFF: case CX_EQ_A2_COEFF:
    case CX_DAC1_CONVERTER_FORMAT: case CX_DAC2_CONVERTER_FORMAT:
    case CX_ADC1_CONVERTER_FORMAT: case CX_ADC2_CONVERTER_FORMAT:
    case CX_CODEC_TEST2: case CX_CODEC_TEST9: case CX_CODEC_TEST20: case CX_CODEC_TEST26:
    case CX_ANALOG_TEST3: case CX_ANALOG_TEST4: case CX_ANALOG_TEST5: case CX_ANALOG_TEST6:
    case CX_ANALOG_TEST7: case CX_ANALOG_TEST8: case CX_ANALOG_TEST9: case CX_ANALOG_TEST10:
    case CX_ANALOG_TEST11: case CX_ANALOG_TEST12: case CX_ANALOG_TEST13:
    case CX_DIGITAL_TEST0: case CX_DIGITAL_TEST1: case CX_DIGITAL_TEST11: case CX_DIGITAL_TEST12:
    case CX_DIGITAL_TEST15: case CX_DIGITAL_TEST16: case CX_DIGITAL_TEST17: case CX_DIGITAL_TEST18:
    case CX_DIGITAL_TEST19: case CX_DIGITAL_TEST20:
        return 2;
    default:
        return 1;
    }
}

/* cx2072x_reg_write: address big endian, value little endian, register width */
static BOOLEAN CxWr(SST_CODEC *c, ULONG reg, ULONG val)
{
    UCHAR b[6];
    ULONG n = CxSize(reg), i;

    b[0] = (UCHAR)(reg >> 8);
    b[1] = (UCHAR)reg;
    for (i = 0; i < n; i++) b[2 + i] = (UCHAR)(val >> (8 * i));
    return CRaw(c, b, 2 + n, NULL, 0);
}

/* cx2072x_reg_read: write the address, repeated start, read the register width */
static BOOLEAN CxRd(SST_CODEC *c, ULONG reg, ULONG *val)
{
    UCHAR a[2], r[4] = { 0, 0, 0, 0 };
    ULONG n = CxSize(reg);

    a[0] = (UCHAR)(reg >> 8);
    a[1] = (UCHAR)reg;
    *val = 0xffffffff;
    if (!CRaw(c, a, 2, r, n)) return FALSE;
    *val = (ULONG)r[0] | ((ULONG)r[1] << 8) | ((ULONG)r[2] << 16) | ((ULONG)r[3] << 24);
    return TRUE;
}

static BOOLEAN CxUpd(SST_CODEC *c, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;

    if (!CxRd(c, reg, &v)) return FALSE;
    return CxWr(c, reg, (v & ~mask) | (val & mask));
}

static BOOLEAN CxSeq(SST_CODEC *c, const ULONG *rv, ULONG pairs)
{
    BOOLEAN ok = TRUE;
    ULONG i;

    for (i = 0; i < pairs; i++) ok &= CxWr(c, rv[2 * i], rv[2 * i + 1]);
    return ok;
}

static BOOLEAN CxProbe(SST_CODEC *c)
{
    ULONG ven, rev = 0;

    if (!CxRd(c, CX_VENDOR_ID, &ven)) return FALSE;
    CxRd(c, CX_REVISION_ID, &rev);
    /* HDA-style vendor id: Conexant 0x14f1 in the upper half */
    if ((ven >> 16) != 0x14f1 && (ven & 0xffff) != 0x14f1) {
        LOGI("cx2072x: %s at %x: vendor id %08x is not Conexant", c->Hid, c->Addr, ven);
        return FALSE;
    }
    LOGI("cx2072x: codec version %08x,%08x", ven, rev);
    return TRUE;
}

/* cx2072x_config_pll: pre-divider from the MCLK table, then integer or fractional feedback */
static BOOLEAN CxPll(SST_CODEC *c, ULONG mclk, ULONG rate)
{
    static const ULONG preDiv[][2] = {
        { 6144000, 1 }, { 12288000, 2 }, { 19200000, 3 }, { 26000000, 4 }, { 28224000, 5 },
        { 36864000, 6 }, { 36864000, 7 }, { 48000000, 8 }, { 49152000, 8 },
    };
    ULONG div = 8, in, out, intDiv, fracDiv, frac = 0, i, spSync = 2, clkSample = 96;
    BOOLEAN ok = TRUE;

    switch (rate) {
    case 48000: case 32000: case 24000: case 16000: break;
    case 96000: spSync = 1; clkSample = 48; break;
    case 192000: spSync = 0; clkSample = 24; break;
    default: LOGE("cx2072x: unsupported rate %u", rate); return FALSE;
    }
    for (i = 0; i < sizeof(preDiv) / sizeof(preDiv[0]); i++)
        if (mclk <= preDiv[i][0]) { div = preDiv[i][1]; break; }
    in = mclk / div;
    out = rate * 3072;
    intDiv = out / in;
    fracDiv = out - intDiv * in;
    if (fracDiv) {
        /* fracDiv * 1000 / in, digit by digit (fracDiv * 1000 can exceed 32 bits) */
        ULONG rem = fracDiv, q = 0, d;
        for (d = 0; d < 3; d++) { rem *= 10; q = q * 10 + rem / in; rem %= in; }
        fracDiv = q;                    /* < 1000; ((1 << 20) - 4) / 7 = 149796 exactly */
        frac = ((4000 + fracDiv) * 149796 + 499) / 1000;
    }

    ok &= CxWr(c, CX_ANALOG_TEST4, 0x40 | (((div - 1) * 2) << 8));
    if (!fracDiv) {
        ok &= CxWr(c, CX_ANALOG_TEST7, 0x100);          /* integer mode */
    } else {
        ok &= CxWr(c, CX_ANALOG_TEST6, frac & 0xfff);
        ok &= CxWr(c, CX_ANALOG_TEST7, (frac >> 12) & 0xff);
    }
    ok &= CxWr(c, CX_ANALOG_TEST8, intDiv - 1);

    if (!fracDiv) {
        ok &= CxWr(c, CX_DIGITAL_TEST16, 0x00);         /* PLL tracking off */
    } else {
        ok &= CxWr(c, CX_DIGITAL_TEST16, (spSync << 4) & 0xf0);
        ok &= CxWr(c, CX_DIGITAL_TEST17, clkSample);
        ok &= CxWr(c, CX_DIGITAL_TEST18, clkSample * 3 / 2);
        ok &= CxWr(c, CX_DIGITAL_TEST19, 0x01);
        ok &= CxWr(c, CX_DIGITAL_TEST20, 0x02);
        ok &= CxUpd(c, CX_DIGITAL_TEST16, 0x01, 0x01);
    }
    LOGI("cx2072x: PLL MCLK %u / %u, N %u frac %x%s", mclk, div, intDiv, frac, fracDiv ? " (tracking)" : "");
    return ok;
}

/* cx2072x_config_i2spcm for I2S, NB_NF, codec = clock consumer */
static BOOLEAN CxI2s(SST_CODEC *c)
{
    ULONG frame = (ULONG)c->Slots * c->Slot, ss = c->Valid == 16 ? 16 : 24;
    ULONG pulse, right, pauseCyc, pausePos, half, reg1, reg2, reg3, reg6;
    BOOLEAN ok = TRUE;

    if (frame < 16 || frame > 256) { LOGE("cx2072x: frame of %u bits not supported", frame); return FALSE; }
    if (c->Slots != 2) LOGE("cx2072x: %u slots per frame, configured as 2-channel I2S", c->Slots);

    pulse = frame / 2;                  /* I2S: WS 50 % duty, one bit data delay, WS and BCLK inverted */
    right = (frame / 2) / BITS_PER_SLOT;
    pauseCyc = (frame / 2) % BITS_PER_SLOT;
    pausePos = right * BITS_PER_SLOT;

    /* reg1: data_one_line, ws_pol, ws_wid[8:2], frm_len[13:9], sa_size[15:14]; tx in the upper half */
    half = 1 | (1 << 1) | ((pulse - 1) << 2) | ((frame / BITS_PER_SLOT - 1) << 9) | ((ss / BITS_PER_SLOT - 1) << 14);
    reg1 = half | (half << 16);
    /* reg2/3: en_ch1..6 [5:0], slot_1 [10:6], slot_2 [15:11], ..., master 28, endian_sel 30 (0 = big),
     * dstart_dly 31 (I2S one bit delay); channel 1/2 = ADC1 L/R (tx) and DAC1 L/R (rx) */
    reg2 = 0x03 | (0 << 6) | (right << 11) | (1UL << 31);
    reg3 = 0x03 | (0 << 6) | (right << 11) | (1UL << 31);
    /* reg6: rx_pause_cycles [7:5], rx_pause_start_pos [15:8], tx_pause_cycles [23:21], tx pos [31:24] */
    reg6 = (pauseCyc << 5) | (pausePos << 8) | (pauseCyc << 21) | (pausePos << 24);

    ok &= CxWr(c, CX_I2SPCM_CONTROL5, 0);               /* no BCLK output (consumer) */
    ok &= CxWr(c, CX_I2SPCM_CONTROL1, reg1);
    ok &= CxWr(c, CX_I2SPCM_CONTROL2, reg2);
    ok &= CxWr(c, CX_I2SPCM_CONTROL3, reg3);
    ok &= CxWr(c, CX_I2SPCM_CONTROL4, 0);
    ok &= CxWr(c, CX_I2SPCM_CONTROL6, reg6);
    ok &= CxWr(c, CX_I2SPCM_CONTROL5, 0);
    /* regdbt2 = 0xac (EAPD pad config, i2s_bclk_en) | i2s_bclk_invert */
    ok &= CxWr(c, CX_DIGITAL_BIOS_TEST2, 0xac | (1 << 8));
    LOGI("cx2072x: I2S frame %u bits, %u-bit samples, right slot at bit %u, pause %u", frame, ss, pausePos,
         pauseCyc);
    return ok;
}

static void CxAfg(SST_CODEC *c, BOOLEAN on)
{
    if (on) {
        CxWr(c, CX_AFG_POWER_STATE, CX_PS_D0);
        /* afg_power_ev POST_PMU (Linux's update_bits has a zero mask there; the intent is to clear it) */
        CxUpd(c, CX_DIGITAL_BIOS_TEST0, 0x10, 0x00);
    } else {
        CxUpd(c, CX_DIGITAL_BIOS_TEST0, 0x10, 0x10);    /* afg_power_ev PRE_PMD */
        CxWr(c, CX_AFG_POWER_STATE, CX_PS_D3);
    }
}

static void CxDacGain(SST_CODEC *c, ULONG l, ULONG r, BOOLEAN mute)
{
    CxWr(c, CX_DAC1_AMP_GAIN_LEFT, (mute ? CX_AMP_MUTE : 0) | l);
    CxWr(c, CX_DAC1_AMP_GAIN_RIGHT, (mute ? CX_AMP_MUTE : 0) | r);
}

static BOOLEAN CxInit(SST_CODEC *c)
{
    /* cx2072x_reg_init */
    static const ULONG init[] = {
        CX_ANALOG_TEST9,  0x080,        /* DC offset calibration */
        CX_CODEC_TEST26,  0x65f,        /* disable the PA */
        CX_ANALOG_TEST10, 0x289,        /* speaker output gain */
        CX_CODEC_TEST20,  0xf05,
        CX_CODEC_TESTXX,  0x380,        /* 1-byte register: 0x80 reaches the chip, as in Linux */
        CX_CODEC_TEST26,  0xb90,
        CX_CODEC_TEST9,   0x001,        /* 30 Hz high pass filter */
        CX_ANALOG_TEST3,  0x300,        /* disable PCBEEP pad */
        CX_CODEC_TEST24,  0x100,        /* disable SnM mode (1-byte register, as in Linux) */
        CX_PORTD_PIN_CTRL, 0x020,       /* PortD input */
        CX_GPIO_ENABLE,   0x040,        /* GPIO7 = headset button */
        CX_GPIO_UM_ENABLE, 0x040,
        CX_UM_RESPONSE,   0x080,
        CX_DIGITAL_TEST12, 0x0c4,       /* headset button */
        CX_DIGITAL_TEST0, 0x415,        /* class-D powered down while idle */
    };
    ULONG mclk = c->Mclk ? c->Mclk : 19200000, ven = 0, rev = 0, in = c->Mic == 1 ? 2 : 1;
    BOOLEAN ok = TRUE;

    c->Priv[P_MUTE] = 0;
    c->Priv[P_PORT] = c->Mic == 1 ? CX_PORTC_POWER_STATE : CX_PORTD_POWER_STATE;
    c->Priv[P_MCLK] = mclk;
    if (!c->Mclk) LOGI("cx2072x: no MCLK from the SSP, assuming the 19.2 MHz platform clock");
    CxRd(c, CX_VENDOR_ID, &ven);
    CxRd(c, CX_REVISION_ID, &rev);

    /* cx2072x_probe: AFG to D0, init table, PortC input, BIOS test 2 bits, (clocks), AFG back to D3.
     * No software reset: Linux has none; every register this driver relies on is written here. */
    ok &= CxWr(c, CX_AFG_POWER_STATE, CX_PS_D0);
    if (!ok) return FALSE;
    ok &= CxSeq(c, init, sizeof(init) / sizeof(init[0]) / 2);
    ok &= CxUpd(c, CX_PORTC_PIN_CTRL, CX_PIN_IN, CX_PIN_IN);
    ok &= CxUpd(c, CX_DIGITAL_BIOS_TEST2, 0x84, 0x84);
    /* from cx2072x_enable_jack_detect, minus the interrupt setup: headset type and tip detection */
    ok &= CxWr(c, CX_DIGITAL_TEST15, 0x73);             /* Nokia and Apple headsets, 275 ms monitor */
    ok &= CxWr(c, CX_ANALOG_TEST12, 0x300);             /* TIP detection off */

    /* hw_params: PLL from MCLK, I2S consumer */
    ok &= CxPll(c, mclk, c->Rate);
    ok &= CxI2s(c);

    /* playback routing: DAC1 -> PortA (headphone, HP amp) and PortG (class-D speaker) */
    ok &= CxWr(c, CX_PORTA_CONNECTION_SELECT, 0);
    ok &= CxWr(c, CX_PORTG_CONNECTION_SELECT, 0);
    ok &= CxWr(c, CX_PORTA_PIN_CTRL, CX_PIN_HP | CX_PIN_OUT);
    ok &= CxWr(c, CX_PORTG_PIN_CTRL, CX_PIN_OUT);
    CxDacGain(c, CX_AMP_0DB, CX_AMP_0DB, TRUE);

    /* capture routing: ADC1 <- PortD (0: headset) or PortC (1: internal), boost +24 dB, ADC amp 0 dB */
    ok &= CxWr(c, CX_ADC1_CONNECTION_SELECT, in);       /* 0 B, 1 D, 2 C, 3 widget15, 4 E, 5 F, 6 H */
    ok &= CxWr(c, CX_ADC1_AMP_GAIN_LEFT_0 + 4 * in, CX_AMP_0DB);
    ok &= CxWr(c, CX_ADC1_AMP_GAIN_RIGHT_0 + 4 * in, CX_AMP_0DB);
    ok &= CxWr(c, CX_PORTD_GAIN_LEFT, 2);
    ok &= CxWr(c, CX_PORTD_GAIN_RIGHT, 2);
    ok &= CxWr(c, CX_PORTC_GAIN_LEFT, 2);
    ok &= CxWr(c, CX_PORTC_GAIN_RIGHT, 2);
    ok &= CxUpd(c, CX_ANALOG_TEST11, CX_HS_BIAS, 0);
    ok &= CxWr(c, CX_PORTD_PIN_CTRL, CX_PIN_IN);

    /* every widget idle (D3), then the AFG */
    ok &= CxWr(c, CX_DAC1_POWER_STATE, CX_PS_D3);
    ok &= CxWr(c, CX_DAC2_POWER_STATE, CX_PS_D3);
    ok &= CxWr(c, CX_ADC1_POWER_STATE, CX_PS_D3);
    ok &= CxWr(c, CX_ADC2_POWER_STATE, CX_PS_D3);
    ok &= CxWr(c, CX_MIXER_POWER_STATE, CX_PS_D3);
    ok &= CxWr(c, CX_PORTA_POWER_STATE, CX_PS_D3);
    ok &= CxWr(c, CX_PORTG_POWER_STATE, CX_PS_D3);
    ok &= CxWr(c, CX_PORTC_POWER_STATE, CX_PS_D3);
    ok &= CxWr(c, CX_PORTD_POWER_STATE, CX_PS_D3);
    CxAfg(c, FALSE);
    if (!ok) return FALSE;

    c->Caps = CC_PLAY | CC_REC;
    LOGI("cx2072x: %08x rev %08x, MCLK %u Hz, mic on %s", ven, rev, mclk, c->Mic == 1 ? "PortC" : "PortD");
    return TRUE;
}

static void CxStream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (!rec && on) {
        if (!c->RecOn) CxAfg(c, TRUE);
        CxWr(c, CX_PORTA_POWER_STATE, CX_PS_D0);        /* supplies, then the DAC */
        CxWr(c, CX_PORTG_POWER_STATE, CX_PS_D0);
        CxWr(c, CX_DAC1_POWER_STATE, CX_PS_D0);
        if (!c->Priv[P_MUTE]) {
            CxUpd(c, CX_DAC1_AMP_GAIN_LEFT, CX_AMP_MUTE, 0);
            CxUpd(c, CX_DAC1_AMP_GAIN_RIGHT, CX_AMP_MUTE, 0);
        }
    } else if (!rec) {
        CxUpd(c, CX_DAC1_AMP_GAIN_LEFT, CX_AMP_MUTE, CX_AMP_MUTE);
        CxUpd(c, CX_DAC1_AMP_GAIN_RIGHT, CX_AMP_MUTE, CX_AMP_MUTE);
        CxWr(c, CX_DAC1_POWER_STATE, CX_PS_D3);
        CxWr(c, CX_PORTG_POWER_STATE, CX_PS_D3);
        CxWr(c, CX_PORTA_POWER_STATE, CX_PS_D3);
        if (!c->RecOn) CxAfg(c, FALSE);
    } else if (on) {
        if (!c->PlayOn) CxAfg(c, TRUE);
        CxWr(c, c->Priv[P_PORT], CX_PS_D0);
        if (c->Mic != 1) {
            CxUpd(c, CX_ANALOG_TEST11, CX_HS_BIAS, CX_HS_BIAS);         /* "Headset Bias" */
            CxWr(c, CX_PORTD_PIN_CTRL, CX_PIN_IN | CX_PIN_VREF);        /* "PortD Mic Bias" */
        }
        CxWr(c, CX_ADC1_POWER_STATE, CX_PS_D0);
    } else {
        CxWr(c, CX_ADC1_POWER_STATE, CX_PS_D3);
        if (c->Mic != 1) {
            CxWr(c, CX_PORTD_PIN_CTRL, CX_PIN_IN);
            CxUpd(c, CX_ANALOG_TEST11, CX_HS_BIAS, 0);
        }
        CxWr(c, c->Priv[P_PORT], CX_PS_D3);
        if (!c->PlayOn) CxAfg(c, FALSE);
    }
}

static void CxVolume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    /* DAC1 amp: 0x4a = 0 dB, 1 dB steps down to -74 dB; mute bit only while playing or when muting */
    c->Priv[P_MUTE] = mute;
    CxDacGain(c, CVolSteps(left, CX_AMP_0DB), CVolSteps(right, CX_AMP_0DB), mute || !c->PlayOn);
}

static void CxShutdown(SST_CODEC *c)
{
    CxDacGain(c, CX_AMP_0DB, CX_AMP_0DB, TRUE);
    CxWr(c, CX_PORTD_PIN_CTRL, CX_PIN_IN);
    CxUpd(c, CX_ANALOG_TEST11, CX_HS_BIAS, 0);
    CxWr(c, CX_DAC1_POWER_STATE, CX_PS_D3);
    CxWr(c, CX_ADC1_POWER_STATE, CX_PS_D3);
    CxWr(c, CX_PORTA_POWER_STATE, CX_PS_D3);
    CxWr(c, CX_PORTG_POWER_STATE, CX_PS_D3);
    CxWr(c, CX_PORTC_POWER_STATE, CX_PS_D3);
    CxWr(c, CX_PORTD_POWER_STATE, CX_PS_D3);
    CxAfg(c, FALSE);
}

const CODEC_DRV g_DrvCx2072x = { "CX2072X", "14F10720\0", CODEC_HEADSET, 2, 4,
                                 CxProbe, CxInit, CxStream, CxVolume, CxShutdown };
