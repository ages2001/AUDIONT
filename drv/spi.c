/*
 * AUDIONT - SPI masters of the Intel LPSS (PXA2xx-type "LPSS SSP"), polled.
 *
 * The Intel LPSS SPI functions (Sunrise Point and later PCHs, Broxton / Apollo Lake, ...) are not
 * DesignWare SSI blocks: Linux drives them with spi-pxa2xx.c (types LPSS_SPT_SSP, LPSS_BXT_SSP,
 * LPSS_CNL_SSP, chosen by intel-lpss-pci.c).  Registers (include/linux/pxa2xx_ssp.h): SSCR0 / SSCR1 /
 * SSSR / SSDR at 0x00..0x10; the LPSS private block at 0x200: CLOCK_PARAMS 0x00 (M/N divider, enable,
 * update), RESETS 0x04, SSP_REG 0x20, CS_CONTROL 0x24 (software chip select: SW_MODE 0, CS_HIGH 1,
 * CS select 9:8 on BXT / CNL), CLOCK_GATE 0x38, CAPS 0xFC (BXT / CNL: CS_EN 12:9).
 *
 * A device's SpiSerialBus resource names the controller ("\_SB.PC00.SPI1"); its AML device gives
 * the PCI function (_ADR), the PCI device id the input clock and type (tables generated from Linux
 * intel-lpss-pci.c, like i2c.c).  Motorola SPI, 8-bit frames, the device's clock phase / polarity;
 * SCLK = ssp_clk / (SCR + 1) <= the requested rate (ssp_get_clk_div).  Chip select: the controller's
 * native CS in software mode (lpss_ssp_cs_control) or a GPIO line (cs-gpios); a GPIO chip select
 * leaves the native one deasserted, as the Linux SPI core does for spi-pxa2xx.
 *
 * Bay Trail / Cherry Trail LPSS SPI in ACPI mode (private block at 0x400) and the DesignWare SSI of
 * the Elkhart Lake PSE are not handled.
 */
#include "codec.h"

#define SSCR0           0x00
#define SSCR1           0x04
#define SSSR            0x08
#define SSDR            0x10
#define SSTO            0x28
#define SSPSP           0x2C

#define SSCR0_DSS8      0x07            /* DataSize(8) */
#define SSCR0_SSE       0x80
#define SSCR1_SPO       0x08
#define SSCR1_SPH       0x10
#define SSCR1_TFT8      (7UL << 6)      /* SSCR1_TxTresh(8) */
#define SSCR1_RFT8      (7UL << 10)     /* SSCR1_RxTresh(8) */
#define SSSR_TNF        0x04
#define SSSR_RNE        0x08
#define SSSR_BSY        0x10
#define SSSR_ROR        0x80
#define SSSR_CLEAR      (SSSR_ROR | (1UL << 18) | (1UL << 19) | (1UL << 21))   /* ROR, PINT, TINT, TUR */

#define LPSS_PRIV       0x200
#define LPSS_CLOCK      0x00            /* bit 0 enable, M 15:1, N 30:16, bit 31 update */
#define LPSS_RESETS     0x04
#define LPSS_REMAP_LO   0x40
#define LPSS_REMAP_HI   0x44
#define LPSS_CS_CTRL    0x24
#define LPSS_CS_SW_MODE 0x01
#define LPSS_CS_HIGH    0x02
#define LPSS_CLOCK_GATE 0x38
#define LPSS_CAPS       0xFC

#define T_SPT 0
#define T_BXT 1
#define T_CNL 2

typedef struct _SPI_BUS {
    char    Name[5];
    PUCHAR  Regs;
    ULONG   Pa;
    ULONG   ClkHz;              /* SSP clock after the LPSS M/N divider */
    UCHAR   Type;               /* T_xxx */
    UCHAR   NumCs;
    BOOLEAN Up;
    ULONG   Dev, Fn;
    ULONG   Errors;
} SPI_BUS;

