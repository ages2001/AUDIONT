/*
 * AUDIONT - the audio DSPs of Bay Trail, Cherry Trail / Braswell and
 * Broadwell (Intel "LPE" / Wildcat Point audio DSP) with Sound Open Firmware
 * (IPC3).  These have no HD Audio controller: the DSP moves the samples with
 * its own DMA engines from a ring in host memory described by a page table,
 * and drives the I2S codec on one of its SSP ports.
 *
 *   - found as a PCI function (BIOS "PCI mode": 8086:0F28, 22A8, 9CB6) or as
 *     an ACPI device (80860F28, 808622A8, INT3438); in ACPI mode the memory
 *     resources often come from firmware NVS fields, which acpi.c resolves
 *   - firmware: fw\sof-byt.ri, sof-cht.ri, sof-bdw.ri ("Reef" modules whose
 *     blocks are copied into IRAM / DRAM of the DSP BAR), then the DSP runs and
 *     sends FW_READY through the shim IPC registers and the mailbox
 *   - pipelines: sstipc3.c; codecs: sstcodec.c (I2C through the LPSS)
 *   - position: the DMA controller of the DSP (the channel whose source or
 *     destination address lies in our ring), else the stream position the
 *     firmware writes to the mailbox with each period
 *   - polled: the IPC registers are looked at from the engine timer
 */
#include "codec.h"

#define G32U(p) ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8) | ((ULONG)(p)[2] << 16) | ((ULONG)(p)[3] << 24))

/* Atom (Bay Trail, Cherry Trail) */
#define BYT_IRAM      0x0C0000
#define BYT_DRAM      0x100000
#define BYT_SHIM      0x140000
#define BYT_MBOX      0x144000
#define BYT_DMAC0     0x098000
#define BYT_DMAC1     0x09C000
#define BYT_DMAC2     0x094000
#define BYT_BAR       0x200000
/* Broadwell */
#define BDW_IRAM      0x0A0000
#define BDW_DRAM      0x000000
#define BDW_SHIM      0x0FB000
#define BDW_MBOX      0x09E000
#define BDW_DMAC0     0x0FE000
#define BDW_DMAC1     0x0FF000
#define BDW_BAR       0x100000

/* shim (offsets from the shim base) */
#define SHIM_CSR      0x00
#define SHIM_IMRX     0x28
#define SHIM_IMRD     0x30
#define SHIM_IPCX     0x38
#define SHIM_IPCD     0x40
#define SHIM_CLKCTL   0x78
#define SHIM_CSR2     0x80
#define SHIM_LTRC     0xE0
#define SHIM_HMDC     0xE8
/* Atom CSR */
#define BYT_CSR_RST        0x1
#define BYT_CSR_VECTOR_SEL 0x2
#define BYT_CSR_STALL      0x4
#define BYT_CSR_PWAITMODE  0x8
/* Broadwell CSR */
#define CSR_RST            0x2
#define CSR_SBCS1          0x8
#define CSR_DCS(x)         ((ULONG)(x) << 4)
#define CSR_DCS_MASK       (7UL << 4)
#define CSR_STALL          0x400
#define CSR_S1IOCS         (1UL << 23)
#define CSR_LPCS           (1UL << 31)
/* PCI configuration shadow of Broadwell */
#define PCI_PMCS      0x84
#define PCI_VDRTCTL0  0xA0
#define PCI_VDRTCTL2  0xA8

/* DesignWare DMA channels: SAR / DAR, 0x58 bytes apart, 8 channels */
#define DW_CH_SIZE    0x58
#define DW_CHEN       0x3A0

static const SST_PLAT g_LpePlats[] = {
    { 0x0F28, SST_BYT, SST_FW_SOF3, "fw\\sof-byt.ri", "Bay Trail", 1, 1, 1, 3 },
    { 0x0F28, SST_BYT, SST_FW_SOF3, "fw\\sof-byt.ri", "Bay Trail-CR", 1, 1, 1, 1 },   /* codec on SSP0 */
    { 0x22A8, SST_CHT, SST_FW_SOF3, "fw\\sof-cht.ri", "Cherry Trail", 1, 1, 1, 6 },
    { 0x9CB6, SST_BDW, SST_FW_SOF3, "fw\\sof-bdw.ri", "Broadwell", 1, 1, 1, 2 },
};

