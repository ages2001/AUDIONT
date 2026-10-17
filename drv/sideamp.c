/*
 * AUDIONT - speaker amplifiers behind an HD Audio codec ("side codecs", Linux sound/hda/codecs/side-codecs).
 *
 * On many laptops the Realtek codec (ALC245 / ALC285 / ALC287 / ALC294 ...) feeds the speaker
 * amplifiers over I2S and the amplifiers are controlled by the host over I2C or SPI; without that
 * setup they stay silent.  One ACPI device describes all amplifiers, one I2cSerialBus or
 * SpiSerialBus resource each:
 *   CSC3551, CLSA0100, CLSA0101   Cirrus CS35L41 (cod_cs35l41.c, Linux cs35l41_hda.c)
 *   CSC3554, CSC3556, CSC3557     Cirrus CS35L54 / 56 / 57 (cod_cs35l56.c, ROM firmware)
 *   TIAS2781, TXNW2781, TXNW2770  TI TAS2781 / TAS2770: not driven - the HDA driver needs the
 *                                 per-laptop RCA and DSP files of linux-firmware ti/tas2781/
 *                                 (register sequences, TDM setup and power-up blocks come only
 *                                 from them)
 *
 * The codec is the I2S clock provider: 48 kHz, 2 slots of 32 bits, 24 valid bits, BCLK 3.072 MHz,
 * no MCLK (cs35l41_hda_config / cs35l56_hda_dai_config).
 *
 * CS35L41 board data, as Linux cs35l41_hda_read_acpi / cs35l41_add_dsd_properties:
 *  - the SSID is the device's _SUB ("17AA3847") or, without one, the codec subsystem id;
 *  - CLSA0100 / CLSA0101 (Lenovo Legion, no _DSD): lenovo_legion_no_acpi;
 *  - SSID in the cs35l41_config_table (ported completely below, g_L41Props): generic_dsd_config;
 *    the four HP 103C8C66/67/68/6A without _DSD: hp_i2c_int_2amp_dual_spkid;
 *  - else the _DSD properties (cs35l41_hda_parse_acpi): cirrus,dev-index, cirrus,speaker-position,
 *    cirrus,gpio1-func, cirrus,gpio2-func, cirrus,boost-peak-milliamp, cirrus,boost-ind-nanohenry,
 *    cirrus,boost-cap-microfarad (internal boost when any is given, else external), reset-gpios,
 *    spk-id-gpios, cs-gpios (10431A63: speaker id on _CRS GPIO 2, missing_speaker_id_gpio2);
 *  - none of these: no boost configuration is guessed - AMP_BST_SAFE (boost converter and bypass
 *    FET off, see cod_cs35l41.c), channels left / right by address or chip select order, the first
 *    GpioIo only released (the CSC3541 behaviour).
 * Registry: SideAmps 0 disables all of this; SideAmpBoost 1 internal (with SideAmpBstInd nH,
 * SideAmpBstIpk mA, SideAmpBstCap uF), 2 external with VSPK switch, 3 external, 4 safe overrides
 * the boost type of every CS35L41.
 *
 * Index of an amplifier (its channel and per-amp _DSD values): position of its I2C address or SPI
 * chip select in cirrus,dev-index; table boards: chip select (SPI), address - 0x40 (more than 2
 * amps) or 0x40 -> 0, other -> 1.  Reset: each distinct reset line is pulsed once before the
 * probe (low 2 ms, high, 2 ms) - a shared line is not pulsed per amp, as Linux ("Reset line busy,
 * assuming shared reset") - and driven low again at shutdown (cs35l41_hda_remove after
 * cs35l41_safe_reset, cs35l56_hda_remove), except on boards without VSPK switch.
 *
 * SPI: the SpiSerialBus resources name the LPSS SPI controller (spi.c); chip select by the
 * controller or by the cs-gpios _DSD property of the amplifier or controller node (an integer
 * Zero entry: native chip select), or, for the 2-amp table boards with cs_gpio_index, a _CRS GPIO
 * for the amp on chip select 1 (generic_dsd_config: the first amp keeps the native one, the GPIO
 * is held high while it talks).  CS35L4x on SPI: 32-bit address, 16 pad bits, 32-bit value, read
 * flag 0x80 (cs35l41_regmap_spi / cs35l56_regmap_spi).
 *
 * Power: SideAmpPower is called from HdaStartPlay / HdaStopPlay at DISPATCH_LEVEL; a worker thread
 * runs the chips' Stream callbacks (they poll for tens of milliseconds).  Amplifiers start in
 * index order and stop in reverse order.
 */
#include "codec.h"

extern const CODEC_DRV g_DrvCs35l41Hda, g_DrvCs35l56;

#define SA_MAX   4
#define K_L41    1
#define K_L56    2
#define K_TAS    3

/* Linux cs35l41_hda_property.c cs35l41_config_table: SSID, amps, boost, speaker position per amp
   index, _CRS GPIO index of reset / speaker id / SPI chip select (-1 none), boost inductor nH,
   peak current mA, capacitor uF */
#define B_INT 0
#define B_EXT 1
typedef struct {
    ULONG  Ssid;
    UCHAR  NAmps, Ext;
    char   Ch[5];
    signed char Reset, SpkId, Cs;
    USHORT Ind, Ipk;
    UCHAR  Cap;
} L41_PROP;

