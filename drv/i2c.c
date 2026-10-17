/*
 * AUDIONT - I2C masters of the Intel LPSS (Synopsys DesignWare I2C), polled.
 *
 * The codec's I2cSerialBus resource names its controller ("\_SB.PCI0.I2C1");
 * the controller's AML device gives the PCI function (_ADR, PCH LPSS: Sunrise
 * Point and later, Apollo Lake, ...) or, on Bay Trail / Cherry Trail in ACPI
 * mode, its memory resource.  The LPSS private registers (at 0x200 of the PCI
 * BAR, 0x800 of the ACPI one) take the function out of reset.
 *
 * AMD FCH I2C (AMDI0010, AMDI0019, AMDI0510, AMD0010; also DesignWare, ACPI
 * memory resource) has no LPSS private registers; its clock is 150 MHz
 * (133 MHz for AMD0010), as Linux acpi_apd.c.
 *
 * Standard mode (100 kHz) with SCL counts from the controller's input clock;
 * unknown functions get the counts of the fastest LPSS clock (216 MHz), which
 * is slower than 100 kHz on the others and valid for every device.
 */
#include "codec.h"

/* DesignWare registers */
#define IC_CON              0x00
#define IC_TAR              0x04
#define IC_DATA_CMD         0x10
#define IC_SS_SCL_HCNT      0x14
#define IC_SS_SCL_LCNT      0x18
#define IC_INTR_MASK        0x30
#define IC_RAW_INTR_STAT    0x34
#define IC_RX_TL            0x38
#define IC_TX_TL            0x3C
#define IC_CLR_INTR         0x40
#define IC_CLR_TX_ABRT      0x54
#define IC_CLR_STOP_DET     0x60
#define IC_ENABLE           0x6C
#define IC_STATUS           0x70
#define IC_TXFLR            0x74
#define IC_RXFLR            0x78
#define IC_SDA_HOLD         0x7C
#define IC_TX_ABRT_SOURCE   0x80
#define IC_ENABLE_STATUS    0x9C
#define IC_COMP_PARAM_1     0xF4
#define IC_COMP_VERSION     0xF8
#define IC_COMP_TYPE        0xFC
#define DW_COMP_TYPE        0x44570140UL

#define CON_MASTER          0x01
#define CON_SPEED_STD       0x02
#define CON_RESTART_EN      0x20
#define CON_SLAVE_DISABLE   0x40
#define CMD_READ            0x100
#define CMD_STOP            0x200
#define CMD_RESTART         0x400
#define ST_TFNF             0x02
#define ST_TFE              0x04
#define ST_RFNE             0x08
#define ST_MST_ACTIVITY     0x20
#define INTR_TX_ABRT        0x40
#define INTR_STOP_DET       0x200

/* LPSS private registers */
#define LPSS_RESETS         0x04
#define LPSS_RESETS_FUNC    0x3
#define LPSS_RESETS_IDMA    0x4

struct _I2C_BUS {
    char    Name[5];
    PUCHAR  Regs;
    ULONG   Pa, Size;
    ULONG   ClkMhz;
    ULONG   Fifo;
    ULONG   Bus, Dev, Fn;       /* PCI function, Bus 0xFF: ACPI */
    USHORT  Tar;                /* address programmed */
    BOOLEAN Up;
    ULONG   Aborts;
};

#define MAX_BUS 8
static I2C_BUS g_Bus[MAX_BUS];
static ULONG g_NBus;

#define R32(b, o)     MMIO32((b)->Regs, (o))
#define W32(b, o, v)  (MMIO32((b)->Regs, (o)) = (ULONG)(v))

