/*
 * AUDIONT - Cirrus Logic CS35L54 / CS35L56 / CS35L57 smart amplifiers behind an HD Audio codec
 * ("CSC3554", "CSC3556", "CSC3557"; sideamp.c), running their ROM firmware.
 *
 * I2C or SPI, 32-bit register address / 32-bit value, big endian (cs35l56_regmap_i2c; SPI:
 * cs35l56_regmap_spi, 16 pad bits, read flag 0x80, set up by sideamp.c).
 * Reference: sound/hda/codecs/side-codecs/cs35l56_hda.c (cs35l56_hda_common_probe,
 * cs35l56_hda_play / _pause, cs35l56_hda_dai_config) and sound/soc/codecs/cs35l56-shared.c
 * (cs35l56_hw_init, cs35l56_system_reset, cs35l56_wait_for_firmware_boot, cs35l56_set_patch,
 * cs35l56_mbox_send, cs35l56_runtime_suspend_common).
 *
 * Unlike the CS35L41 the amplifier is always run by its HALO DSP: the chip boots a firmware from
 * ROM, and play / pause are mailbox commands to it.  Linux additionally downloads a patch (.wmfw)
 * and the laptop's tuning (.bin) from linux-firmware cirrus/ when the BIOS has not ("FIRMWARE_MISSING"
 * in PROTECTION_STATUS); without them it still sends AUDIO_PLAY to the ROM firmware, which is what
 * this driver does - untuned (no speaker-specific EQ / protection limits from the tuning, channel
 * routing as the ROM firmware's default: DSP1RX1/2_INPUT belong to the firmware and are not written).
 *
 * ASP1 (cs35l56_hda_dai_config): I2S consumer, BCLK 3.072 MHz, 32-cycle slots, 24-bit words, RX1 / RX2
 * from slots 0 / 1, TX1..4 (VMON, IMON, ERRVOL, VDDBMON) on slots 0..3 with only TX<index> enabled.
 * Volume: none (the codec's).  GPIO: sideamp.c handles reset-gpios (one pulse at probe).
 */
#include "codec.h"

#define L56_DEVID              0x00000000
#define L56_REVID              0x00000004   /* AREVID 7:4, MTLREVID 3:0 */
#define L56_OTPID              0x00000010
#define L56_ASP1_ENABLES1      0x00004800   /* RX2 17, RX1 16, TX4..1 3:0 */
#define L56_ASP1_CONTROL1      0x00004804
#define L56_ASP1_CONTROL2      0x00004808
#define L56_ASP1_CONTROL3      0x0000480C
#define L56_ASP1_FRAME_CTRL1   0x00004810
#define L56_ASP1_FRAME_CTRL5   0x00004820
#define L56_ASP1_DATA_CTRL1    0x00004830
#define L56_ASP1_DATA_CTRL5    0x00004840
#define L56_ASP1TX1_INPUT      0x00004C20
#define L56_SWIRE_DP3_CH1      0x00004C70
#define L56_IRQ1_STATUS        0x0000E004
#define L56_IRQ1_MASK_1        0x0000E090
#define L56_IRQ1_MASK_8        0x0000E0AC
#define L56_IRQ1_MASK_18       0x0000E0D4
#define L56_IRQ1_MASK_20       0x0000E0DC
#define L56_DSP_MBOX_1_RAW     0x00011000
#define L56_DSP_VIRT1_MBOX_1   0x00011020
#define L56_DSP_VIRT1_MBOX_2   0x00011024
#define L56_DSP_RESTRICT_STS1  0x000190F0
#define L56_DSP1_FW_VER        0x02800010
#define L56_B0_HALO_STATE      0x028021E0
#define L56_B2_HALO_STATE      0x02803D20
#define L56_MAIN_USER_MUTE     0x03400024
#define L56_MAIN_USER_VOLUME   0x0340002C
#define L56_MAIN_POSTURE       0x03400094
#define L56_PROTECTION_STATUS  0x034000D8   /* FIRMWARE_MISSING 0 */
#define L56_TRANSDUCER_PS      0x03400150

#define MBOX_PING              0x0A000000
#define MBOX_AUDIO_PLAY        0x0B000001
#define MBOX_AUDIO_PAUSE       0x0B000002
#define MBOX_HIBERNATE_NOW     0x02000001
#define MBOX_PREVENT_HIBERNATE 0x02000003
#define MBOX_ALLOW_HIBERNATE   0x02000004
#define MBOX_SYSTEM_RESET      0x02000007
#define HALO_BOOT_DONE         2
#define PS0                    0
#define PS3                    3

/* Priv[] */
#define P_REV                  0
#define P_TYPE                 1            /* 0x54 / 0x56 / 0x57 */
#define P_FWMISSING            2

static ULONG L56Halo(SST_CODEC *c)
{
    return c->Priv[P_REV] == 0xB0 ? L56_B0_HALO_STATE : L56_B2_HALO_STATE;   /* cs35l56_set_fw_reg_table */
}

