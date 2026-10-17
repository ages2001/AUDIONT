/*
 * AUDIONT - Maxim (Analog Devices) SoundWire amplifiers:
 *   MAX98373 (Linux sound/soc/codecs/max98373-sdw.c, max98373.c)
 *     DP1 playback (2 channels) -> "DAI Sel" mono mix (PCM_TO_SPK_MONO_MIX_1 7:6) -> class D;
 *     DP3 I/V sense: not used.  Non-SDCA (class 0): the SoundWire clock is set in the
 *     vendor register SOUNDWIRE_CTRL (max98373_clock_config).
 *   MAX98363 (Linux sound/soc/codecs/max98363.c)
 *     DP1 playback, one channel, no vendor setup at all (the amplifier runs while the port carries
 *     data); SCP_BUS_CLOCK_BASE / BUSCLOCK_SCALE are written by the driver (Linux
 *     prop.clock_reg_supported) since the bus core only does it for SDCA-class parts.
 *
 * Channel selection: the MAX98373 port takes both channels (mirrored) and DAI Sel picks
 * "Left" (0) for an even Index, "Right" (1) for an odd one, "LeftRight" (2, the Linux default,
 * (L+R)/2) for a single amplifier.  The MAX98363 port has one channel: the bus core gives the
 * amplifiers of one link consecutive channels of the stream (ACPI order: the uid 0 part takes
 * the left, uid 1 the right, as Linux' mx8363_2_adr "Left" / "Right"); a MAX98363 alone on its
 * link plays the left channel.
 *
 * The link clocks are not part of SDW_DEV; SdwAmpClocks re-derives them from the ACPI link
 * subproperties exactly as sdw.c LinkProps does (also used by sdw_rt1308.c).
 */
#include "sdw.h"

/* ------------------------------------------------------------------ */
/* link clocks (sdw.c LinkProps / SetFrequency)                         */
/* ------------------------------------------------------------------ */
typedef struct { const char *Name; ULONG Hda; AML_DEV Ctrl; BOOLEAN Found, Strong; } CTRLSCAN;

static BOOLEAN IsDevice(const AML_DEV *a) { return a->T[a->Start] == 0x5B; }

static BOOLEAN ScanCtrl(const AML_DEV *a, void *ctx)
{
    CTRLSCAN *c = (CTRLSCAN *)ctx;
    ULONGLONG adr, pa;
    AML_DEV p;
    BOOLEAN strong;
    if (!AmlAdr(a, &adr) || adr != 0x40000000ULL) return TRUE;
    if (c->Name && memcmp(a->Name, c->Name, 4)) return TRUE;
    strong = AmlParent(a, &p) && IsDevice(&p) && AmlAdr(&p, &pa) && pa == c->Hda;
    if (!c->Found || (strong && !c->Strong)) { c->Ctrl = *a; c->Found = TRUE; c->Strong = strong; }
    return !strong;
}

/* the IP clock / 2 (the bus core's MCLK) and the bus clock of the peripheral's link */
BOOLEAN SdwAmpClocks(const SDW_DEV *d, ULONG *mclk, ULONG *bus)
{
    CTRLSCAN c;
    AML_DEV p, sub;
    ULONGLONG v, a;
    ULONG f[8], n, i, ip, bc = 0;
    char name[40];
    memset(&c, 0, sizeof(c));
    c.Hda = (d->Sst->Card->Dev << 16) | d->Sst->Card->Fn;
    if (AmlParent(&d->Acpi, &p)) {
        if (IsDevice(&p) && AmlAdr(&p, &a) && a == 0x40000000ULL) { c.Ctrl = p; c.Found = TRUE; }
        else if (!IsDevice(&p)) c.Name = p.Name;          /* Scope (..SNDW) in an SSDT: the controller of that name */
    }
    if (!c.Found) AmlForEachDevice(ScanCtrl, &c);
    if (!c.Found && c.Name) { c.Name = NULL; AmlForEachDevice(ScanCtrl, &c); }
    ip = d->Sst->P->Gen == SST_CNL ? 24000000 : 38400000;
    OsSprintf(name, sizeof(name), "mipi-sdw-link-%u-subproperties", d->Link);
    if (!c.Found || !SdwAcpiSub(&c.Ctrl, name, &sub)) return FALSE;
    if (SdwAcpiInt(&sub, "intel-sdw-ip-clock", &v) && v) ip = (ULONG)v;
    if (SdwAcpiInt(&sub, "mipi-sdw-max-clock-frequency", &v)) bc = (ULONG)v;
    n = SdwAcpiInts(&sub, "mipi-sdw-clock-frequencies-supported", f, 8);
    for (i = 0; i < n && !bc; i++) if (f[i] > bc) bc = f[i];
    *mclk = ip / 2;
    if (!bc) bc = !(*mclk % 4800000) ? 4800000 : !(*mclk % 6000000) ? 6000000 : *mclk / 4;
    *bus = bc;
    return TRUE;
}