static const L41_PROP g_L41Props[] = {
    { 0x10251826, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x1025182C, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x10251844, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x10280B27, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10280B28, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10280BEB, 2, B_EXT, "LR--",   1,  -1,   0,    0,    0,  0 },
    { 0x10280C4D, 4, B_INT, "LRLR",   0,   1,  -1, 1000, 4500, 24 },
    { 0x103C89C6, 2, B_INT, "RL--",  -1,  -1,  -1, 1000, 4500, 24 },
    { 0x103C8A28, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A29, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A2A, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A2B, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A2C, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A2D, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A2E, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A30, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A31, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8A6E, 4, B_EXT, "LLRR",   0,  -1,  -1,    0,    0,  0 },
    { 0x103C8B63, 4, B_EXT, "RLRL",  -1,  -1,  -1,    0,    0,  0 },
    { 0x103C8BB3, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BB4, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BDD, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BDE, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BDF, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE0, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE1, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE2, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE3, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE5, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE6, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE7, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE8, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8BE9, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8B3A, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8C15, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4000, 24 },
    { 0x103C8C16, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4000, 24 },
    { 0x103C8C17, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4000, 24 },
    { 0x103C8C4D, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8C4E, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8C4F, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8C50, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8C51, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8CDD, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4100, 24 },
    { 0x103C8CDE, 2, B_INT, "LR--",   0,   1,  -1, 1000, 3900, 24 },
    { 0x104312AF, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10431433, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431463, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431473, 2, B_INT, "LR--",   1,  -1,   0, 1000, 4500, 24 },
    { 0x10431483, 2, B_INT, "LR--",   1,  -1,   0, 1000, 4500, 24 },
    { 0x10431493, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x104314D3, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x104314E3, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431503, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431533, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431573, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10431663, 2, B_INT, "LR--",   1,  -1,   0, 1000, 4500, 24 },
    { 0x10431683, 2, B_EXT, "LR--",   0,   1,  -1,    0,    0,  0 },
    { 0x104316A3, 2, B_EXT, "LR--",   1,   2,   0,    0,    0,  0 },
    { 0x104316D3, 2, B_EXT, "LR--",   1,   2,   0,    0,    0,  0 },
    { 0x104316F3, 2, B_EXT, "LR--",   1,   2,   0,    0,    0,  0 },
    { 0x104317F3, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431863, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x104318D3, 2, B_EXT, "LR--",   0,   1,  -1,    0,    0,  0 },
    { 0x10431A83, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431B93, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10431C9F, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10431CAF, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10431CCF, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10431CDF, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10431CEF, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10431D1F, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431DA2, 2, B_EXT, "LR--",   1,   2,   0,    0,    0,  0 },
    { 0x10431E02, 2, B_EXT, "LR--",   1,   2,   0,    0,    0,  0 },
    { 0x10431E12, 2, B_EXT, "LR--",   0,   1,  -1,    0,    0,  0 },
    { 0x10431EE2, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x10431F12, 2, B_INT, "LR--",   0,   1,  -1, 1000, 4500, 24 },
    { 0x10431F1F, 2, B_EXT, "LR--",   1,  -1,   0,    0,    0,  0 },
    { 0x10431F62, 2, B_EXT, "LR--",   1,   2,   0,    0,    0,  0 },
    { 0x10433A20, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10433A30, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10433A40, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10433A50, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x10433A60, 2, B_INT, "LR--",   1,   2,   0, 1000, 4500, 24 },
    { 0x17AA3865, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x17AA3866, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x17AA386E, 2, B_EXT, "LR--",   0,   2,  -1,    0,    0,  0 },
    { 0x17AA386F, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x17AA3874, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x17AA3877, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x17AA3878, 2, B_EXT, "LR--",   0,  -1,  -1,    0,    0,  0 },
    { 0x17AA38A9, 2, B_EXT, "LR--",   0,   2,  -1,    0,    0,  0 },
    { 0x17AA38AB, 2, B_EXT, "LR--",   0,   2,  -1,    0,    0,  0 },
    { 0x17AA38B4, 2, B_EXT, "LR--",   0,   1,  -1,    0,    0,  0 },
    { 0x17AA38B5, 2, B_EXT, "LR--",   0,   1,  -1,    0,    0,  0 },
    { 0x17AA38B6, 2, B_EXT, "LR--",   0,   1,  -1,    0,    0,  0 },
    { 0x17AA38B7, 2, B_EXT, "LR--",   0,   1,  -1,    0,    0,  0 },
    { 0x17AA38C7, 4, B_INT, "RLRL",   0,   2,  -1, 1000, 4500, 24 },
    { 0x17AA38C8, 4, B_INT, "RLRL",   0,   2,  -1, 1000, 4500, 24 },
    { 0x17AA38F9, 2, B_EXT, "RL--",   0,   2,  -1,    0,    0,  0 },
    { 0x17AA38FA, 2, B_EXT, "RL--",   0,   2,  -1,    0,    0,  0 },
    { 0x17AA3929, 4, B_INT, "RLRL",   0,   2,  -1, 1000, 4500, 24 },
    { 0x17AA392B, 4, B_INT, "RLRL",   0,   2,  -1, 1000, 4500, 24 },
};

/* hp_i2c_int_2amp_dual_spkid (only without _DSD) */
static const ULONG g_HpDualSpkid[] = { 0x103C8C66, 0x103C8C67, 0x103C8C68, 0x103C8C6A };

#define SRC_NONE    0
#define SRC_TABLE   1       /* generic_dsd_config */
#define SRC_HPDUAL  2       /* hp_i2c_int_2amp_dual_spkid */
#define SRC_LEGION  3       /* lenovo_legion_no_acpi */
#define SRC_DSD     4       /* cs35l41_hda_parse_acpi / cs35l56_hda_read_acpi */
#define SRC_UNKNOWN 5
static const char *const g_SrcName[] = { "-", "Linux property table", "Linux HP dual speaker id quirk",
                                         "Linux Legion quirk", "ACPI _DSD", "no board data" };