/* a write without the core's retry: the reset command may not be acknowledged by a chip that resets */
static void L56WriteOnce(SST_CODEC *c, ULONG reg, ULONG val)
{
    UCHAR w[10];
    ULONG n = 0, i;
    for (i = 0; i < 4; i++) w[n++] = (UCHAR)(reg >> (24 - 8 * i));
    if (c->Spi) { w[n++] = 0; w[n++] = 0; }
    for (i = 0; i < 4; i++) w[n++] = (UCHAR)(val >> (24 - 8 * i));
    if (c->Spi) SpiXfer(c->Spi, w, n, NULL, 0);
    else I2cXfer(c->Bus, c->Addr, w, n, NULL, 0);
}

/* cs35l56_issue_wake_event: two dummy transactions wake the control port, then tIRS */
static void L56Wake(SST_CODEC *c)
{
    ULONG v;
    CRd(c, L56_IRQ1_STATUS, &v);
    CDelay(1);
    CRd(c, L56_IRQ1_STATUS, &v);
    CDelay(3);
}

/* cs35l56_wait_for_firmware_boot: HALO_STATE >= BOOT_DONE (and not 0xFFFF), 250 ms */
static BOOLEAN L56WaitBoot(SST_CODEC *c)
{
    ULONG i, v = 0;
    for (i = 0; i < 250; i++) {
        if (CRd(c, L56Halo(c), &v) && v < 0xFFFF && v >= HALO_BOOT_DONE) return TRUE;
        CDelay(1);
    }
    LOGE("CS35L56 %s: firmware boot timed out, HALO_STATE %x", c->Path, v);
    return FALSE;
}

/* cs35l56_mbox_send: the command, then MBOX_1 reads 0 when the firmware took it (5 ms) */
static BOOLEAN L56Mbox(SST_CODEC *c, ULONG cmd)
{
    ULONG i, v = 1;
    if (!CWr(c, L56_DSP_VIRT1_MBOX_1, cmd)) return FALSE;
    for (i = 0; i < 7; i++) {
        if (CRd(c, L56_DSP_VIRT1_MBOX_1, &v) && v == 0) return TRUE;
        CDelay(1);
    }
    LOGE("CS35L56 %s: mailbox command %08x not taken (%08x)", c->Path, cmd, v);
    return FALSE;
}

static BOOLEAN L56PollPs(SST_CODEC *c, ULONG ps, BOOLEAN atLeast, ULONG ms)
{
    ULONG i, v = 0xFFFFFFFF;
    for (i = 0; i < ms; i++) {
        if (CRd(c, L56_TRANSDUCER_PS, &v) && (atLeast ? v >= ps : v == ps)) return TRUE;
        CDelay(1);
    }
    LOGI("CS35L56 %s: power state %u not reached (%x)", c->Path, ps, v);
    return FALSE;
}

static BOOLEAN L56IdOk(ULONG id)
{
    id &= 0x00FFFFFF;
    return id == 0x35A54 || id == 0x35A56 || id == 0x35A57;
}

static BOOLEAN L56Probe(SST_CODEC *c)
{
    ULONG id;
    if (!CRd(c, L56_DEVID, &id)) {
        L56Wake(c);                                             /* hibernating, or just out of reset */
        if (!CRd(c, L56_DEVID, &id)) return FALSE;
    }
    return L56IdOk(id);
}

/* cs35l56_hw_init: IRQ masks the driver relies on (re-applied after the reset: regcache_sync) */
static BOOLEAN L56Masks(SST_CODEC *c)
{
    return CWr(c, L56_IRQ1_MASK_20, 0xFFFFFFFF) && CUpd(c, L56_IRQ1_MASK_1, 0x80000000, 0) &&
           CUpd(c, L56_IRQ1_MASK_8, 0x80000000, 0);
}

