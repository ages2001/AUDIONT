/*
 * AUDIONT - VIA AC'97 back-end (scatter/gather DMA "SGD" engine)
 *
 *   VT82C686A/B (1106:3058)            : playback SGD 0x00, capture 0x10
 *   VT8233/8233A/8235/8237 (1106:3059) : multi-channel playback 0x40, capture 0x60
 */
#include "audiont.h"
#include "ac97.h"

#define VIA_686   1
#define VIA_8233  2

#define REG_STATUS   0x00
#define REG_CONTROL  0x01
#define REG_TYPE     0x02
#define REG_TABLE    0x04
#define REG_STOPIDX  0x08
#define REG_COUNT    0x0C
#define REG_AC97     0x80
#define REG_GPI_INTR 0x8C

#define ST_FLAG      0x01
#define ST_EOL       0x02
#define ST_STOPPED   0x04
#define CTL_INT_FLAG 0x01
#define CTL_INT_EOL  0x02
#define CTL_PAUSE    0x08
#define CTL_AUTOSTART 0x20
#define CTL_TERMINATE 0x40
#define CTL_START    0x80
#define TYPE686_AUTOSTART 0x80
#define TYPE686_16BIT     0x20
#define TYPE686_STEREO    0x10
#define TYPE686_INT_EOL   0x02
#define TYPE686_INT_FLAG  0x01
#define TBL_EOL      0x80000000UL
#define TBL_FLAG     0x40000000UL

#define AC97_BUSY    (1UL << 24)
#define AC97_READ    (1UL << 23)
#define AC97_VALID   (1UL << 25)

#define IOB(c) ((USHORT)(c)->IoBase0)

static BOOLEAN ViaCodecReady(CARD *c)
{
    ULONG i;
    for (i = 0; i < 2000; i++) {
        if (!(inl(IOB(c) + REG_AC97) & AC97_BUSY)) return TRUE;
        OsStallUs(1);
    }
    return FALSE;
}

static USHORT ViaCodecRead(CARD *c, ULONG reg)
{
    ULONG i, v, again;
    for (again = 0; again < 4; again++) {
        ViaCodecReady(c);
        outl(IOB(c) + REG_AC97, AC97_VALID | AC97_READ | ((reg & 0x7F) << 16));
        OsStallUs(20);
        for (i = 0; i < 1000; i++) {
            v = inl(IOB(c) + REG_AC97);
            if ((v & AC97_VALID) && !(v & AC97_BUSY)) {
                OsStallUs(25);
                return (USHORT)inl(IOB(c) + REG_AC97);
            }
            OsStallUs(1);
        }
    }
    return 0xFFFF;
}

static void ViaCodecWrite(CARD *c, ULONG reg, USHORT v)
{
    ViaCodecReady(c);
    outl(IOB(c) + REG_AC97, (reg << 16) | v);
    ViaCodecReady(c);
}

static void ChanReset(CARD *c, UCHAR base)
{
    outb(IOB(c) + base + REG_CONTROL, CTL_PAUSE | CTL_TERMINATE | 0x01);
    (void)inb(IOB(c) + base + REG_CONTROL);
    OsStallUs(50);
    outb(IOB(c) + base + REG_CONTROL, 0);
    outb(IOB(c) + base + REG_STATUS, ST_FLAG | ST_EOL | ST_STOPPED);
    if (c->Ac97->ViaType == VIA_686) outb(IOB(c) + base + REG_TYPE, 0);
}

static void BuildTable(CARD *c, PULONG t, ULONG ringPa)
{
    ULONG i;
    for (i = 0; i < RING_PERIODS; i++) {
        t[i * 2] = ringPa + i * PERIOD_BYTES;
        t[i * 2 + 1] = PERIOD_BYTES | (c->UseInterrupt ? TBL_FLAG : 0) |
                       (i == RING_PERIODS - 1 ? TBL_EOL : 0);
    }
}