struct _SPI_DEV {
    SPI_BUS *Bus;
    UCHAR    Cs;                /* native chip select */
    UCHAR    Mode;              /* AML_SPI_xxx */
    BOOLEAN  UseGpio, GpioHigh; /* GPIO chip select, active level */
    GPIO_PIN CsGpio;
    ULONG    Hz;                /* requested */
    ULONG    Scr;               /* SSCR0.SCR */
    char     Name[12];
};

#define MAX_SPI_BUS 4
#define MAX_SPI_DEV 8
static SPI_BUS g_SpiBus[MAX_SPI_BUS];
static ULONG g_NSpiBus;
static SPI_DEV g_SpiDev[MAX_SPI_DEV];
static ULONG g_NSpiDev;

#define R32(b, o)     MMIO32((b)->Regs, (o))
#define W32(b, o, v)  (MMIO32((b)->Regs, (o)) = (ULONG)(v))
#define PR32(b, o)    MMIO32((b)->Regs, LPSS_PRIV + (o))
#define PW32(b, o, v) (MMIO32((b)->Regs, LPSS_PRIV + (o)) = (ULONG)(v))

/* LPSS SPI functions: input clock (MHz) and SSP type by PCI device id (Linux intel-lpss-pci.c:
   spt_spi_info 120 MHz SPT, bxt_spi_info 100 MHz BXT, cnl_spi_info 120 MHz CNL, tgl_spi_info 100 MHz CNL) */
static BOOLEAN LpssSpi(USHORT dev, ULONG *mhz, UCHAR *type)
{
    static const struct { USHORT Lo, Hi; UCHAR Mhz, Type; } t[] = {
        { 0x02AA, 0x02AB, 120, T_CNL }, { 0x02FB, 0x02FB, 120, T_CNL }, { 0x06AA, 0x06AB, 120, T_CNL },
        { 0x06FB, 0x06FB, 120, T_CNL }, { 0x0AC2, 0x0AC2, 100, T_BXT }, { 0x0AC4, 0x0AC4, 100, T_BXT },
        { 0x0AC6, 0x0AC6, 100, T_BXT }, { 0x1AC2, 0x1AC2, 100, T_BXT }, { 0x1AC4, 0x1AC4, 100, T_BXT },
        { 0x1AC6, 0x1AC6, 100, T_BXT }, { 0x31C2, 0x31C2, 100, T_BXT }, { 0x31C4, 0x31C4, 100, T_BXT },
        { 0x31C6, 0x31C6, 100, T_BXT }, { 0x34AA, 0x34AB, 120, T_CNL }, { 0x34FB, 0x34FB, 120, T_CNL },
        { 0x43AA, 0x43AB, 100, T_CNL }, { 0x43FB, 0x43FB, 100, T_CNL }, { 0x43FD, 0x43FD, 100, T_CNL },
        { 0x4B2A, 0x4B2B, 100, T_BXT }, { 0x4B37, 0x4B37, 100, T_BXT }, { 0x4D27, 0x4D27, 100, T_CNL },
        { 0x4D30, 0x4D30, 100, T_CNL }, { 0x4D46, 0x4D46, 100, T_CNL }, { 0x4DAA, 0x4DAB, 120, T_CNL },
        { 0x4DFB, 0x4DFB, 120, T_CNL }, { 0x51AA, 0x51AB, 100, T_CNL }, { 0x51FB, 0x51FB, 100, T_CNL },
        { 0x54AA, 0x54AB, 100, T_CNL }, { 0x54FB, 0x54FB, 100, T_CNL }, { 0x5AC2, 0x5AC2, 100, T_BXT },
        { 0x5AC4, 0x5AC4, 100, T_BXT }, { 0x5AC6, 0x5AC6, 100, T_BXT }, { 0x6E2A, 0x6E2B, 100, T_CNL },
        { 0x6E5E, 0x6E5E, 100, T_CNL }, { 0x7727, 0x7727, 100, T_CNL }, { 0x7730, 0x7730, 100, T_CNL },
        { 0x7746, 0x7746, 100, T_CNL }, { 0x7A2A, 0x7A2B, 100, T_CNL }, { 0x7A79, 0x7A79, 100, T_CNL },
        { 0x7A7B, 0x7A7B, 100, T_CNL }, { 0x7AAA, 0x7AAB, 100, T_CNL }, { 0x7AF9, 0x7AF9, 100, T_CNL },
        { 0x7AFB, 0x7AFB, 100, T_CNL }, { 0x7E27, 0x7E27, 100, T_CNL }, { 0x7E30, 0x7E30, 100, T_CNL },
        { 0x7E46, 0x7E46, 100, T_CNL }, { 0x7F2A, 0x7F2B, 100, T_CNL }, { 0x7F5E, 0x7F5F, 100, T_CNL },
        { 0x98AA, 0x98AA, 100, T_BXT }, { 0x9D29, 0x9D2A, 120, T_SPT }, { 0x9DAA, 0x9DAB, 120, T_CNL },
        { 0x9DFB, 0x9DFB, 120, T_CNL }, { 0xA0AA, 0xA0AB, 120, T_CNL }, { 0xA0DE, 0xA0DF, 120, T_CNL },
        { 0xA0FB, 0xA0FB, 120, T_CNL }, { 0xA0FD, 0xA0FE, 120, T_CNL }, { 0xA129, 0xA12A, 120, T_SPT },
        { 0xA2A9, 0xA2AA, 120, T_SPT }, { 0xA32A, 0xA32B, 120, T_CNL }, { 0xA37B, 0xA37B, 120, T_CNL },
        { 0xA3A9, 0xA3AA, 120, T_SPT }, { 0xA827, 0xA827, 100, T_CNL }, { 0xA830, 0xA830, 100, T_CNL },
        { 0xA846, 0xA846, 100, T_CNL }, { 0xD327, 0xD327, 100, T_CNL }, { 0xD330, 0xD330, 100, T_CNL },
        { 0xD347, 0xD347, 100, T_CNL }, { 0xE327, 0xE327, 100, T_CNL }, { 0xE330, 0xE330, 100, T_CNL },
        { 0xE346, 0xE346, 100, T_CNL }, { 0xE427, 0xE427, 100, T_CNL }, { 0xE430, 0xE430, 100, T_CNL },
        { 0xE446, 0xE446, 100, T_CNL },
    };
    ULONG i;
    for (i = 0; i < ARRAYSIZE(t); i++)
        if (dev >= t[i].Lo && dev <= t[i].Hi) { *mhz = t[i].Mhz; *type = t[i].Type; return TRUE; }
    return FALSE;
}

