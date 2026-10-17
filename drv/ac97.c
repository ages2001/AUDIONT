/*
 * AUDIONT - AC'97 back-end for Intel ICH compatible controllers
 *
 * Covers Intel 82801AA..ICH7 / 440MX / ESB, nVidia nForce..MCP, AMD 768/8111,
 * SiS 7012 (byte-based PICB, swapped SR/PICB) and any controller announcing
 * class 04/01 with an ICH style register file.  VIA SGD controllers live in
 * via.c.  The AC'97 codec part (ac97_codec_*) is shared with via.c.
 */
#include "audiont.h"
#include "ac97.h"

/* bus master register offsets */
#define PI_BASE   0x00
#define PO_BASE   0x10
#define MC_BASE   0x20
#define X_BDBAR   0x00
#define X_CIV     0x04
#define X_LVI     0x05
#define X_SR      0x06
#define X_PICB    0x08
#define X_PIV     0x0A
#define X_CR      0x0B
#define GLOB_CNT  0x2C
#define GLOB_STA  0x30
#define CAS       0x34

#define SR_DCH    0x01
#define SR_CELV   0x02
#define SR_LVBCI  0x04
#define SR_BCIS   0x08
#define SR_FIFOE  0x10
#define CR_RPBM   0x01
#define CR_RR     0x02
#define CR_LVBIE  0x04
#define CR_FEIE   0x08
#define CR_IOCE   0x10

#define BDL_IOC   0x80000000UL

/* GLOB_STA */
#define GS_PCR    0x00000100UL     /* primary codec ready */
#define GS_SCR    0x00000200UL     /* secondary codec ready */
#define GS_TCR    0x10000000UL     /* tertiary codec ready (ICH4+) */
#define GS_RCS    0x00008000UL     /* codec read timed out */
#define GS_RWC    (GS_RCS | 0x000000E0UL)   /* RCS, MINT, POINT, PIINT */

#define AF_GENERIC 0x01             /* not in the table, ICH layout assumed */
#define AF_ICH4    0x02             /* ICH4+: six DMA engines, tertiary codec */

/* ------------------------------------------------------------------ */
/* register access (I/O or memory mapped)                              */
/* ------------------------------------------------------------------ */
static UCHAR  BmRd8(CARD *c, ULONG r)  { AC97_STATE *a = c->Ac97; return a->BmMmio ? MMIO8(a->BmMmio, r)  : inb((USHORT)(c->IoBase1 + r)); }
static USHORT BmRd16(CARD *c, ULONG r) { AC97_STATE *a = c->Ac97; return a->BmMmio ? MMIO16(a->BmMmio, r) : inw((USHORT)(c->IoBase1 + r)); }
static ULONG  BmRd32(CARD *c, ULONG r) { AC97_STATE *a = c->Ac97; return a->BmMmio ? MMIO32(a->BmMmio, r) : inl((USHORT)(c->IoBase1 + r)); }
static void   BmWr8(CARD *c, ULONG r, UCHAR v)   { AC97_STATE *a = c->Ac97; if (a->BmMmio) MMIO8(a->BmMmio, r) = v;  else outb((USHORT)(c->IoBase1 + r), v); }
static void   BmWr16(CARD *c, ULONG r, USHORT v) { AC97_STATE *a = c->Ac97; if (a->BmMmio) MMIO16(a->BmMmio, r) = v; else outw((USHORT)(c->IoBase1 + r), v); }
static void   BmWr32(CARD *c, ULONG r, ULONG v)  { AC97_STATE *a = c->Ac97; if (a->BmMmio) MMIO32(a->BmMmio, r) = v; else outl((USHORT)(c->IoBase1 + r), v); }

static void CodecWait(CARD *c)
{
    AC97_STATE *a = c->Ac97;
    ULONG i;
    for (i = 0; i < 100; i++) {            /* codec access semaphore, ~1 ms */
        if (!(BmRd8(c, CAS) & 1)) return;
        OsStallUs(10);
    }
    /* an access to a register the codec does not have may leave the semaphore set:
       a dummy read of register 0 releases it (as Linux does), then go on regardless */
    if (a->NamMmio) (void)MMIO16(a->NamMmio, 0); else (void)inw((USHORT)c->IoBase0);
}