typedef struct {
    GPIO_PIN Pin;
    BOOLEAN  ActiveLow;     /* _DSD active_low: logical level inverted */
} SA_LINE;

typedef struct _SIDEAMP {
    CARD      *Card;
    SST_CODEC  C[SA_MAX];
    ULONG      N;
    UCHAR      Kind, Src;
    BOOLEAN    Spi, HaveSub;
    ULONG      Ssid, CodecVendor;
    char       Hid[12];
    char       Path[40];
    SA_LINE    Reset[SA_MAX];
    ULONG      NReset;
    BOOLEAN    ResetAtEnd;
    LONG       SpkId[SA_MAX];
    /* worker */
    OSLOCK     Lock;
    OSEVENT    Ev, Done;
    BOOLEAN    Want, On, Thread, Quit;
} SIDEAMP;

/* ------------------------------------------------------------------ */
/* ACPI helpers                                                         */
/* ------------------------------------------------------------------ */
typedef struct { const char *Name; AML_DEV Dev; AML_RES Res; BOOLEAN Found; } FIND_GPIO;
static BOOLEAN FindGpioDev(const AML_DEV *d, void *p)
{
    FIND_GPIO *f = (FIND_GPIO *)p;
    if (memcmp(d->Name, f->Name, 4)) return TRUE;
    AmlResources(d, &f->Res);
    if (!f->Res.NGpio) return TRUE;
    f->Dev = *d;
    f->Found = TRUE;
    return FALSE;
}

/* a _DSD GPIO reference: { ^DEV, crs_index, pin_index, active_low }; an integer entry: none */
typedef struct { BOOLEAN Ok; char Dev[5]; ULONG Crs; BOOLEAN ActiveLow; } DSD_GPIO;

/* the integers after a reference are its arguments (Linux __acpi_node_get_property_reference takes
   up to 8 of them), so a Zero placeholder only counts where no reference precedes it */
static ULONG DsdGpios(const AML_DEV *d, const char *prop, DSD_GPIO *g, ULONG max)
{
    AML_ELEM e[32];
    ULONG n = AmlDsdPkg(d, prop, e, 32), i = 0, k = 0, a;
    while (i < n && k < max) {
        memset(&g[k], 0, sizeof(g[k]));
        if (e[i].Kind == AML_EL_REF) {
            ULONGLONG arg[3] = { 0, 0, 0 };
            memcpy(g[k].Dev, e[i].Name, 4);
            for (a = 0, i++; i < n && e[i].Kind == AML_EL_INT && a < 8; i++, a++) if (a < 3) arg[a] = e[i].Int;
            g[k].Ok = TRUE;
            g[k].Crs = (ULONG)arg[0];
            g[k].ActiveLow = arg[2] != 0;
        } else i++;
        k++;
    }
    return k;
}

/* the GpioIo / GpioInt of a _DSD reference (the amplifier device itself or another one) */
static BOOLEAN DsdGpioRes(const AML_DEV *self, const AML_RES *selfRes, const DSD_GPIO *g, AML_GPIO *out)
{
    FIND_GPIO f;
    const AML_RES *r = selfRes;
    if (!g->Ok) return FALSE;
    if (memcmp(g->Dev, self->Name, 4)) {
        memset(&f, 0, sizeof(f));
        f.Name = g->Dev;
        AmlForEachDevice(FindGpioDev, &f);
        if (!f.Found) { LOGI("sideamp: GPIO reference %s not found", g->Dev); return FALSE; }
        r = &f.Res;
    }
    if (g->Crs >= r->NGpio) { LOGI("sideamp: %s has no GPIO resource %u", g->Dev, g->Crs); return FALSE; }
    *out = r->Gpio[g->Crs];
    return TRUE;
}

/* integer array property; count of values */
static ULONG DsdInts(const AML_DEV *d, const char *prop, ULONG *v, ULONG max)
{
    AML_ELEM e[8];
    ULONG n = AmlDsdPkg(d, prop, e, 8), i, k = 0;
    for (i = 0; i < n && k < max; i++) {
        if (e[i].Kind != AML_EL_INT) return 0;
        v[k++] = (ULONG)e[i].Int;
    }
    return k;
}

static BOOLEAN HasDsd(const AML_DEV *d)
{
    AML_ELEM e[1];
    return AmlDsdPkg(d, "cirrus,dev-index", e, 1) || AmlDsdPkg(d, "reset-gpios", e, 1) ||
           AmlDsdPkg(d, "cirrus,speaker-position", e, 1) || AmlDsdPkg(d, "spk-id-gpios", e, 1) ||
           AmlDsdPkg(d, "cs-gpios", e, 1);
}

static BOOLEAN Hex8(const char *s, ULONG *v)
{
    ULONG i, x = 0;
    for (i = 0; i < 8; i++) {
        char ch = s[i];
        x <<= 4;
        if (ch >= '0' && ch <= '9') x |= (ULONG)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') x |= (ULONG)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') x |= (ULONG)(ch - 'A' + 10);
        else return FALSE;
    }
    if (s[8]) return FALSE;
    *v = x;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* discovery                                                            */
/* ------------------------------------------------------------------ */
static const struct { const char *Hid; UCHAR Kind; } g_Hids[] = {
    { "CSC3551", K_L41 }, { "CLSA0100", K_L41 }, { "CLSA0101", K_L41 },
    { "CSC3554", K_L56 }, { "CSC3556", K_L56 }, { "CSC3557", K_L56 },
    { "TIAS2781", K_TAS }, { "TXNW2781", K_TAS }, { "TXNW2770", K_TAS },
};

