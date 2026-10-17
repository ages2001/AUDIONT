/*
 * AUDIONT - Cirrus Logic CS42L43 / CS42L43B SoundWire jack codec with an
 * internal stereo class D speaker amplifier (Dell / Lenovo Meteor Lake,
 * Lunar Lake laptops).  Linux drivers/mfd/cs42l43.c, cs42l43-sdw.c (core:
 * MCU firmware boot, register patch) and sound/soc/codecs/cs42l43.c,
 * cs42l43-jack.c (audio), machine side sound/soc/sdw_utils/soc_sdw_cs42l43.c.
 *
 *   registers  32-bit, 32-bit little-endian values at 4-byte aligned
 *              SoundWire addresses (regmap-sdw: sdw_nwrite / sdw_nread of the
 *              4 bytes, ascending); SCP paging for addresses >= 0x8000
 *   data ports DP5 (2 ch) jack playback, DP2 (2 ch) jack capture; the core
 *              gives a peripheral one port per direction, so the speaker port
 *              DP6 and the 4 channel DMIC port DP1 are not used: DP5 feeds the
 *              headphone mixers (AMP3 / AMP4) AND the speaker mixers (AMP1 /
 *              AMP2), DP2 carries the headset microphone or, by the registry
 *              DWORD Cs42l43Mic, the DMICs
 *   clocking   the PLL locked to the SoundWire bus clock ("SoundWire clock"
 *              sysclk of the machine driver), 48 kHz (SAMPLE_RATE1 = 0x03)
 *   jack       no detection: the jack is forced to a CTIA headset (the
 *              "Jack Override" control of Linux), Cs42l43Jack selects OMTP or
 *              a 3-pole headphone
 *
 * MCU firmware.  The CS42L43 boots a ROM firmware (MCU 0x2001 / BIOS 0x0000)
 * that stops in boot stage 2 asking for a patch.  Linux applies cs42l43.bin
 * (linux-firmware) there, moves the firmware to stage 3 and then disables it
 * ("Firmware too old to support disable" for the ROM revision: the patch is
 * MANDATORY in the Linux flow for an unpatched part) and soft resets the chip.
 * That soft reset detaches the device from the bus and the core has no
 * re-enumeration, so this driver never resets: after the disable handshake
 * it overwrites every register it relies on.  Without the patch file
 * (%SystemRoot%\System32\drivers\audiont\fw\cs42l43.bin) the firmware is left
 * paused in boot stage 2 (it does not touch the registers there) and the
 * driver carries on - unverified, the chip may need the patched firmware.
 *
 * CS42L43B (part 0x2A3B) uses the same flow with its own firmware mailbox and
 * decimator addresses (cs42l43b_* of Linux) - untested.
 *
 * Registry (DWORD): Cs42l43Mic 0 headset microphone (ADC1 IN1), 1 DMIC on
 * PDM1, 2 DMIC on PDM2; Cs42l43Jack 0 CTIA, 1 OMTP, 2 headphone (no mic);
 * Cs42l43Spk 0 leaves the internal speaker amplifier off (boards whose
 * speakers hang on CS35L56 amplifiers).
 */
#include "sdw.h"