/* SCP_BUS_CLOCK_BASE / SCP_BUSCLOCK_SCALE (bus.c sdw_slave_set_frequency) */
static BOOLEAN SetFrequency(SDW_DEV *d, ULONG mclk, ULONG cur)
{
    ULONG scale, idx;
    UCHAR base;
    if (!mclk || !cur) return FALSE;
    if (!(19200000 % mclk)) { mclk = 19200000; base = 1; }
    else if (!(22579200 % mclk)) { mclk = 22579200; base = 4; }
    else if (!(24576000 % mclk)) { mclk = 24576000; base = 3; }
    else if (!(32000000 % mclk)) { mclk = 32000000; base = 5; }
    else if (!(96000000 % mclk)) { mclk = 24000000; base = 2; }
    else return FALSE;
    if (mclk % cur) return FALSE;
    scale = mclk / cur;
    for (idx = 0; idx < 7 && (1UL << idx) != scale; idx++) ;
    if (idx == 7) return FALSE;
    return SdwWrite(d, SDW_SCP_BUS_CLOCK_BASE, base) && SdwWrite(d, SDW_SCP_BUSCLOCK_SCALE_B0, (UCHAR)(idx + 1)) &&
           SdwWrite(d, SDW_SCP_BUSCLOCK_SCALE_B1, (UCHAR)(idx + 1));
}

/* ------------------------------------------------------------------ */
/* MAX98373                                                             */
/* ------------------------------------------------------------------ */
#define R2000_SW_RESET          0x2000
#define R2020_PCM_TX_HIZ_EN_1   0x2020
#define R2021_PCM_TX_HIZ_EN_2   0x2021
#define R2022_PCM_TX_SRC_1      0x2022
#define R2023_PCM_TX_SRC_2      0x2023
#define R2024_PCM_DATA_FMT_CFG  0x2024      /* CHANSZ 7:6: 1 16, 2 24, 3 32 bit */
#define R2025_AUDIO_IF_MODE     0x2025
#define R2028_PCM_SR_SETUP_2    0x2028      /* SPK_SR 7:4, IVADC_SR 3:0: 8 = 48 kHz */
#define R2029_PCM_TO_SPK_MONO_MIX_1 0x2029  /* MONOMIX_CFG 7:6, CH0_SRC 3:0 */
#define R202A_PCM_TO_SPK_MONO_MIX_2 0x202A  /* CH1_SRC 3:0 */
#define R202B_PCM_RX_EN         0x202B
#define R2036_SOUNDWIRE_CTRL    0x2036
#define R203F_AMP_DSP_CFG       0x203F
#define R2043_AMP_EN            0x2043      /* SPKFB_EN 1, SPK_EN 0 */
#define R2046_IV_SENSE_ADC_DSP_CFG 0x2046
#define R2047_IV_SENSE_ADC_EN   0x2047
#define R20B5_BDE_EN            0x20B5
#define R20E2_LIMITER_EN        0x20E2
#define R20FF_GLOBAL_SHDN       0x20FF      /* GLOBAL_EN 0 */
#define R21FF_REV_ID            0x21FF

/* Priv[] */
#define P_ON    0

/* max98373_clock_calculate: the bus clock as family member >> x */
static UCHAR M73Clock(ULONG bus)
{
    static const ULONG fam[] = { 7680000, 8400000, 9600000, 11289600, 12000000, 12288000, 13000000 };
    ULONG x, y;
    for (x = 0; x < 4; x++)
        for (y = 0; y < ARRAYSIZE(fam); y++)
            if (bus == (fam[y] >> x)) return (UCHAR)((x << 3) + y);
    return 0x5;                                         /* 12.288 MHz: the Linux default */
}