typedef struct { AML_DEV Dev; UCHAR Kind; char Hid[12]; BOOLEAN Found; } FIND_AMP;
static BOOLEAN FindAmp(const AML_DEV *d, void *p)
{
    FIND_AMP *f = (FIND_AMP *)p;
    ULONG i;
    AML_RES r;
    for (i = 0; i < ARRAYSIZE(g_Hids); i++) {
        if (!AmlHasId(d, g_Hids[i].Hid)) continue;
        AmlResources(d, &r);
        if (!r.NI2c && !r.NSpi) continue;      /* a reference scope, or a second copy without resources */
        f->Dev = *d;
        f->Kind = g_Hids[i].Kind;
        memcpy(f->Hid, g_Hids[i].Hid, strlen_s(g_Hids[i].Hid) + 1);
        f->Found = TRUE;
        return FALSE;
    }
    return TRUE;
}

/* the reset line of an amp: kept once per distinct pad */
static void AddReset(SIDEAMP *x, const AML_GPIO *g, BOOLEAN activeLow)
{
    ULONG i;
    GPIO_PIN p;
    for (i = 0; i < x->NReset; i++)
        if (x->Reset[i].Pin.Pin == g->Pin && !memcmp(x->Reset[i].Pin.Ctrl, g->Ctrl, 4)) return;
    if (x->NReset >= SA_MAX || !GpioResolve(g, &p)) return;
    x->Reset[x->NReset].Pin = p;
    x->Reset[x->NReset].ActiveLow = activeLow;
    x->NReset++;
}

static void ResetLevel(SA_LINE *l, BOOLEAN run)
{
    GpioSet(&l->Pin, l->ActiveLow ? !run : run);
}

/* speaker id GPIOs (logged; Linux uses them to pick firmware / tuning files) */
static LONG ReadSpkId(const AML_DEV *d, const AML_RES *r, const DSD_GPIO *g, ULONG n)
{
    ULONG i;
    LONG id = 0;
    for (i = 0; i < n; i++) {
        AML_GPIO a;
        GPIO_PIN p;
        int v;
        if (!DsdGpioRes(d, r, &g[i], &a) || !GpioResolve(&a, &p) || (v = GpioGet(&p)) < 0) return -1;
        if (g[i].ActiveLow) v = !v;
        id |= (LONG)v << i;
    }
    return n ? id : -1;
}

static DSD_GPIO CrsRef(const AML_DEV *d, ULONG crs)
{
    DSD_GPIO g;
    memset(&g, 0, sizeof(g));
    g.Ok = TRUE;
    memcpy(g.Dev, d->Name, 4);
    g.Crs = crs;
    return g;
}

/* the cs-gpios entry of a chip select: in the amplifier's _DSD or the controller's */
typedef struct { const char *Name; AML_DEV Dev; BOOLEAN Found; } FIND_CTRL;
static BOOLEAN FindCtrl(const AML_DEV *d, void *p)
{
    FIND_CTRL *f = (FIND_CTRL *)p;
    ULONGLONG adr;
    if (memcmp(d->Name, f->Name, 4) || !AmlAdr(d, &adr)) return TRUE;
    f->Dev = *d;
    f->Found = TRUE;
    return FALSE;
}

static BOOLEAN DsdCsGpio(const AML_DEV *d, const AML_RES *r, const char *ctrl, ULONG cs, GPIO_PIN *pin, BOOLEAN *al)
{
    DSD_GPIO g[8];
    AML_GPIO a;
    ULONG n = DsdGpios(d, "cs-gpios", g, 8);
    const AML_DEV *owner = d;
    const AML_RES *ores = r;
    FIND_CTRL f;
    AML_RES cr;
    if (!n) {
        memset(&f, 0, sizeof(f));
        f.Name = ctrl;
        AmlForEachDevice(FindCtrl, &f);
        if (!f.Found || !(n = DsdGpios(&f.Dev, "cs-gpios", g, 8))) return FALSE;
        AmlResources(&f.Dev, &cr);
        owner = &f.Dev; ores = &cr;
    }
    if (cs >= n || !g[cs].Ok) return FALSE;                    /* Zero entry: native chip select */
    if (!DsdGpioRes(owner, ores, &g[cs], &a) || !GpioResolve(&a, pin)) return FALSE;
    *al = g[cs].ActiveLow;
    return TRUE;
}

static const L41_PROP *FindProp(ULONG ssid)
{
    ULONG i;
    for (i = 0; i < ARRAYSIZE(g_L41Props); i++) if (g_L41Props[i].Ssid == ssid) return &g_L41Props[i];
    return NULL;
}

static BOOLEAN InList(const ULONG *l, ULONG n, ULONG v)
{
    ULONG i;
    for (i = 0; i < n; i++) if (l[i] == v) return TRUE;
    return FALSE;
}

