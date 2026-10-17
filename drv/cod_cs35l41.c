/*
 * AUDIONT - Cirrus Logic CS35L41 boosted smart amplifier, I2S mode, no DSP firmware.
 *
 *   "CSC3541"  on a Smart Sound SSP port (sstcodec.c), ASoC style (sound/soc/codecs/cs35l41.c)
 *   "CSC3551", "CLSA0100", "CLSA0101"  behind an HD Audio codec (sideamp.c), as Linux
 *              sound/hda/codecs/side-codecs/cs35l41_hda.c without firmware
 *
 * I2C or SPI, 32-bit register address / 32-bit value, big endian (cs35l41_regmap_i2c; SPI:
 * cs35l41_regmap_spi, 16 pad bits, read flag 0x80 - set up by sideamp.c in SST_CODEC.SpiPad /
 * SpiRead).  Reference: cs35l41-lib.c (cs35l41_init_boost, cs35l41_boost_config,
 * cs35l41_global_enable, cs35l41_gpio_config, cs35l41_safe_reset) and cs35l41_hda.c.
 *
 * Signal path without the HALO DSP firmware: ASP RX1 -> DACPCM1 (DAC_PCM1_SRC = ASPRX1)
 * -> class-H -> amplifier; the DSP core stays halted (DSP1_CCM_CORE_CTRL = 0, written by
 * cs35l41_register_errata_patch).  ASP: I2S consumer (TDM with more than 2 slots: DSP_A,
 * the only other frame format the chip has - an SSP running DSP_B TDM is not supported),
 * RX slot width = c->Slot, word length = c->Valid.  Clock: the PLL locks to SCLK (BCLK)
 * when its frequency is in cs35l41_pll_sysclk[], else to MCLK (behind an HD Audio codec:
 * 48 kHz, 2 x 32-bit slots, 24 valid bits, SCLK 3.072 MHz = cs35l41_hda_config).
 *
 * Board data (SST_CODEC.Amp, filled by sideamp.c from the Linux cs35l41_hda_property.c table or
 * the _DSD "cirrus,..." properties; Amp.Valid FALSE on the SSP path):
 *   Amp.Boost  AMP_BST_INTERNAL: cs35l41_boost_config with Amp.IndNh / IpkMa / CapUf, BST_EN = 10;
 *              invalid values (Linux refuses to probe) fall back to AMP_BST_SAFE.
 *              AMP_BST_EXTERNAL: VBST from an external supply switched by the amp's GPIO1 (VSPK):
 *              cs35l41_init_boost EXT (GPIO1 low, reset_to_safe, BST_EN = 00 "DIS_FET_OFF"),
 *              GPIO1 high around playback, global enable through the safe_to_active /
 *              active_to_safe sequences, cs35l41_safe_reset at shutdown.
 *              AMP_BST_EXT_NOSW: the same without the VSPK switch (CLSA0100; GPIO1 still driven low).
 *              AMP_BST_SAFE (no board data): handled as external boost but GPIO1 never touched -
 *              the boost converter and the bypass FET stay off, which cannot back-feed an external
 *              VBST rail; on an internal-boost board VBST then only follows VP through the FET's
 *              body diode (reduced output swing, or silence).
 *   Amp.Pos    ASP RX1 slot: left 0, right 1, centre 0 (no L+R mix without the DSP).
 *   Amp.Gpio1 / Gpio2   cirrus,gpioN-func -> GPIO_PAD_CONTROL (cs35l41_hda_apply_properties /
 *              cs35l41_gpio_config): VSPK switch -> GPIO1 output, sync -> MDSYNC, interrupt ->
 *              GPIO2 open-drain IRQ, unused -> Hi-Z.
 * Without board data (SSP path) the defaults below: CS41_BST_IND_NH / CS41_BST_CAP_UF /
 * CS41_BST_IPK_MA (0: unknown) select internal boost, else boost off with the bypass FET on
 * (BST_EN = 01: VBST follows VP) - the long-standing CSC3541 behaviour; the channel follows
 * c->Index (sof_cirrus_common.c cs35l41_channel_map: WL / TL slot 0, WR / TR slot 1).
 *
 * Gain: the firmware-less values of cs35l41_hda.c: AMP_GAIN_PCM 4.5 dB, digital volume 0 dB
 * while playing; AMP_GAIN 0.5 dB / digital mute when stopped (cs35l41_hda_mute).
 *
 * GPIO: c->Gpio[0] is the active-low RESET ("reset-gpios"; sideamp.c puts the resolved line
 * there); it may be shared by all amps, so the chip driver only releases it (drives it high),
 * never pulses it - the software reset (SFT_RESET) is used instead, as cs35l41_hda.c does after
 * its single reset pulse (sideamp.c pulses each distinct reset line once before the probe).
 * The IRQ (GpioInt) is not used: latched protection errors are released at the next stream start.
 */
#include "codec.h"

/* board boost parameters (0: unknown -> bypass mode) */
#ifndef CS41_BST_IND_NH
#define CS41_BST_IND_NH      0
#endif
#ifndef CS41_BST_CAP_UF
#define CS41_BST_CAP_UF      0
#endif
#ifndef CS41_BST_IPK_MA
#define CS41_BST_IPK_MA      0
#endif