typedef struct { const char *Name; AML_DEV Dev; BOOLEAN Found; } FIND_CTX;
static BOOLEAN FindByName(const AML_DEV *d, void *p)
{
    FIND_CTX *f = (FIND_CTX *)p;
    ULONGLONG adr;
    if (memcmp(d->Name, f->Name, 4) || !AmlAdr(d, &adr)) return TRUE;
    f->Dev = *d;
    f->Found = TRUE;
    return FALSE;
}

static void Disable(SPI_BUS *b)
{
    W32(b, SSCR0, R32(b, SSCR0) & ~SSCR0_SSE);
}

static SPI_BUS *OpenBus(const char *ctrl)
{
    FIND_CTX f;
    SPI_BUS *b;
    ULONGLONG adr;
    ULONG i, bar, cmd, pm, mhz = 0, clk, m, n;
    USHORT vid, did;
    for (i = 0; i < g_NSpiBus; i++)
        if (!memcmp(g_SpiBus[i].Name, ctrl, 4)) return g_SpiBus[i].Up ? &g_SpiBus[i] : NULL;
    if (g_NSpiBus >= MAX_SPI_BUS) return NULL;
    b = &g_SpiBus[g_NSpiBus++];
    memset(b, 0, sizeof(*b));
    memcpy(b->Name, ctrl, 4);
    memset(&f, 0, sizeof(f));
    f.Name = ctrl;
    AmlForEachDevice(FindByName, &f);
    if (!f.Found || !AmlAdr(&f.Dev, &adr)) {
        LOGE("spi: controller %s is not a PCI function in the ACPI tables (ACPI-mode LPSS not supported)", ctrl);
        return NULL;
    }
    b->Dev = (ULONG)(adr >> 16) & 0x1F; b->Fn = (ULONG)adr & 7;
    vid = PciRead16(0, b->Dev, b->Fn, 0);
    did = PciRead16(0, b->Dev, b->Fn, 2);
    if (vid != 0x8086) { LOGE("spi: %s: no Intel function at 00:%02x.%u (%04x)", ctrl, b->Dev, b->Fn, vid); return NULL; }
    if (!LpssSpi(did, &mhz, &b->Type)) {
        /* an LPSS SPI the table does not know: the fastest clock keeps SCLK at or below the request */
        mhz = 120; b->Type = T_CNL;
        LOGI("spi: %s: PCI id %04x not in the LPSS table, assuming a 120 MHz CNL-type SSP", ctrl, did);
    }
    pm = PciFindCap(0, b->Dev, b->Fn, 1);
    if (pm && (PciRead16(0, b->Dev, b->Fn, pm + 4) & 3)) {
        PciWrite16(0, b->Dev, b->Fn, pm + 4, (USHORT)(PciRead16(0, b->Dev, b->Fn, pm + 4) & ~3));
        OsSleepMs(10);
    }
    cmd = PciRead16(0, b->Dev, b->Fn, 4);
    if (!(cmd & 2)) PciWrite16(0, b->Dev, b->Fn, 4, (USHORT)(cmd | 2));
    bar = PciRead32(0, b->Dev, b->Fn, 0x10);
    if ((bar & 1) || !(bar & ~0xFFFUL)) { LOGE("spi: %s: BAR %08x not assigned", ctrl, bar); return NULL; }
    if ((bar & 6) == 4 && PciRead32(0, b->Dev, b->Fn, 0x14)) { LOGE("spi: %s: BAR above 4 GB", ctrl); return NULL; }
    b->Pa = bar & ~0xFFFUL;
    b->Regs = (PUCHAR)OsMapMmio(b->Pa, 0x1000);
    if (!b->Regs) return NULL;
    if (R32(b, SSCR0) == 0xFFFFFFFF) {
        LOGE("spi: %s at %08x reads all ones", ctrl, b->Pa);
        OsUnmapMmio(b->Regs, 0x1000); b->Regs = NULL;
        return NULL;
    }
    /* intel_lpss_init_dev: reset, out of reset (function + iDMA), remap address */
    if ((PR32(b, LPSS_RESETS) & 7) != 7) {
        PW32(b, LPSS_RESETS, 0);
        OsStallUs(10);
        PW32(b, LPSS_RESETS, 7);
        OsStallUs(100);
    }
    PW32(b, LPSS_REMAP_LO, b->Pa);
    PW32(b, LPSS_REMAP_HI, 0);
    /* LPSS clock: fractional M/N divider after the root clock (intel_lpss_register_clock_divider);
       an unprogrammed divider is set to unity, then enable (bit 0) and update (bit 31) */
    clk = PR32(b, LPSS_CLOCK);
    m = (clk >> 1) & 0x7FFF; n = (clk >> 16) & 0x7FFF;
    if (!m || !n || m > n) { m = n = 1; }
    PW32(b, LPSS_CLOCK, (m << 1) | (n << 16) | 1);
    PW32(b, LPSS_CLOCK, (m << 1) | (n << 16) | 1 | 0x80000000UL);
    b->ClkHz = (ULONG)((ULONGLONG)mhz * 1000000 * m / n);
    /* pxa2xx_spi_probe: SSP off, Motorola 8 bit, thresholds; LPSS: software chip select, deasserted */
    Disable(b);
    W32(b, SSCR1, SSCR1_RFT8 | SSCR1_TFT8);
    W32(b, SSCR0, (2UL << 8) | SSCR0_DSS8);
    W32(b, SSTO, 0);
    W32(b, SSPSP, 0);
    PW32(b, LPSS_CS_CTRL, PR32(b, LPSS_CS_CTRL) | LPSS_CS_SW_MODE | LPSS_CS_HIGH);
    b->NumCs = 1;
    if (b->Type != T_SPT) {
        ULONG caps = (PR32(b, LPSS_CAPS) >> 9) & 0xF, k;
        for (k = 0; k < 4 && (caps & (1UL << k)); k++) {}
        b->NumCs = (UCHAR)(k ? k : 1);
    }
    b->Up = TRUE;
    LOGI("spi: %s is PCI 00:%02x.%u %04x at %08x, %u Hz SSP clock (M/N %u/%u), %u chip selects",
         ctrl, b->Dev, b->Fn, did, b->Pa, b->ClkHz, m, n, b->NumCs);
    return b;
}