typedef struct _LPE {
    SST     *S;
    ULONG   Shim, Mbox, Iram, Dram, BarSize;
    ULONG   Dmac[3];
    PUCHAR  Pci;                /* Broadwell: PCI configuration shadow (BAR1) */
    ULONG   PciPa;
    DMA_BUFFER PlayPt, RecPt;   /* page tables */
    ULONGLONG PlayPosn, RecPosn;/* bytes from the stream position messages */
    LONG    PlayCh, RecCh;      /* DMA channel found carrying the stream, -1: not yet */
    ULONG   Notes;
} LPE;

#define DSPR(l, o)      MMIO32((l)->S->Dsp, (o))
#define DSPW(l, o, v)   (MMIO32((l)->S->Dsp, (o)) = (ULONG)(v))
#define SHR(l, o)       DSPR(l, (l)->Shim + (o))
#define SHW(l, o, v)    DSPW(l, (l)->Shim + (o), v)
#define SHU(l, o, m, v) SHW(l, o, (SHR(l, o) & ~(ULONG)(m)) | (ULONG)(v))
#define PCIR(l, o)      MMIO32((l)->Pci, (o))
#define PCIU(l, o, m, v) (MMIO32((l)->Pci, (o)) = (MMIO32((l)->Pci, (o)) & ~(ULONG)(m)) | (ULONG)(v))

static LPE *Lp(CARD *c) { return c->Sst ? (LPE *)((SST *)c->Sst)->LpeCtx : NULL; }

/* ------------------------------------------------------------------ */
/* probe                                                                */
/* ------------------------------------------------------------------ */
static const SST_PLAT *PlatFor(USHORT dev, BOOLEAN bytCr)
{
    ULONG i;
    for (i = 0; i < ARRAYSIZE(g_LpePlats); i++)
        if (g_LpePlats[i].Dev == dev && (dev != 0x0F28 || (g_LpePlats[i].Ssp == 1) == bytCr)) return &g_LpePlats[i];
    return NULL;
}

extern const HWOPS g_LpeOps;

/* PCI mode: the function shows up with its BARs */
BOOLEAN LpeProbe(CARD *c)
{
    const SST_PLAT *p;
    if (c->VendorId != 0x8086 || !(p = PlatFor(c->DeviceId, FALSE))) return FALSE;
    if (c->DeviceId == 0x9C36) { LOGI("lpe: Haswell audio DSP: no redistributable firmware, not supported"); return FALSE; }
    if (!OsRegGetDword(L"Sst", 1)) return FALSE;
    c->Type = HW_LPE;
    c->Ops = &g_LpeOps;
    OsSprintf(c->Desc, sizeof(c->Desc), "Intel %s audio DSP", p->Name);
    return TRUE;
}

/* ACPI mode: devices without a PCI function; *mem gets the DSP (and PCI shadow) bases */
typedef struct { ULONG N; struct { USHORT Dev; ULONG Mem[2], Len[2]; ULONG Irqs; char Path[40]; } D[4]; } ACPI_LPE;

static BOOLEAN ScanLpe(const AML_DEV *d, void *ctx)
{
    static const struct { const char *Hid; USHORT Dev; } ids[] = {
        { "80860F28", 0x0F28 }, { "808622A8", 0x22A8 }, { "INT3438", 0x9CB6 }, { "INT33C8", 0x9C36 },
    };
    ACPI_LPE *a = (ACPI_LPE *)ctx;
    ULONG i, k;
    for (i = 0; i < ARRAYSIZE(ids); i++) {
        AML_RES r;
        if (!AmlHasId(d, ids[i].Hid)) continue;
        if (a->N >= 4) return FALSE;
        AmlResources(d, &r);
        for (k = 0; k < a->N; k++) if (a->D[k].Dev == ids[i].Dev) return TRUE;   /* DSDT + SSDT copies */
        a->D[a->N].Dev = ids[i].Dev;
        for (k = 0; k < 2; k++) {
            a->D[a->N].Mem[k] = k < r.NMem ? r.MemBase[k] : 0;
            a->D[a->N].Len[k] = k < r.NMem ? r.MemLen[k] : 0;
            if (k < r.NMem && !a->D[a->N].Mem[k]) a->D[a->N].Mem[k] = AmlCrsMemBase(d, k);
        }
        a->D[a->N].Irqs = r.NIrq;
        memcpy(a->D[a->N].Path, d->Path, sizeof(a->D[a->N].Path));
        a->N++;
        return TRUE;
    }
    return TRUE;
}