static BOOLEAN ChanStart(CARD *c, UCHAR base, ULONG tblOff, ULONG ringPa, BOOLEAN capture)
{
    AC97_STATE *a = c->Ac97;
    ChanReset(c, base);
    BuildTable(c, (PULONG)((PUCHAR)c->Aux.Va + tblOff), ringPa);
    MemoryBarrier();
    outl(IOB(c) + base + REG_TABLE, c->Aux.Pa + tblOff);
    if (a->ViaType == VIA_686) {
        outb(IOB(c) + base + REG_TYPE, TYPE686_AUTOSTART | TYPE686_16BIT | TYPE686_STEREO |
             (c->UseInterrupt ? (TYPE686_INT_EOL | TYPE686_INT_FLAG) : 0));
        outb(IOB(c) + base + REG_CONTROL, CTL_START);
    } else {
        if (capture) {
            outb(IOB(c) + 0x62, 0x40);                    /* capture FIFO enable */
            outb(IOB(c) + 0x63, 0x00);                    /* line/codec 0 */
            outl(IOB(c) + base + REG_STOPIDX, 0xFF000000UL | 0x00200000UL | 0x00100000UL);
        } else {
            outb(IOB(c) + 0x42, 0x80 | (2 << 4));          /* 16-bit, 2 channels */
            /* slot map: samples 1/2 -> slots 3/4 (front L/R), written on every revision
               as Linux does (its 8233A exception is disabled); stop index 0xFF never hit */
            outl(IOB(c) + base + REG_STOPIDX, 0xFF000000UL | (1 << 0) | (2 << 4));
        }
        outb(IOB(c) + base + REG_CONTROL, CTL_START | CTL_AUTOSTART |
             (c->UseInterrupt ? (CTL_INT_FLAG | CTL_INT_EOL) : 0));
    }
    return TRUE;
}

static ULONG ChanPos(CARD *c, UCHAR base, ULONG tblOff)
{
    AC97_STATE *a = c->Ac97;
    ULONG idx = 0, count = 0, pos, tries, i2, c2;
    /* the table pointer / index and the count are not read at the same instant, and the
       count reads 0 at a buffer change: read until two reads agree (Linux does the same) */
    for (tries = 0; tries < 10; tries++) {
        if (a->ViaType == VIA_686) {
            ULONG ptr = inl(IOB(c) + base + REG_TABLE), tbl = c->Aux.Pa + tblOff;
            count = inl(IOB(c) + base + REG_COUNT) & 0xFFFFFF;
            idx = (ptr <= tbl) ? 0 : (((ptr - tbl) / 8) - 1) % RING_PERIODS;
            ptr = inl(IOB(c) + base + REG_TABLE);
            i2 = (ptr <= tbl) ? 0 : (((ptr - tbl) / 8) - 1) % RING_PERIODS;
            c2 = count;
        } else {
            count = inl(IOB(c) + base + REG_COUNT);
            c2 = inl(IOB(c) + base + REG_COUNT);
            idx = (count >> 24) % RING_PERIODS;
            i2 = (c2 >> 24) % RING_PERIODS;
            count &= 0xFFFFFF;
            c2 &= 0xFFFFFF;
        }
        if (count && idx == i2 && (a->ViaType == VIA_686 || count - c2 < PERIOD_BYTES / 4)) break;
    }
    if (count > PERIOD_BYTES) count = PERIOD_BYTES;
    pos = idx * PERIOD_BYTES + PERIOD_BYTES - count;
    return (pos / ENG_FRAME_BYTES) % RING_FRAMES;
}

static UCHAR PlayBase(CARD *c) { return c->Ac97->ViaPlayBase; }
static UCHAR RecBase(CARD *c)  { return c->Ac97->ViaRecBase; }

static BOOLEAN ViaStartPlay(CARD *c) { Ac97Refresh(c); return ChanStart(c, PlayBase(c), 0, c->PlayRing.Pa, FALSE); }
static void    ViaStopPlay(CARD *c)  { ChanReset(c, PlayBase(c)); }
static ULONG   ViaPlayPos(CARD *c)   { return ChanPos(c, PlayBase(c), 0); }
static BOOLEAN ViaStartRec(CARD *c)  { return ChanStart(c, RecBase(c), 0x100, c->RecRing.Pa, TRUE); }
static void    ViaStopRec(CARD *c)   { ChanReset(c, RecBase(c)); }
static ULONG   ViaRecPos(CARD *c)    { return ChanPos(c, RecBase(c), 0x100); }