/* input clock of the LPSS I2C functions by PCI device id (Linux intel-lpss-pci) */
static ULONG LpssClk(USHORT dev)
{
    static const struct { USHORT Lo, Hi; UCHAR Mhz; } t[] = {
        { 0x02C5, 0x02C6, 216 }, { 0x02E8, 0x02EB, 216 }, { 0x06E8, 0x06EB, 216 }, { 0x0AAC, 0x0AAC, 133 },
        { 0x0AAE, 0x0AAE, 133 }, { 0x0AB0, 0x0AB0, 133 }, { 0x0AB2, 0x0AB2, 133 }, { 0x0AB4, 0x0AB4, 133 },
        { 0x0AB6, 0x0AB6, 133 }, { 0x0AB8, 0x0AB8, 133 }, { 0x0ABA, 0x0ABA, 133 }, { 0x1AAC, 0x1AAC, 133 },
        { 0x1AAE, 0x1AAE, 133 }, { 0x1AB0, 0x1AB0, 133 }, { 0x1AB2, 0x1AB2, 133 }, { 0x1AB4, 0x1AB4, 133 },
        { 0x1AB6, 0x1AB6, 133 }, { 0x1AB8, 0x1AB8, 133 }, { 0x1ABA, 0x1ABA, 133 }, { 0x31AC, 0x31AC, 133 },
        { 0x31AE, 0x31AE, 133 }, { 0x31B0, 0x31B0, 133 }, { 0x31B2, 0x31B2, 133 }, { 0x31B4, 0x31B4, 133 },
        { 0x31B6, 0x31B6, 133 }, { 0x31B8, 0x31B8, 133 }, { 0x31BA, 0x31BA, 133 }, { 0x34C5, 0x34C6, 133 },
        { 0x34E8, 0x34EB, 133 }, { 0x43AD, 0x43AE, 133 }, { 0x43D8, 0x43D8, 133 }, { 0x43E8, 0x43EB, 133 },
        { 0x4B44, 0x4B45, 100 }, { 0x4B4B, 0x4B4C, 100 }, { 0x4B78, 0x4B7B, 100 }, { 0x4D50, 0x4D51, 100 },
        { 0x4D78, 0x4D7B, 100 }, { 0x4DC5, 0x4DC6, 133 }, { 0x4DE8, 0x4DEB, 133 }, { 0x51C5, 0x51C6, 133 },
        { 0x51D8, 0x51D9, 133 }, { 0x51E8, 0x51EB, 133 }, { 0x54C5, 0x54C6, 133 }, { 0x54E8, 0x54EB, 133 },
        { 0x5AAC, 0x5AAC, 133 }, { 0x5AAE, 0x5AAE, 133 }, { 0x5AB0, 0x5AB0, 133 }, { 0x5AB2, 0x5AB2, 133 },
        { 0x5AB4, 0x5AB4, 133 }, { 0x5AB6, 0x5AB6, 133 }, { 0x5AB8, 0x5AB8, 133 }, { 0x5ABA, 0x5ABA, 133 },
        { 0x6E4C, 0x6E4F, 100 }, { 0x6E7A, 0x6E7B, 100 }, { 0x7750, 0x7751, 133 }, { 0x7778, 0x777B, 133 },
        { 0x7A4C, 0x7A4F, 133 }, { 0x7A7C, 0x7A7D, 133 }, { 0x7ACC, 0x7ACF, 133 }, { 0x7AFC, 0x7AFD, 133 },
        { 0x7E50, 0x7E51, 133 }, { 0x7E78, 0x7E7B, 133 }, { 0x7F4C, 0x7F4F, 133 }, { 0x7F7A, 0x7F7B, 133 },
        { 0x98C5, 0x98C6, 133 }, { 0x98E8, 0x98EB, 133 }, { 0x9D60, 0x9D65, 120 }, { 0x9DC5, 0x9DC6, 216 },
        { 0x9DE8, 0x9DEB, 216 }, { 0xA0C5, 0xA0C6, 120 }, { 0xA0D8, 0xA0D9, 120 }, { 0xA0E8, 0xA0EB, 120 },
        { 0xA160, 0xA162, 120 }, { 0xA2E0, 0xA2E3, 120 }, { 0xA368, 0xA36B, 216 }, { 0xA3E0, 0xA3E3, 120 },
        { 0xA850, 0xA851, 100 }, { 0xA878, 0xA87B, 100 }, { 0xD350, 0xD351, 100 }, { 0xD378, 0xD37B, 100 },
        { 0xE350, 0xE351, 100 }, { 0xE378, 0xE37B, 100 }, { 0xE450, 0xE451, 100 }, { 0xE478, 0xE47B, 100 },
    };
    ULONG i;
    for (i = 0; i < ARRAYSIZE(t); i++)
        if (dev >= t[i].Lo && dev <= t[i].Hi) return t[i].Mhz;
    return 0;
}