/* cards for the ACPI-mode DSPs (NT 3.x / 4.0 scan; called after the PCI scan) */
void LpeScanAcpi(void)
{
    ACPI_LPE a;
    ULONG i, k;
    if (!OsRegGetDword(L"Sst", 1)) return;
    memset(&a, 0, sizeof(a));
    AmlForEachDevice(ScanLpe, &a);
    for (i = 0; i < a.N; i++) {
        BOOLEAN dup = FALSE;
        for (k = 0; k < MAX_CARDS; k++)
            if (g_Cards[k] && g_Cards[k]->Type == HW_LPE && g_Cards[k]->DeviceId == a.D[i].Dev) dup = TRUE;
        if (dup) continue;
        if (a.D[i].Dev == 0x9C36) { LOGI("lpe: Haswell audio DSP (%s): no redistributable firmware", a.D[i].Path); continue; }
        if (!a.D[i].Mem[0]) { LOGE("lpe: %s: the DSP memory resource is not known", a.D[i].Path); continue; }
        LOGI("lpe: ACPI %s: %04x at %08x (%x), %08x, %u interrupts", a.D[i].Path, a.D[i].Dev, a.D[i].Mem[0],
             a.D[i].Len[0], a.D[i].Mem[1], a.D[i].Irqs);
        /* Bay Trail-CR has one interrupt resource less than Bay Trail-T (Linux soc_intel_is_byt_cr) */
        CardProbeAcpi(a.D[i].Dev, a.D[i].Mem[0], a.D[i].Mem[1], a.D[i].Dev == 0x0F28 && a.D[i].Irqs < 6);
    }
}

/* ------------------------------------------------------------------ */
/* DSP                                                                  */
/* ------------------------------------------------------------------ */
static BOOLEAN BdwD0(LPE *l)
{
    ULONG i;
    if (!l->Pci) return TRUE;
    PCIU(l, PCI_VDRTCTL2, (1UL << 1) | (1UL << 10), 0);                 /* core / trunk clock gating off */
    PCIU(l, PCI_VDRTCTL0, 1UL, 1UL);                                     /* D3 power gating off */
    PCIU(l, PCI_PMCS, 3, 0);                                             /* D0 */
    for (i = 0; i < 10 && (PCIR(l, PCI_PMCS) & 3); i++) OsSleepMs(20);
    if (PCIR(l, PCI_PMCS) & 3) { LOGE("lpe: Broadwell DSP stays in D%u", PCIR(l, PCI_PMCS) & 3); return FALSE; }
    SHU(l, SHIM_CSR, CSR_S1IOCS | CSR_SBCS1 | CSR_LPCS, 0);              /* SSP1 19.2 MHz, no low power clock */
    SHU(l, SHIM_CSR, CSR_STALL | CSR_DCS_MASK, CSR_STALL | CSR_DCS(4));  /* stalled, 192 / 96 MHz */
    SHU(l, SHIM_CLKCTL, (3UL << 24) | (1UL << 18) | (1UL << 16), (3UL << 24) | (1UL << 18) | (1UL << 16));
    return TRUE;
}

