/*
 * AUDIONT - Cirrus Logic CS35L56 / CS35L57 / CS35L63 (CS35L62) smart amplifiers on SoundWire,
 * running their ROM firmware.  Linux sound/soc/codecs/cs35l56-sdw.c (bus binding),
 * cs35l56.c (cs35l56_init, cs35l56_play_event, cs35l56_sdw_dai_hw_params), cs35l56-shared.c
 * (cs35l56_hw_init, cs35l56_set_patch, cs35l56_mbox_send, cs35l56_runtime_suspend_common).
 * The I2C / SPI side-amplifier driver for HD Audio laptops is cod_cs35l56.c; nothing is shared.
 *
 * Register access: the 32-bit registers of the chip are at SoundWire address reg + 0x8000
 * (paged), little endian, one byte per address (cs35l56_regmap_swab_bus_sdw over the sdw-le32
 * regmap: LSB first, MSB last).  SoundWire ports: DP1 sink (2 channels, full channel prepare
 * state machine), DP3 source (I/V feedback: not used).
 *
 * The HALO DSP always runs: the ROM firmware boots by itself, play / pause are mailbox
 * commands.  Linux downloads a patch (.wmfw) and the tuning (.bin) when PROTECTION_STATUS
 * says FIRMWARE_MISSING; like cod_cs35l56.c this driver plays with the ROM firmware (no tuning:
 * no speaker-specific EQ / protection limits; no EFI calibration).  The channel an amplifier
 * plays is the firmware's: DSP1RX1 / DSP1RX2_INPUT belong to it (the tuning of each amplifier
 * selects SDW1RX1 or SDW1RX2), so both channels are sent to every amplifier (mirrored, as
 * Linux' asoc_sdw_hw_params does for playback) and the inputs are only logged.
 *
 * Init (cs35l56_sdw_init -> cs35l56_init): wake (two dummy reads), REVID, firmware boot wait
 * (HALO_STATE), DEVID, secure / OTP / firmware version, IRQ masks, cs35l56_set_patch (SDCA
 * defaults the firmware may change, user mute / volume / posture), auto-hibernate prevented,
 * ASP1 DOUT Hi-Z, the Cirrus SoundWire interrupts masked (commands are polled).  Linux' soft
 * reset after the first init (no reset GPIO) is NOT done: on SoundWire the chip detaches and
 * re-enumerates after SYSTEM_RESET, which the bus core does not handle for a peripheral; the
 * patch writes put the registers the firmware may have changed back to their defaults.
 * Stream: DAPM "PLAY" supply - AUDIO_PLAY (not waited for), then TRANSDUCER_ACTUAL_PS == PS0
 * (50 ms); off: AUDIO_PAUSE (acknowledged).  Shutdown: cs35l56_runtime_suspend_common for
 * SoundWire (PS3 wait, BOOT_DONE cleared; no hibernation, Linux can_hibernate is I2C / SPI only).
 */
#include "sdw.h"

#define SDW_ADDR_OFFSET         0x8000
#define SDW_GEN_INT_STAT_1      0xc0
#define SDW_GEN_INT_MASK_1      0xc1

#define L56_DEVID               0x0000000
#define L56_REVID               0x0000004
#define L56_OTPID               0x0000010
#define L56_ASP1_CONTROL3       0x000480C       /* ASP1_DOUT_HIZ_CTRL 1:0 */
#define L56_DSP1RX1_INPUT       0x0004C40
#define L56_DSP1RX2_INPUT       0x0004C44
#define L56_SWIRE_DP3_CH1_INPUT 0x0004C70
#define L56_IRQ1_STATUS         0x000E004
#define L56_IRQ1_EINT_4         0x000E01C       /* OTP_BOOT_DONE 1 */
#define L56_IRQ1_MASK_1         0x000E090       /* AMP_SHORT_ERR 31 */
#define L56_IRQ1_MASK_8         0x000E0AC       /* TEMP_ERR 31 */
#define L56_IRQ1_MASK_18        0x000E0D4
#define L56_IRQ1_MASK_20        0x000E0DC
#define L56_DSP_VIRTUAL1_MBOX_1 0x0011020
#define L56_DSP_RESTRICT_STS1   0x00190F0
#define L56_DSP1_FW_VER         0x2800010
/* the firmware registers (cs35l56_fw_reg, cs35l56_b2_fw_reg, cs35l63_fw_reg) */
#define L56_B0_HALO_STATE       0x28021E0
#define L56_B2_HALO_STATE       0x2803D20
#define L56_USER_MUTE           0x3400024
#define L56_USER_VOLUME         0x340002C
#define L56_POSTURE             0x3400094
#define L56_PROTECTION_STATUS   0x34000D8       /* FIRMWARE_MISSING 0 */
#define L56_TRANSDUCER_PS       0x3400150
#define L63_HALO_STATE          0x2803C04
#define L63_USER_MUTE           0x3400020
#define L63_USER_VOLUME         0x3400028
#define L63_POSTURE             0x3400068
#define L63_PROTECTION_STATUS   0x340009C
#define L63_TRANSDUCER_PS       0x34000F4