/* the controller's AML device */
typedef struct { const char *Name; AML_DEV Dev; BOOLEAN Found; } FIND_CTX;
static BOOLEAN FindByName(const AML_DEV *d, void *p)
{
    FIND_CTX *f = (FIND_CTX *)p;
    ULONGLONG adr;
    AML_RES r;
    if (memcmp(d->Name, f->Name, 4)) return TRUE;
    /* the same name may appear as a reference scope; take one that describes hardware */
    AmlResources(d, &r);
    if (!AmlAdr(d, &adr) && !r.NMem) return TRUE;
    f->Dev = *d;
    f->Found = TRUE;
    return FALSE;
}

static BOOLEAN Disable(I2C_BUS *b)
{
    ULONG i;
    W32(b, IC_ENABLE, 0);
    for (i = 0; i < 500; i++) {
        if (!(R32(b, IC_ENABLE_STATUS) & 1)) return TRUE;
        OsStallUs(25);
    }
    return FALSE;
}

static BOOLEAN Setup(I2C_BUS *b)
{
    ULONG clk = b->ClkMhz ? b->ClkMhz : 216, hcnt, lcnt, p1, hold;
    if (R32(b, IC_COMP_TYPE) != DW_COMP_TYPE) {
        LOGE("i2c: %s at %08x is not a DesignWare I2C (type %08x)", b->Name, b->Pa, R32(b, IC_COMP_TYPE));
        return FALSE;
    }
    if (!Disable(b)) LOGI("i2c: %s does not disable", b->Name);
    /* 100 kHz: tHIGH 4.0 us, tLOW 4.7 us + tf 0.3 us (Linux i2c_dw_scl_hcnt / _lcnt) */
    hcnt = clk * 4 - 8;
    lcnt = clk * 5 - 1;
    W32(b, IC_SS_SCL_HCNT, hcnt);
    W32(b, IC_SS_SCL_LCNT, lcnt);
    hold = R32(b, IC_SDA_HOLD);
    if (!(hold & 0xFFFF)) W32(b, IC_SDA_HOLD, (clk * 300 + 999) / 1000);   /* 300 ns */
    W32(b, IC_CON, CON_MASTER | CON_SPEED_STD | CON_RESTART_EN | CON_SLAVE_DISABLE);
    W32(b, IC_INTR_MASK, 0);
    W32(b, IC_RX_TL, 0);
    W32(b, IC_TX_TL, 0);
    p1 = R32(b, IC_COMP_PARAM_1);
    b->Fifo = ((p1 >> 16) & 0xFF) + 1;
    if (b->Fifo < 2 || b->Fifo > 256) b->Fifo = 16;
    b->Tar = 0xFFFF;
    (void)R32(b, IC_CLR_INTR);
    LOGI("i2c: %s at %08x (%u MHz%s), version %08x, FIFO %u, SCL %u/%u", b->Name, b->Pa, clk,
         b->ClkMhz ? "" : " assumed", R32(b, IC_COMP_VERSION), b->Fifo, hcnt, lcnt);
    return TRUE;
}