static void Reset(LPE *l)
{
    SST *s = l->S;
    if (s->P->Gen == SST_BDW) {
        SHU(l, SHIM_CSR, CSR_RST | CSR_STALL, CSR_RST | CSR_STALL);
        OsStallUs(10000);
        SHU(l, SHIM_CSR, CSR_RST | CSR_STALL, CSR_STALL);
        if (l->Pci) {
            PCIU(l, PCI_VDRTCTL2, (1UL << 1) | (1UL << 10), (1UL << 1) | (1UL << 10));
            OsStallUs(50);
            PCIU(l, PCI_VDRTCTL2, 1UL << 31, 0);                         /* audio PLL on */
            PCIU(l, PCI_VDRTCTL0, 0xFFFFFFFCUL, 0);                      /* SRAM power gating off */
        }
        SHU(l, SHIM_CSR2, 1UL << 2, 1UL << 2);                           /* no DMA finish for SSP1 */
        SHU(l, SHIM_HMDC, 0xFFUL | (0xFFUL << 7), 0xFFUL | (0xFFUL << 7));
        SHW(l, SHIM_IMRX, 3);                                            /* polled */
        SHW(l, SHIM_IPCX, 0);
        SHW(l, SHIM_IPCD, 0);
        SHW(l, SHIM_CSR2, 0x6);
        SHW(l, SHIM_LTRC, 0x300A);
        return;
    }
    SHW(l, SHIM_IMRX, 3);
    SHW(l, SHIM_IMRX + 4, 0);
    SHU(l, SHIM_CSR, BYT_CSR_RST | BYT_CSR_VECTOR_SEL | BYT_CSR_STALL, BYT_CSR_RST | BYT_CSR_VECTOR_SEL | BYT_CSR_STALL);
    OsStallUs(15);
    SHU(l, SHIM_CSR, BYT_CSR_RST, 0);                                    /* out of reset, still stalled */
}

static BOOLEAN Run(LPE *l)
{
    ULONG i;
    if (l->S->P->Gen == SST_BDW) {
        SHU(l, SHIM_HMDC, 0xFFUL | (0xFFUL << 7), 0);
        SHU(l, SHIM_CSR, CSR_STALL, 0);
        return TRUE;
    }
    SHU(l, SHIM_CSR, BYT_CSR_STALL, 0);
    for (i = 0; i < 10; i++) {
        if (!(SHR(l, SHIM_CSR) & BYT_CSR_PWAITMODE)) return TRUE;
        OsSleepMs(100);
    }
    return FALSE;
}

/* "Reef" image: modules of blocks with absolute offsets in the DSP BAR (IRAM 1, DRAM 2, SRAM 3) */
static BOOLEAN Load(LPE *l)
{
    SST *s = l->S;
    const UCHAR *img, *p, *end;
    ULONG off = 0, nmod, m, total = 0;
    s->Img = OsLoadFile(s->P->File, &s->ImgLen);
    if (!s->Img) {
        LOGE("lpe: firmware %s not found (copy it to %%SystemRoot%%\\System32\\drivers\\audiont\\%s)", s->P->File,
             s->P->File);
        return FALSE;
    }
    img = s->Img;
    end = img + s->ImgLen;
    if (s->ImgLen > 16 && G32U(img) == 0x6E614D58UL) off = G32U(img + 4);   /* "XMan" extended manifest */
    if (off + 16 > s->ImgLen || memcmp(img + off, "Reef", 4)) { LOGE("lpe: %s is not a SOF image", s->P->File); return FALSE; }
    nmod = G32U(img + off + 8);
    p = img + off + 16;
    for (m = 0; m < nmod && p + 12 <= end; m++) {
        ULONG size = G32U(p + 4), nb = G32U(p + 8), b;
        const UCHAR *q = p + 12, *mend = p + 12 + size;
        if (mend > end) { LOGE("lpe: %s: module %u truncated", s->P->File, m); return FALSE; }
        for (b = 0; b < nb && q + 12 <= mend; b++) {
            ULONG type = G32U(q), bs = G32U(q + 4), bo = G32U(q + 8), i;
            const UCHAR *src = q + 12;
            if (src + bs > mend) { LOGE("lpe: %s: block %u truncated", s->P->File, b); return FALSE; }
            if ((type >= 1 && type <= 3) && bs && !(bs & 3) && bo + bs <= l->BarSize) {
                for (i = 0; i < bs; i += 4) DSPW(l, bo + i, G32U(src + i));
                total += bs;
            } else if (bs) LOGV("lpe: block type %u size %x at %x skipped", type, bs, bo);
            q = src + bs;
        }
        p = mend;
    }
    LOGI("lpe: %s, %u bytes, %u modules, %u bytes to the DSP", s->P->File, s->ImgLen, nmod, total);
    return total != 0;
}

