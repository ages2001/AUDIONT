#include "codec.h"

/*
 * Everest ES8316 and its pin/register compatible ES8336 (ACPI ESSX8316 on Bay Trail / Cherry Trail,
 * ESSX8336 on SOF platforms).  Ported from Linux sound/soc/codecs/es8316.c and the machine drivers
 * sound/soc/intel/boards/bytcht_es8316.c and sof_es8336.c.
 *
 * I2C: 8-bit register, 8-bit value.
 *
 * Clocking: the chip has no PLL and, per the datasheet (regs 0x01/0x02/0x09), no way to clock the
 * core from BCLK: in slave mode it auto-detects the MCLK/LRCK ratio (256, 384, 400, 500, 512, 768,
 * 1024; MCLK can be halved by CLKMGR_CLKSW bit 7).  So MCLK is mandatory.  With c->Mclk == 0 we
 * assume 19.2 MHz: both Intel machine drivers hard-code that rate (pmc_plt_clk_3 on BYT/CHT, the
 * SSP MCLK on SOF platforms) and the codec reads no other clock.
 *
 * Outputs: only HPOL/HPOR.  The speakers are fed from the same pins through an external amplifier
 * (or an analog switch) enabled by a GpioIo line of the codec device:
 *   bytcht_es8316.c: "speaker-enable" = GpioIo/GpioInt entry 0, active high.
 *   sof_es8336.c (default quirk): "speakers-enable" = GpioIo 0 (enable_gpio0 is declared active-low,
 *     but the machine driver writes the inverted logical value, so the pad is HIGH while the
 *     speakers play - the same as on BYT/CHT).  Boards with SOF_ES8336_SPEAKERS_EN_GPIO1_QUIRK use
 *     GpioIo 1 instead, boards with SOF_ES8336_HEADPHONE_GPIO have a "headphone-enable" line on the
 *     other index.
 *   Here: GpioIo 0 high while playback runs (speaker enable on the default mapping).  The default
 *   sof_es8336 quirk leaves a second GpioIo alone, but every Linux mapping that uses index 1 makes it
 *   a speakers-enable or headphone-enable line (active high), so with no jack detection we drive
 *   index 1 high with playback as well (all outputs on).  On the Huawei boards with the
 *   HEADPHONE_GPIO quirk Linux keeps headphone-enable low while the speakers run; if a board mutes
 *   its speakers with both lines high, that is the case to look at.  Both lines are low when idle;
 *   the amplifier is switched on 70 ms after the codec path is up and off before it goes down
 *   (sof_es8336 pcm_pop_work / trigger STOP).
 *
 * Microphone (single mono ADC behind a differential PGA, sent on both I2S channels), c->Mic:
 *   0 (default): LIN2/RIN2 - the headset mic on the default maps of both machine drivers
 *                (sof_es8336 without SOC_ES8336_HEADSET_MIC1, bytcht BYT_CHT_ES8316_INTMIC_IN1_MAP).
 *   1:           LIN1/RIN1 - the internal analog mic on those boards, or the headset mic on boards
 *                with SOC_ES8336_HEADSET_MIC1 (Huawei BOHB/NDZ) / BYT_CHT_ES8316_INTMIC_IN2_MAP
 *                (BYTCR default, Irbis NB41, ...).
 *   (the ES8316 DMIC input is not offered: Linux cannot route it either.)
 *   Gain: PGA +21 dB (ADC_PGAGAIN), ADC digital 0 dB, ALC off, MICBIAS on while capturing.
 *
 * Probe: the chip has no id register.  We require that it answers, that it is not an ES8326 (which
 * has 0x83/0x26 at 0xFD/0xFE) and that a DAC volume register reads back what was written.
 */