static void NativeCs(SPI_DEV *d, BOOLEAN assert);

SPI_DEV *SpiOpen(const char *ctrl, ULONG cs, ULONG hz, ULONG mode, const GPIO_PIN *csGpio, BOOLEAN csGpioActiveHigh)
{
    SPI_BUS *b;
    SPI_DEV *d;
    if (!ctrl[0] || g_NSpiDev >= MAX_SPI_DEV) return NULL;
    if (!(b = OpenBus(ctrl))) return NULL;
    if (!csGpio && cs >= b->NumCs) {
        LOGE("spi: %s has %u chip selects, CS%u requested", ctrl, b->NumCs, cs);
        return NULL;
    }
    d = &g_SpiDev[g_NSpiDev++];
    memset(d, 0, sizeof(*d));
    d->Bus = b;
    d->Cs = (UCHAR)cs;
    d->Mode = (UCHAR)mode;
    d->Hz = hz ? hz : 1000000;
    /* ssp_get_clk_div: SCR = ceil(ssp_clk / rate) - 1, 12 bits */
    d->Scr = (b->ClkHz + d->Hz - 1) / d->Hz;
    d->Scr = d->Scr ? d->Scr - 1 : 0;
    if (d->Scr > 0xFFF) d->Scr = 0xFFF;
    if (csGpio && csGpio->Cfg) {
        d->UseGpio = TRUE;
        d->CsGpio = *csGpio;
        d->GpioHigh = csGpioActiveHigh;
        GpioSet(&d->CsGpio, !d->GpioHigh);                     /* deasserted */
    } else if (mode & AML_SPI_CS_HIGH) {
        NativeCs(d, FALSE);                                     /* idle level of an active-high select */
    }
    if (mode & AML_SPI_3WIRE) LOGE("spi: %s CS%u: 3-wire mode not supported", ctrl, cs);
    OsSprintf(d->Name, sizeof(d->Name), "%s.%u%s", b->Name, cs, d->UseGpio ? "g" : "");
    LOGI("spi: %s: SCLK %u Hz (asked %u), mode %u%s", d->Name, b->ClkHz / (d->Scr + 1), d->Hz,
         ((mode & AML_SPI_CPOL) ? 2 : 0) | ((mode & AML_SPI_CPHA) ? 1 : 0),
         d->UseGpio ? ", GPIO chip select" : "");
    return d;
}