/* page table of a physically contiguous ring: 20-bit page numbers, two in five bytes */
static BOOLEAN PageTable(DMA_BUFFER *pt, const DMA_BUFFER *ring, SST_PATH *p)
{
    ULONG pages = (ring->Size + 4095) / 4096, i;
    UCHAR *t;
    if (!pt->Va && !OsAllocDma(pt, 4096, FALSE)) return FALSE;
    t = (UCHAR *)pt->Va;
    memset(t, 0, 4096);
    for (i = 0; i < pages; i++) {
        ULONG pfn = (ring->Pa >> 12) + i, idx = (i * 5) >> 1;
        if (i & 1) {
            t[idx] = (UCHAR)((t[idx] & 0x0F) | ((pfn << 4) & 0xF0));
            t[idx + 1] = (UCHAR)(pfn >> 4); t[idx + 2] = (UCHAR)(pfn >> 12);
        } else {
            t[idx] = (UCHAR)pfn; t[idx + 1] = (UCHAR)(pfn >> 8);
            t[idx + 2] = (UCHAR)((t[idx + 2] & 0xF0) | ((pfn >> 16) & 0x0F));
        }
    }
    p->HostBufPa = pt->Pa;
    p->HostBufPages = pages;
    p->HostBufSize = ring->Size;
    return TRUE;
}

/* messages from the DSP: stream positions (the posn struct is in the mailbox), others logged */
static void Poll(CARD *c)
{
    LPE *l = Lp(c);
    SST *s;
    ULONG pri, ex, n;
    if (!l) return;
    s = l->S;
    for (n = 0; n < 4 && SstRxPending(s, &pri, &ex); n++) {
        UCHAR h[8];
        SstMemFromDsp(s, s->UpOff, h, 8);
        if ((G32U(h + 4) & 0xFFFF0000UL) == 0x60080000UL || (G32U(h + 4) & 0xFFFF0000UL) == 0x60090000UL) {
            /* SOF_IPC_STREAM_POSITION / TRIG_XRUN */
            l->Notes++;
        } else if ((G32U(h + 4) & 0xF0000000UL) == 0x70000000UL && !s->Ready) {
            Ipc3Notify(s);
        } else LOGT("lpe: message %08x", G32U(h + 4));
        SstRxDone(s);
    }
}

static ULONGLONG Posn(LPE *l, SST_PATH *p)
{
    UCHAR b[8];
    if (!p->Running || !l->S->StreamOff) return 0;
    SstMemFromDsp(l->S, l->S->StreamOff + p->PosnOff + 28, b, 8);     /* sof_ipc_stream_posn.host_posn */
    return (ULONGLONG)G32U(b) | ((ULONGLONG)G32U(b + 4) << 32);
}

/* the DMA channel whose host-side address lies in the ring */
static LONG FindCh(LPE *l, ULONG pa, ULONG size, BOOLEAN rec)
{
    ULONG d, ch;
    for (d = 0; d < 3; d++) {
        if (!l->Dmac[d]) continue;
        for (ch = 0; ch < 8; ch++) {
            ULONG a = DSPR(l, l->Dmac[d] + ch * DW_CH_SIZE + (rec ? 0x08 : 0x00));
            if (a >= pa && a < pa + size) return (LONG)(d * 8 + ch);
        }
    }
    return -1;
}

static ULONG RingPos(LPE *l, SST_PATH *p, LONG *ch, const DMA_BUFFER *ring, BOOLEAN rec)
{
    ULONG a;
    if (*ch < 0) *ch = FindCh(l, ring->Pa, ring->Size, rec);
    if (*ch >= 0) {
        a = DSPR(l, l->Dmac[*ch / 8] + (*ch % 8) * DW_CH_SIZE + (rec ? 0x08 : 0x00));
        if (a >= ring->Pa && a < ring->Pa + ring->Size) return (a - ring->Pa) / ENG_FRAME_BYTES;
        *ch = -1;
    }
    return (ULONG)((Posn(l, p) % ring->Size) / ENG_FRAME_BYTES);
}

/* ------------------------------------------------------------------ */
/* HWOPS                                                                */
/* ------------------------------------------------------------------ */
extern BOOLEAN SstCodecsProbe(SST *s);
extern void    SstCodecsPower(SST *s, SST_EP *e, BOOLEAN on);
extern void    SstCodecsShutdown(SST *s);
extern ULONG   SstCodecsDump(SST *s, char *buf, ULONG size);
extern SST_EP *SstCodecsPlayEp(SST *s);