static BOOLEAN ViaIsr(CARD *c)
{
    BOOLEAN ours = FALSE;
    UCHAR bases[2], i, st;
    bases[0] = PlayBase(c); bases[1] = RecBase(c);
    for (i = 0; i < 2; i++) {
        st = inb(IOB(c) + bases[i] + REG_STATUS);
        if (st & (ST_FLAG | ST_EOL | ST_STOPPED)) {
            outb(IOB(c) + bases[i] + REG_STATUS, st & (ST_FLAG | ST_EOL | ST_STOPPED));
            ours = TRUE;
        }
    }
    return ours;
}

static NTSTATUS ViaInit(CARD *c)
{
    AC97_STATE *a;
    UCHAR v;
    ULONG i;
    a = c->Ac97 = OsAlloc(sizeof(AC97_STATE));
    if (!a) return STATUS_INSUFFICIENT_RESOURCES;
    a->CodecRead = ViaCodecRead;
    a->CodecWrite = ViaCodecWrite;
    a->ViaType = (c->DeviceId == 0x3058) ? VIA_686 : VIA_8233;
    a->ViaPlayBase = a->ViaType == VIA_686 ? 0x00 : 0x40;
    a->ViaRecBase  = a->ViaType == VIA_686 ? 0x10 : 0x60;
    if (!(c->Bar[0] & 1)) return STATUS_DEVICE_DOES_NOT_EXIST;
    c->IoBase0 = (USHORT)(c->Bar[0] & ~3UL);
    if (!c->IoBase0) return STATUS_DEVICE_DOES_NOT_EXIST;
    a->HwOk = TRUE;
    if (!OsAllocDma(&c->Aux, 4096, c->DmaUncached)) return STATUS_INSUFFICIENT_RESOURCES;

    ChanReset(c, a->ViaPlayBase);
    ChanReset(c, a->ViaRecBase);

    v = PciRead8(c->Bus, c->Dev, c->Fn, 0x40);
    if (!(v & 0x01)) {
        /* codec not ready: full AC-link reset as Linux does */
        PciWrite8(c->Bus, c->Dev, c->Fn, 0x41, 0xE0);  /* enable | reset deasserted | sync */
        OsStallUs(100);
        PciWrite8(c->Bus, c->Dev, c->Fn, 0x41, 0x00);  /* link off, reset asserted */
        OsStallUs(100);
        PciWrite8(c->Bus, c->Dev, c->Fn, 0x41, 0xCC);  /* enable, no reset, VRA, PCM */
        OsStallUs(100);
    }
    if ((PciRead8(c->Bus, c->Dev, c->Fn, 0x41) & 0xCC) != 0xCC)
        PciWrite8(c->Bus, c->Dev, c->Fn, 0x41, 0xCC);
    for (i = 0; i < 1000; i++) {
        if (PciRead8(c->Bus, c->Dev, c->Fn, 0x40) & 0x01) break;
        OsStallUs(1000);
    }
    if (a->ViaType == VIA_686) PciWrite8(c->Bus, c->Dev, c->Fn, 0x48, 0);   /* FM NMI off */
    outl(IOB(c) + REG_GPI_INTR, 0);
    LOGV("via: io=%x type=%u rev %02x link=%02x", c->IoBase0, a->ViaType, c->Revision,
         PciRead8(c->Bus, c->Dev, c->Fn, 0x40));
    if (!NT_SUCCESS(Ac97CodecInit(c))) return STATUS_DEVICE_DOES_NOT_EXIST;
    c->CanRecord = TRUE;
    return STATUS_SUCCESS;
}

static void ViaShutdown(CARD *c)
{
    if (!c->Ac97 || !c->Ac97->HwOk) return;
    ChanReset(c, PlayBase(c));
    ChanReset(c, RecBase(c));
}

const HWOPS g_ViaOps = {
    "ac97-via", ViaInit, ViaShutdown,
    ViaStartPlay, ViaStopPlay, ViaPlayPos, NULL,
    ViaStartRec, ViaStopRec, ViaRecPos, NULL,
    ViaIsr, Ac97ApplyLine, Ac97ApplyRecSrc, Ac97Dump
};