/* the per-board configuration of every resource; returns the number of amplifiers set up */
static ULONG Build(SIDEAMP *x, const AML_DEV *d, ULONG codecSubsys)
{
    AML_RES r;
    ULONG nres, i, k, nval = 0, devIdx[8], pos[8], g1[8], g2[8], ipk[8], ind[8], cap[8];
    ULONG nPos = 0, nG1 = 0, nG2 = 0, nIpk = 0, nInd = 0, nCap = 0, namps = 0, bt;
    DSD_GPIO rst[SA_MAX], spk[8];
    ULONG nRst = 0, nSpk = 0;
    const L41_PROP *prop = NULL;
    BOOLEAN dsd = HasDsd(d);
    char sub[24];
    ULONG regBoost = OsRegGetDword(L"SideAmpBoost", 0);

    AmlResources(d, &r);
    x->Spi = !r.NI2c && r.NSpi;
    nres = x->Spi ? r.NSpi : r.NI2c;
    if (nres > SA_MAX) nres = SA_MAX;
    /* SSID: _SUB, else the codec subsystem id (Linux falls back to it only at bind time) */
    if (AmlNamedStr(d, "_SUB", sub, sizeof(sub)) && Hex8(sub, &x->Ssid)) x->HaveSub = TRUE;
    else {
        if (sub[0]) LOGI("sideamp: %s: _SUB \"%s\" is not an 8-digit subsystem id", d->Path, sub);
        x->Ssid = codecSubsys;
    }

    /* which source of board data */
    if (x->Kind == K_L41) {
        if (!strcmp_s(x->Hid, "CLSA0100") || !strcmp_s(x->Hid, "CLSA0101")) x->Src = SRC_LEGION;
        else if (!dsd && InList(g_HpDualSpkid, ARRAYSIZE(g_HpDualSpkid), x->Ssid)) x->Src = SRC_HPDUAL;
        else if ((prop = FindProp(x->Ssid)) != NULL) x->Src = SRC_TABLE;
        else if (dsd) x->Src = SRC_DSD;
        else x->Src = SRC_UNKNOWN;
    } else x->Src = SRC_DSD;

    if (x->Src == SRC_DSD) {
        nval = DsdInts(d, "cirrus,dev-index", devIdx, 8);
        if (!nval) {
            if (x->Kind == K_L56) { LOGE("sideamp: %s: no cirrus,dev-index property", d->Path); return 0; }
            LOGI("sideamp: %s: _DSD without cirrus,dev-index", d->Path);
            x->Src = SRC_UNKNOWN;
        }
    }
    if (x->Src == SRC_DSD && x->Kind == K_L41) {
        nPos = DsdInts(d, "cirrus,speaker-position", pos, 8);
        nG1 = DsdInts(d, "cirrus,gpio1-func", g1, 8);
        nG2 = DsdInts(d, "cirrus,gpio2-func", g2, 8);
        nIpk = DsdInts(d, "cirrus,boost-peak-milliamp", ipk, 8);
        nInd = DsdInts(d, "cirrus,boost-ind-nanohenry", ind, 8);
        nCap = DsdInts(d, "cirrus,boost-cap-microfarad", cap, 8);
        if (nPos < nval || nG1 < nval || nG2 < nval) {
            /* cs35l41_hda_parse_acpi fails on these: no guessing beyond the safe mode */
            LOGE("sideamp: %s: _DSD lacks cirrus,speaker-position / gpio1-func / gpio2-func", d->Path);
            x->Src = SRC_UNKNOWN;
        } else {
            ULONGLONG t;
            if (AmlDsdInt(d, "cirrus,boost-type", &t))
                LOGI("sideamp: %s: cirrus,boost-type %u (ignored: Linux derives the type from the boost values)",
                     d->Path, (ULONG)t);
        }
    }
    if (x->Src == SRC_DSD || (x->Src == SRC_TABLE && dsd)) {
        nRst = DsdGpios(d, "reset-gpios", rst, SA_MAX);
        nSpk = DsdGpios(d, "spk-id-gpios", spk, 8);
        if (!nSpk && x->Ssid == 0x10431A63) { spk[0] = CrsRef(d, 2); nSpk = 1; }    /* missing_speaker_id_gpio2 */
    }
    if (x->Kind == K_L41 && x->Src == SRC_UNKNOWN)
        LOGI("sideamp: %s: SSID %08x is not in the Linux table and there is no usable _DSD: boost not configured "
             "(safe mode, boost converter and bypass FET off; registry SideAmpBoost overrides)", d->Path, x->Ssid);
    switch (x->Src) {
    case SRC_TABLE: namps = prop->NAmps; break;
    case SRC_HPDUAL: case SRC_LEGION: namps = 2; break;
    case SRC_DSD: namps = nval; break;
    default: namps = nres; break;
    }
    if (namps > SA_MAX) namps = SA_MAX;

    for (i = 0; i < nres && x->N < SA_MAX; i++) {
        SST_CODEC *c = &x->C[x->N];
        ULONG id = x->Spi ? r.SpiCs[i] : r.I2c[i].Addr;
        LONG idx = -1;
        AMP_CFG a;
        AML_GPIO g;
        GPIO_PIN cs;
        BOOLEAN useCs = FALSE, csAl = FALSE;
        memset(&a, 0, sizeof(a));
        a.Valid = TRUE;
        a.Gpio1 = a.Gpio2 = AMP_GPIO_NONE;

        switch (x->Src) {
        case SRC_TABLE:
            idx = x->Spi ? (LONG)id : namps > 2 ? (LONG)id - 0x40 : id == 0x40 ? 0 : 1;
            break;
        case SRC_HPDUAL: case SRC_LEGION:
            idx = id == 0x40 ? 0 : 1;
            break;
        case SRC_DSD:
            for (k = 0; k < nval; k++) if (devIdx[k] == id) { idx = (LONG)k; break; }
            break;
        default:
            for (idx = 0, k = 0; k < nres; k++)
                if ((x->Spi ? r.SpiCs[k] : r.I2c[k].Addr) < id) idx++;
            break;
        }
        if (idx < 0 || (ULONG)idx >= namps) {
            LOGV("sideamp: %s: %s %x has no amplifier index", d->Path, x->Spi ? "chip select" : "address", id);
            continue;
        }

        /* board data */
        if (x->Kind == K_L41) {
            switch (x->Src) {
            case SRC_TABLE:
                a.Pos = prop->Ch[idx] == 'R' ? AMP_POS_RIGHT : AMP_POS_LEFT;
                if (prop->Ext == B_INT) {
                    a.Boost = AMP_BST_INTERNAL; a.IndNh = prop->Ind; a.IpkMa = prop->Ipk; a.CapUf = prop->Cap;
                    a.Gpio1 = AMP_GPIO_UNUSED;
                } else {
                    a.Boost = AMP_BST_EXTERNAL;
                    a.Gpio1 = AMP_GPIO_VSPK;
                }
                a.Gpio2 = AMP_GPIO_IRQ;
                break;
            case SRC_HPDUAL:
                a.Pos = (UCHAR)idx;
                a.Boost = AMP_BST_INTERNAL; a.IndNh = 1000; a.IpkMa = 4100; a.CapUf = 24;
                a.Gpio1 = AMP_GPIO_UNUSED; a.Gpio2 = AMP_GPIO_IRQ;
                break;
            case SRC_LEGION:
                a.Pos = (UCHAR)idx;
                a.Gpio2 = AMP_GPIO_IRQ;
                if (!strcmp_s(x->Hid, "CLSA0100")) a.Boost = AMP_BST_EXT_NOSW;
                else { a.Boost = AMP_BST_EXTERNAL; a.Gpio1 = AMP_GPIO_VSPK; }
                break;
            case SRC_DSD:
                a.Pos = (UCHAR)(pos[idx] > AMP_POS_CENTER ? AMP_POS_LEFT : pos[idx]);
                a.Gpio1 = (UCHAR)(g1[idx] <= AMP_GPIO_SYNC ? g1[idx] : AMP_GPIO_NONE);
                a.Gpio2 = (UCHAR)(g2[idx] <= AMP_GPIO_SYNC ? g2[idx] : AMP_GPIO_NONE);
                a.IpkMa = (USHORT)(nIpk > (ULONG)idx ? ipk[idx] : 0);
                a.IndNh = (USHORT)(nInd > (ULONG)idx ? ind[idx] : 0);
                a.CapUf = (USHORT)(nCap > (ULONG)idx ? cap[idx] : 0);
                a.Boost = (a.IpkMa || a.IndNh || a.CapUf) ? AMP_BST_INTERNAL : AMP_BST_EXTERNAL;
                break;
            default:
                a.Pos = (UCHAR)(idx & 1);
                a.Boost = AMP_BST_SAFE;
                break;
            }
            bt = regBoost;
            if (bt >= 1 && bt <= 4) {
                a.Boost = (UCHAR)bt;
                if (bt == AMP_BST_INTERNAL) {
                    a.IndNh = (USHORT)OsRegGetDword(L"SideAmpBstInd", 0);
                    a.IpkMa = (USHORT)OsRegGetDword(L"SideAmpBstIpk", 0);
                    a.CapUf = (USHORT)OsRegGetDword(L"SideAmpBstCap", 0);
                }
                if (bt == AMP_BST_EXTERNAL && a.Gpio1 == AMP_GPIO_NONE) a.Gpio1 = AMP_GPIO_VSPK;
            }
        } else a.Pos = (UCHAR)(idx & 1);

        /* reset line (released by the pulse in SideAmpProbe) */
        memset(&c->Gpio[0], 0, sizeof(c->Gpio[0]));
        c->NGpio = 0;
        if (x->Src == SRC_DSD || (x->Src == SRC_TABLE && dsd)) {
            ULONG gi = (ULONG)idx < nRst ? (ULONG)idx : 0;
            if (nRst && DsdGpioRes(d, &r, &rst[gi], &g)) AddReset(x, &g, rst[gi].ActiveLow);
        } else if (x->Src == SRC_TABLE) {
            if (prop->Reset >= 0 && (ULONG)prop->Reset < r.NGpio) AddReset(x, &r.Gpio[prop->Reset], FALSE);
        } else if (x->Src == SRC_HPDUAL || x->Src == SRC_LEGION) {
            if (r.NGpio) AddReset(x, &r.Gpio[0], FALSE);
        } else {
            /* unknown board: the first GpioIo, only ever released (CSC3541 behaviour, by the chip driver) */
            for (k = 0; k < r.NGpio; k++)
                if (!r.Gpio[k].Int) { if (GpioResolve(&r.Gpio[k], &c->Gpio[0])) c->NGpio = 1; break; }
        }

        /* speaker id (logged) */
        x->SpkId[x->N] = -1;
        if (x->Kind == K_L41) {
            if (x->Src == SRC_TABLE && !dsd && prop->SpkId >= 0) {
                DSD_GPIO s1 = CrsRef(d, (ULONG)prop->SpkId);
                x->SpkId[x->N] = ReadSpkId(d, &r, &s1, 1);
            } else if (x->Src == SRC_HPDUAL || x->Src == SRC_LEGION) {
                DSD_GPIO s1 = CrsRef(d, x->Src == SRC_HPDUAL ? (ULONG)idx + 1 : 2);
                x->SpkId[x->N] = ReadSpkId(d, &r, &s1, 1);
            }
        }
        if (nSpk && namps && !(nSpk % namps)) {
            ULONG per = nSpk / namps;
            x->SpkId[x->N] = ReadSpkId(d, &r, spk + per * (ULONG)idx, per);
        }

        /* bus */
        memset(&cs, 0, sizeof(cs));
        if (x->Spi) {
            if (DsdCsGpio(d, &r, r.SpiCtrl[i], id, &cs, &csAl)) useCs = TRUE;
            else if (x->Src == SRC_TABLE && !dsd && prop->Cs >= 0 && namps == 2 && (ULONG)prop->Cs < r.NGpio) {
                /* generic_dsd_config: the amp on chip select 1 uses the GPIO; the other one the native CS
                   with the GPIO held high (SpiOpen of amp 1 deasserts it before anything is sent) */
                if (id == 1 && GpioResolve(&r.Gpio[prop->Cs], &cs)) useCs = TRUE;
            }
        }
        c->Drv = x->Kind == K_L41 ? &g_DrvCs35l41Hda : &g_DrvCs35l56;
        c->RegBytes = c->Drv->RegBytes;
        c->ValBytes = c->Drv->ValBytes;
        if (x->Spi) {
            ULONG hz = r.SpiSpeed[i];
            if (!hz || hz > 4000000) hz = hz ? 4000000 : 1000000;     /* CS35L41_SPI_MAX_FREQ */
            c->Spi = SpiOpen(r.SpiCtrl[i], id, hz, r.SpiMode[i], useCs ? &cs : NULL,
                             (BOOLEAN)(((r.SpiMode[i] & AML_SPI_CS_HIGH) != 0) ^ csAl));
            c->SpiPad = 2;
            c->SpiRead = 0x80;
            if (!c->Spi) { LOGE("sideamp: %s: SPI controller %s unusable", d->Path, r.SpiCtrl[i]); continue; }
        } else {
            c->Bus = I2cOpen(r.I2c[i].Ctrl);
            c->Addr = r.I2c[i].Addr;
            if (!c->Bus) { LOGE("sideamp: %s: I2C controller %s unusable", d->Path, r.I2c[i].Ctrl); continue; }
        }
        c->Amp = a;
        c->Index = (UCHAR)idx;
        c->Count = (UCHAR)namps;
        c->Port = 0;
        c->Rate = 48000; c->Slots = 2; c->Slot = 32; c->Valid = 24;
        c->Bclk = c->Rate * c->Slots * c->Slot;
        c->Mclk = 0;
        memcpy(c->Hid, x->Hid, sizeof(c->Hid));
        {
            ULONG pl = strlen_s(d->Path), st = pl > 30 ? pl - 30 : 0;
            OsSprintf(c->Path, sizeof(c->Path), "%s#%u", d->Path + st, (ULONG)idx);
        }
        if (x->Kind == K_L41 && a.Boost == AMP_BST_EXT_NOSW) x->ResetAtEnd = FALSE;
        x->N++;
    }
    return x->N;
}