const char *SpiName(SPI_DEV *d) { return d ? d->Name : "-"; }

/* lpss_ssp_cs_control: select the CS output first (BXT / CNL), wait two SSP clocks, then the level */
static void NativeCs(SPI_DEV *d, BOOLEAN assert)
{
    SPI_BUS *b = d->Bus;
    ULONG v = PR32(b, LPSS_CS_CTRL), nv;
    BOOLEAN high = (d->Mode & AML_SPI_CS_HIGH) ? assert : !assert;
    if (assert && b->Type != T_SPT && ((v >> 8) & 3) != d->Cs) {
        v = (v & ~0x300UL) | ((ULONG)d->Cs << 8);
        PW32(b, LPSS_CS_CTRL, v);
        OsStallUs(1);
    }
    nv = high ? (v | LPSS_CS_HIGH) : (v & ~LPSS_CS_HIGH);
    if (nv == v) return;
    PW32(b, LPSS_CS_CTRL, nv);
    if (b->Type != T_SPT) {
        /* cs_clk_stays_gated: force the clock on briefly so the CS pin follows */
        ULONG g = PR32(b, LPSS_CLOCK_GATE);
        if ((g & 3) != 3) {
            PW32(b, LPSS_CLOCK_GATE, (g & ~3UL) | 3);
            PW32(b, LPSS_CLOCK_GATE, g & ~3UL);
        }
    }
}