static BOOLEAN M73Probe(SDW_DEV *d)
{
    d->Kind = SDW_KIND_AMP;
    d->Play.Num = 1; d->Play.Ch = 2; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = FALSE;   /* simple_ch_prep_sm */
    memset(&d->Rec, 0, sizeof(d->Rec));                 /* DP3 I/V sense not used */
    d->Paging = TRUE;
    return TRUE;
}

static ULONG Prop(SDW_DEV *d, const char *name, ULONG def)
{
    ULONGLONG v;
    return SdwAcpiInt(&d->Acpi, name, &v) ? (ULONG)v & 0xF : def;
}

static BOOLEAN M73Init(SDW_DEV *d)
{
    ULONG e0 = d->Errors, i, vslot, islot, fbslot, mclk = 0, bus = 0;
    UCHAR rev = 0, clk, mix, chsz;
    BOOLEAN ok = FALSE;
    /* max98373_reset: SOFT_RESET, then REV_ID readable within 3 x 10 ms */
    if (!SdwWrite(d, R2000_SW_RESET, 0x01)) return FALSE;
    for (i = 0; i < 3 && !ok; i++) { SdwDelay(10); ok = SdwRead(d, R21FF_REV_ID, &rev); }
    if (!ok) { LOGE("sdw: %s: MAX98373 reset failed", d->Name); return FALSE; }
    vslot = Prop(d, "maxim,vmon-slot-no", 0);           /* max98373_slot_config */
    islot = Prop(d, "maxim,imon-slot-no", 1);
    fbslot = Prop(d, "maxim,spkfb-slot-no", 2);
    /* max98373_io_init */
    SdwWrite(d, R2025_AUDIO_IF_MODE, 3);                /* SoundWire mode */
    SdwWrite(d, R2047_IV_SENSE_ADC_EN, 3);
    SdwWrite(d, R2036_SOUNDWIRE_CTRL, 5);
    SdwWrite(d, R2028_PCM_SR_SETUP_2, 0x88);
    SdwWrite(d, R2020_PCM_TX_HIZ_EN_1, 0xFF);
    SdwWrite(d, R2021_PCM_TX_HIZ_EN_2, 0xFF);
    SdwWrite(d, R2029_PCM_TO_SPK_MONO_MIX_1, 0x80);     /* LeftRight, CH0 from slot 0 */
    SdwWrite(d, R202A_PCM_TO_SPK_MONO_MIX_2, 0x01);     /* CH1 from slot 1 */
    SdwWrite(d, R203F_AMP_DSP_CFG, 0x03);               /* DC blocker, dither */
    SdwWrite(d, R2046_IV_SENSE_ADC_DSP_CFG, 0x07);
    SdwWrite(d, R2022_PCM_TX_SRC_1, (UCHAR)((islot << 4) | vslot));
    SdwUpdate(d, vslot < 8 ? R2020_PCM_TX_HIZ_EN_1 : R2021_PCM_TX_HIZ_EN_2, (UCHAR)(1U << (vslot & 7)), 0);
    SdwUpdate(d, islot < 8 ? R2020_PCM_TX_HIZ_EN_1 : R2021_PCM_TX_HIZ_EN_2, (UCHAR)(1U << (islot & 7)), 0);
    SdwWrite(d, R2023_PCM_TX_SRC_2, (UCHAR)fbslot);
    SdwUpdate(d, R2043_AMP_EN, 0x01, 0x01);             /* speaker enable */
    SdwWrite(d, R20B5_BDE_EN, 1);
    SdwWrite(d, R20E2_LIMITER_EN, 1);
    /* max98373_clock_config (bus_config) */
    if (!SdwAmpClocks(d, &mclk, &bus)) LOGI("sdw: %s: link clock not found in ACPI, SWCLK setting 12.288 MHz", d->Name);
    clk = M73Clock(bus);
    SdwWrite(d, R2036_SOUNDWIRE_CTRL, clk);
    SdwWrite(d, R2028_PCM_SR_SETUP_2, 0x88);
    /* hw_params: channel size = the port's word length (Linux: 32-bit S32_LE words), 48 kHz speaker and IV */
    chsz = d->Bits <= 16 ? 0x40 : d->Bits <= 24 ? 0x80 : 0xC0;
    SdwUpdate(d, R2024_PCM_DATA_FMT_CFG, 0xC0, chsz);
    SdwUpdate(d, R2028_PCM_SR_SETUP_2, 0xFF, 0x88);
    /* DAI Sel */
    mix = d->Count >= 2 ? ((d->Index & 1) ? 0x40 : 0x00) : 0x80;
    SdwUpdate(d, R2029_PCM_TO_SPK_MONO_MIX_1, 0xC0, mix);
    SdwUpdate(d, R20FF_GLOBAL_SHDN, 0x01, 0);
    SdwUpdate(d, R202B_PCM_RX_EN, 0x01, 0);
    d->Priv[P_ON] = FALSE;
    LOGI("sdw: %s: MAX98373 rev %02x, bus %u Hz (SOUNDWIRE_CTRL %02x), %s (amplifier %u of %u), %u bus errors in init",
         d->Name, rev, bus, clk, mix == 0x80 ? "LeftRight" : mix ? "Right" : "Left", d->Index, d->Count,
         d->Errors - e0);
    return d->Errors - e0 < 8;
}