/* registers (es8316.h) */
#define ES8316_RESET            0x00
#define ES8316_CLKMGR_CLKSW     0x01
#define ES8316_CLKMGR_CLKSEL    0x02
#define ES8316_CLKMGR_ADCOSR    0x03
#define ES8316_CLKMGR_ADCDIV1   0x04
#define ES8316_CLKMGR_ADCDIV2   0x05
#define ES8316_CLKMGR_DACDIV1   0x06
#define ES8316_CLKMGR_DACDIV2   0x07
#define ES8316_CLKMGR_CPDIV     0x08
#define ES8316_SERDATA1         0x09
#define ES8316_SERDATA_ADC      0x0a
#define ES8316_SERDATA_DAC      0x0b
#define ES8316_SYS_VMIDSEL      0x0c
#define ES8316_SYS_PDN          0x0d
#define ES8316_HPMIX_SEL        0x13
#define ES8316_HPMIX_SWITCH     0x14
#define ES8316_HPMIX_PDN        0x15
#define ES8316_HPMIX_VOL        0x16
#define ES8316_CPHP_OUTEN       0x17
#define ES8316_CPHP_ICAL_VOL    0x18
#define ES8316_CPHP_PDN1        0x19
#define ES8316_CPHP_PDN2        0x1a
#define ES8316_ADC_PDN_LINSEL   0x22
#define ES8316_ADC_PGAGAIN      0x23
#define ES8316_ADC_D2SEPGA      0x24
#define ES8316_ADC_DMIC         0x25
#define ES8316_ADC_MUTE         0x26
#define ES8316_ADC_VOLUME       0x27
#define ES8316_ADC_ALC1         0x29
#define ES8316_ADC_ALC_NG       0x2e
#define ES8316_DAC_PDN          0x2f
#define ES8316_DAC_SET1         0x30
#define ES8316_DAC_SET2         0x31
#define ES8316_DAC_SET3         0x32
#define ES8316_DAC_VOLL         0x33
#define ES8316_DAC_VOLR         0x34
#define ES8316_GPIO_DEBOUNCE    0x4e

/* ES8316_RESET */
#define RESET_CSM_ON            0x80
/* ES8316_CLKMGR_CLKSW: mclk_div2, mclk_on, bclk_on, clk_cp_on, clk_adc_on, clk_dac_on, anaclk_adc, anaclk_dac */
#define CLKSW_MCLK_DIV          0x80
#define CLKSW_MCLK_ON           0x40
#define CLKSW_BCLK_ON           0x20
#define CLKSW_CP                0x10    /* "Headphone Charge Pump Clock" */
#define CLKSW_ADC               0x08    /* "ADC Clock" */
#define CLKSW_DAC               0x04    /* "DAC Clock" (also needed by the ADC, see es8316.c routes) */
#define CLKSW_ANA               0x03    /* analog ADC/DAC clocks, on by default, never touched by Linux */
/* ES8316_SERDATA_ADC / _DAC */
#define SERDATA2_FMT_I2S        0x00
#define SERDATA2_LEN_24         0x00
#define SERDATA2_LEN_16         0x0c
#define SERDATA_ADC_OFF         0x40    /* "I2S OUT" AIF widget, bit 6, inverted */
/* ES8316_SYS_PDN (1 = power down) */
#define PDN_DAC_VREF            0x01
#define PDN_ADC_VREF            0x02
#define PDN_ADC_BIAS            0x04
#define PDN_BIAS                0x08    /* "Bias" (ibiasgen) */
#define PDN_ANA                 0x10    /* "Analog power" */
#define PDN_MICBIAS             0x20    /* "Mic Bias" */
/* ES8316_DAC_SET1 */
#define DAC_SET1_MUTE           0x20
#define DAC_SET1_SOFTRAMP       0x10

#define ES_GPIO_SPK             0       /* see the header comment */
#define ES_GPIO_SPK2            1

/* driver private state in c->Priv */
#define P_CLKSW     0           /* CLKMGR_CLKSW base value (MCLK_ON | BCLK_ON | ANA | MCLK_DIV) */
#define P_LINSEL    1           /* ADC_PDN_LINSEL[5:4] */
#define P_MUTE      2           /* user mute */

static BOOLEAN Es16Pick(ULONG sysclk, ULONG rate, ULONG *clk)
{
    /* es8316_pcm_hw_params: try the halved MCLK first, then the full one */
    static const USHORT ratio[] = { 256, 384, 400, 500, 512, 768, 1024 };
    ULONG pass, i, f;

    for (pass = 0; pass < 2; pass++) {
        f = pass ? sysclk : sysclk / 2;
        for (i = 0; i < sizeof(ratio) / sizeof(ratio[0]); i++)
            if (f % ratio[i] == 0 && f / ratio[i] == rate) { *clk = f; return TRUE; }
    }
    return FALSE;
}

static BOOLEAN Es16Probe(SST_CODEC *c)
{
    ULONG v, id1, id2, old;

    if (!CRd(c, ES8316_RESET, &v)) return FALSE;
    /* an ES8326 answers with its chip id; the ES8316 has nothing there */
    if (CRd(c, 0xfd, &id1) && CRd(c, 0xfe, &id2) && id1 == 0x83 && id2 == 0x26) {
        LOGI("es8316: %s at %x is an ES8326, not ours", c->Hid, c->Addr);
        return FALSE;
    }
    /* no id register: check a plain read/write register (DAC_VOLL, 8 bits) */
    if (!CRd(c, ES8316_DAC_VOLL, &old)) return FALSE;
    if (!CWr(c, ES8316_DAC_VOLL, 0x5a) || !CRd(c, ES8316_DAC_VOLL, &v)) return FALSE;
    CWr(c, ES8316_DAC_VOLL, old);
    if (v != 0x5a) {
        LOGI("es8316: %s at %x does not behave like an ES8316 (%x)", c->Hid, c->Addr, v);
        return FALSE;
    }
    return TRUE;
}