I2C_BUS *I2cOpen(const char *ctrl)
{
    FIND_CTX f;
    I2C_BUS *b;
    ULONG i, priv;
    ULONGLONG adr;
    for (i = 0; i < g_NBus; i++)
        if (!memcmp(g_Bus[i].Name, ctrl, 4)) return g_Bus[i].Up ? &g_Bus[i] : NULL;
    if (g_NBus >= MAX_BUS) return NULL;
    b = &g_Bus[g_NBus++];
    memset(b, 0, sizeof(*b));
    memcpy(b->Name, ctrl, 4);
    memset(&f, 0, sizeof(f));
    f.Name = ctrl;
    AmlForEachDevice(FindByName, &f);
    if (!f.Found) { LOGE("i2c: controller %s not in the ACPI tables", ctrl); return NULL; }
    if (AmlAdr(&f.Dev, &adr)) {
        ULONG bar, cmd, pm;
        USHORT vid, did;
        b->Bus = 0; b->Dev = (ULONG)(adr >> 16) & 0x1F; b->Fn = (ULONG)adr & 7;
        vid = PciRead16(0, b->Dev, b->Fn, 0);
        did = PciRead16(0, b->Dev, b->Fn, 2);
        if (vid != 0x8086) { LOGE("i2c: %s: no Intel function at 00:%02x.%u (%04x)", ctrl, b->Dev, b->Fn, vid); return NULL; }
        /* D0, memory decoding on */
        pm = PciFindCap(0, b->Dev, b->Fn, 1);
        if (pm && (PciRead16(0, b->Dev, b->Fn, pm + 4) & 3)) {
            PciWrite16(0, b->Dev, b->Fn, pm + 4, (USHORT)(PciRead16(0, b->Dev, b->Fn, pm + 4) & ~3));
            OsSleepMs(10);
        }
        cmd = PciRead16(0, b->Dev, b->Fn, 4);
        if (!(cmd & 2)) PciWrite16(0, b->Dev, b->Fn, 4, (USHORT)(cmd | 2));
        bar = PciRead32(0, b->Dev, b->Fn, 0x10);
        if ((bar & 1) || !(bar & ~0xFFFUL)) { LOGE("i2c: %s: BAR %08x not assigned", ctrl, bar); return NULL; }
        if ((bar & 6) == 4 && PciRead32(0, b->Dev, b->Fn, 0x14)) { LOGE("i2c: %s: BAR above 4 GB", ctrl); return NULL; }
        b->Pa = bar & ~0xFFFUL;
        b->Size = 0x1000;
        b->ClkMhz = LpssClk(did);
        priv = 0x200;
        LOGV("i2c: %s is PCI 00:%02x.%u %04x", ctrl, b->Dev, b->Fn, did);
    } else {
        AML_RES r;
        char hid[16];
        AmlResources(&f.Dev, &r);
        if (!r.NMem) { LOGE("i2c: %s has no memory resource", ctrl); return NULL; }
        AmlHid(&f.Dev, hid, sizeof(hid));
        b->Bus = 0xFF;
        b->Pa = r.MemBase[0];
        b->Size = 0x1000;
        b->ClkMhz = 100;                              /* Bay Trail / Cherry Trail LPSS */
        priv = 0x800;
        if (hid[0] == 'A' && hid[1] == 'M' && hid[2] == 'D') {
            b->ClkMhz = strcmp_s(hid, "AMD0010") ? 150 : 133;     /* acpi_apd.c wt_i2c / cz_i2c */
            priv = 0;                                 /* no LPSS private registers */
        }
    }
    b->Regs = (PUCHAR)OsMapMmio(b->Pa, b->Size);
    if (!b->Regs) return NULL;
    /* out of reset (function and its DMA) */
    if (priv && (MMIO32(b->Regs, priv + LPSS_RESETS) & (LPSS_RESETS_FUNC | LPSS_RESETS_IDMA)) !=
        (LPSS_RESETS_FUNC | LPSS_RESETS_IDMA)) {
        MMIO32(b->Regs, priv + LPSS_RESETS) = 0;
        OsStallUs(10);
        MMIO32(b->Regs, priv + LPSS_RESETS) = LPSS_RESETS_FUNC | LPSS_RESETS_IDMA;
        OsStallUs(100);
    }
    if (!Setup(b)) { OsUnmapMmio(b->Regs, b->Size); b->Regs = NULL; return NULL; }
    b->Up = TRUE;
    return b;
}