/* ------------------------------------------------------------------ */
/* worker                                                               */
/* ------------------------------------------------------------------ */
static void Apply(SIDEAMP *x, BOOLEAN on)
{
    ULONG k;
    for (k = 0; k < x->N; k++) {
        SST_CODEC *c = &x->C[on ? k : x->N - 1 - k];
        if (!c->Ready || !c->Drv->Stream || c->PlayOn == on) continue;
        c->Drv->Stream(c, FALSE, on);
        c->PlayOn = on;
    }
    x->On = on;
}

static void Worker(void *ctx)
{
    SIDEAMP *x = (SIDEAMP *)ctx;
    for (;;) {
        BOOLEAN want;
        OsEventWait(&x->Ev, 1000);
        for (;;) {
            OsLockAcquire(&x->Lock);
            want = x->Want;
            OsLockRelease(&x->Lock);
            if (want == x->On || x->Quit) break;
            Apply(x, want);
        }
        if (x->Quit) break;
    }
    OsEventSet(&x->Done);
}

/* ------------------------------------------------------------------ */
/* interface                                                            */
/* ------------------------------------------------------------------ */
static CARD *g_SideAmpCard;     /* the amplifiers belong to one HD Audio card (the first with a codec) */

BOOLEAN SideAmpProbe(CARD *card, ULONG codecVendor, ULONG codecSubsys)
{
    FIND_AMP f;
    SIDEAMP *x;
    ULONG i, ok = 0;
    if (card->SideAmp || !OsRegGetDword(L"SideAmps", 1)) return FALSE;
    if (g_SideAmpCard) {
        LOGV("sideamp: amplifiers already driven by card%u", g_SideAmpCard->Index);
        return FALSE;
    }
    memset(&f, 0, sizeof(f));
    AmlForEachDevice(FindAmp, &f);
    if (!f.Found) return FALSE;
    if (f.Kind == K_TAS) {
        LOGI("sideamp: %s %s: TI TAS2781 amplifiers need the RCA / DSP files of linux-firmware ti/tas2781 "
             "(SSID %08x): not supported, the speakers stay silent", f.Hid, f.Dev.Path, codecSubsys);
        return FALSE;
    }
    x = (SIDEAMP *)OsAlloc(sizeof(SIDEAMP));
    if (!x) return FALSE;
    x->Card = card;
    x->Kind = f.Kind;
    x->CodecVendor = codecVendor;
    x->ResetAtEnd = TRUE;
    memcpy(x->Hid, f.Hid, sizeof(x->Hid));
    memcpy(x->Path, f.Dev.Path, sizeof(x->Path));
    if (!Build(x, &f.Dev, codecSubsys)) {
        LOGE("sideamp: %s %s: no usable amplifier", x->Hid, x->Path);
        SpiCloseAll();
        OsFree(x);
        return FALSE;
    }
    LOGI("sideamp: %s %s, codec %08x, SSID %08x (%s): %u %s amplifier(s), %s", x->Hid, x->Path, codecVendor,
         x->Ssid, x->HaveSub ? "_SUB" : "codec", x->N, x->Spi ? "SPI" : "I2C", g_SrcName[x->Src]);
    /* one reset pulse per line, then the chips boot (cs35l41_hda_probe / cs35l56_hda_common_probe) */
    for (i = 0; i < x->NReset; i++) ResetLevel(&x->Reset[i], FALSE);
    if (x->NReset) {
        CDelay(2);
        for (i = 0; i < x->NReset; i++) ResetLevel(&x->Reset[i], TRUE);
        CDelay(2);
    }
    for (i = 0; i < x->N; i++) {
        SST_CODEC *c = &x->C[i];
        if (!c->Drv->Probe(c)) {
            LOGE("sideamp: %s %s does not answer at %s", c->Drv->Name, c->Path, c->Spi ? SpiName(c->Spi) : I2cName(c->Bus));
            continue;
        }
        if (!c->Drv->Init(c)) { LOGE("sideamp: %s %s did not initialise", c->Drv->Name, c->Path); continue; }
        c->Ready = TRUE;
        ok++;
        LOGI("sideamp: %s %s at %s%s%02x, %s, speaker id %d", c->Drv->Name, c->Path,
             c->Spi ? SpiName(c->Spi) : I2cName(c->Bus), c->Spi ? "" : ":", c->Spi ? 0 : c->Addr,
             c->Amp.Pos == AMP_POS_RIGHT ? "right" : c->Amp.Pos == AMP_POS_CENTER ? "centre" : "left",
             (int)x->SpkId[i]);
    }
    if (!ok) {
        LOGE("sideamp: no amplifier came up");
        for (i = 0; i < x->NReset && x->ResetAtEnd; i++) ResetLevel(&x->Reset[i], FALSE);
        SpiCloseAll();
        OsFree(x);
        return FALSE;
    }
    OsLockInit(&x->Lock);
    OsEventInit(&x->Ev);
    OsEventInit(&x->Done);
    x->Thread = OsThreadStart(Worker, x);
    card->SideAmp = x;
    g_SideAmpCard = card;
    return TRUE;
}