static void Es16Spk(SST_CODEC *c, BOOLEAN on)
{
    CGpio(c, ES_GPIO_SPK, on);
    if (c->NGpio > 1) CGpio(c, ES_GPIO_SPK2, on);
}

static BOOLEAN Es16Init(SST_CODEC *c)
{
    ULONG sysclk = c->Mclk ? c->Mclk : 19200000, clk, lrck, bclkdiv, wl, clksw;
    BOOLEAN ok = TRUE;

    Es16Spk(c, FALSE);

    if (!c->Mclk) LOGI("es8316: no MCLK from the SSP, assuming the 19.2 MHz platform clock");
    if (!c->Rate || !Es16Pick(sysclk, c->Rate, &clk)) {
        LOGE("es8316: MCLK %u Hz cannot clock %u Hz frames (ratios 256..1024)", sysclk, c->Rate);
        return FALSE;
    }
    if (c->Slots != 2) LOGE("es8316: %u slots per frame, the chip only does 2-channel I2S", c->Slots);

    lrck = clk / c->Rate;
    wl = c->Valid == 16 ? 16 : 24;
    bclkdiv = (lrck / 4) / wl;          /* only used in master mode, written as Linux does */
    clksw = CLKSW_MCLK_ON | CLKSW_BCLK_ON | CLKSW_ANA | (clk != sysclk ? CLKSW_MCLK_DIV : 0);
    c->Priv[P_CLKSW] = clksw;
    c->Priv[P_LINSEL] = c->Mic == 1 ? 0x00 : 0x10;     /* LInSel: 0 lin1-rin1, 1 lin2-rin2 */
    c->Priv[P_MUTE] = 0;

    /* es8316_probe: reset, then the current state machine on */
    ok &= CWr(c, ES8316_RESET, 0x3f);
    CDelay(5);
    ok &= CWr(c, ES8316_RESET, RESET_CSM_ON);
    CDelay(30);
    if (!ok) return FALSE;

    /* vendor values from es8316_probe: VMID (silent output without it), ADC OSR for CHT */
    ok &= CWr(c, ES8316_SYS_VMIDSEL, 0xff);
    ok &= CWr(c, ES8316_CLKMGR_ADCOSR, 0x32);

    /* everything down first (register defaults, idle state of every DAPM widget) */
    ok &= CWr(c, ES8316_GPIO_DEBOUNCE, 0x00);          /* no jack-detect interrupt */
    ok &= CWr(c, ES8316_DAC_SET1, DAC_SET1_SOFTRAMP | DAC_SET1_MUTE);
    ok &= CWr(c, ES8316_CPHP_OUTEN, 0x00);
    ok &= CWr(c, ES8316_HPMIX_PDN, 0x33);
    ok &= CWr(c, ES8316_DAC_PDN, 0x11);
    ok &= CWr(c, ES8316_ADC_PDN_LINSEL, 0xc0 | c->Priv[P_LINSEL]);
    ok &= CWr(c, ES8316_CPHP_PDN2, 0x22);
    ok &= CWr(c, ES8316_CPHP_PDN1, 0x06);
    ok &= CWr(c, ES8316_CPHP_ICAL_VOL, 0x88);           /* pdn_Lical/Rical, HP volume 0 dB */
    ok &= CWr(c, ES8316_SYS_PDN, 0x3f);

    /* DAI: slave, normal BCLK, I2S, word length from c->Valid (es8316_set_dai_fmt / hw_params) */
    ok &= CWr(c, ES8316_SERDATA1, bclkdiv & 0x1f);
    ok &= CWr(c, ES8316_SERDATA_ADC, SERDATA_ADC_OFF | (wl == 16 ? SERDATA2_LEN_16 : SERDATA2_LEN_24) |
              SERDATA2_FMT_I2S);
    ok &= CWr(c, ES8316_SERDATA_DAC, (wl == 16 ? SERDATA2_LEN_16 : SERDATA2_LEN_24) | SERDATA2_FMT_I2S);
    ok &= CWr(c, ES8316_CLKMGR_CLKSW, clksw);           /* MCLK and BCLK inputs on, block clocks off */
    ok &= CUpd(c, ES8316_CLKMGR_ADCDIV1, 0x0f, lrck >> 8);
    ok &= CWr(c, ES8316_CLKMGR_ADCDIV2, lrck & 0xff);
    ok &= CUpd(c, ES8316_CLKMGR_DACDIV1, 0x0f, lrck >> 8);
    ok &= CWr(c, ES8316_CLKMGR_DACDIV2, lrck & 0xff);

    /* playback routing: LDATA->LDAC, RDATA->RDAC (DAC_SET1[7:6] = 0), DACs -> HP mixers, no bypass */
    ok &= CWr(c, ES8316_HPMIX_SEL, 0x00);
    ok &= CWr(c, ES8316_HPMIX_SWITCH, 0x88);            /* LD2LHPMIX | RD2RHPMIX */
    ok &= CWr(c, ES8316_HPMIX_VOL, 0xbb);               /* HP mixer 0 dB (both nibbles 1011) */
    ok &= CWr(c, ES8316_DAC_SET2, 0x00);
    ok &= CWr(c, ES8316_DAC_SET3, 0x00);
    ok &= CWr(c, ES8316_DAC_VOLL, 0x00);                /* 0 dB, 0.5 dB/step down to 0xc0 = -96 dB */
    ok &= CWr(c, ES8316_DAC_VOLR, 0x00);

    /* capture: differential PGA +21 dB, analog ADC (no DMIC), HPF on, ADC 0 dB, ALC and noise gate off */
    ok &= CWr(c, ES8316_ADC_PGAGAIN, 0x90);
    ok &= CWr(c, ES8316_ADC_DMIC, 0x08);
    ok &= CWr(c, ES8316_ADC_MUTE, 0x10);                /* soft ramp, not muted */
    ok &= CWr(c, ES8316_ADC_VOLUME, 0x00);
    ok &= CUpd(c, ES8316_ADC_ALC1, 0xc0, 0x00);
    ok &= CUpd(c, ES8316_ADC_ALC_NG, 0x20, 0x00);

    if (!ok) return FALSE;
    c->Caps = CC_PLAY | CC_REC;
    LOGI("es8316: %s, MCLK %u Hz%s, LRCK divider %u, %u-bit I2S, mic on %s", c->Hid, sysclk,
         clk != sysclk ? " / 2" : "", lrck, wl, c->Priv[P_LINSEL] ? "IN2" : "IN1");
    return TRUE;
}