const char *I2cName(I2C_BUS *b) { return b ? b->Name : "-"; }

static BOOLEAN WaitIdle(I2C_BUS *b)
{
    ULONG i;
    for (i = 0; i < 2000; i++) {
        if (!(R32(b, IC_STATUS) & ST_MST_ACTIVITY)) return TRUE;
        OsStallUs(10);
    }
    return FALSE;
}

/* write w (wn bytes), then read rn bytes with a repeated start; one STOP at the end */
BOOLEAN I2cXfer(I2C_BUS *b, USHORT addr, const UCHAR *w, ULONG wn, UCHAR *r, ULONG rn)
{
    ULONG total = wn + rn, sent = 0, got = 0, t, raw, inflight = 0;
    if (!b || !b->Up || !total) return FALSE;
    if (!WaitIdle(b)) {
        LOGE("i2c: %s busy, resetting it", b->Name);
        Setup(b);
    }
    if (b->Tar != addr) {
        Disable(b);
        W32(b, IC_TAR, addr & 0x7F);
        b->Tar = addr;
    }
    (void)R32(b, IC_CLR_INTR);
    W32(b, IC_ENABLE, 1);
    /* 100 kHz: ~90 us a byte; allow 2 ms a byte plus 10 ms */
    for (t = 0; t < 10000 + total * 2000; t += 5) {
        raw = R32(b, IC_RAW_INTR_STAT);
        if (raw & INTR_TX_ABRT) break;
        while (sent < total && (R32(b, IC_STATUS) & ST_TFNF) && inflight < b->Fifo) {
            ULONG cmd;
            if (sent < wn) cmd = w[sent];
            else {
                cmd = CMD_READ;
                if (sent == wn && wn) cmd |= CMD_RESTART;
                inflight++;
            }
            if (sent == total - 1) cmd |= CMD_STOP;
            W32(b, IC_DATA_CMD, cmd);
            sent++;
        }
        while (got < rn && (R32(b, IC_STATUS) & ST_RFNE)) {
            r[got++] = (UCHAR)R32(b, IC_DATA_CMD);
            inflight--;
        }
        if (sent == total && got == rn && (raw & INTR_STOP_DET)) break;
        OsStallUs(5);
    }
    raw = R32(b, IC_RAW_INTR_STAT);
    if (raw & INTR_TX_ABRT) {
        ULONG src = R32(b, IC_TX_ABRT_SOURCE);
        (void)R32(b, IC_CLR_TX_ABRT);
        b->Aborts++;
        if (b->Aborts < 20 || g_DebugLevel >= 2)
            LOGV("i2c: %s addr %02x: abort %08x (%s)", b->Name, addr, src,
                 (src & 0x7) ? "no ACK" : (src & 0x1000) ? "arbitration lost" : "error");
        W32(b, IC_ENABLE, 0);
        return FALSE;
    }
    if (got != rn || sent != total) {
        LOGE("i2c: %s addr %02x: timeout (%u/%u sent, %u/%u read)", b->Name, addr, sent, total, got, rn);
        Setup(b);
        return FALSE;
    }
    (void)R32(b, IC_CLR_STOP_DET);
    return TRUE;
}

void I2cCloseAll(void)
{
    ULONG i;
    for (i = 0; i < g_NBus; i++) {
        I2C_BUS *b = &g_Bus[i];
        if (b->Regs) {
            Disable(b);
            OsUnmapMmio(b->Regs, b->Size);
        }
    }
    memset(g_Bus, 0, sizeof(g_Bus));
    g_NBus = 0;
}