void SideAmpPower(CARD *card, BOOLEAN on)
{
    SIDEAMP *x = (SIDEAMP *)card->SideAmp;
    if (!x) return;
    if (!x->Thread) {
        if (KeGetCurrentIrql() < DISPATCH_LEVEL) Apply(x, on);
        return;
    }
    OsLockAcquire(&x->Lock);
    x->Want = on;
    OsLockRelease(&x->Lock);
    OsEventSet(&x->Ev);
}

/* another card's Smart Sound codecs still use the shared I2C / GPIO mappings */
static BOOLEAN OthersUseI2c(void)
{
    ULONG i;
    for (i = 0; i < MAX_CARDS; i++)
        if (g_Cards[i] && g_Cards[i]->Sst && ((SST *)g_Cards[i]->Sst)->Codecs) return TRUE;
    return FALSE;
}

void SideAmpShutdown(CARD *card)
{
    SIDEAMP *x = (SIDEAMP *)card->SideAmp;
    ULONG i;
    if (!x) return;
    if (x->Thread) {
        x->Quit = TRUE;
        OsEventSet(&x->Ev);
        OsEventWait(&x->Done, 5000);
    }
    for (i = 0; i < x->N; i++) {
        SST_CODEC *c = &x->C[x->N - 1 - i];
        if (c->Ready && c->Drv->Shutdown) c->Drv->Shutdown(c);
        c->Ready = FALSE;
        c->PlayOn = FALSE;
    }
    if (x->ResetAtEnd)
        for (i = 0; i < x->NReset; i++) ResetLevel(&x->Reset[i], FALSE);
    SpiCloseAll();
    if (!OthersUseI2c()) {
        I2cCloseAll();
        GpioCloseAll();
    }
    card->SideAmp = NULL;
    if (g_SideAmpCard == card) g_SideAmpCard = NULL;
    OsFree(x);
}