/* registers (include/linux/mfd/cs42l43-regs.h) */
#define DEVID                   0x00003000
#define REVID                   0x00003004
#define RELID                   0x0000300C
#define MCLK_SRC_SEL            0x00007004
#define  OSC_PLL_MCLK_SEL       0x00000001
#define CCM_BLK_CLK_CONTROL     0x00007010
#define SAMPLE_RATE1            0x00007014
#define PLL_CONTROL             0x00007034
#define  PLL_REFCLK_EN          0x00000008
#define  PLL_REFCLK_DIV_MASK    0x00000006
#define  PLL_REFCLK_SRC_MASK    0x00000001      /* 1: SoundWire clock (CS42L43_SYSCLK_SDW) */
#define OSC_DIV_SEL             0x00007068
#define  OSC_DIV2_EN            0x00000001      /* required for SoundWire reads while the PLL is off */
#define ADC_B_CTRL1             0x00008000
#define ADC_B_CTRL2             0x00008004
#define  PGA_WIDESWING_MODE_EN  0x00000080
#define  ADC_AIN_SEL            0x00000010      /* ADC1 input: 0 IN1 (headset), 1 IN2 */
#define  ADC_PGA_GAIN_MASK      0x0000000F      /* 0xF -6 dB, 0 0 dB .. 3 +18 dB */
#define INTP_VOLUME_CTRL1       0x00009008      /* speaker L */
#define INTP_VOLUME_CTRL2       0x0000900C      /* speaker R */
#define  AMP1_2_VU              0x00000200
#define  AMP_MUTE               0x00000100
#define  AMP_VOL_MASK           0x000000FF      /* -64 dB + 0.5 dB steps: 0x80 = 0 dB */
#define OTP_REVISION_ID         0x0000B02C
#define SWIRE_DP2_CH1_INPUT     0x0000C2C0
#define SWIRE_DP2_CH2_INPUT     0x0000C2D0
#define AMP1MIX_INPUT1          0x0000C620      /* speaker L, inputs 1..4 at +4 */
#define AMP2MIX_INPUT1          0x0000C630      /* speaker R */
#define AMP3MIX_INPUT1          0x0000C640      /* headphone L */
#define AMP4MIX_INPUT1          0x0000C650      /* headphone R */
#define  MIXER_VOL_0DB          (0x40UL << 17)  /* MIXER_VOL 23:17, 0x20 -32 dB .. 0x50 +16 dB */
#define PDNCNTL                 0x00010000
#define  RING_SENSE_EN          0x00000002
#define TIPSENSE_DEB_CTRL       0x00010020
#define  TIPSENSE_INV           0x00000080
#define  TIPSENSE_FALL_MASK     0x00000038
#define  TIPSENSE_RISE_MASK     0x00000007
#define HS2                     0x00010040
#define  HS_CLAMP_DISABLE       0x10000000
#define  HSBIAS_RAMP_MASK       0x0C000000
#define  HSBIAS_RAMP_SHIFT      26
#define  HSDET_MODE_MASK        0x00018000
#define  HSDET_MANUAL_MASK      0x00006000
#define  HSDET_MANUAL_SHIFT     13
#define  AUTO_HSDET_TIME_MASK   0x00000700
#define  AUTO_HSDET_TIME_SHIFT  8
#define  AMP3_4_GNDREF_HS3      0x00000080
#define  AMP3_4_GNDREF_HS4      0x00000040
#define  HSBIAS_GNDREF_HS3      0x00000020
#define  HSBIAS_GNDREF_HS4      0x00000010
#define  HSBIAS_OUT_HS3         0x00000008
#define  HSBIAS_OUT_HS4         0x00000004
#define  HSGND_HS3              0x00000002
#define  HSGND_HS4              0x00000001
#define MCU_SW_INTERRUPT        0x00010094
#define  CONTROL_IND            0x00000004
#define  PATCH_IND              0x00000001
#define STEREO_MIC_CTRL         0x000100A4
#define  STEREO_MIC_MASK        0x0000000F      /* HS2_BIAS_EN, HS1_BIAS_EN, JACK_STEREO_CONFIG */
#define STEREO_MIC_CLAMP_CTRL   0x000100C4
#define  SMIC_HPAMP_CLAMP_DIS_FRC 0x00000001
#define  SMIC_HPAMP_CLAMP_DIS_FRC_VAL 0x00000002
#define BLOCK_EN3               0x00010108
#define  ADC1_EN                0x00000001
#define  PDM1_DIN_EN            0x0000000C      /* L | R */
#define  PDM2_DIN_EN            0x00000030
#define BLOCK_EN6               0x00010114
#define  MIXER_EN               0x00000001
#define BLOCK_EN8               0x0001011C
#define  HP_EN                  0x00000001
#define BLOCK_EN10              0x00010124
#define  AMP1_EN                0x00000001
#define  AMP2_EN                0x00000002
#define MIC_DETECT_CONTROL_1    0x00011074
#define  HSBIAS_MODE_MASK       0x00000006      /* 1 off (clamped), 2 low, 3 normal */
#define  HSBIAS_MODE_SHIFT      1
#define HS_BIAS_SENSE_AND_CLAMP_AUTOCONTROL 0x00011090
#define  JACKDET_MODE_MASK      0xC0000000
#define  JACKDET_MODE_SHIFT     30
#define  S0_AUTO_ADCMUTE_DISABLE 0x00800000
#define  HSBIAS_SENSE_TRIP_MASK 0x00000007
#define MIC_DETECT_CONTROL_ANDROID 0x000110B0
#define CTRL_REG                0x00014000
#define  PLL_MODE_BYPASS_MASK   0x00000006      /* BYPASS_500 | BYPASS_1029 */
#define  PLL_EN                 0x00000001
#define FDIV_FRAC               0x00014004
#define CAL_RATIO               0x00014008
#define DACCNFG2                0x00017004
#define HPPATHVOL               0x0001700C      /* AMP3 8:0, AMP4 24:16: 9-bit signed 0.5 dB, 0 = 0 dB */
#define PLL_INT                 0x0001B00C      /* the xxx_INT registers clear on read (precious) */
#define  PLL_READY_INT          0x00000001
#define SOFT_INT                0x0001B010
#define MSM_INT                 0x0001B018
#define  HP_STARTUP_DONE_INT    0x00000800
#define  HP_SHUTDOWN_DONE_INT   0x00000400
#define CLASS_D_AMP_INT         0x0001B030
#define  AMP1_STARTUP_DONE_INT  0x00000040
#define  AMP2_STARTUP_DONE_INT  0x00000080
#define  AMP1_SHUTDOWN_DONE_INT 0x00000100
#define  AMP2_SHUTDOWN_DONE_INT 0x00000200
#define SHADOW(r)               ((r) + 0x300)   /* xxx_INT_SHADOW: the same latches, not cleared by reads */
#define BOOT_CONTROL            0x00101000
#define  LOCK_HW_STS            0x00000002
#define PATCH_START_ADDR        0x00114004
#define  SOFT_PATCH_APPLIED_INT 0x00000001
#define  SOFT_CONTROL_APPLIED_INT 0x00000010
#define FW_SH_BOOT_CFG_NEED_CONFIGS 0x0011F8F8
/* CS42L43 / CS42L43B firmware mailboxes */
#define A_MCU_SW_REV            0x00114000
#define A_NEED_CONFIGS          0x0011400C
#define A_BOOT_STATUS           0x0011401C
#define A_MM_CTRL_SELECTION     0x0011FE0C
#define A_MM_MCU_CFG_REG        0x0011FE10
#define B_MCU_SW_REV            0x00117314
#define B_NEED_CONFIGS          0x00117320
#define B_BOOT_STATUS           0x00117330
#define B_MM_CTRL_SELECTION     0x00117E0C
#define B_MM_MCU_CFG_REG        0x00117E10
#define MM_MCU_CFG_DISABLE_VAL  0xF05AA50FUL
#define A_DECIM_VOL_CTRL        0x00008050      /* CH1_CH2, CH3_CH4 at +4 */
#define B_DECIM_VOL_CTRL        0x00008280      /* CH1_CH2, CH3_CH4 at +4, CH5_CH6 at +0x10 */

#define DEVID_A                 0x00042A43UL
#define DEVID_B                 0x0042A43BUL
#define MCU_ROM_REV             0x2001
#define MCU_ROM_BIOS_REV        0x0000
#define MCU_SUPPORTED_REV       0x2105
#define MCU_SUPPORTED_BIOS_REV  0x0001
#define MCU_SHADOW_REGS_REV     0x2200
#define BIOS_SHADOW_REGS_REV    0x1002
#define FW_FILE                 "fw\\cs42l43.bin"     /* linux-firmware cirrus/cs42l43.bin */
#define FW_HDR_SIZE             32              /* struct cs42l43_patch_header */
#define FW_FORMAT               3
#define FW_UPDATE_OFFSET        0x100000UL