static void Cs(SPI_DEV *d, BOOLEAN assert)
{
    if (d->UseGpio) GpioSet(&d->CsGpio, d->GpioHigh ? assert : !assert);
    else NativeCs(d, assert);
}

BOOLEAN SpiXfer(SPI_DEV *d, const UCHAR *w, ULONG wn, UCHAR *r, ULONG rn)
{
    SPI_BUS *b;
    ULONG total = wn + rn, sent = 0, got = 0, t, cr1;
    BOOLEAN ok = TRUE;
    if (!d || !(b = d->Bus) || !b->Up || !total) return FALSE;
    /* pxa2xx_spi_transfer_one: flush, clear status, SSP off, mode and divider, SSP on */
    Disable(b);
    for (t = 0; t < 64 && (R32(b, SSSR) & SSSR_RNE); t++) (void)R32(b, SSDR);
    W32(b, SSSR, SSSR_CLEAR);
    cr1 = SSCR1_RFT8 | SSCR1_TFT8 | ((d->Mode & AML_SPI_CPHA) ? SSCR1_SPH : 0) | ((d->Mode & AML_SPI_CPOL) ? SSCR1_SPO : 0);
    W32(b, SSCR1, cr1);
    W32(b, SSCR0, (d->Scr << 8) | SSCR0_DSS8);
    W32(b, SSCR0, (d->Scr << 8) | SSCR0_DSS8 | SSCR0_SSE);
    Cs(d, TRUE);
    /* full duplex, 8 frames in flight at most (the FIFO is 16 deep on every LPSS SSP); bytes past wn
       clock out zeros and are the read data.  Time limit: 1 ms a byte at the slowest rate + 10 ms */
    for (t = 0; got < total; t++) {
        while (sent < total && sent - got < 8 && (R32(b, SSSR) & SSSR_TNF)) {
            W32(b, SSDR, sent < wn ? w[sent] : 0);
            sent++;
        }
        while (got < sent && (R32(b, SSSR) & SSSR_RNE)) {
            UCHAR v = (UCHAR)R32(b, SSDR);
            if (got >= wn && r) r[got - wn] = v;
            got++;
        }
        if (got >= total) break;
        if (t > 2000 + total * 200) { ok = FALSE; break; }
        OsStallUs(5);
    }
    for (t = 0; t < 2000 && (R32(b, SSSR) & SSSR_BSY); t++) OsStallUs(5);   /* cs_deassert: idle first */
    Cs(d, FALSE);
    if (R32(b, SSSR) & SSSR_ROR) ok = FALSE;
    Disable(b);
    if (!ok && b->Errors++ < 16)
        LOGE("spi: %s: transfer failed (%u/%u sent, %u done, SSSR %08x)", d->Name, sent, total, got, R32(b, SSSR));
    return ok;
}

void SpiCloseAll(void)
{
    ULONG i;
    for (i = 0; i < g_NSpiDev; i++)
        if (g_SpiDev[i].UseGpio) GpioSet(&g_SpiDev[i].CsGpio, !g_SpiDev[i].GpioHigh);
    for (i = 0; i < g_NSpiBus; i++) {
        SPI_BUS *b = &g_SpiBus[i];
        if (b->Regs) {
            Disable(b);
            OsUnmapMmio(b->Regs, 0x1000);
        }
    }
    memset(g_SpiBus, 0, sizeof(g_SpiBus));
    memset(g_SpiDev, 0, sizeof(g_SpiDev));
    g_NSpiBus = g_NSpiDev = 0;
}