ULONG SideAmpDump(CARD *card, char *buf, ULONG size)
{
    SIDEAMP *x = (SIDEAMP *)card->SideAmp;
    static const char *const bst[] = { "-", "internal boost", "external boost + VSPK switch", "external boost",
                                       "safe (no boost data)" };
    ULONG n = 0, i;
    if (!x || size < 200) return 0;
    n += OsSprintf(buf + n, size - n, "Speaker amplifiers: %s %s, SSID %08x (%s), %s, %u reset line(s)\r\n", x->Hid,
                   x->Path, x->Ssid, x->HaveSub ? "_SUB" : "codec", g_SrcName[x->Src], x->NReset);
    for (i = 0; i < x->N && n + 200 < size; i++) {
        SST_CODEC *c = &x->C[i];
        n += OsSprintf(buf + n, size - n, "  amplifier %s %s at %s", c->Drv->Name, c->Path,
                       c->Spi ? SpiName(c->Spi) : I2cName(c->Bus));
        if (!c->Spi) n += OsSprintf(buf + n, size - n, ":%02x", c->Addr);
        n += OsSprintf(buf + n, size - n, ", amp %u of %u, %s", c->Index, c->Count,
                       c->Amp.Pos == AMP_POS_RIGHT ? "right" : c->Amp.Pos == AMP_POS_CENTER ? "centre" : "left");
        if (c->Drv == &g_DrvCs35l41Hda) {
            n += OsSprintf(buf + n, size - n, ", %s", c->Amp.Boost <= AMP_BST_SAFE ? bst[c->Amp.Boost] : "?");
            if (c->Amp.Boost == AMP_BST_INTERNAL)
                n += OsSprintf(buf + n, size - n, " %u nH %u mA %u uF", c->Amp.IndNh, c->Amp.IpkMa, c->Amp.CapUf);
        }
        n += OsSprintf(buf + n, size - n, ", speaker id %d%s%s, %u bus errors\r\n", (int)x->SpkId[i],
                       c->Ready ? "" : ", not initialised", c->PlayOn ? ", playing" : "", c->Errors);
    }
    return n;
}