/* DAPM "Amp Enable" (DAC_E on PCM_RX_EN 0): POST_PMU GLOBAL_EN + 30 ms, PRE_PMD GLOBAL_EN off + 30 ms */
static void M73Stream(SDW_DEV *d, BOOLEAN rec, BOOLEAN on)
{
    if (rec) return;
    if (on) {
        SdwUpdate(d, R202B_PCM_RX_EN, 0x01, 0x01);
        SdwUpdate(d, R20FF_GLOBAL_SHDN, 0x01, 0x01);
        SdwDelay(30);
    } else {
        SdwUpdate(d, R20FF_GLOBAL_SHDN, 0x01, 0);
        SdwDelay(30);
        SdwUpdate(d, R202B_PCM_RX_EN, 0x01, 0);
    }
    d->Priv[P_ON] = on;
}

static void M73Shutdown(SDW_DEV *d)
{
    if (!d->Ready) return;
    if (d->Priv[P_ON]) M73Stream(d, FALSE, FALSE);
    SdwUpdate(d, R20FF_GLOBAL_SHDN, 0x01, 0);
}

static const USHORT g_M73Parts[] = { 0x8373, 0 };
const SDW_DRV g_SdwMax98373 = { "MAX98373", 0x019F, g_M73Parts, SDW_KIND_AMP, M73Probe, M73Init, M73Stream,
                                 M73Shutdown };

/* ------------------------------------------------------------------ */
/* MAX98363                                                             */
/* ------------------------------------------------------------------ */
#define M63_R2040_AMP_VOL       0x2040
#define M63_R2041_AMP_GAIN      0x2041
#define M63_R21FF_REV_ID        0x21FF

static BOOLEAN M63Probe(SDW_DEV *d)
{
    d->Kind = SDW_KIND_AMP;
    d->Play.Num = 1; d->Play.Ch = 1; d->Play.Type = SDW_DP_FULL; d->Play.CpSm = FALSE;   /* mono, simple_ch_prep_sm */
    memset(&d->Rec, 0, sizeof(d->Rec));
    d->Paging = TRUE;
    return TRUE;
}

static BOOLEAN M63Init(SDW_DEV *d)
{
    ULONG e0 = d->Errors, mclk = 0, bus = 0;
    UCHAR rev = 0, vol = 0, gain = 0;
    BOOLEAN clk;
    /* max98363_io_init: the revision is the only access */
    if (!SdwRead(d, M63_R21FF_REV_ID, &rev)) return FALSE;
    clk = SdwAmpClocks(d, &mclk, &bus) && SetFrequency(d, mclk, bus);
    SdwRead(d, M63_R2040_AMP_VOL, &vol);
    SdwRead(d, M63_R2041_AMP_GAIN, &gain);
    LOGI("sdw: %s: MAX98363 rev %02x, %s %u / %u Hz, digital volume %02x, speaker gain %u, amplifier %u of %u "
         "(channel by port position)", d->Name, rev, clk ? "clock scale set for" : "clock scale NOT set,", mclk, bus,
         vol, gain, d->Index, d->Count);
    return d->Errors - e0 < 8;
}

static const USHORT g_M63Parts[] = { 0x8363, 0 };
const SDW_DRV g_SdwMax98363 = { "MAX98363", 0x019F, g_M63Parts, SDW_KIND_AMP, M63Probe, M63Init, NULL, NULL };