#define MBOX_AUDIO_PLAY         0x0B000001
#define MBOX_AUDIO_PAUSE        0x0B000002
#define MBOX_PREVENT_HIBERNATE  0x02000003
#define HALO_BOOT_DONE          2
#define PS0                     0
#define PS3                     3

/* Priv[] */
#define P_TYPE  0               /* 0x54 / 0x56 / 0x57 / 0x62 / 0x63 */
#define P_REV   1
#define P_ON    2
#define P_LOGGED 3

static BOOLEAN Is63(SDW_DEV *d) { return d->Priv[P_TYPE] == 0x63 || d->Priv[P_TYPE] == 0x62; }
static ULONG Halo(SDW_DEV *d)                           /* cs35l56_set_fw_reg_table */
{
    return Is63(d) ? L63_HALO_STATE : d->Priv[P_REV] == 0xB0 ? L56_B0_HALO_STATE : L56_B2_HALO_STATE;
}
static ULONG TransducerPs(SDW_DEV *d) { return Is63(d) ? L63_TRANSDUCER_PS : L56_TRANSDUCER_PS; }

static BOOLEAN Wr32(SDW_DEV *d, ULONG reg, ULONG v)
{
    ULONG i;
    for (i = 0; i < 4; i++) if (!SdwWrite(d, reg + SDW_ADDR_OFFSET + i, (UCHAR)(v >> (8 * i)))) return FALSE;
    return TRUE;
}

static BOOLEAN Rd32(SDW_DEV *d, ULONG reg, ULONG *v)
{
    ULONG i;
    UCHAR b;
    *v = 0;
    for (i = 0; i < 4; i++) {
        if (!SdwRead(d, reg + SDW_ADDR_OFFSET + i, &b)) return FALSE;
        *v |= (ULONG)b << (8 * i);
    }
    return TRUE;
}

static BOOLEAN Upd32(SDW_DEV *d, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;
    return Rd32(d, reg, &v) && Wr32(d, reg, (v & ~mask) | (val & mask));
}

/* cs35l56_wait_for_firmware_boot: 2 <= HALO_STATE < 0xFFFF within 250 ms */
static BOOLEAN WaitBoot(SDW_DEV *d)
{
    ULONG i, v = 0;
    for (i = 0; i < 250; i++) {
        if (Rd32(d, Halo(d), &v) && v < 0xFFFF && v >= HALO_BOOT_DONE) return TRUE;
        SdwDelay(1);
    }
    LOGE("sdw: %s: firmware boot timed out, HALO_STATE %x", d->Name, v);
    return FALSE;
}

/* cs35l56_mbox_send: MBOX_1 reads 0 once the firmware took the command (5 ms) */
static BOOLEAN Mbox(SDW_DEV *d, ULONG cmd)
{
    ULONG i, v = 1;
    if (!Wr32(d, L56_DSP_VIRTUAL1_MBOX_1, cmd)) return FALSE;
    for (i = 0; i < 6; i++) {
        if (Rd32(d, L56_DSP_VIRTUAL1_MBOX_1, &v) && v == 0) return TRUE;
        SdwDelay(1);
    }
    LOGE("sdw: %s: mailbox command %08x not taken (%08x)", d->Name, cmd, v);
    return FALSE;
}

static BOOLEAN PollPs(SDW_DEV *d, ULONG ps, BOOLEAN atLeast, ULONG ms)
{
    ULONG i, v = 0xFFFFFFFF;
    for (i = 0; i <= ms; i++) {
        if (Rd32(d, TransducerPs(d), &v) && (atLeast ? v >= ps : v == ps)) return TRUE;
        SdwDelay(1);
    }
    LOGI("sdw: %s: power state %u not reached (%x)", d->Name, ps, v);
    return FALSE;
}

static BOOLEAN Probe(SDW_DEV *d)
{
    d->Kind = SDW_KIND_AMP;
    d->Play.Num = 1; d->Play.Ch = 2; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = TRUE;    /* no simple_ch_prep_sm */
    memset(&d->Rec, 0, sizeof(d->Rec));                 /* DP3 feedback not used */
    d->Paging = TRUE;
    d->Priv[P_TYPE] = d->Part & 0xFF;                   /* sdw_device_id driver_data until DEVID is read */
    return TRUE;
}