static USHORT IchCodecRead(CARD *c, ULONG reg)
{
    AC97_STATE *a = c->Ac97;
    USHORT v;
    CodecWait(c);
    v = a->NamMmio ? MMIO16(a->NamMmio, reg) : inw((USHORT)(c->IoBase0 + reg));
    if (BmRd32(c, GLOB_STA) & 0x8000) {      /* RCS: read timed out */
        BmWr32(c, GLOB_STA, 0x8000);
        return 0xFFFF;
    }
    return v;
}

static void IchCodecWrite(CARD *c, ULONG reg, USHORT v)
{
    AC97_STATE *a = c->Ac97;
    CodecWait(c);
    if (a->NamMmio) MMIO16(a->NamMmio, reg) = v;
    else outw((USHORT)(c->IoBase0 + reg), v);
}

/* ------------------------------------------------------------------ */
/* controller identification                                           */
/* ------------------------------------------------------------------ */
typedef struct { USHORT Ven, Dev; HWTYPE Type; const char *Name; } AC97_ID;
static const AC97_ID g_Ac97Ids[] = {
    { 0x8086, 0x2415, HW_AC97_ICH, "Intel 82801AA (ICH) AC'97" },
    { 0x8086, 0x2425, HW_AC97_ICH, "Intel 82801AB (ICH0) AC'97" },
    { 0x8086, 0x2445, HW_AC97_ICH, "Intel 82801BA (ICH2) AC'97" },
    { 0x8086, 0x2485, HW_AC97_ICH, "Intel 82801CA (ICH3) AC'97" },
    { 0x8086, 0x24C5, HW_AC97_ICH, "Intel 82801DB (ICH4) AC'97" },
    { 0x8086, 0x24D5, HW_AC97_ICH, "Intel 82801EB (ICH5) AC'97" },
    { 0x8086, 0x25A6, HW_AC97_ICH, "Intel 6300ESB AC'97" },
    { 0x8086, 0x266E, HW_AC97_ICH, "Intel 82801FB (ICH6) AC'97" },
    { 0x8086, 0x27DE, HW_AC97_ICH, "Intel 82801GB (ICH7) AC'97" },
    { 0x8086, 0x2698, HW_AC97_ICH, "Intel ESB2 AC'97" },
    { 0x8086, 0x7195, HW_AC97_ICH, "Intel 440MX AC'97" },
    { 0x1039, 0x7012, HW_AC97_SIS, "SiS 7012 AC'97" },
    { 0x10DE, 0x01B1, HW_AC97_NV,  "nVidia nForce AC'97" },
    { 0x10DE, 0x006A, HW_AC97_NV,  "nVidia nForce2 AC'97" },
    { 0x10DE, 0x008A, HW_AC97_NV,  "nVidia CK8 AC'97" },
    { 0x10DE, 0x00DA, HW_AC97_NV,  "nVidia nForce3 AC'97" },
    { 0x10DE, 0x00EA, HW_AC97_NV,  "nVidia nForce3 250 AC'97" },
    { 0x10DE, 0x0059, HW_AC97_NV,  "nVidia CK804 AC'97" },
    { 0x10DE, 0x003A, HW_AC97_NV,  "nVidia MCP04 AC'97" },
    { 0x10DE, 0x026B, HW_AC97_NV,  "nVidia MCP51 AC'97" },
    { 0x1022, 0x746D, HW_AC97_ICH, "AMD 8111 AC'97" },
    { 0x1022, 0x7445, HW_AC97_ICH, "AMD 768 AC'97" },
    { 0x1106, 0x3058, HW_AC97_VIA, "VIA VT82C686 AC'97" },
    { 0x1106, 0x3059, HW_AC97_VIA, "VIA VT8233/8235/8237 AC'97" },
    { 0, 0, 0, NULL }
};