#define L41_DEVID            0x00000000
#define L41_REVID            0x00000004     /* AREVID 7:4, MTLREVID 3:0 */
#define L41_OTPID            0x00000010
#define L41_SFT_RESET        0x00000020
#define L41_TEST_KEY_CTL     0x00000040
#define L41_OTP_MEM0         0x00000400
#define L41_PWR_CTRL1        0x00002014     /* GLOBAL_EN 0 */
#define L41_PWR_CTRL2        0x00002018     /* IMON 13, VMON 12, BST_EN 5:4, AMP_EN 0 */
#define L41_PWR_CTRL3        0x0000201C     /* DRE 20, CLASS H 4 */
#define L41_GPIO_PAD_CONTROL 0x0000242C     /* GPIO2_CTRL 26:24, GPIO1_CTRL 17:16 */
#define L41_PROTECT_REL      0x00002034     /* PROTECT_REL_ERR_IGN */
#define L41_PLL_CLK_CTRL     0x00002C04     /* OPENLOOP 11, REFCLK_FREQ 10:5, PLL_CLK_EN 4, SEL 2:0 */
#define L41_GLOBAL_CLK_CTRL  0x00002C0C     /* GLOBAL_FS 4:0 */
#define L41_TST_FS_MON0      0x00002D10
#define L41_BST_PEAK_CUR     0x00003808
#define L41_BST_COEFF        0x00003810
#define L41_BST_SLOPE_LBST   0x00003814
#define L41_BST_DCM_CTRL     0x0000381C
#define L41_VIMON_RESYNC     0x00004100
#define L41_VPVBST_FS_SEL    0x00004400
#define L41_SP_ENABLES       0x00004800     /* ASP_RX2_EN 17, ASP_RX1_EN 16, TX4..1 3:0 */
#define L41_SP_RATE_CTRL     0x00004804
#define L41_SP_FORMAT        0x00004808     /* RX/TX width 31:24/23:16, FMT 10:8, SCLK inv/mstr 6/4, LRCLK 2/0 */
#define L41_SP_HIZ_CTRL      0x0000480C
#define L41_SP_FRAME_RX_SLOT 0x00004820     /* RX2 slot 13:8, RX1 slot 5:0 */
#define L41_SP_RX_WL         0x00004840
#define L41_DAC_PCM1_SRC     0x00004C00
#define L41_ASP_TX3_SRC      0x00004C28
#define L41_ASP_TX4_SRC      0x00004C2C
#define L41_AMP_DIG_VOL_CTRL 0x00006000
#define L41_OTP_TRIM_30      0x00007418
#define L41_AMP_GAIN_CTRL    0x00006C04     /* AMP_GAIN_PCM 9:5 */
#define L41_IRQ1_STATUS1     0x00010010
#define L41_IRQ1_STATUS3     0x00010018
#define L41_IRQ1_STATUS4     0x0001001C
#define L41_IRQ1_MASK1       0x00010110
#define L41_IRQ1_DB3         0x00010318
#define L41_IRQ2_DB3         0x00010B18
#define L41_GPIO1_CTRL1      0x00011008     /* DIR 31 (1 input), LVL 15, POL 12 */
#define L41_GPIO2_CTRL1      0x0001100C
#define L41_DSP1_CCM_CORE    0x02BC1000
#define L41_DSP1_XM_ACCEL    0x02BC2020
#define L41_DSP1_YM_ACCEL    0x02BC20E0

#define L41_CHIP_ID          0x35A40
#define L41R_CHIP_ID         0x35B40
#define L41_OTP_BOOT_DONE    0x00000002     /* IRQ1_STATUS4 */
#define L41_OTP_BOOT_ERR     0x80000000     /* IRQ1_STATUS3 */
#define L41_PUP_DONE         0x01000000     /* IRQ1_STATUS1 */
#define L41_PDN_DONE         0x00800000
#define L41_INT1_MASK_DEFAULT 0x7FFCFE3F

/* Priv[] */
#define P_MODE               0              /* M_xxx: boost mode in use */
#define P_PUPLOG             1              /* PUP_DONE timeout already logged */

#define M_BYPASS             0              /* boost off, bypass FET on (BST_EN 01): SSP default */
#define M_INT                1              /* internal boost (BST_EN 10) */
#define M_EXT                2              /* external boost, VSPK switch on GPIO1 (BST_EN 00) */
#define M_EXT_NOSW           3              /* external boost, no VSPK switch */
#define M_SAFE               4              /* no board data: external handling, GPIO1 untouched */
#define IS_EXT(m)            ((m) >= M_EXT)

static const char *const g_L41Mode[] = { "bypass (VBST = VP)", "internal boost", "external boost + VSPK switch",
                                         "external boost", "safe (boost and bypass FET off)" };

static BOOLEAN L41Seq(SST_CODEC *c, const ULONG *rv, ULONG pairs)
{
    return CSeq(c, rv, pairs);
}

static BOOLEAN L41TestKey(SST_CODEC *c, BOOLEAN unlock)
{
    if (unlock)
        return CWr(c, L41_TEST_KEY_CTL, 0x55) && CWr(c, L41_TEST_KEY_CTL, 0xAA);
    return CWr(c, L41_TEST_KEY_CTL, 0xCC) && CWr(c, L41_TEST_KEY_CTL, 0x33);
}