static void LpeShutdown(CARD *c);

static NTSTATUS LpeInit(CARD *c)
{
    SST *s;
    LPE *l;
    const SST_PLAT *p = PlatFor(c->DeviceId, (c->HwFlags & 1) != 0);
    ULONG bar0 = c->Bar[0] & ~0xFUL, bar1 = c->Bar[1] & ~0xFUL, i;
    if (!p) return STATUS_NO_SUCH_DEVICE;
    if (!bar0 || (c->Bar[0] & 1)) { LOGE("lpe: DSP BAR %08x not usable", c->Bar[0]); return STATUS_NO_SUCH_DEVICE; }
    s = (SST *)OsAlloc(sizeof(SST));
    l = (LPE *)OsAlloc(sizeof(LPE));
    if (!s || !l) { OsFree(s); OsFree(l); return STATUS_INSUFFICIENT_RESOURCES; }
    s->Card = c; s->P = p; s->Lpe = TRUE;
    s->LpeCtx = l;
    l->S = s;
    l->PlayCh = l->RecCh = -1;
    if (p->Gen == SST_BDW) {
        l->Shim = BDW_SHIM; l->Mbox = BDW_MBOX; l->Iram = BDW_IRAM; l->Dram = BDW_DRAM; l->BarSize = BDW_BAR;
        l->Dmac[0] = BDW_DMAC0; l->Dmac[1] = BDW_DMAC1;
    } else {
        l->Shim = BYT_SHIM; l->Mbox = BYT_MBOX; l->Iram = BYT_IRAM; l->Dram = BYT_DRAM; l->BarSize = BYT_BAR;
        l->Dmac[0] = BYT_DMAC0; l->Dmac[1] = BYT_DMAC1;
        if (p->Gen == SST_CHT) l->Dmac[2] = BYT_DMAC2;
    }
    s->DspPa = bar0; s->DspSize = l->BarSize;
    s->Dsp = (PUCHAR)OsMapMmio(bar0, l->BarSize);
    c->Sst = s;
    if (!s->Dsp) { LOGE("lpe: cannot map the DSP BAR %08x", bar0); LpeShutdown(c); return STATUS_NO_SUCH_DEVICE; }
    if (p->Gen == SST_BDW && bar1 && !(c->Bar[1] & 1)) {
        l->PciPa = bar1;
        l->Pci = (PUCHAR)OsMapMmio(bar1, 0x1000);
    }
    /* IPC: IPCX / IPCD; on Atom 64 bit wide with BUSY / DONE in the upper half */
    if (p->Gen == SST_BDW) {
        s->ReqOff = l->Shim + SHIM_IPCX; s->ReqExt = l->Shim + SHIM_IPCX;       /* 0, then BUSY */
        s->AckOff = l->Shim + SHIM_IPCX; s->RspOff = l->Shim + SHIM_IPCD; s->RspExt = l->Shim + SHIM_IPCD;
    } else {
        s->ReqOff = l->Shim + SHIM_IPCX + 4; s->ReqExt = l->Shim + SHIM_IPCX;
        s->AckOff = l->Shim + SHIM_IPCX + 4; s->RspOff = l->Shim + SHIM_IPCD + 4; s->RspExt = l->Shim + SHIM_IPCD;
    }
    s->ReqBusy = 1UL << 31; s->AckDone = 1UL << 30; s->RspBusy = 1UL << 31; s->RspDone = 1UL << 30;
    s->SramBase = l->Mbox; s->SramWin = 0;            /* SOF windows are offsets from the mailbox */
    s->UpOff = l->Mbox; s->UpSize = 0x400;
    s->DownOff = l->Mbox + 0x400; s->DownSize = 0x400;
    s->StatusOff = l->Mbox;
    LOGI("lpe: %s audio DSP at %08x%s", p->Name, bar0, l->Pci ? " (PCI shadow mapped)" : "");
    if (!BdwD0(l)) { LpeShutdown(c); return STATUS_NO_SUCH_DEVICE; }
    /* codecs first: their ports decide the SSP formats (sstcodec.c adds the endpoints for SOF IPC3) */
    SstCodecsProbe(s);
    Reset(l);
    if (!Load(l)) { LpeShutdown(c); return STATUS_NO_SUCH_DEVICE; }
    if (!Run(l)) { LOGE("lpe: the DSP does not leave wait mode"); LpeShutdown(c); return STATUS_NO_SUCH_DEVICE; }
    if (!Ipc3WaitReady(s, 2000)) {
        LOGE("lpe: no FW_READY (IPCX %08x IPCD %08x CSR %08x)", SHR(l, SHIM_IPCX + 4), SHR(l, SHIM_IPCD + 4),
             SHR(l, SHIM_CSR));
        LpeShutdown(c);
        return STATUS_NO_SUCH_DEVICE;
    }
    /* endpoints: the codec's port */
    if (!(s->PlayEp = SstCodecsPlayEp(s)))
        for (i = 0; i < s->NEp; i++) if (s->Ep[i].Dir == 0) { s->PlayEp = &s->Ep[i]; break; }
    for (i = 0; i < s->NEp; i++)
        if (s->Ep[i].Dir == 1 && s->Ep[i].Line < LINE_COUNT && !s->RecEp[s->Ep[i].Line]) s->RecEp[s->Ep[i].Line] = &s->Ep[i];
    if (!s->PlayEp) { LOGE("lpe: no codec found: nothing to play to"); LpeShutdown(c); return STATUS_NO_SUCH_DEVICE; }
    if (c->PlayRing.Pa + RING_BYTES > 0x80000000UL) LOGE("lpe: ring above 2 GB: the DSP DMA cannot reach it");
    if (!Ipc3PathCreate(s, &s->Play, s->PlayEp, FALSE)) { LpeShutdown(c); return STATUS_NO_SUCH_DEVICE; }
    c->Lines[LINE_MASTER].Present = TRUE;
    c->Lines[LINE_WAVE].Present = TRUE;
    for (i = 0; i < LINE_COUNT; i++) {
        if (!s->RecEp[i]) continue;
        c->RecSrcMask |= 1UL << i;
        c->Lines[i].Present = TRUE;
        c->CanRecord = c->RecRing.Va != NULL;
        if (!c->RecSrc) c->RecSrc = 1UL << i;
    }
    OsSprintf(c->Codec, sizeof(c->Codec), "%s", s->PlayEp->Codec ? s->PlayEp->Codec->Drv->Name : "I2S codec");
    LOGI("lpe: ready, playback to %s on SSP%u", s->PlayEp->Name, s->PlayEp->Port);
    return STATUS_SUCCESS;
}