static void Es16Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    ULONG clk = c->Priv[P_CLKSW];

    if (!rec && on) {
        /* DAPM power-up order: supplies, DACs, mixers, mixer outputs, charge pump / HP drivers */
        CWr(c, ES8316_CLKMGR_CLKSW, clk | CLKSW_DAC | CLKSW_CP | (c->RecOn ? CLKSW_ADC : 0));
        CUpd(c, ES8316_SYS_PDN, PDN_DAC_VREF | PDN_BIAS | PDN_ANA, 0);
        CUpd(c, ES8316_CPHP_ICAL_VOL, 0x88, 0x00);      /* "Left/Right Headphone ical" */
        CUpd(c, ES8316_CPHP_PDN1, 0x04, 0x00);          /* "Headphone Out" */
        CUpd(c, ES8316_CPHP_PDN2, 0x20, 0x00);          /* "Headphone Charge Pump" */
        CWr(c, ES8316_DAC_PDN, 0x00);
        CWr(c, ES8316_HPMIX_PDN, 0x11);                 /* mixers (bits 5/1) */
        CWr(c, ES8316_HPMIX_PDN, 0x00);                 /* mixer outputs (bits 4/0) */
        CWr(c, ES8316_CPHP_OUTEN, 0x66);                /* EnHPL/R + HPL/R_outen */
        if (!c->Priv[P_MUTE]) CUpd(c, ES8316_DAC_SET1, DAC_SET1_MUTE, 0);
        CDelay(70);                                     /* sof_es8336 pcm_pop_work delay */
        Es16Spk(c, TRUE);
    } else if (!rec) {
        Es16Spk(c, FALSE);
        CUpd(c, ES8316_DAC_SET1, DAC_SET1_MUTE, DAC_SET1_MUTE);
        CWr(c, ES8316_CPHP_OUTEN, 0x00);
        CWr(c, ES8316_HPMIX_PDN, 0x11);
        CWr(c, ES8316_HPMIX_PDN, 0x33);
        CWr(c, ES8316_DAC_PDN, 0x11);
        CUpd(c, ES8316_CPHP_PDN2, 0x20, 0x20);
        CUpd(c, ES8316_CPHP_PDN1, 0x04, 0x04);
        CUpd(c, ES8316_CPHP_ICAL_VOL, 0x88, 0x88);
        CUpd(c, ES8316_SYS_PDN, PDN_DAC_VREF | (c->RecOn ? 0 : PDN_BIAS | PDN_ANA),
             PDN_DAC_VREF | (c->RecOn ? 0 : PDN_BIAS | PDN_ANA));
        CWr(c, ES8316_CLKMGR_CLKSW, clk | (c->RecOn ? CLKSW_ADC | CLKSW_DAC : 0));
    } else if (on) {
        CWr(c, ES8316_CLKMGR_CLKSW, clk | CLKSW_ADC | CLKSW_DAC | (c->PlayOn ? CLKSW_CP : 0));
        CUpd(c, ES8316_SYS_PDN, PDN_MICBIAS | PDN_ANA | PDN_BIAS | PDN_ADC_BIAS | PDN_ADC_VREF, 0);
        CDelay(20);                                     /* MICBIAS settling, as in es8316_enable_micbias_* */
        CWr(c, ES8316_ADC_PDN_LINSEL, 0x40 | c->Priv[P_LINSEL]);     /* "Line input PGA" */
        CWr(c, ES8316_ADC_PDN_LINSEL, 0x00 | c->Priv[P_LINSEL]);     /* "Mono ADC" */
        CUpd(c, ES8316_SERDATA_ADC, SERDATA_ADC_OFF, 0);
    } else {
        CUpd(c, ES8316_SERDATA_ADC, SERDATA_ADC_OFF, SERDATA_ADC_OFF);
        CWr(c, ES8316_ADC_PDN_LINSEL, 0x40 | c->Priv[P_LINSEL]);
        CWr(c, ES8316_ADC_PDN_LINSEL, 0xc0 | c->Priv[P_LINSEL]);
        CUpd(c, ES8316_SYS_PDN, PDN_MICBIAS | PDN_ADC_BIAS | PDN_ADC_VREF | (c->PlayOn ? 0 : PDN_BIAS | PDN_ANA),
             PDN_MICBIAS | PDN_ADC_BIAS | PDN_ADC_VREF | (c->PlayOn ? 0 : PDN_BIAS | PDN_ANA));
        CWr(c, ES8316_CLKMGR_CLKSW, clk | (c->PlayOn ? CLKSW_DAC | CLKSW_CP : 0));
    }
}