/* regmap_read_poll_timeout(.., 1000 us, 100 ms) */
static BOOLEAN L41Poll(SST_CODEC *c, ULONG reg, ULONG mask)
{
    ULONG i, v;
    for (i = 0; i < 100; i++) {
        if (CRd(c, reg, &v) && (v & mask))
            return TRUE;
        CDelay(1);
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* OTP trim unpack (cs35l41_otp_unpack)                                 */
/* ------------------------------------------------------------------ */
typedef struct { ULONG Reg; UCHAR Shift, Size; } L41_OTP;

static const L41_OTP g_Otp1[] = {
    { 0x00002030, 0, 4 }, { 0x00002030, 7, 1 }, { 0x0000208C, 24, 6 }, { 0x00002090, 14, 4 },
    { 0x00002090, 10, 4 }, { 0x0000300C, 11, 4 }, { 0x0000394C, 23, 2 }, { 0x00003950, 0, 7 },
    { 0x00003950, 8, 7 }, { 0x00003950, 16, 8 }, { 0x00017040, 16, 8 }, { 0x00017040, 24, 8 },
    { 0x00017044, 0, 24 },
};

static const L41_OTP g_Otp2[] = {
    { 0x00002030, 0, 4 }, { 0x00002030, 7, 1 }, { 0x0000208C, 24, 6 }, { 0x00002090, 14, 4 },
    { 0x00002090, 10, 4 }, { 0x0000300C, 11, 4 }, { 0x0000394C, 23, 2 }, { 0x00003950, 0, 7 },
    { 0x00003950, 8, 7 }, { 0x00003950, 16, 8 }, { 0x00003950, 24, 8 }, { 0x00003954, 0, 7 },
    { 0x00003954, 8, 7 }, { 0x00003954, 16, 8 }, { 0x00003954, 24, 8 }, { 0x00003958, 0, 7 },
    { 0x00003958, 8, 7 }, { 0x00003958, 16, 8 }, { 0x00003958, 24, 8 }, { 0x0000395C, 0, 7 },
    { 0x0000395C, 8, 7 }, { 0x0000395C, 16, 8 }, { 0x0000395C, 24, 8 }, { 0x0000416C, 0, 8 },
    { 0x00004160, 0, 7 }, { 0x0000416C, 8, 8 }, { 0x00004160, 16, 10 }, { 0x0000416C, 16, 12 },
    { 0x0000416C, 28, 1 }, { 0x00004170, 0, 6 }, { 0x00004170, 6, 1 }, { 0x00004170, 8, 6 },
    { 0x00004170, 14, 1 }, { 0x00004170, 16, 9 }, { 0x00004360, 0, 5 }, { 0x00004360, 6, 9 },
    { 0x00004448, 0, 8 }, { 0x00004448, 8, 8 }, { 0x00004448, 16, 8 }, { 0x00004448, 24, 8 },
    { 0x0000444C, 0, 3 }, { 0x00006E30, 0, 5 }, { 0x00006E30, 8, 5 }, { 0x00006E30, 16, 5 },
    { 0x00006E30, 24, 5 }, { 0x00006E34, 0, 5 }, { 0x00006E34, 8, 5 }, { 0x00006E34, 16, 5 },
    { 0x00006E34, 24, 5 }, { 0x00006E38, 0, 5 }, { 0x00006E38, 8, 5 }, { 0x00006E38, 16, 5 },
    { 0x00006E38, 24, 5 }, { 0x00006E3C, 0, 5 }, { 0x00006E3C, 8, 5 }, { 0x00006E3C, 16, 5 },
    { 0x00006E3C, 24, 5 }, { 0x00006E40, 0, 5 }, { 0x00006E40, 8, 5 }, { 0x00006E40, 16, 5 },
    { 0x00006E40, 24, 5 }, { 0x00006E44, 0, 5 }, { 0x00006E48, 0, 10 }, { 0x00006E48, 10, 10 },
    { 0x00006E48, 20, 10 }, { 0x00006E4C, 0, 10 }, { 0x00006E4C, 10, 10 }, { 0x00006E4C, 20, 10 },
    { 0x00006E50, 0, 10 }, { 0x00006E50, 10, 10 }, { 0x00006E50, 20, 10 }, { 0x00006E54, 0, 10 },
    { 0x00006E54, 10, 10 }, { 0x00006E54, 20, 10 }, { 0x00006E58, 0, 10 }, { 0x00006E58, 10, 10 },
    { 0x00006E58, 20, 10 }, { 0x00006E5C, 0, 10 }, { 0x00006E5C, 10, 10 }, { 0x00006E5C, 20, 10 },
    { 0x00006E60, 0, 10 }, { 0x00006E60, 10, 10 }, { 0x00006E60, 20, 10 }, { 0x00006E64, 0, 10 },
    { 0x00007418, 7, 5 }, { 0x0000741C, 0, 5 }, { 0x0000741C, 11, 4 }, { 0x0000741C, 19, 4 },
    { 0x00007434, 17, 1 }, { 0x00007434, 18, 7 }, { 0x00007068, 0, 9 }, { 0x0000410C, 7, 1 },
    { 0x0000400C, 0, 7 }, { 0x00004000, 11, 1 }, { 0x00017040, 0, 8 }, { 0x00017040, 8, 8 },
    { 0x00017040, 16, 8 }, { 0x00017040, 24, 8 }, { 0x00017044, 0, 24 },
};

/* GENMASK(h, l) for 0 <= l <= h <= 31 */
static ULONG L41Mask(ULONG h, ULONG l)
{
    return (h >= 31 ? 0xFFFFFFFF : ((1u << (h + 1)) - 1)) & ~((1u << l) - 1);
}

/* must run with the test key unlocked; every map starts at word 2, bit 16 */
static BOOLEAN L41OtpUnpack(SST_CODEC *c, ULONG *otpId)
{
    ULONG mem[32], id, i, n, bo = 16, wo = 2, val;
    const L41_OTP *map;

    if (!CRd(c, L41_OTPID, &id))
        return FALSE;
    *otpId = id;
    switch (id) {
    case 0x01: case 0x08: map = g_Otp1; n = sizeof(g_Otp1) / sizeof(g_Otp1[0]); break;
    case 0x02: case 0x03: case 0x06: map = g_Otp2; n = sizeof(g_Otp2) / sizeof(g_Otp2[0]); break;
    default:
        LOGE("CS35L41 %s: no OTP map for OTP ID %u", c->Path, id);
        return FALSE;
    }
    for (i = 0; i < 32; i++)
        if (!CRd(c, L41_OTP_MEM0 + i * 4, &mem[i]))
            return FALSE;

    for (i = 0; i < n; i++) {
        ULONG sz = map[i].Size;
        if (wo >= 32)
            return FALSE;
        if (bo + sz - 1 >= 32) {
            val = (mem[wo] & L41Mask(31, bo)) >> bo;
            if (++wo >= 32)
                return FALSE;
            val |= (mem[wo] & L41Mask(bo + sz - 33, 0)) << (32 - bo);
            bo += sz - 32;
        } else if (sz) {
            val = (mem[wo] & L41Mask(bo + sz - 1, bo)) >> bo;
            bo += sz;
        } else {
            val = 0;
        }
        if (bo == 32) {
            bo = 0;
            wo++;
        }
        if (map[i].Reg && sz) {
            ULONG m = L41Mask(map[i].Shift + sz - 1, map[i].Shift);
            if (!CUpd(c, map[i].Reg, m, (val << map[i].Shift) & m))
                return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* errata patches (cs35l41_register_errata_patch, test key unlocked)    */
/* ------------------------------------------------------------------ */
static const ULONG g_ErrA0[] = {
    0x00003854, 0x05180240,
    L41_VIMON_RESYNC, 0x00000000,
    0x00004310, 0x00000000,
    L41_VPVBST_FS_SEL, 0x00000000,
    L41_OTP_TRIM_30, 0x9091A1C8,
    0x00003014, 0x0200EE0E,
    L41_BST_DCM_CTRL, 0x00000051,
    0x00000054, 0x00000004,
    L41_IRQ1_DB3, 0x00000000,
    L41_IRQ2_DB3, 0x00000000,
    L41_DSP1_YM_ACCEL, 0x00000000,
    L41_DSP1_XM_ACCEL, 0x00000000,
    L41_PWR_CTRL2, 0x00000000,
    L41_AMP_GAIN_CTRL, 0x00000000,
    L41_ASP_TX3_SRC, 0x00000000,
    L41_ASP_TX4_SRC, 0x00000000,
};

/* B0 and B2 use the same list */
static const ULONG g_ErrB[] = {
    L41_VIMON_RESYNC, 0x00000000,
    0x00004310, 0x00000000,
    L41_VPVBST_FS_SEL, 0x00000000,
    L41_BST_DCM_CTRL, 0x00000051,
    L41_DSP1_YM_ACCEL, 0x00000000,
    L41_DSP1_XM_ACCEL, 0x00000000,
    L41_PWR_CTRL2, 0x00000000,
    L41_AMP_GAIN_CTRL, 0x00000000,
    L41_ASP_TX3_SRC, 0x00000000,
    L41_ASP_TX4_SRC, 0x00000000,
};

/* ------------------------------------------------------------------ */
/* clocks                                                               */
/* ------------------------------------------------------------------ */
static const struct { ULONG Freq; UCHAR Cfg; } g_L41Clk[] = {
    { 32768, 0x00 }, { 8000, 0x01 }, { 11025, 0x02 }, { 12000, 0x03 }, { 16000, 0x04 }, { 22050, 0x05 },
    { 24000, 0x06 }, { 32000, 0x07 }, { 44100, 0x08 }, { 48000, 0x09 }, { 88200, 0x0A }, { 96000, 0x0B },
    { 128000, 0x0C }, { 176400, 0x0D }, { 192000, 0x0E }, { 256000, 0x0F }, { 352800, 0x10 },
    { 384000, 0x11 }, { 512000, 0x12 }, { 705600, 0x13 }, { 750000, 0x14 }, { 768000, 0x15 },
    { 1000000, 0x16 }, { 1024000, 0x17 }, { 1200000, 0x18 }, { 1411200, 0x19 }, { 1500000, 0x1A },
    { 1536000, 0x1B }, { 2000000, 0x1C }, { 2048000, 0x1D }, { 2400000, 0x1E }, { 2822400, 0x1F },
    { 3000000, 0x20 }, { 3072000, 0x21 }, { 3200000, 0x22 }, { 4000000, 0x23 }, { 4096000, 0x24 },
    { 4800000, 0x25 }, { 5644800, 0x26 }, { 6000000, 0x27 }, { 6144000, 0x28 }, { 6250000, 0x29 },
    { 6400000, 0x2A }, { 6500000, 0x2B }, { 6750000, 0x2C }, { 7526400, 0x2D }, { 8000000, 0x2E },
    { 8192000, 0x2F }, { 9600000, 0x30 }, { 11289600, 0x31 }, { 12000000, 0x32 }, { 12288000, 0x33 },
    { 12500000, 0x34 }, { 12800000, 0x35 }, { 13000000, 0x36 }, { 13500000, 0x37 }, { 19200000, 0x38 },
    { 22579200, 0x39 }, { 24000000, 0x3A }, { 24576000, 0x3B }, { 25000000, 0x3C }, { 25600000, 0x3D },
    { 26000000, 0x3E }, { 27000000, 0x3F },
};

/* cs35l41_fs_mon (SCLK <= 6.144 MHz); larger: fixed 0x10 / 0x24 */
static const struct { ULONG Freq; USHORT Fs1, Fs2; } g_L41FsMon[] = {
    { 32768, 2254, 3754 }, { 8000, 9220, 15364 }, { 11025, 6148, 10244 }, { 12000, 6148, 10244 },
    { 16000, 4612, 7684 }, { 22050, 3076, 5124 }, { 24000, 3076, 5124 }, { 32000, 2308, 3844 },
    { 44100, 1540, 2564 }, { 48000, 1540, 2564 }, { 88200, 772, 1284 }, { 96000, 772, 1284 },
    { 128000, 580, 964 }, { 176400, 388, 644 }, { 192000, 388, 644 }, { 256000, 292, 484 },
    { 352800, 196, 324 }, { 384000, 196, 324 }, { 512000, 148, 244 }, { 705600, 100, 164 },
    { 750000, 100, 164 }, { 768000, 100, 164 }, { 1000000, 76, 124 }, { 1024000, 76, 124 },
    { 1200000, 64, 104 }, { 1411200, 52, 84 }, { 1500000, 52, 84 }, { 1536000, 52, 84 },
    { 2000000, 40, 64 }, { 2048000, 40, 64 }, { 2400000, 34, 54 }, { 2822400, 28, 44 },
    { 3000000, 28, 44 }, { 3072000, 28, 44 }, { 3200000, 27, 42 }, { 4000000, 22, 34 },
    { 4096000, 22, 34 }, { 4800000, 19, 29 }, { 5644800, 16, 24 }, { 6000000, 16, 24 },
    { 6144000, 16, 24 }, { 12288000, 0, 0 },
};

static LONG L41ClkCfg(ULONG freq)
{
    ULONG i;
    for (i = 0; i < sizeof(g_L41Clk) / sizeof(g_L41Clk[0]); i++)
        if (g_L41Clk[i].Freq == freq)
            return g_L41Clk[i].Cfg;
    return -1;
}

/* GLOBAL_FS code (cs35l41_fs_rates) */
static LONG L41FsCfg(ULONG rate)
{
    static const struct { ULONG Rate; UCHAR Cfg; } t[] = {
        { 12000, 0x01 }, { 24000, 0x02 }, { 48000, 0x03 }, { 96000, 0x04 }, { 192000, 0x05 },
        { 11025, 0x09 }, { 22050, 0x0A }, { 44100, 0x0B }, { 88200, 0x0C }, { 176400, 0x0D },
        { 8000, 0x11 }, { 16000, 0x12 }, { 32000, 0x13 },
    };
    ULONG i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (t[i].Rate == rate)
            return t[i].Cfg;
    return -1;
}

/* ------------------------------------------------------------------ */
/* boost (cs35l41_init_boost, cs35l41_boost_config)                      */
/* ------------------------------------------------------------------ */
/* cs35l41-lib.c sequences for VBST supplied from outside (test key unlocked / locked inside) */
static const ULONG g_ResetToSafe[] = {
    L41_TEST_KEY_CTL, 0x00000055, L41_TEST_KEY_CTL, 0x000000AA,
    0x00007438, 0x00585941, 0x00007414, 0x08C82222, 0x0000742C, 0x00000009,
    L41_TEST_KEY_CTL, 0x000000CC, L41_TEST_KEY_CTL, 0x00000033,
};
static const ULONG g_SafeToActiveStart[] = {
    L41_TEST_KEY_CTL, 0x00000055, L41_TEST_KEY_CTL, 0x000000AA,
    0x0000742C, 0x0000000F, 0x0000742C, 0x00000079, 0x00007438, 0x00585941,
    L41_PWR_CTRL1, 0x00000001,                                  /* GLOBAL_EN */
};
static const ULONG g_SafeToActiveEnSpk[] = { 0x0000742C, 0x000000F9, 0x00007438, 0x00580941 };
static const ULONG g_ActiveToSafeStart[] = {
    L41_TEST_KEY_CTL, 0x00000055, L41_TEST_KEY_CTL, 0x000000AA,
    0x00007438, 0x00585941, L41_PWR_CTRL1, 0x00000000, 0x0000742C, 0x00000009,
};
static const ULONG g_ActiveToSafeEnd[] = {
    0x00007438, 0x00580941, L41_TEST_KEY_CTL, 0x000000CC, L41_TEST_KEY_CTL, 0x00000033,
};

/* cs35l41_boost_config: FALSE when the components are out of range (nothing written) */
static BOOLEAN L41BoostConfig(SST_CODEC *c, ULONG ind, ULONG cap, ULONG ipk, BOOLEAN *ok)
{
    static const UCHAR k1[4][5] = {
        { 0x24, 0x32, 0x32, 0x4F, 0x57 }, { 0x24, 0x32, 0x32, 0x4F, 0x57 },
        { 0x40, 0x32, 0x32, 0x4F, 0x57 }, { 0x40, 0x32, 0x32, 0x4F, 0x57 } };
    static const UCHAR k2[4][5] = {
        { 0x24, 0x49, 0x66, 0xA3, 0xEA }, { 0x24, 0x49, 0x66, 0xA3, 0xEA },
        { 0x48, 0x49, 0x66, 0xA3, 0xEA }, { 0x48, 0x49, 0x66, 0xA3, 0xEA } };
    static const UCHAR slope[4] = { 0x75, 0x6B, 0x3B, 0x28 };
    ULONG l, r;
    *ok = TRUE;
    switch (ind) {
    case 1000: l = 0; break;            /* 1.0 uH */
    case 1200: l = 1; break;
    case 1500: l = 2; break;
    case 2200: l = 3; break;
    default: return FALSE;
    }
    if (ipk < 1600 || ipk > 4500) return FALSE;
    r = cap <= 19 ? 0 : cap <= 50 ? 1 : cap <= 100 ? 2 : cap <= 200 ? 3 : 4;
    *ok = CUpd(c, L41_BST_COEFF, 0xFFFF, (ULONG)k1[l][r] | ((ULONG)k2[l][r] << 8)) &&
          CUpd(c, L41_BST_SLOPE_LBST, 0xFF03, ((ULONG)slope[l] << 8) | l) &&
          CUpd(c, L41_BST_PEAK_CUR, 0x7F, ((ipk - 1600) / 50) + 0x10) &&
          CUpd(c, L41_PWR_CTRL2, 0x30, 0x20);                    /* BST_EN_DEFAULT */
    return TRUE;
}

static ULONG L41BstEn(ULONG mode)
{
    return mode == M_INT ? 0x20 : mode == M_BYPASS ? 0x10 : 0x00;
}

static BOOLEAN L41Boost(SST_CODEC *c)
{
    ULONG mode, ind, cap, ipk;
    BOOLEAN ok = TRUE;
    if (c->Amp.Valid) {
        ind = c->Amp.IndNh; cap = c->Amp.CapUf; ipk = c->Amp.IpkMa;
        mode = c->Amp.Boost == AMP_BST_INTERNAL ? M_INT : c->Amp.Boost == AMP_BST_EXTERNAL ? M_EXT :
               c->Amp.Boost == AMP_BST_EXT_NOSW ? M_EXT_NOSW : M_SAFE;
    } else {
        ind = CS41_BST_IND_NH; cap = CS41_BST_CAP_UF; ipk = CS41_BST_IPK_MA;
        mode = M_INT;
    }
    if (mode == M_INT && !L41BoostConfig(c, ind, cap, ipk, &ok)) {
        if (c->Amp.Valid) {
            LOGE("CS35L41 %s: invalid boost components (%u nH, %u uF, %u mA): boost left off", c->Path, ind, cap, ipk);
            mode = M_SAFE;
        } else mode = M_BYPASS;                                  /* unknown components: VBST = VP */
    }
    c->Priv[P_MODE] = mode;
    if (!ok) return FALSE;
    if (mode == M_BYPASS) return CUpd(c, L41_PWR_CTRL2, 0x30, 0x10);
    if (mode == M_INT) return TRUE;
    /* external VBST (and the safe mode): GPIO1 low = VSPK off, reset_to_safe, BST_DIS_FET_OFF */
    if (mode != M_SAFE && !CWr(c, L41_GPIO1_CTRL1, 0x00000001)) return FALSE;
    return CSeq(c, g_ResetToSafe, ARRAYSIZE(g_ResetToSafe) / 2) && CUpd(c, L41_PWR_CTRL2, 0x30, 0x00);
}

/* cs35l41_hda_apply_properties + cs35l41_gpio_config (pol_inv never set) */
static BOOLEAN L41GpioConfig(SST_CODEC *c)
{
    BOOLEAN ok = TRUE;
    ULONG f;
    if (!c->Amp.Valid || c->Priv[P_MODE] == M_SAFE) return TRUE;
    /* VSPK switch: GPIO1 output (CS35L41_GPIO1_GPIO); sync: MDSYNC; unused: Hi-Z.  The direction is
       written whether or not the function is given (out_en only for the VSPK switch) */
    f = c->Amp.Gpio1 == AMP_GPIO_VSPK ? 1 : c->Amp.Gpio1 == AMP_GPIO_SYNC ? 2 : 0;
    ok &= CUpd(c, L41_GPIO1_CTRL1, 0x80001000, f == 1 ? 0 : 0x80000000);
    ok &= CUpd(c, L41_GPIO2_CTRL1, 0x80001000, 0x80000000);
    if (c->Amp.Gpio1 != AMP_GPIO_NONE) ok &= CUpd(c, L41_GPIO_PAD_CONTROL, 0x00030000, f << 16);
    if (c->Amp.Gpio2 != AMP_GPIO_NONE) {
        f = c->Amp.Gpio2 == AMP_GPIO_IRQ ? 2 : 0;               /* CS35L41_GPIO2_INT_OPEN_DRAIN */
        ok &= CUpd(c, L41_GPIO_PAD_CONTROL, 0x07000000, f << 24);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* driver                                                               */
/* ------------------------------------------------------------------ */
static BOOLEAN L41IdOk(SST_CODEC *c, ULONG *devid, ULONG *revid)
{
    ULONG id, rev;
    if (!CRd(c, L41_DEVID, &id) || !CRd(c, L41_REVID, &rev))
        return FALSE;
    if (devid)
        *devid = id;
    if (revid)
        *revid = rev;
    /* CS35L41 has an even MTLREVID, CS35L41R an odd one */
    return id == ((rev & 0x0F) % 2 ? L41R_CHIP_ID : L41_CHIP_ID);
}

static BOOLEAN L41Probe(SST_CODEC *c)
{
    if (L41IdOk(c, NULL, NULL))
        return TRUE;
    /* SSP candidates: only a chip that may still be in reset gets another chance; behind an HD Audio
       codec (sideamp.c just pulsed the reset line) the OTP boot may still be running */
    if (!c->NGpio && !c->Amp.Valid)
        return FALSE;
    /* held in reset: release RESET (GpioIo 0), 2 ms, then OTP boot */
    if (c->NGpio) {
        CGpio(c, 0, TRUE);
        CDelay(3);
    }
    L41Poll(c, L41_IRQ1_STATUS4, L41_OTP_BOOT_DONE);
    return L41IdOk(c, NULL, NULL);
}

static BOOLEAN L41Init(SST_CODEC *c)
{
    ULONG devid = 0, revid = 0, otpid = 0, st, slot, sel, ref, fsmon, i;
    LONG refcfg, fs;

    c->Priv[P_PUPLOG] = 0;
    fs = L41FsCfg(c->Rate);
    if (fs < 0 || c->Slot < 16 || c->Slot > 32) {
        LOGE("CS35L41 %s: unsupported rate %u / %u-bit slots", c->Path, c->Rate, c->Slot);
        return FALSE;
    }

    if (c->NGpio)
        CGpio(c, 0, TRUE);                                     /* RESET released (shared: no pulse) */
    CWr(c, L41_SFT_RESET, 0x5A000000);                         /* CS35L41_SOFTWARE_RESET */
    CDelay(3);
    if (!L41Poll(c, L41_IRQ1_STATUS4, L41_OTP_BOOT_DONE)) {
        LOGE("CS35L41 %s: OTP_BOOT_DONE timeout", c->Path);
        return FALSE;
    }
    if (!CRd(c, L41_IRQ1_STATUS3, &st) || (st & L41_OTP_BOOT_ERR)) {
        LOGE("CS35L41 %s: OTP boot error", c->Path);
        return FALSE;
    }
    if (!L41IdOk(c, &devid, &revid)) {
        LOGE("CS35L41 %s: device id %x / rev %x", c->Path, devid, revid);
        return FALSE;
    }

    /* cs35l41_probe: errata, OTP trims (test key unlocked) */
    if (!L41TestKey(c, TRUE))
        return FALSE;
    switch (revid) {
    case 0xA0:
        L41Seq(c, g_ErrA0, sizeof(g_ErrA0) / sizeof(g_ErrA0[0]) / 2);
        break;
    case 0xB0:
    case 0xB2:
        L41Seq(c, g_ErrB, sizeof(g_ErrB) / sizeof(g_ErrB[0]) / 2);
        break;
    default:
        LOGE("CS35L41 %s: no errata patch for revision %02x", c->Path, revid);
        break;
    }
    CWr(c, L41_DSP1_CCM_CORE, 0);                              /* DSP core halted */
    if (!L41OtpUnpack(c, &otpid)) {
        L41TestKey(c, FALSE);
        LOGE("CS35L41 %s: OTP unpack failed", c->Path);
        return FALSE;
    }
    if (!L41TestKey(c, FALSE))
        return FALSE;

    if (!CWr(c, L41_IRQ1_MASK1, L41_INT1_MASK_DEFAULT) || !L41Boost(c) || !L41GpioConfig(c))
        return FALSE;

    /* clocks: PLL reference = SCLK when listed, else MCLK (cs35l41_component_set_sysclk) */
    ref = c->Bclk;
    sel = 0;                                                   /* PLLSRC_SCLK */
    refcfg = L41ClkCfg(ref);
    if (refcfg < 0 && c->Mclk) {
        ref = c->Mclk;
        sel = 5;                                               /* PLLSRC_MCLK */
        refcfg = L41ClkCfg(ref);
    }
    if (refcfg < 0) {
        LOGE("CS35L41 %s: no PLL reference (BCLK %u, MCLK %u)", c->Path, c->Bclk, c->Mclk);
        return FALSE;
    }
    if (!CUpd(c, L41_GLOBAL_CLK_CTRL, 0x1F, (ULONG)fs) ||
        !CUpd(c, L41_PLL_CLK_CTRL, 0x0800, 0x0800) ||                    /* open loop */
        !CUpd(c, L41_PLL_CLK_CTRL, 0x07E0, (ULONG)refcfg << 5) ||
        !CUpd(c, L41_PLL_CLK_CTRL, 0x0010, 0x0000) ||
        !CUpd(c, L41_PLL_CLK_CTRL, 0x0007, sel) ||
        !CUpd(c, L41_PLL_CLK_CTRL, 0x0800, 0x0000) ||
        !CUpd(c, L41_PLL_CLK_CTRL, 0x0010, 0x0010))                      /* PLL_CLK_EN */
        return FALSE;
    /* cs35l41_dai_set_sysclk: FS monitor windows for the SCLK frequency */
    fsmon = 0;
    if (c->Bclk > 6144000) {
        fsmon = 0x10 | (0x24 << 12);
    } else {
        for (i = 0; i < sizeof(g_L41FsMon) / sizeof(g_L41FsMon[0]); i++)
            if (g_L41FsMon[i].Freq == c->Bclk)
                fsmon = g_L41FsMon[i].Fs1 | (((ULONG)g_L41FsMon[i].Fs2 << 12) & 0x00FFF800);
    }
    if (fsmon)
        CWr(c, L41_TST_FS_MON0, fsmon);
    else
        LOGE("CS35L41 %s: no FS monitor setting for BCLK %u", c->Path, c->Bclk);
    if (L41ClkCfg(c->Bclk) >= 0)
        CUpd(c, L41_SP_RATE_CTRL, 0x3F, (ULONG)L41ClkCfg(c->Bclk));   /* ASP_BCLK_FREQ (as cs35l41_hda) */

    /* ASP: consumer, I2S (FMT 2) or DSP_A (FMT 0), normal polarity, RX slot width / word length */
    if (c->Amp.Valid) slot = c->Amp.Pos == AMP_POS_RIGHT ? 1 : 0;      /* cs35l41_hda_channel_map */
    else slot = c->Count <= 1 ? 0 : (c->Index & 1);
    if (!CUpd(c, L41_SP_FORMAT, 0xFF000755, ((ULONG)c->Slot << 24) | ((c->Slots > 2 ? 0u : 2u) << 8)) ||
        !CUpd(c, L41_SP_RX_WL, 0x3F, c->Valid) ||
        !CUpd(c, L41_SP_FRAME_RX_SLOT, 0x3F3F, slot | ((slot ^ 1) << 8)) ||   /* RX1 = slot, RX2 = other */
        !CWr(c, L41_SP_HIZ_CTRL, 0x00000002) ||                             /* DOUT Hi-Z in unused slots */
        !CWr(c, L41_SP_ENABLES, 0x00000000) ||                              /* no TX, RX off until play */
        !CWr(c, L41_DAC_PCM1_SRC, 0x00000008) ||                            /* DACPCM1 = ASPRX1 */
        !CWr(c, L41_ASP_TX3_SRC, 0) || !CWr(c, L41_ASP_TX4_SRC, 0) ||
        /* muted, amplifier off */
        !CWr(c, L41_AMP_GAIN_CTRL, 0x00000000) ||
        !CWr(c, L41_AMP_DIG_VOL_CTRL, 0x0000A678) ||
        !CUpd(c, L41_PWR_CTRL2, 0x3331, L41BstEn(c->Priv[P_MODE])) ||       /* AMP + monitors off, BST_EN */
        !CUpd(c, L41_PWR_CTRL1, 0x01, 0x00))
        return FALSE;

    c->Caps = CC_PLAY;
    LOGI("CS35L41 %s: id %x rev %02x OTP %u, slot %u, %u x %u-bit %s, PLL ref %s %u, %s", c->Path, devid,
         revid, otpid, slot, c->Slots, c->Slot, c->Slots > 2 ? "DSP_A" : "I2S", sel ? "MCLK" : "SCLK", ref,
         g_L41Mode[c->Priv[P_MODE]]);
    return TRUE;
}

/* cs35l41_irq: release latched protection errors (amp short, temperature, boost) */
static void L41ReleaseErrors(SST_CODEC *c)
{
    static const struct { ULONG Irq, Rel; BOOLEAN Bst; } e[] = {
        { 0x80000000, 0x02, FALSE },        /* AMP_SHORT_ERR */
        { 0x00008000, 0x20, FALSE },        /* TEMP_WARN */
        { 0x00020000, 0x40, FALSE },        /* TEMP_ERR */
        { 0x00000040, 0x08, TRUE },         /* BST_OVP_ERR */
        { 0x00000080, 0x10, TRUE },         /* BST_DCM_UVP_ERR */
        { 0x00000100, 0x04, TRUE },         /* BST_SHORT_ERR */
    };
    ULONG st, i;
    if (!CRd(c, L41_IRQ1_STATUS1, &st))
        return;
    for (i = 0; i < sizeof(e) / sizeof(e[0]); i++) {
        if (!(st & e[i].Irq))
            continue;
        LOGE("CS35L41 %s: protection error %08x released", c->Path, e[i].Irq);
        if (e[i].Bst && c->Priv[P_MODE] == M_INT)
            CUpd(c, L41_PWR_CTRL2, 0x30, 0x00);
        CWr(c, L41_IRQ1_STATUS1, e[i].Irq);
        CWr(c, L41_PROTECT_REL, 0);
        CUpd(c, L41_PROTECT_REL, e[i].Rel, e[i].Rel);
        CUpd(c, L41_PROTECT_REL, e[i].Rel, 0);
        if (e[i].Bst && c->Priv[P_MODE] == M_INT)
            CUpd(c, L41_PWR_CTRL2, 0x30, 0x20);
    }
}

static void L41Patch(SST_CODEC *c, ULONG v2084)
{
    L41TestKey(c, TRUE);
    CWr(c, 0x00002084, v2084);
    L41TestKey(c, FALSE);
}

/* cs35l41_global_enable: GLOBAL_EN with PUP_DONE / PDN_DONE (internal boost and bypass), or the
   safe_to_active / active_to_safe sequences when VBST comes from outside */
static BOOLEAN L41GlobalEnable(SST_CODEC *c, BOOLEAN on)
{
    ULONG v, mask = on ? L41_PUP_DONE : L41_PDN_DONE, mode = c->Priv[P_MODE];
    if (!CRd(c, L41_PWR_CTRL1, &v) || (v & 1) == (ULONG)(on ? 1 : 0))
        return TRUE;
    if (!IS_EXT(mode)) {
        CUpd(c, L41_PWR_CTRL1, 0x01, on ? 0x01 : 0x00);
        if (!L41Poll(c, L41_IRQ1_STATUS1, mask) && (!on || !c->Priv[P_PUPLOG])) {
            /* PUP_DONE needs the PLL, which locks once the I2S clocks run: like Linux, go on after 100 ms */
            if (on) c->Priv[P_PUPLOG] = 1;
            LOGI("CS35L41 %s: %s timeout", c->Path, on ? "PUP_DONE" : "PDN_DONE");
        }
        CWr(c, L41_IRQ1_STATUS1, mask);
        return TRUE;
    }
    if (on) {
        if (!CSeq(c, g_SafeToActiveStart, ARRAYSIZE(g_SafeToActiveStart) / 2)) { L41TestKey(c, FALSE); return FALSE; }
        if (!L41Poll(c, L41_IRQ1_STATUS1, L41_PUP_DONE)) {
            LOGE("CS35L41 %s: PUP_DONE timeout (external boost): speaker output stays off", c->Path);
            L41TestKey(c, FALSE);
            return FALSE;
        }
        CWr(c, L41_IRQ1_STATUS1, L41_PUP_DONE);
        CSeq(c, g_SafeToActiveEnSpk, ARRAYSIZE(g_SafeToActiveEnSpk) / 2);
        return L41TestKey(c, FALSE);
    }
    if (!CSeq(c, g_ActiveToSafeStart, ARRAYSIZE(g_ActiveToSafeStart) / 2)) { L41TestKey(c, FALSE); return FALSE; }
    if (!L41Poll(c, L41_IRQ1_STATUS1, L41_PDN_DONE)) {
        LOGE("CS35L41 %s: PDN_DONE timeout (external boost)", c->Path);
        L41TestKey(c, FALSE);
        return FALSE;
    }
    CWr(c, L41_IRQ1_STATUS1, L41_PDN_DONE);
    return CSeq(c, g_ActiveToSafeEnd, ARRAYSIZE(g_ActiveToSafeEnd) / 2);
}

static void L41Mute(SST_CODEC *c, BOOLEAN mute)
{
    /* cs35l41_hda_mute / _unmute: AMP_GAIN_PCM 0.5 dB + digital mute, or 4.5 dB + 0 dB with HPF */
    if (mute) {
        CWr(c, L41_AMP_GAIN_CTRL, 0x00000000);
        CWr(c, L41_AMP_DIG_VOL_CTRL, 0x0000A678);
    } else {
        CWr(c, L41_AMP_DIG_VOL_CTRL, 0x00008000);
        CWr(c, L41_AMP_GAIN_CTRL, 0x00000084);
    }
}

static void L41Off(SST_CODEC *c)
{
    L41Mute(c, TRUE);
    if (c->Amp.Valid) {
        /* cs35l41_hda_pause_start / _pause_done */
        L41GlobalEnable(c, FALSE);
        CUpd(c, L41_PWR_CTRL2, 0x01, 0x00);                      /* AMP_EN */
        if (c->Priv[P_MODE] == M_EXT)
            CWr(c, L41_GPIO1_CTRL1, 0x00000001);                  /* VSPK off */
    } else {
        ULONG v;
        /* "Main AMP" off, then POST_PMD: global disable, wait PDN_DONE, pdn patch */
        CUpd(c, L41_PWR_CTRL2, 0x01, 0x00);
        if (CRd(c, L41_PWR_CTRL1, &v) && (v & 1)) {
            L41GlobalEnable(c, FALSE);
            L41Patch(c, 0x002F1AA3);
        }
        CUpd(c, L41_PWR_CTRL3, 0x10, 0x00);                     /* CLASS H */
    }
    CUpd(c, L41_SP_ENABLES, 0x00030000, 0);                      /* ASPRX1/2 */
}

static void L41Stream(SST_CODEC *c, BOOLEAN rec, BOOLEAN on)
{
    ULONG v;
    if (rec)
        return;
    if (!on) {
        L41Off(c);
        return;
    }
    L41ReleaseErrors(c);
    /* cs35l41_update_mixer / DAPM: ASPRX1/2 (AIF_IN), PCM Source = ASP, CLASS H */
    CUpd(c, L41_SP_ENABLES, 0x00030000, 0x00030000);
    CUpd(c, L41_PWR_CTRL3, 0x10, 0x10);
    if (c->Amp.Valid) {
        /* cs35l41_hda_play_start: AMP_EN, VSPK on; cs35l41_hda_play_done: global enable, unmute */
        CUpd(c, L41_PWR_CTRL2, 0x01, 0x01);
        if (c->Priv[P_MODE] == M_EXT)
            CWr(c, L41_GPIO1_CTRL1, 0x00008001);
        L41GlobalEnable(c, TRUE);
    } else {
        /* "Main AMP" PRE_PMU (pup patch, global enable), then AMP_EN */
        if (CRd(c, L41_PWR_CTRL1, &v) && !(v & 1)) {
            L41Patch(c, 0x002F1AA0);                             /* cs35l41_pup_patch */
            L41GlobalEnable(c, TRUE);
        }
        CUpd(c, L41_PWR_CTRL2, 0x01, 0x01);                      /* AMP_EN */
    }
    L41Mute(c, FALSE);
}

static void L41Shutdown(SST_CODEC *c)
{
    L41Off(c);
    /* cs35l41_safe_reset: with an external VSPK switch, VSPK off and the safe_to_reset sequence */
    if (c->Priv[P_MODE] == M_EXT) {
        static const ULONG a[] = { L41_TEST_KEY_CTL, 0x00000055, L41_TEST_KEY_CTL, 0x000000AA, 0x0000393C, 0x000000C0 };
        static const ULONG b[] = { 0x0000393C, 0x00000000, 0x00007414, 0x00C82222, 0x0000742C, 0x00000000,
                                   L41_TEST_KEY_CTL, 0x000000CC, L41_TEST_KEY_CTL, 0x00000033 };
        CWr(c, L41_GPIO1_CTRL1, 0x00000001);
        CSeq(c, a, ARRAYSIZE(a) / 2);
        CDelay(6);
        CSeq(c, b, ARRAYSIZE(b) / 2);
    }
}

const CODEC_DRV g_DrvCs35l41 = { "CS35L41", "CSC3541\0", CODEC_AMP, 4, 4,
                                 L41Probe, L41Init, L41Stream, NULL, L41Shutdown };

/* the same chip behind an HD Audio codec (sideamp.c; not in g_CodecDrvs) */
const CODEC_DRV g_DrvCs35l41Hda = { "CS35L41", "CSC3551\0CLSA0100\0CLSA0101\0", CODEC_AMP, 4, 4,
                                    L41Probe, L41Init, L41Stream, NULL, L41Shutdown };