static void LpeShutdown(CARD *c)
{
    SST *s = (SST *)c->Sst;
    LPE *l = Lp(c);
    if (!s) return;
    if (s->Ready) {
        if (s->Play.Created) Ipc3PathDelete(s, &s->Play);
        if (s->Rec.Created) Ipc3PathDelete(s, &s->Rec);
    }
    SstCodecsShutdown(s);
    if (l) {
        if (s->Dsp) {
            if (s->P->Gen == SST_BDW) SHU(l, SHIM_CSR, CSR_RST | CSR_STALL, CSR_RST | CSR_STALL);
            else SHU(l, SHIM_CSR, BYT_CSR_RST | BYT_CSR_VECTOR_SEL | BYT_CSR_STALL,
                     BYT_CSR_RST | BYT_CSR_VECTOR_SEL | BYT_CSR_STALL);
        }
        if (l->PlayPt.Va) OsFreeDma(&l->PlayPt);
        if (l->RecPt.Va) OsFreeDma(&l->RecPt);
        if (l->Pci) OsUnmapMmio(l->Pci, 0x1000);
        OsFree(l);
    }
    if (s->Dsp) OsUnmapMmio(s->Dsp, s->DspSize);
    if (s->Img) OsFree(s->Img);
    OsFree(s);
    c->Sst = NULL;
}

static SST_EP *RecEp(CARD *c)
{
    SST *s = (SST *)c->Sst;
    ULONG k;
    for (k = 0; k < LINE_COUNT; k++) if ((c->RecSrc & (1UL << k)) && s->RecEp[k]) return s->RecEp[k];
    return NULL;
}