/* mixer / mux sources (cs42l43_mixer_values; B: cs42l43b_mixer_values) */
#define SRC_NONE                0x00
#define SRC_DEC1                0x10
#define SRC_DEC2                0x11
#define SRC_DEC3                0x12            /* B: decimator 5 */
#define SRC_DEC4                0x13            /* B: decimator 6 */
#define SRC_B_DEC3              0x80
#define SRC_B_DEC4              0x81
#define SRC_DP5RX1              0x28
#define SRC_DP5RX2              0x29

/* decimator volume register fields: the odd decimator of a pair in 15:0, the even one in 31:16 */
#define DEC_ODD_MUTE            0x00008000UL
#define DEC_ODD_RAMP            0x00000038UL
#define DEC_ODD_VOL             0x00003FC0UL
#define DEC_EVEN_MUTE           0x80000000UL
#define DEC_EVEN_RAMP           0x00380000UL
#define DEC_EVEN_VOL            0x3FC00000UL
#define DEC_VOL_0DB             ((0x80UL << 6) | (0x80UL << 22))   /* -64 dB + 0.5 dB steps */

#define MIC_HEADSET 0
#define MIC_PDM1    1
#define MIC_PDM2    2
#define JACK_CTIA   0
#define JACK_OMTP   1
#define JACK_3POLE  2

typedef struct _L43 {
    ULONG   DevId;                  /* the variant from the part id (DEVID_A / DEVID_B) */
    ULONG   BusClk;                 /* SoundWire bus clock = PLL reference */
    ULONG   Mcu, Bios;              /* firmware revisions */
    ULONG   BootReg, RevReg, NeedReg, MmSelReg, MmCfgReg;
    ULONG   DecReg, DecMask, DecMute, MicEn, Src1, Src2;   /* capture path of Mic */
    UCHAR   Mic, Jack, Spk, BiasLow;
    UCHAR   RampMs, RampIdx;
    UCHAR   Pll, Out, In;           /* PLL + mixer core, playback outputs, capture path powered */
    UCHAR   Stage, Patched, Disabled, Tried;
} L43;
typedef char L43PrivFits[sizeof(L43) <= sizeof(((SDW_DEV *)0)->Priv) ? 1 : -1];

static L43 *P(SDW_DEV *d) { return (L43 *)d->Priv; }