BOOLEAN Ac97Probe(CARD *c)
{
    ULONG i, cls = PciRead32(c->Bus, c->Dev, c->Fn, 8) >> 8;
    for (i = 0; g_Ac97Ids[i].Ven; i++) {
        if (g_Ac97Ids[i].Ven == c->VendorId && g_Ac97Ids[i].Dev == c->DeviceId) {
            c->Type = g_Ac97Ids[i].Type;
            if (c->VendorId == 0x8086 && c->DeviceId >= 0x24C5 && c->DeviceId != 0x7195) c->HwFlags |= AF_ICH4;
            OsSprintf(c->Desc, sizeof(c->Desc), "%s", g_Ac97Ids[i].Name);
            c->Ops = (c->Type == HW_AC97_VIA) ? &g_ViaOps : &g_Ac97Ops;
            return TRUE;
        }
    }
    /* unknown multimedia audio controller with two I/O BARs: maybe ICH compatible (checked
       again in IchInit before anything is written).  Vendors whose class 04/01 parts have
       their own, different register sets are left alone. */
    if (cls == 0x040100 && (c->Bar[0] & 1) && (c->Bar[1] & 1) && OsRegGetDword(L"GenericAc97", 1)) {
        static const USHORT other[] = { 0x1106, 0x10B9, 0x1412, 0x1102, 0x1274, 0x125D, 0x1073,
            0x13F6, 0x1023, 0x12EB, 0x1319, 0x5333, 0x1013, 0x4005, 0x10C8, 0x1005, 0x14F1,
            0x127A, 0x1489, 0x11D4, 0x8384, 0x1186, 0x1095, 0x8086, 0x1039, 0x10DE, 0 };
        for (i = 0; other[i]; i++) if (other[i] == c->VendorId) return FALSE;
        c->Type = HW_AC97_ICH;
        c->HwFlags |= AF_GENERIC;
        OsSprintf(c->Desc, sizeof(c->Desc), "AC'97 controller %04x:%04x", c->VendorId, c->DeviceId);
        c->Ops = &g_Ac97Ops;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* DMA                                                                 */
/* ------------------------------------------------------------------ */
static void ChanReset(CARD *c, ULONG base)
{
    ULONG i;
    BmWr8(c, base + X_CR, 0);
    for (i = 0; i < 100 && !(BmRd16(c, base + c->Ac97->RegSr) & SR_DCH); i++) OsStallUs(10);
    BmWr8(c, base + X_CR, CR_RR);
    for (i = 0; i < 100 && (BmRd8(c, base + X_CR) & CR_RR); i++) OsStallUs(10);
    BmWr16(c, base + c->Ac97->RegSr, SR_LVBCI | SR_BCIS | SR_FIFOE);
}

static void BuildBdl(CARD *c, PULONG bdl, ULONG ringPa)
{
    ULONG i, len = c->Ac97->PosShift ? PERIOD_BYTES / 2 : PERIOD_BYTES;
    for (i = 0; i < 32; i++) {
        bdl[i * 2] = ringPa + (i % RING_PERIODS) * PERIOD_BYTES;
        bdl[i * 2 + 1] = len | (c->UseInterrupt ? BDL_IOC : 0);
    }
}

static BOOLEAN ChanStart(CARD *c, ULONG base, ULONG bdlOff, ULONG ringPa)
{
    ChanReset(c, base);
    BuildBdl(c, (PULONG)((PUCHAR)c->Aux.Va + bdlOff), ringPa);
    MemoryBarrier();
    BmWr32(c, base + X_BDBAR, c->Aux.Pa + bdlOff);
    BmWr8(c, base + X_LVI, (UCHAR)((BmRd8(c, base + X_CIV) - 1) & 31));
    BmWr8(c, base + X_CR, (UCHAR)(CR_RPBM | (c->UseInterrupt ? (CR_IOCE | CR_FEIE) : 0)));
    return TRUE;
}

static ULONG ChanPos(CARD *c, ULONG base)
{
    AC97_STATE *a = c->Ac97;
    ULONG civ, picb, tries, pos;
    for (tries = 0; tries < 10; tries++) {
        civ = BmRd8(c, base + X_CIV) & 31;
        picb = BmRd16(c, base + a->RegPicb);
        if ((BmRd8(c, base + X_CIV) & 31) == civ) break;
    }
    picb <<= a->PosShift;                      /* to bytes */
    if (picb > PERIOD_BYTES) picb = PERIOD_BYTES;
    pos = (civ % RING_PERIODS) * PERIOD_BYTES + (PERIOD_BYTES - picb);
    return (pos / ENG_FRAME_BYTES) % RING_FRAMES;
}

static void ChanTick(CARD *c, ULONG base)
{
    UCHAR civ = BmRd8(c, base + X_CIV) & 31;
    BmWr8(c, base + X_LVI, (UCHAR)((civ - 1) & 31));
    if (BmRd16(c, base + c->Ac97->RegSr) & SR_DCH) {
        /* DMA halted (should not happen): restart */
        LOGV("ac97: channel %x halted, restarting", base);
        BmWr8(c, base + X_CR, (UCHAR)(BmRd8(c, base + X_CR) | CR_RPBM));
    }
}

static BOOLEAN IchStartPlay(CARD *c) { return ChanStart(c, PO_BASE, 0, c->PlayRing.Pa); }
static void    IchStopPlay(CARD *c)  { ChanReset(c, PO_BASE); }
static ULONG   IchPlayPos(CARD *c)   { return ChanPos(c, PO_BASE); }
static void    IchPlayTick(CARD *c)  { ChanTick(c, PO_BASE); }
static BOOLEAN IchStartRec(CARD *c)  { return ChanStart(c, PI_BASE, 0x100, c->RecRing.Pa); }
static void    IchStopRec(CARD *c)   { ChanReset(c, PI_BASE); }
static ULONG   IchRecPos(CARD *c)    { return ChanPos(c, PI_BASE); }
static void    IchRecTick(CARD *c)   { ChanTick(c, PI_BASE); }

static BOOLEAN IchIsr(CARD *c)
{
    AC97_STATE *a = c->Ac97;
    BOOLEAN ours = FALSE;
    USHORT sr;
    sr = BmRd16(c, PO_BASE + a->RegSr);
    if (sr & (SR_LVBCI | SR_BCIS | SR_FIFOE)) { BmWr16(c, PO_BASE + a->RegSr, sr & 0x1C); ours = TRUE; }
    sr = BmRd16(c, PI_BASE + a->RegSr);
    if (sr & (SR_LVBCI | SR_BCIS | SR_FIFOE)) { BmWr16(c, PI_BASE + a->RegSr, sr & 0x1C); ours = TRUE; }
    sr = BmRd16(c, MC_BASE + a->RegSr);
    if (sr & (SR_LVBCI | SR_BCIS | SR_FIFOE)) { BmWr16(c, MC_BASE + a->RegSr, sr & 0x1C); ours = TRUE; }
    return ours;
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */
static NTSTATUS IchInit(CARD *c)
{
    AC97_STATE *a;
    ULONG cnt, i, sta = 0, ready;

    a = c->Ac97 = OsAlloc(sizeof(AC97_STATE));
    if (!a) return STATUS_INSUFFICIENT_RESOURCES;
    a->CodecRead = IchCodecRead;
    a->CodecWrite = IchCodecWrite;
    a->RegSr = X_SR; a->RegPicb = X_PICB; a->PosShift = 1;
    if (c->Type == HW_AC97_SIS) { a->RegSr = X_PICB; a->RegPicb = X_SR; a->PosShift = 0; }

    /* ICH4 and later: legacy I/O BARs need IOSE */
    if (c->HwFlags & AF_ICH4)
        PciWrite8(c->Bus, c->Dev, c->Fn, 0x41, (UCHAR)(PciRead8(c->Bus, c->Dev, c->Fn, 0x41) | 1));

    if ((c->Bar[0] & 1) && (c->Bar[1] & 1) && (c->Bar[0] & ~3UL) && (c->Bar[1] & ~3UL)) {
        c->IoBase0 = (USHORT)(c->Bar[0] & ~3UL);
        c->IoBase1 = (USHORT)(c->Bar[1] & ~3UL);
    } else if (!(c->Bar[2] & 1) && (c->Bar[2] & ~15UL) && !(c->Bar[3] & 1) && (c->Bar[3] & ~15UL)) {
        a->NamMmio = OsMapMmio(c->Bar[2] & ~15UL, 512);
        a->BmMmio  = OsMapMmio(c->Bar[3] & ~15UL, 256);
        if (!a->NamMmio || !a->BmMmio) return STATUS_INSUFFICIENT_RESOURCES;
    } else {
        LOGE("ac97: no usable BARs (%08x %08x %08x %08x)", c->Bar[0], c->Bar[1], c->Bar[2], c->Bar[3]);
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    LOGV("ac97: nam=%x nabm=%x mmio=%p/%p", c->IoBase0, c->IoBase1, a->NamMmio, a->BmMmio);

    if (c->HwFlags & AF_GENERIC) {
        /* read-only plausibility check of the ICH register file before touching it */
        ULONG gs = BmRd32(c, GLOB_STA);
        if (gs == 0xFFFFFFFF || (BmRd8(c, PO_BASE + X_CIV) & 0xE0) || (BmRd8(c, PI_BASE + X_CIV) & 0xE0) ||
            (BmRd8(c, PO_BASE + X_CR) & 0xE0) || (BmRd8(c, PO_BASE + X_LVI) & 0xE0)) {
            LOGI("ac97: %04x:%04x does not look ICH compatible (GLOB_STA %08x), skipped",
                 c->VendorId, c->DeviceId, gs);
            return STATUS_DEVICE_DOES_NOT_EXIST;
        }
    }

    a->HwOk = TRUE;
    if (!OsAllocDma(&c->Aux, 4096, c->DmaUncached)) return STATUS_INSUFFICIENT_RESOURCES;

    /* stop all DMA engines (ICH4+: also 2nd mic, 2nd PCM in, S/PDIF; nForce: S/PDIF) */
    ChanReset(c, PO_BASE); ChanReset(c, PI_BASE); ChanReset(c, MC_BASE);
    if (c->HwFlags & AF_ICH4) { ChanReset(c, 0x40); ChanReset(c, 0x50); ChanReset(c, 0x60); }
    if (c->Type == HW_AC97_NV) ChanReset(c, 0x70);

    /* clear stale status (codec read timeout, channel interrupts) */
    sta = BmRd32(c, GLOB_STA);
    if (sta & GS_RWC) BmWr32(c, GLOB_STA, sta & GS_RWC);
    ready = GS_PCR | GS_SCR | ((c->HwFlags & AF_ICH4) ? GS_TCR : 0);

    /* AC-link cold reset (only if no codec is ready yet) */
    cnt = BmRd32(c, GLOB_CNT);
    cnt &= ~(0x08UL | 0x01UL);                    /* ACLINK on, GPI interrupt off */
    if (c->Type == HW_AC97_SIS) cnt &= ~0xC0UL;   /* SiS: 2-channel PCM */
    else cnt &= ~0x00700000UL;                    /* 2-channel PCM, 16-bit samples (ICH4 20-bit off) */
    if (!(sta & ready) || !(cnt & 2)) {
        BmWr32(c, GLOB_CNT, cnt & ~2UL);
        OsStallUs(20);
        BmWr32(c, GLOB_CNT, cnt | 2UL);
        for (i = 0; i < 1000; i++) {               /* up to 1 s for a codec to come up */
            sta = BmRd32(c, GLOB_STA);
            if (sta & ready) break;
            OsStallUs(1000);
        }
        LOGV("ac97: cold reset, GLOB_STA=%08x after %u ms", sta, i);
        if (!(sta & ready)) LOGI("ac97: no codec ready after reset (GLOB_STA %08x), trying anyway", sta);
    } else {
        BmWr32(c, GLOB_CNT, cnt | 2UL);
    }
    if (!(sta & GS_PCR) && (sta & ready)) LOGI("ac97: only a secondary codec is ready (GLOB_STA %08x)", sta);
    if (c->Type == HW_AC97_SIS)
        BmWr16(c, 0x4C, (USHORT)(BmRd16(c, 0x4C) | 1));   /* unmute secondary codec link */

    if (!NT_SUCCESS(Ac97CodecInit(c))) return STATUS_DEVICE_DOES_NOT_EXIST;
    c->CanRecord = TRUE;
    return STATUS_SUCCESS;
}

static void IchShutdown(CARD *c)
{
    if (!c->Ac97) return;
    if (c->Ac97->HwOk) { ChanReset(c, PO_BASE); ChanReset(c, PI_BASE); }
    c->Ac97->HwOk = FALSE;
    if (c->Ac97->NamMmio) OsUnmapMmio(c->Ac97->NamMmio, 512);
    if (c->Ac97->BmMmio) OsUnmapMmio(c->Ac97->BmMmio, 256);
    c->Ac97->NamMmio = c->Ac97->BmMmio = NULL;
}

const HWOPS g_Ac97Ops = {
    "ac97-ich", IchInit, IchShutdown,
    IchStartPlay, IchStopPlay, IchPlayPos, IchPlayTick,
    IchStartRec, IchStopRec, IchRecPos, IchRecTick,
    IchIsr, Ac97ApplyLine, Ac97ApplyRecSrc, Ac97Dump
};