static BOOLEAN LpeStartPlay(CARD *c)
{
    SST *s = (SST *)c->Sst;
    LPE *l = Lp(c);
    if (!PageTable(&l->PlayPt, &c->PlayRing, &s->Play)) return FALSE;
    l->PlayCh = -1;
    SstCodecsPower(s, s->PlayEp, TRUE);
    return Ipc3PathRun(s, &s->Play, TRUE, FALSE);
}

static void LpeStopPlay(CARD *c)
{
    SST *s = (SST *)c->Sst;
    Ipc3PathRun(s, &s->Play, FALSE, FALSE);
    SstCodecsPower(s, s->PlayEp, FALSE);
}

static ULONG LpePlayPos(CARD *c)
{
    SST *s = (SST *)c->Sst;
    LPE *l = Lp(c);
    Poll(c);
    return RingPos(l, &s->Play, &l->PlayCh, &c->PlayRing, FALSE);
}

static BOOLEAN LpeStartRec(CARD *c)
{
    SST *s = (SST *)c->Sst;
    LPE *l = Lp(c);
    SST_EP *e = RecEp(c);
    if (!e || !c->RecRing.Va) return FALSE;
    if (!s->Rec.Created || s->Rec.Ep != e) {
        if (s->Rec.Created) Ipc3PathDelete(s, &s->Rec);
        if (!Ipc3PathCreate(s, &s->Rec, e, TRUE)) return FALSE;
    }
    if (!PageTable(&l->RecPt, &c->RecRing, &s->Rec)) return FALSE;
    l->RecCh = -1;
    SstCodecsPower(s, e, TRUE);
    return Ipc3PathRun(s, &s->Rec, TRUE, TRUE);
}

static void LpeStopRec(CARD *c)
{
    SST *s = (SST *)c->Sst;
    if (!s->Rec.Created) return;
    Ipc3PathRun(s, &s->Rec, FALSE, TRUE);
    SstCodecsPower(s, s->Rec.Ep, FALSE);
}

static ULONG LpeRecPos(CARD *c)
{
    SST *s = (SST *)c->Sst;
    LPE *l = Lp(c);
    Poll(c);
    return RingPos(l, &s->Rec, &l->RecCh, &c->RecRing, TRUE);
}

static void LpeTick(CARD *c) { Poll(c); }

static void LpeApplyRecSrc(CARD *c) { (void)c; }       /* the capture path follows RecSrc at StartRec */

static ULONG LpeDump(CARD *c, char *buf, ULONG size)
{
    SST *s = (SST *)c->Sst;
    LPE *l = Lp(c);
    ULONG n = 0, i;
    if (!s || !l) return 0;
    n += OsSprintf(buf + n, size - n, "\r\nAudio DSP: %s, SOF %s, %s at %08x\r\n", s->P->Name, s->FwVer, s->P->File,
                   s->DspPa);
    n += OsSprintf(buf + n, size - n, "CSR %08x IPCX %08x:%08x IPCD %08x:%08x, %u position messages, DMA ch %d/%d\r\n",
                   SHR(l, SHIM_CSR), SHR(l, SHIM_IPCX + 4), SHR(l, SHIM_IPCX), SHR(l, SHIM_IPCD + 4), SHR(l, SHIM_IPCD),
                   l->Notes, l->PlayCh, l->RecCh);
    for (i = 0; i < s->NEp && n + 100 < size; i++)
        n += OsSprintf(buf + n, size - n, "  SSP%u %s: %s%s\r\n", s->Ep[i].Port, s->Ep[i].Dir ? "capture" : "playback",
                       s->Ep[i].Name, &s->Ep[i] == s->PlayEp ? " [playback]" : "");
    n += SstCodecsDump(s, buf + n, size - n);
    return n;
}

const HWOPS g_LpeOps = {
    "Intel LPE audio DSP", LpeInit, LpeShutdown,
    LpeStartPlay, LpeStopPlay, LpePlayPos, LpeTick,
    LpeStartRec, LpeStopRec, LpeRecPos, NULL,
    NULL, NULL, LpeApplyRecSrc, LpeDump, NULL
};