/* ------------------------------------------------------------------ */
/* 32-bit register access (regmap-sdw: 4 single-byte transfers, ascending address, little endian) */
/* ------------------------------------------------------------------ */
static BOOLEAN Rd(SDW_DEV *d, ULONG reg, ULONG *v)
{
    UCHAR b[4];
    ULONG i;
    for (i = 0; i < 4; i++) if (!SdwRead(d, reg + i, &b[i])) return FALSE;
    *v = b[0] | ((ULONG)b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
    return TRUE;
}

static ULONG Get(SDW_DEV *d, ULONG reg)
{
    ULONG v = 0xFFFFFFFFUL;
    Rd(d, reg, &v);
    return v;
}

static BOOLEAN Wr(SDW_DEV *d, ULONG reg, ULONG v)
{
    ULONG i;
    for (i = 0; i < 4; i++) if (!SdwWrite(d, reg + i, (UCHAR)(v >> (8 * i)))) return FALSE;
    return TRUE;
}

static BOOLEAN Upd(SDW_DEV *d, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;
    if (!Rd(d, reg, &v)) return FALSE;
    if (((v & ~mask) | (val & mask)) == v) return TRUE;
    return Wr(d, reg, (v & ~mask) | (val & mask));
}

/* regmap_read_poll_timeout: (reg & mask) == want, polled every 'step' ms */
static BOOLEAN Poll(SDW_DEV *d, ULONG reg, ULONG mask, ULONG want, ULONG step, ULONG ms, ULONG *last)
{
    ULONG t, v = 0;
    for (t = 0;; t += step) {
        if (Rd(d, reg, &v) && (v & mask) == want) break;
        if (t >= ms) { if (last) *last = v; return FALSE; }
        SdwDelay(step);
    }
    if (last) *last = v;
    return TRUE;
}

/* the interrupt-driven completions of Linux, polled: the latch (xxx_INT_SHADOW) shows the event; Clear()
   reads the clear-on-read xxx_INT register before the operation is started */
static void Clear(SDW_DEV *d, ULONG intReg) { ULONG v; Rd(d, intReg, &v); }

static BOOLEAN WaitInt(SDW_DEV *d, ULONG intReg, ULONG bits, ULONG ms, const char *what)
{
    ULONG t, v, seen = 0;
    for (t = 0;; t += 2) {
        /* the shadow first (not cleared by the read), then the latch itself: an event is never lost */
        if (Rd(d, SHADOW(intReg), &v)) seen |= v;
        if ((seen & bits) != bits && Rd(d, intReg, &v)) seen |= v;
        if ((seen & bits) == bits) return TRUE;
        if (t >= ms) break;
        SdwDelay(2);
    }
    LOGE("sdw: %s: %s timed out (status %08x)", d->Name, what, seen);
    return FALSE;
}

static ULONG Fls(ULONG v) { ULONG n = 0; while (v) { n++; v >>= 1; } return n; }

/* ------------------------------------------------------------------ */
/* MCU firmware (cs42l43_mcu_update)                                    */
/* ------------------------------------------------------------------ */
#define STEP_DONE  0
#define STEP_AGAIN 1
#define STEP_FAIL  2

/* cs42l43_mcu_load_firmware: the whole file (header included) to load_addr + 0x100000, PATCH_IND, PATCH_APPLIED */
static BOOLEAN LoadPatch(SDW_DEV *d)
{
    ULONG len = 0, load, i, n, v = 0, t0;
    UCHAR *fw = OsLoadFile(FW_FILE, &len);
    BOOLEAN ok = TRUE;
    if (!fw) return FALSE;
    if (len < FW_HDR_SIZE || (fw[0] | (fw[1] << 8)) != FW_FORMAT) {
        LOGE("sdw: %s: %s: bad patch file (%u bytes, format %u)", d->Name, FW_FILE, len,
             len >= 2 ? fw[0] | (fw[1] << 8) : 0);
        OsFree(fw);
        return FALSE;
    }
    load = fw[28] | ((ULONG)fw[29] << 8) | ((ULONG)fw[30] << 16) | ((ULONG)fw[31] << 24);
    n = len & ~3UL;                                                 /* firmware->size / sizeof(u32) words */
    t0 = OsTimeMs();
    ok &= Wr(d, PATCH_START_ADDR, load);
    for (i = 0; i < n && ok; i++) ok &= SdwWrite(d, load + FW_UPDATE_OFFSET + i, fw[i]);
    OsFree(fw);
    if (!ok) { LOGE("sdw: %s: patch download failed at byte %u", d->Name, i); return FALSE; }
    Wr(d, MCU_SW_INTERRUPT, PATCH_IND);
    Wr(d, MCU_SW_INTERRUPT, 0);
    if (!Poll(d, SHADOW(SOFT_INT), SOFT_PATCH_APPLIED_INT, SOFT_PATCH_APPLIED_INT, 5, 500, &v)) {
        LOGE("sdw: %s: firmware patch not applied (%08x)", d->Name, v);
        return FALSE;
    }
    LOGI("sdw: %s: %s applied, %u bytes at %x in %u ms", d->Name, FW_FILE, n, load, OsTimeMs() - t0);
    return TRUE;
}

/* cs42l43_mcu_stage_2_3: NEED_CONFIGS = 0 (later firmware: the shadow register) moves the firmware to stage 3 */
static BOOLEAN Stage23(SDW_DEV *d, BOOLEAN shadow)
{
    L43 *p = P(d);
    ULONG v = 0;
    Wr(d, shadow && p->DevId == DEVID_A ? FW_SH_BOOT_CFG_NEED_CONFIGS : p->NeedReg, 0);
    if (Poll(d, p->BootReg, 0xFFFFFFFFUL, 3, 5, 20, &v)) return TRUE;
    LOGE("sdw: %s: firmware did not move to boot stage 3 (%x)", d->Name, v);
    return FALSE;
}

/* cs42l43_mcu_disable without its final soft reset (it would detach the device, see the top) */
static BOOLEAN McuDisable(SDW_DEV *d)
{
    L43 *p = P(d);
    ULONG v = 0;
    Wr(d, p->MmCfgReg, MM_MCU_CFG_DISABLE_VAL);
    Wr(d, p->MmSelReg, 1);                                          /* FW_MM_CTRL_MCU_SEL */
    Wr(d, MCU_SW_INTERRUPT, CONTROL_IND);
    Wr(d, MCU_SW_INTERRUPT, 0);
    if (Poll(d, SHADOW(SOFT_INT), SOFT_CONTROL_APPLIED_INT, SOFT_CONTROL_APPLIED_INT, 5, 20, &v)) return TRUE;
    LOGE("sdw: %s: firmware disable not acknowledged (%08x)", d->Name, v);
    return FALSE;
}

static ULONG BootStep(SDW_DEV *d)
{
    L43 *p = P(d);
    ULONG boot, rev, sec;
    BOOLEAN shadow;
    Clear(d, SOFT_INT);                                             /* stale software interrupts */
    if (!Rd(d, p->BootReg, &boot) || !Rd(d, p->RevReg, &rev) || !Rd(d, BOOT_CONTROL, &sec)) {
        LOGE("sdw: %s: firmware status not readable", d->Name);
        return STEP_FAIL;
    }
    p->Bios = ((((rev & 0x000F0000UL) << 12) | ((rev & 0x00F00000UL) << 4) | ((rev & 0xFF000000UL) >> 8)) >> 16);
    p->Mcu = ((rev & 0xFUL) << 12) | ((rev & 0xF0UL) << 4) | ((rev & 0xFF00UL) >> 8);
    p->Patched = p->Mcu != MCU_ROM_REV || p->Bios != MCU_ROM_BIOS_REV;
    p->Stage = (UCHAR)boot;
    shadow = p->Mcu >= MCU_SHADOW_REGS_REV || p->Bios >= BIOS_SHADOW_REGS_REV;
    LOGV("sdw: %s: firmware %x / %x, boot stage %u%s", d->Name, p->Mcu, p->Bios, boot,
         (sec & LOCK_HW_STS) ? ", secure" : "");
    if (!p->Patched && (sec & LOCK_HW_STS)) { LOGE("sdw: %s: unpatched secure device", d->Name); return STEP_FAIL; }
    switch (boot) {
    case 2:
        if (p->Patched) return Stage23(d, shadow) ? STEP_AGAIN : STEP_FAIL;
        if (p->Tried) {
            LOGE("sdw: %s: firmware still at the ROM revision after the patch", d->Name);
            return STEP_DONE;
        }
        p->Tried = TRUE;                                            /* the firmware can be patched only once */
        if (LoadPatch(d)) return STEP_AGAIN;
        LOGE("sdw: %s: ROM firmware needs the patch %s (linux-firmware cirrus/cs42l43.bin, copy it to %%SystemRoot%%\\System32\\drivers\\audiont\\); "
             "continuing with the firmware paused in boot stage 2", d->Name, FW_FILE);
        return STEP_DONE;
    case 3:
        if (!p->Patched) {
            /* Linux: NEED_CONFIGS = patch, HAVE_CONFIGS = 0, soft reset back to stage 2 - needs a re-enumeration */
            LOGE("sdw: %s: unpatched firmware running (stage 3), returning to stage 2 needs a soft reset: "
                 "the firmware stays active", d->Name);
            return STEP_DONE;
        }
        if (p->Mcu < MCU_SUPPORTED_REV && p->Bios < MCU_SUPPORTED_BIOS_REV) {
            LOGE("sdw: %s: firmware %x / %x too old to be disabled, it stays active", d->Name, p->Mcu, p->Bios);
            return STEP_DONE;
        }
        p->Disabled = McuDisable(d);
        return STEP_DONE;
    case 4:
        p->Disabled = TRUE;                                         /* already disabled (warm start) */
        return STEP_DONE;
    default:
        LOGE("sdw: %s: invalid boot status %u", d->Name, boot);
        return STEP_FAIL;
    }
}

static BOOLEAN Boot(SDW_DEV *d)
{
    ULONG i, r = STEP_AGAIN;
    for (i = 0; i < 5 && r == STEP_AGAIN; i++) r = BootStep(d);    /* CS42L43_MCU_UPDATE_RETRIES */
    if (r == STEP_AGAIN) LOGE("sdw: %s: firmware update did not finish", d->Name);
    return r == STEP_DONE;
}

/* cs42l43_reva_patch (regmap_register_patch after the firmware update) */
static const ULONG g_RevA[] = {
    0x4000, 0x00000055, 0x4000, 0x000000AA, 0x10084, 0x00000000, 0x1741C, 0x00CD2000, 0x1718C, 0x00000003,
    0x4000, 0x00000000,
    CCM_BLK_CLK_CONTROL, 0x00000002, HPPATHVOL, 0x011B011B, OSC_DIV_SEL, 0x00000001, DACCNFG2, 0x00000005,
    MIC_DETECT_CONTROL_ANDROID, 0x80790079, RELID, 0x0000000F,
};

/* ------------------------------------------------------------------ */
/* clocks: the PLL from the SoundWire clock (cs42l43_enable_pll / cs42l43_disable_pll)                */
/* ------------------------------------------------------------------ */
static const struct { ULONG Freq, Div, Mode, Cal; } g_Pll[] = {
    { 2400000, 0x50000000UL, 0x1, 0xA4 },
    { 3000000, 0x40000000UL, 0x1, 0x83 },
    { 3072000, 0x40000000UL, 0x3, 0x80 },
};

/* the bus clock (cs42l43_sdw_bus_config: curr_dr_freq / 2): read back from SCP_BusClock_Base / Scale that the
   core programmed (sdw_slave_set_frequency), else the link's DisCo clock as the core chooses it */
static ULONG BusClock(SDW_DEV *d)
{
    static const ULONG base[] = { 0, 19200000, 24000000, 24576000, 22579200, 32000000 };
    UCHAR b = 0, s = 0;
    AML_DEV ctrl, sub;
    ULONGLONG v;
    ULONG f[8], n, i, clk = 0;
    char name[40];
    if (SdwRead(d, SDW_SCP_BUS_CLOCK_BASE, &b) && SdwRead(d, SDW_SCP_BUSCLOCK_SCALE_B0, &s) &&
        b >= 1 && b <= 5 && s >= 1 && s <= 7)
        return base[b] >> (s - 1);
    OsSprintf(name, sizeof(name), "mipi-sdw-link-%u-subproperties", d->Link);
    if (AmlParent(&d->Acpi, &ctrl) && SdwAcpiSub(&ctrl, name, &sub)) {
        if (SdwAcpiInt(&sub, "mipi-sdw-max-clock-frequency", &v)) clk = (ULONG)v;
        n = SdwAcpiInts(&sub, "mipi-sdw-clock-frequencies-supported", f, 8);
        for (i = 0; i < n && !clk; i++) if (f[i] > clk) clk = f[i];
    }
    return clk ? clk : 4800000;
}

static BOOLEAN PllOn(SDW_DEV *d)
{
    L43 *p = P(d);
    ULONG freq = p->BusClk, div, i;
    if (Fls(freq) < Fls(g_Pll[ARRAYSIZE(g_Pll) - 1].Freq)) div = 99;
    else div = Fls(freq) - Fls(g_Pll[ARRAYSIZE(g_Pll) - 1].Freq);
    for (i = 0; div <= 3 && i < ARRAYSIZE(g_Pll) && (freq >> div) != g_Pll[i].Freq; i++) ;
    if (div > 3 || i == ARRAYSIZE(g_Pll)) {
        LOGE("sdw: %s: no PLL configuration for a %u Hz bus clock", d->Name, freq);
        return FALSE;
    }
    Upd(d, PLL_CONTROL, PLL_REFCLK_DIV_MASK | PLL_REFCLK_SRC_MASK, (div << 1) | 1);
    Wr(d, FDIV_FRAC, g_Pll[i].Div);
    Upd(d, CTRL_REG, PLL_MODE_BYPASS_MASK, g_Pll[i].Mode << 1);
    Upd(d, CAL_RATIO, 0xFF, g_Pll[i].Cal);
    Upd(d, PLL_CONTROL, PLL_REFCLK_EN, PLL_REFCLK_EN);
    Clear(d, PLL_INT);
    Upd(d, CTRL_REG, PLL_EN, PLL_EN);
    if (!WaitInt(d, PLL_INT, PLL_READY_INT, 200, "PLL lock")) {
        Upd(d, CTRL_REG, PLL_EN, 0);
        Upd(d, PLL_CONTROL, PLL_REFCLK_EN, 0);
        return FALSE;
    }
    /* no reads between these two: reads need OSC_DIV2_EN or the PLL, and the PLL cannot be selected with
       OSC_DIV2_EN set */
    Wr(d, OSC_DIV_SEL, 0);
    Wr(d, MCLK_SRC_SEL, OSC_PLL_MCLK_SEL);
    OsStallUs(5);
    return TRUE;
}

static void PllOff(SDW_DEV *d)
{
    Wr(d, MCLK_SRC_SEL, 0);
    OsStallUs(5);
    Wr(d, OSC_DIV_SEL, OSC_DIV2_EN);
    Upd(d, CTRL_REG, PLL_EN, 0);
    Upd(d, PLL_CONTROL, PLL_REFCLK_EN, 0);
}

/* DAPM supplies of every path: "PLL" and "Mixer Core" (all routes go through the mixer muxes) */
static BOOLEAN Clocks(SDW_DEV *d, BOOLEAN on)
{
    L43 *p = P(d);
    if (on == p->Pll) return TRUE;
    if (on) {
        if (!PllOn(d)) return FALSE;
        Upd(d, BLOCK_EN6, MIXER_EN, MIXER_EN);
    } else {
        Upd(d, BLOCK_EN6, MIXER_EN, 0);
        PllOff(d);
    }
    p->Pll = on;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* paths                                                                */
/* ------------------------------------------------------------------ */
static void SpkMute(SDW_DEV *d, BOOLEAN mute)
{
    ULONG v = 0x80 | (mute ? AMP_MUTE : 0);                         /* 0 dB (the machine driver's limit) */
    Upd(d, INTP_VOLUME_CTRL1, AMP_MUTE | AMP_VOL_MASK, v);
    Upd(d, INTP_VOLUME_CTRL2, AMP_MUTE | AMP_VOL_MASK, v);
    Upd(d, INTP_VOLUME_CTRL1, AMP1_2_VU, AMP1_2_VU);                /* cs42l43_spk_vu_sync */
    Upd(d, INTP_VOLUME_CTRL1, AMP1_2_VU, 0);
}

/* DAPM PGAs "AMP1", "AMP2" (BLOCK_EN10, waits for AMPn_STARTUP_DONE, 100 ms) and "HP" (BLOCK_EN8, the chip's
   own headphone power / de-pop state machine, HP_STARTUP_DONE, 2 s); off: the SHUTDOWN_DONE events */
static void Outputs(SDW_DEV *d, BOOLEAN on)
{
    L43 *p = P(d);
    ULONG amps = p->Spk ? AMP1_EN | AMP2_EN : 0;
    if (on == p->Out) return;
    if (!on && amps) SpkMute(d, TRUE);
    Clear(d, MSM_INT);
    Clear(d, CLASS_D_AMP_INT);
    if (amps) Upd(d, BLOCK_EN10, AMP1_EN | AMP2_EN, on ? amps : 0);
    Upd(d, BLOCK_EN8, HP_EN, on ? HP_EN : 0);
    if (amps)
        WaitInt(d, CLASS_D_AMP_INT, on ? AMP1_STARTUP_DONE_INT | AMP2_STARTUP_DONE_INT :
                AMP1_SHUTDOWN_DONE_INT | AMP2_SHUTDOWN_DONE_INT, 2 * 100, on ? "speaker startup" : "speaker shutdown");
    WaitInt(d, MSM_INT, on ? HP_STARTUP_DONE_INT : HP_SHUTDOWN_DONE_INT, 2000, on ? "HP startup" : "HP shutdown");
    if (on && amps) SpkMute(d, FALSE);
    p->Out = on;
}

/* cs42l43_start_hs_bias (no bias sense / auto clamp: nothing would service their interrupts) / stop_hs_bias */
static void Bias(SDW_DEV *d, BOOLEAN on)
{
    L43 *p = P(d);
    if (p->Mic != MIC_HEADSET || p->Jack == JACK_3POLE) return;
    if (on) {
        Upd(d, HS2, HS_CLAMP_DISABLE, HS_CLAMP_DISABLE);
        Upd(d, MIC_DETECT_CONTROL_1, HSBIAS_MODE_MASK, (p->BiasLow ? 2UL : 3UL) << HSBIAS_MODE_SHIFT);
        SdwDelay(p->RampMs);
    } else {
        Upd(d, MIC_DETECT_CONTROL_1, HSBIAS_MODE_MASK, 1UL << HSBIAS_MODE_SHIFT);
        Upd(d, HS2, HS_CLAMP_DISABLE, 0);
    }
}

/* ADC1 / PDM PGAs with cs42l43_mic_ev: the decimator muted with its ramp off while the input starts, then the
   cached mute / ramp restored; off: the enables cleared (adc_ev PRE_PMD, PGA power down) */
static void Capture(SDW_DEV *d, BOOLEAN on)
{
    L43 *p = P(d);
    ULONG cache = 0;
    if (on == p->In) return;
    if (on) {
        Bias(d, TRUE);
        Rd(d, p->DecReg, &cache);
        Upd(d, p->DecReg, p->DecMask, p->DecMute);
        Upd(d, BLOCK_EN3, p->MicEn, p->MicEn);
        Upd(d, p->DecReg, p->DecMask, cache);
    } else {
        Upd(d, BLOCK_EN3, p->MicEn, 0);
        Bias(d, FALSE);
    }
    p->In = on;
}

/* the capture path of the microphone selection (cs42l43_mic_ev decimator mapping per variant) */
static void MicPath(L43 *p)
{
    BOOLEAN b = p->DevId == DEVID_B;
    ULONG base = b ? B_DECIM_VOL_CTRL : A_DECIM_VOL_CTRL;
    switch (p->Mic) {
    case MIC_PDM1:                                                  /* A: decimators 1, 2; B: 3, 4 */
        p->MicEn = PDM1_DIN_EN;
        p->Src1 = b ? SRC_B_DEC3 : SRC_DEC1; p->Src2 = b ? SRC_B_DEC4 : SRC_DEC2;
        p->DecReg = base + (b ? 4 : 0);
        break;
    case MIC_PDM2:                                                  /* A: decimators 3, 4; B: 5, 6 */
        p->MicEn = PDM2_DIN_EN;
        p->Src1 = SRC_DEC3; p->Src2 = SRC_DEC4;
        p->DecReg = base + (b ? 0x10 : 4);
        break;
    default:                                                        /* ADC1 -> decimator 1, mono on both */
        p->MicEn = ADC1_EN;
        p->Src1 = p->Src2 = SRC_DEC1;
        p->DecReg = base;
        break;
    }
    if (p->Mic == MIC_HEADSET) { p->DecMask = DEC_ODD_MUTE | DEC_ODD_RAMP; p->DecMute = DEC_ODD_MUTE; }
    else {
        p->DecMask = DEC_ODD_MUTE | DEC_ODD_RAMP | DEC_EVEN_MUTE | DEC_EVEN_RAMP;
        p->DecMute = DEC_ODD_MUTE | DEC_EVEN_MUTE;
    }
}

/* cs42l43_set_jack with the default properties, then the "Jack Override" CTIA / OMTP / headphone setting */
static void Jack(SDW_DEV *d)
{
    L43 *p = P(d);
    static const ULONG hs[3] = {
        AMP3_4_GNDREF_HS3 | HSBIAS_GNDREF_HS3 | HSBIAS_OUT_HS4 | HSGND_HS3,
        (1UL << HSDET_MANUAL_SHIFT) | AMP3_4_GNDREF_HS4 | HSBIAS_GNDREF_HS4 | HSBIAS_OUT_HS3 | HSGND_HS4,
        (2UL << HSDET_MANUAL_SHIFT) | AMP3_4_GNDREF_HS3 | AMP3_4_GNDREF_HS4 | HSBIAS_GNDREF_HS3 | HSBIAS_GNDREF_HS4 |
        HSGND_HS3 | HSGND_HS4,
    };
    ULONGLONG v;
    ULONG mode = 3;                                                 /* tip sense pull-up on */
    if (SdwAcpiInt(&d->Acpi, "cirrus,tip-disable-pullup", &v) && v) mode = 1;
    /* tip sense: inverted (the hardware wants it), 500 ms debounce both ways */
    Upd(d, TIPSENSE_DEB_CTRL, TIPSENSE_INV | TIPSENSE_FALL_MASK | TIPSENSE_RISE_MASK, TIPSENSE_INV | (3 << 3) | 3);
    Upd(d, HS_BIAS_SENSE_AND_CLAMP_AUTOCONTROL, JACKDET_MODE_MASK | S0_AUTO_ADCMUTE_DISABLE | HSBIAS_SENSE_TRIP_MASK,
        (mode << JACKDET_MODE_SHIFT) | S0_AUTO_ADCMUTE_DISABLE);
    Upd(d, PDNCNTL, RING_SENSE_EN, 0);
    /* HS2: bias ramp, 50 ms detect time; the override's switch settings (it leaves HSDET_MODE at 0) */
    Upd(d, HS2, HSBIAS_RAMP_MASK | AUTO_HSDET_TIME_MASK | HSDET_MODE_MASK | HSDET_MANUAL_MASK | 0xFF,
        ((ULONG)p->RampIdx << HSBIAS_RAMP_SHIFT) | (4UL << AUTO_HSDET_TIME_SHIFT) | hs[p->Jack]);
    Upd(d, STEREO_MIC_CTRL, STEREO_MIC_MASK, 0);
    Upd(d, STEREO_MIC_CLAMP_CTRL, SMIC_HPAMP_CLAMP_DIS_FRC | SMIC_HPAMP_CLAMP_DIS_FRC_VAL, SMIC_HPAMP_CLAMP_DIS_FRC);
    Upd(d, ADC_B_CTRL1, PGA_WIDESWING_MODE_EN, 0);
    Upd(d, ADC_B_CTRL2, PGA_WIDESWING_MODE_EN, 0);
}

/* ------------------------------------------------------------------ */
/* driver                                                               */
/* ------------------------------------------------------------------ */
static BOOLEAN Probe(SDW_DEV *d)
{
    d->Kind = SDW_KIND_HEADSET;
    /* cs42l43_read_prop: full ports, 24 bit, no simple_ch_prep_sm (channel prepare handshake) */
    d->Play.Num = 5; d->Play.Ch = 2; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = TRUE;
    d->Rec.Num = 2; d->Rec.Ch = 2; d->Rec.Type = SDW_DP_FULL; d->Rec.CpSm = TRUE;
    d->Paging = TRUE;
    return TRUE;
}

static void Settings(SDW_DEV *d)
{
    static const UCHAR ramps[4] = { 10, 40, 90, 170 };
    L43 *p = P(d);
    ULONGLONG v;
    ULONG i;
    p->Mic = (UCHAR)OsRegGetDword(L"Cs42l43Mic", MIC_HEADSET);
    if (p->Mic > MIC_PDM2) p->Mic = MIC_HEADSET;
    p->Jack = (UCHAR)OsRegGetDword(L"Cs42l43Jack", JACK_CTIA);
    if (p->Jack > JACK_3POLE) p->Jack = JACK_CTIA;
    p->Spk = OsRegGetDword(L"Cs42l43Spk", 1) != 0;
    p->BiasLow = SdwAcpiInt(&d->Acpi, "cirrus,bias-low", &v) && v;
    p->RampIdx = 3;                                                 /* cirrus,bias-ramp-ms default 170 */
    if (SdwAcpiInt(&d->Acpi, "cirrus,bias-ramp-ms", &v))
        for (i = 0; i < 4; i++) if (v == ramps[i]) p->RampIdx = (UCHAR)i;
    p->RampMs = ramps[p->RampIdx];
    MicPath(p);
}

static BOOLEAN Init(SDW_DEV *d)
{
    L43 *p = P(d);
    ULONG e0 = d->Errors, id = 0, rev = 0, otp = 0, i, spkL, spkR;
    BOOLEAN b = d->Part == 0x2A3B;
    memset(p, 0, sizeof(*p));
    p->DevId = b ? DEVID_B : DEVID_A;
    p->BootReg = b ? B_BOOT_STATUS : A_BOOT_STATUS;
    p->RevReg = b ? B_MCU_SW_REV : A_MCU_SW_REV;
    p->NeedReg = b ? B_NEED_CONFIGS : A_NEED_CONFIGS;
    p->MmSelReg = b ? B_MM_CTRL_SELECTION : A_MM_CTRL_SELECTION;
    p->MmCfgReg = b ? B_MM_MCU_CFG_REG : A_MM_MCU_CFG_REG;
    Settings(d);
    /* cs42l43_wait_for_attach: OSC_DIV2_EN before any SoundWire read */
    if (!Wr(d, OSC_DIV_SEL, OSC_DIV2_EN) || !Rd(d, DEVID, &id)) {
        LOGE("sdw: %s: no register access", d->Name);
        return FALSE;
    }
    if (id != p->DevId) { LOGE("sdw: %s: device id %06x, expected %06x", d->Name, id, p->DevId); return FALSE; }
    Rd(d, REVID, &rev);
    Rd(d, OTP_REVISION_ID, &otp);
    if (!Boot(d)) return FALSE;
    for (i = 0; i < ARRAYSIZE(g_RevA); i += 2) Wr(d, g_RevA[i], g_RevA[i + 1]);
    p->BusClk = BusClock(d);
    /* everything off; 48 kHz (cs42l43_set_sample_rate: sample rate 1 for all SoundWire ports) */
    Wr(d, MCLK_SRC_SEL, 0);
    Wr(d, OSC_DIV_SEL, OSC_DIV2_EN);
    Upd(d, CTRL_REG, PLL_EN, 0);
    Upd(d, PLL_CONTROL, PLL_REFCLK_EN, 0);
    Upd(d, BLOCK_EN3, ADC1_EN | PDM1_DIN_EN | PDM2_DIN_EN, 0);
    Upd(d, BLOCK_EN6, MIXER_EN, 0);
    Upd(d, BLOCK_EN8, HP_EN, 0);
    Upd(d, BLOCK_EN10, AMP1_EN | AMP2_EN, 0);
    Upd(d, SAMPLE_RATE1, 0x1F, 0x03);
    /* routing: DP5RX1 / 2 -> headphone L / R and speaker L / R mixers (input 1 at 0 dB, inputs 2..4 none) */
    spkL = p->Spk ? SRC_DP5RX1 : SRC_NONE;
    spkR = p->Spk ? SRC_DP5RX2 : SRC_NONE;
    for (i = 0; i < 4; i++) {
        Wr(d, AMP1MIX_INPUT1 + 4 * i, MIXER_VOL_0DB | (i ? SRC_NONE : spkL));
        Wr(d, AMP2MIX_INPUT1 + 4 * i, MIXER_VOL_0DB | (i ? SRC_NONE : spkR));
        Wr(d, AMP3MIX_INPUT1 + 4 * i, MIXER_VOL_0DB | (i ? SRC_NONE : SRC_DP5RX1));
        Wr(d, AMP4MIX_INPUT1 + 4 * i, MIXER_VOL_0DB | (i ? SRC_NONE : SRC_DP5RX2));
    }
    Wr(d, HPPATHVOL, 0);                                            /* headphone digital volume 0 dB both */
    SpkMute(d, TRUE);
    /* capture: decimator(s) 0 dB unmuted -> DP2TX1 / 2; ADC1 from IN1 with the PGA at +18 dB (its maximum) */
    Wr(d, SWIRE_DP2_CH1_INPUT, p->Src1);
    Wr(d, SWIRE_DP2_CH2_INPUT, p->Src2);
    Upd(d, p->DecReg, p->DecMute | (p->DecMute & DEC_EVEN_MUTE ? DEC_ODD_VOL | DEC_EVEN_VOL : DEC_ODD_VOL),
        DEC_VOL_0DB & (p->DecMute & DEC_EVEN_MUTE ? DEC_ODD_VOL | DEC_EVEN_VOL : DEC_ODD_VOL));
    Upd(d, ADC_B_CTRL1, ADC_AIN_SEL | ADC_PGA_GAIN_MASK, 3);
    Jack(d);
    Upd(d, MIC_DETECT_CONTROL_1, HSBIAS_MODE_MASK, 1UL << HSBIAS_MODE_SHIFT);   /* bias off */
    Upd(d, HS2, HS_CLAMP_DISABLE, 0);
    Clear(d, PLL_INT);
    Clear(d, MSM_INT);
    Clear(d, CLASS_D_AMP_INT);
    LOGI("sdw: %s: CS42L43%s rev %02x otp %02x, firmware %x / %x stage %u%s, bus clock %u Hz, mic %s, jack %s%s, "
         "%u bus errors in init", d->Name, b ? "B" : "", rev, otp, p->Mcu, p->Bios, p->Stage,
         p->Disabled ? " (disabled)" : " (active)", p->BusClk,
         p->Mic == MIC_PDM1 ? "PDM1" : p->Mic == MIC_PDM2 ? "PDM2" : "headset",
         p->Jack == JACK_OMTP ? "OMTP" : p->Jack == JACK_3POLE ? "3-pole" : "CTIA", p->Spk ? ", speaker amp" : "",
         d->Errors - e0);
    return d->Errors - e0 < 16;
}

/* DAPM order: supplies (PLL, mixer core) first, then the PGAs; off in reverse; the PLL is shared */
static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    L43 *p = P(d);
    if (on) {
        if (!Clocks(d, TRUE)) return;
        if (rec) Capture(d, TRUE); else Outputs(d, TRUE);
    } else {
        if (rec) Capture(d, FALSE); else Outputs(d, FALSE);
        if (!p->Out && !p->In) Clocks(d, FALSE);
    }
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    Outputs(d, FALSE);
    Capture(d, FALSE);
    Clocks(d, FALSE);
}

static const USHORT g_Parts[] = { 0x4243, 0x2A3B, 0 };
const SDW_DRV g_SdwCs42l43 = { "CS42L43", 0x01FA, g_Parts, SDW_KIND_HEADSET, Probe, Init, Stream, Shutdown };