static BOOLEAN L56Init(SST_CODEC *c)
{
    static const ULONG patch[] = {
        /* cs35l56_set_patch: SDCA defaults the firmware may change, and registers a soft reset keeps */
        L56_SWIRE_DP3_CH1, 0x18, L56_SWIRE_DP3_CH1 + 4, 0x19, L56_SWIRE_DP3_CH1 + 8, 0x29,
        L56_SWIRE_DP3_CH1 + 12, 0x28, L56_IRQ1_MASK_18, 0x1F7DF0FF,
        L56_MAIN_USER_MUTE, 0, L56_MAIN_USER_VOLUME, 0, L56_MAIN_POSTURE, 0,
    };
    static const ULONG dai[] = {
        /* cs35l56_hda_dai_config */
        L56_ASP1_CONTROL1, 0x00000021,          /* ASP1_BCLK_FREQ 3.072 MHz */
        L56_ASP1_CONTROL2, 0x20200200,          /* RX / TX 32 cycles a slot, I2S */
        L56_ASP1_CONTROL3, 0x00000003,          /* DOUT Hi-Z in unused slots */
        L56_ASP1_FRAME_CTRL1, 0x03020100,       /* TX1..4 slots 0..3 */
        L56_ASP1_FRAME_CTRL5, 0x00020100,       /* RX1..3 slots 0..2 */
        L56_ASP1_DATA_CTRL5, 0x00000018,        /* RX word length 24 */
        L56_ASP1_DATA_CTRL1, 0x00000018,        /* TX word length 24 */
        L56_ASP1_ENABLES1, 0x00000000,
        L56_ASP1TX1_INPUT, 0x18, L56_ASP1TX1_INPUT + 4, 0x19,     /* VMON, IMON */
        L56_ASP1TX1_INPUT + 8, 0x20, L56_ASP1TX1_INPUT + 12, 0x28, /* ERRVOL, VDDBMON */
    };
    ULONG rev = 0, id = 0, otp = 0, sec = 0, prot = 0, fw = 0, i;

    if (c->Rate != 48000 || c->Slot != 32) {
        LOGE("CS35L56 %s: only 48 kHz with 32-bit slots is set up (%u, %u)", c->Path, c->Rate, c->Slot);
        return FALSE;
    }
    CDelay(3);                                                  /* control port ready (tIRS) */
    if (!CRd(c, L56_REVID, &rev)) {
        L56Wake(c);
        if (!CRd(c, L56_REVID, &rev)) return FALSE;
    }
    c->Priv[P_REV] = rev & 0xFF;
    if (!L56WaitBoot(c)) return FALSE;
    if (!CRd(c, L56_DEVID, &id) || !L56IdOk(id)) {
        LOGE("CS35L56 %s: device id %x", c->Path, id);
        return FALSE;
    }
    c->Priv[P_TYPE] = id & 0xFF;
    CRd(c, L56_DSP_RESTRICT_STS1, &sec);
    CRd(c, L56_OTPID, &otp);
    if (!L56Masks(c)) return FALSE;

    /* cs35l56_system_reset (SPI: cs35l56_spi_system_reset, a PING delimits the reset), then boot */
    if (c->Spi) {
        L56WriteOnce(c, L56_DSP_VIRT1_MBOX_1, MBOX_SYSTEM_RESET);
        CDelay(5);
        L56WriteOnce(c, L56_DSP_MBOX_1_RAW, MBOX_PING);
        CDelay(5);
        for (i = 0; i < 250; i++) {
            ULONG v;
            if (CRd(c, L56_DSP_VIRT1_MBOX_2, &v) && v && v != 0xFFFFFFFF) break;
            CDelay(1);
        }
    } else {
        L56WriteOnce(c, L56Halo(c), 0);
        L56WriteOnce(c, L56_DSP_VIRT1_MBOX_1, MBOX_SYSTEM_RESET);
        CDelay(3);
    }
    if (!L56WaitBoot(c)) return FALSE;
    if (!CSeq(c, patch, ARRAYSIZE(patch) / 2) || !L56Masks(c)) return FALSE;
    /* auto-hibernate off: the driver decides when the amp sleeps */
    if (!L56Mbox(c, MBOX_PREVENT_HIBERNATE)) return FALSE;
    if (!CSeq(c, dai, ARRAYSIZE(dai) / 2)) return FALSE;
    CRd(c, L56_PROTECTION_STATUS, &prot);
    CRd(c, L56_DSP1_FW_VER, &fw);
    c->Priv[P_FWMISSING] = prot & 1;
    c->Caps = CC_PLAY;
    LOGI("CS35L56 %s: CS35L%02X%s rev %02x OTP %u, firmware %u.%u.%u %s, amp %u (TX%u)", c->Path, c->Priv[P_TYPE],
         (sec & 7) ? "s" : "", c->Priv[P_REV], otp, fw >> 16, (fw >> 8) & 0xFF, fw & 0xFF,
         (prot & 1) ? "(ROM, not patched: no tuning)" : "(patched by the BIOS)", c->Index, (c->Index & 3) + 1);
    return TRUE;
}

static void L56Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    if (rec) return;
    if (on) {
        /* cs35l56_hda_play: AUDIO_PLAY, wait for PS0, then the ASP inputs and this amp's TX slot */
        if (L56Mbox(c, MBOX_AUDIO_PLAY)) L56PollPs(c, PS0, FALSE, 50);
        CUpd(c, L56_ASP1_ENABLES1, 0x0003000F, 0x00030000 | (1UL << (c->Index & 3)));
    } else {
        /* cs35l56_hda_pause */
        L56Mbox(c, MBOX_AUDIO_PAUSE);
        CUpd(c, L56_ASP1_ENABLES1, 0x0003000F, 0);
    }
}

static void L56Shutdown(SST_CODEC *c)
{
    L56Stream(c, FALSE, FALSE);
    /* cs35l56_runtime_suspend_common: PS3, allow hibernation, hibernate now (the last access) */
    L56PollPs(c, PS3, TRUE, 300);
    L56Mbox(c, MBOX_ALLOW_HIBERNATE);
    L56WriteOnce(c, L56_DSP_VIRT1_MBOX_1, MBOX_HIBERNATE_NOW);
}

/* behind an HD Audio codec only (sideamp.c; not in g_CodecDrvs) */
const CODEC_DRV g_DrvCs35l56 = { "CS35L56", "CSC3554\0CSC3556\0CSC3557\0", CODEC_AMP, 4, 4,
                                 L56Probe, L56Init, L56Stream, NULL, L56Shutdown };