static BOOLEAN Init(SDW_DEV *d)
{
    ULONG e0 = d->Errors, rev = 0, id = 0, sec = 0, otp = 0, prot = 0, fw = 0, v;
    UCHAR st;
    BOOLEAN is63;
    d->Priv[P_ON] = FALSE;
    /* cs35l56_issue_wake_event: two dummy reads 1 ms apart, then tIRS (2.2 ms) */
    Rd32(d, L56_IRQ1_STATUS, &v);
    SdwDelay(1);
    Rd32(d, L56_IRQ1_STATUS, &v);
    SdwDelay(3);
    if (!Rd32(d, L56_REVID, &rev)) return FALSE;
    d->Priv[P_REV] = rev & 0xFF;
    if (!WaitBoot(d)) return FALSE;
    if (!Rd32(d, L56_DEVID, &id)) return FALSE;
    id &= 0x00FFFFFF;
    switch (id) {
    case 0x35A54: case 0x35A56: case 0x35A57: d->Priv[P_TYPE] = id & 0xFF; break;
    case 0x35A630: case 0x35A620: d->Priv[P_TYPE] = (id >> 4) & 0xFF; break;
    default:
        LOGE("sdw: %s: unknown device id %x", d->Name, id);
        return FALSE;
    }
    is63 = Is63(d);
    Rd32(d, L56_DSP_RESTRICT_STS1, &sec);
    Rd32(d, L56_OTPID, &otp);
    Rd32(d, is63 ? L63_PROTECTION_STATUS : L56_PROTECTION_STATUS, &prot);
    Rd32(d, L56_DSP1_FW_VER, &fw);
    /* wake source and *_BLOCKED interrupts default to unmasked */
    Wr32(d, L56_IRQ1_MASK_20, 0xFFFFFFFF);
    Upd32(d, L56_IRQ1_MASK_1, 0x80000000, 0);
    Upd32(d, L56_IRQ1_MASK_8, 0x80000000, 0);
    /* cs35l56_set_patch */
    Wr32(d, L56_SWIRE_DP3_CH1_INPUT, 0x18);             /* VMON */
    Wr32(d, L56_SWIRE_DP3_CH1_INPUT + 4, 0x19);         /* IMON */
    Wr32(d, L56_SWIRE_DP3_CH1_INPUT + 8, 0x29);         /* VBSTMON */
    Wr32(d, L56_SWIRE_DP3_CH1_INPUT + 12, 0x28);        /* VDDBMON */
    Wr32(d, L56_IRQ1_MASK_18, 0x1F7DF0FF);
    Wr32(d, is63 ? L63_USER_MUTE : L56_USER_MUTE, 0);
    Wr32(d, is63 ? L63_USER_VOLUME : L56_USER_VOLUME, 0);   /* 0 dB */
    Wr32(d, is63 ? L63_POSTURE : L56_POSTURE, 0);
    /* auto-hibernate off: the driver decides when the amplifier sleeps */
    if (!Mbox(d, MBOX_PREVENT_HIBERNATE)) return FALSE;
    Upd32(d, L56_ASP1_CONTROL3, 0x3, 0x3);              /* ASP1 DOUT Hi-Z when not transmitting */
    /* cs35l56_mask_soundwire_interrupts */
    SdwWrite(d, SDW_GEN_INT_MASK_1, 0);
    SdwRead(d, SDW_GEN_INT_STAT_1, &st);
    SdwWrite(d, SDW_GEN_INT_STAT_1, 0xFF);
    d->Priv[P_LOGGED] = FALSE;
    LOGI("sdw: %s: CS35L%02X%s rev %02x OTP %u, firmware %u.%u.%u %s, amplifier %u of %u (channel: firmware), "
         "%u bus errors in init", d->Name, d->Priv[P_TYPE], (sec & 7) ? "s" : "", d->Priv[P_REV], otp, fw >> 16,
         (fw >> 8) & 0xFF, fw & 0xFF, (prot & 1) ? "(ROM, not patched: no tuning)" : "(patched by the BIOS)",
         d->Index, d->Count, d->Errors - e0);
    return d->Errors - e0 < 8;
}

static void Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    if (rec) return;
    if (on) {
        /* DAPM "PLAY": PRE_PMU AUDIO_PLAY without waiting for the acknowledgement, POST_PMU PS0 */
        Wr32(d, L56_DSP_VIRTUAL1_MBOX_1, MBOX_AUDIO_PLAY);
        PollPs(d, PS0, FALSE, 50);
        if (!d->Priv[P_LOGGED]) {
            ULONG rx1 = 0, rx2 = 0;
            Rd32(d, L56_DSP1RX1_INPUT, &rx1);
            Rd32(d, L56_DSP1RX2_INPUT, &rx2);
            LOGI("sdw: %s: playing, DSP1RX1 / RX2 inputs %02x / %02x (0x44 / 0x45: SoundWire DP1 channel 1 / 2)",
                 d->Name, rx1, rx2);
            d->Priv[P_LOGGED] = TRUE;
        }
    } else {
        Mbox(d, MBOX_AUDIO_PAUSE);                      /* POST_PMD */
    }
    d->Priv[P_ON] = on;
}

static void Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    if (d->Priv[P_ON]) Stream(d, FALSE, FALSE);
    PollPs(d, PS3, TRUE, 300);
    Wr32(d, L56_IRQ1_EINT_4, 0x2);                      /* clear OTP_BOOT_DONE: detects a reboot */
}

static const USHORT g_Parts[] = { 0x3556, 0x3557, 0x3562, 0x3563, 0 };
const SDW_DRV g_SdwCs35l56 = { "CS35L56", 0x01FA, g_Parts, SDW_KIND_AMP, Probe, Init, Stream, Shutdown };