static void Es16Volume(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute)
{
    /* DAC_VOLL/R: 0x00 = 0 dB ... 0xc0 = -96 dB, 0.5 dB steps */
    CWr(c, ES8316_DAC_VOLL, 0xc0 - CVolSteps(left, 0xc0));
    CWr(c, ES8316_DAC_VOLR, 0xc0 - CVolSteps(right, 0xc0));
    c->Priv[P_MUTE] = mute;
    if (mute || c->PlayOn) CUpd(c, ES8316_DAC_SET1, DAC_SET1_MUTE, mute ? DAC_SET1_MUTE : 0);
}

static void Es16Shutdown(SST_CODEC *c)
{
    Es16Spk(c, FALSE);
    CUpd(c, ES8316_DAC_SET1, DAC_SET1_MUTE, DAC_SET1_MUTE);
    CUpd(c, ES8316_SERDATA_ADC, SERDATA_ADC_OFF, SERDATA_ADC_OFF);
    CWr(c, ES8316_CPHP_OUTEN, 0x00);
    CWr(c, ES8316_HPMIX_PDN, 0x33);
    CWr(c, ES8316_DAC_PDN, 0x11);
    CWr(c, ES8316_ADC_PDN_LINSEL, 0xc0 | c->Priv[P_LINSEL]);
    CUpd(c, ES8316_CPHP_PDN2, 0x20, 0x20);
    CUpd(c, ES8316_CPHP_PDN1, 0x04, 0x04);
    CUpd(c, ES8316_CPHP_ICAL_VOL, 0x88, 0x88);
    CWr(c, ES8316_SYS_PDN, 0x3f);
    CWr(c, ES8316_CLKMGR_CLKSW, CLKSW_ANA);             /* MCLK/BCLK inputs and block clocks off */
    CWr(c, ES8316_RESET, 0x3f);                         /* blocks in reset, state machine off */
}

const CODEC_DRV g_DrvEs8316 = { "ES8316", "ESSX8316\0ESSX8336\0", CODEC_HEADSET, 1, 1,
                                Es16Probe, Es16Init, Es16Stream, Es16Volume, Es16Shutdown };
