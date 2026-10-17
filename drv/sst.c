/*
 * AUDIONT - Intel Smart Sound Technology: platforms, DSP boot, IPC transport,
 * firmware manifest, endpoint discovery and the glue to the HD Audio back-end.
 *
 * Boot follows the Linux avs and SOF drivers:
 *   cAVS 1.5 (SKL/KBL)   core 0 powered and out of reset, code loader DMA
 *                        (CLDMA, a 128 KB ring refilled through SPIB) feeds
 *                        the ROM until it reports "base firmware entered"
 *   cAVS 1.5+..2.5       the ROM gets ROM_CONTROL / purge with the DMA id of
 *                        an HD Audio host output stream that carries the image
 *                        (APL..TGL; ICL+ with a running capture stream and the
 *                        95 us LTR guard band, "ICCMAX")
 *   ACE (MTL..NVL)       DSP subsystem and the HP power domain first, then the
 *                        primary core through DSP2CxCTL, otherwise as cAVS
 * after which the firmware announces itself with FW_READY.
 */
#include "sdw.h"

#define G32U(p) ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8) | ((ULONG)(p)[2] << 16) | ((ULONG)(p)[3] << 24))
void Ipc3Notify(SST *s);          /* sstipc3.c */

/* ------------------------------------------------------------------ */
/* platforms                                                           */
/* ------------------------------------------------------------------ */
#define AVS(d, g, dir, n, cores, init)   { d, g, SST_FW_AVS,  "fw\\avs-" dir ".bin", n, cores, init, init, 6 }
#define SOF3(d, g, f, n, cores, ssp)     { d, g, SST_FW_SOF3, "fw\\" f, n, cores, 1, 1, ssp }
#define SOF4(d, g, f, n, cores, ssp)     { d, g, SST_FW_SOF4, "fw\\i4-" f, n, cores, 1, 1, ssp }

static const SST_PLAT g_Plats[] = {
    AVS(0x9D70, SST_SKL, "skl", "Skylake-LP", 2, 1),
    AVS(0xA170, SST_SKL, "skl", "Skylake", 2, 1),
    AVS(0x9D71, SST_SKL, "skl", "Kaby Lake-LP", 2, 1),
    AVS(0xA171, SST_SKL, "skl", "Kaby Lake", 2, 1),
    AVS(0xA2F0, SST_SKL, "skl", "Kaby Lake-H", 2, 1),
    AVS(0xA3F0, SST_SKL, "skl", "Comet Lake-S", 2, 1),
    AVS(0x5A98, SST_APL, "apl", "Apollo Lake", 2, 3),
    AVS(0x3198, SST_APL, "apl", "Gemini Lake", 2, 3),
    AVS(0x9DC8, SST_CNL, "cnl", "Cannon Lake-LP", 4, 1),
    AVS(0xA348, SST_CNL, "cnl", "Cannon Lake-H", 4, 1),
    AVS(0x02C8, SST_CNL, "cnl", "Comet Lake-LP", 4, 1),
    AVS(0x06C8, SST_CNL, "cnl", "Comet Lake-H", 4, 1),
    AVS(0xF1C8, SST_CNL, "cnl", "Rocket Lake-S", 4, 1),
    SOF3(0x34C8, SST_ICL, "sof-icl.ri", "Ice Lake-LP", 4, 6),
    SOF3(0x3DC8, SST_ICL, "sof-icl.ri", "Ice Lake-H", 4, 6),
    SOF3(0x38C8, SST_ICL, "sof-jsl.ri", "Ice Lake-N", 2, 6),
    SOF3(0x4DC8, SST_ICL, "sof-jsl.ri", "Jasper Lake", 2, 6),
    SOF3(0x4B55, SST_TGL, "sof-ehl.ri", "Elkhart Lake", 4, 6),
    SOF3(0x4B58, SST_TGL, "sof-ehl.ri", "Elkhart Lake", 4, 6),
    SOF4(0xA0C8, SST_TGL, "tgl.ri", "Tiger Lake-LP", 4, 3),
    SOF4(0xF0C8, SST_TGL, "tgl.ri", "Comet Lake-R", 4, 3),
    SOF4(0x43C8, SST_TGL, "tglh.ri", "Tiger Lake-H", 2, 3),
    SOF4(0x7AD0, SST_TGL, "adls.ri", "Alder Lake-S", 2, 3),
    SOF4(0x51C8, SST_TGL, "adl.ri", "Alder Lake-P", 4, 3),
    SOF4(0x51C9, SST_TGL, "adl.ri", "Alder Lake-PS", 4, 3),
    SOF4(0x51CC, SST_TGL, "adl.ri", "Alder Lake-M", 4, 3),
    SOF4(0x51CD, SST_TGL, "adl.ri", "Alder Lake-PX", 4, 3),
    SOF4(0x54C8, SST_TGL, "adln.ri", "Alder Lake-N", 2, 3),
    SOF4(0x7A50, SST_TGL, "adls.ri", "Raptor Lake-S", 2, 3),
    SOF4(0x51CA, SST_TGL, "adl.ri", "Raptor Lake-P", 4, 3),
    SOF4(0x51CB, SST_TGL, "adl.ri", "Raptor Lake-P", 4, 3),
    SOF4(0x51CE, SST_TGL, "adl.ri", "Raptor Lake-M", 4, 3),
    SOF4(0x51CF, SST_TGL, "adl.ri", "Raptor Lake-PX", 4, 3),
    SOF4(0x7E28, SST_MTL, "mtl.ri", "Meteor Lake", 3, 3),
    SOF4(0x7728, SST_MTL, "mtl.ri", "Arrow Lake", 3, 3),
    SOF4(0x7F50, SST_MTL, "arls.ri", "Arrow Lake-S", 2, 3),
    SOF4(0xA828, SST_LNL, "lnl.ri", "Lunar Lake", 5, 3),
    SOF4(0xE428, SST_PTL, "ptl.ri", "Panther Lake", 5, 3),
    SOF4(0xE328, SST_PTL, "ptl.ri", "Panther Lake-H", 5, 3),
    SOF4(0x4D28, SST_PTL, "wcl.ri", "Wildcat Lake", 2, 3),
    SOF4(0xD328, SST_NVL, "nvl.ri", "Nova Lake", 4, 3),
    SOF4(0x6E50, SST_NVL, "nvls.ri", "Nova Lake-S", 2, 3),
};

static const SST_PLAT *FindPlat(USHORT dev)
{
    ULONG i;
    for (i = 0; i < ARRAYSIZE(g_Plats); i++) if (g_Plats[i].Dev == dev) return &g_Plats[i];
    return NULL;
}

#define ACE(s) ((s)->P->Gen >= SST_MTL && (s)->P->Gen < SST_BYT)

/* ------------------------------------------------------------------ */
/* registers                                                           */
/* ------------------------------------------------------------------ */
#define ADSPCS   0x04
#define ADSPIC   0x08
#define ADSPIS   0x0C
#define CS_CRST(m)  (m)
#define CS_CSTALL(m) ((m) << 8)
#define CS_SPA(m)   ((m) << 16)
#define CS_CPA(m)   ((m) << 24)

/* HD Audio */
#define GCAP      0x00
#define INTCTL    0x20
#define SD_BASE   0x80
#define SD_CTL    0x00
#define SD_STS    0x03
#define SD_LPIB   0x04
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FMT    0x12
#define SD_BDPL   0x18
#define SD_BDPU   0x1C
#define VS_EM2    0x1030
#define VS_LTRP   0x1048
#define PPCTL     0x04
#define PPSTS     0x08
#define PPCTL_GPROCEN (1UL << 30)
#define PPCTL_PIE     (1UL << 31)

/* ROM status codes */
#define FSR_INIT_DONE   0x1
#define FSR_FW_ENTERED  0x5
#define SKL_FW_ENTERED  0xF

/* ACE */
#define MTL_HFDSSCS     0x1000
#define MTL_HFPWRCTL    0x1D18
#define MTL_HFPWRSTS    0x1D1C
#define PTL_HFPWRCTL2   0x1D20
#define PTL_HFPWRSTS2   0x1D24
#define MTL_DSP2CXCTL   0x178D04
#define MTL_FLGPXQWY    0x163200
#define LNL_HFDSC       0x160200

static void Udelay(ULONG us) { OsStallUs(us); }

static BOOLEAN PollDsp(SST *s, ULONG off, ULONG mask, ULONG val, ULONG us)
{
    ULONG t;
    for (t = 0; t < us; t += 10) {
        if ((DSP32(s, off) & mask) == val) return TRUE;
        Udelay(10);
    }
    return (DSP32(s, off) & mask) == val;
}

static void DspUpdate(SST *s, ULONG off, ULONG mask, ULONG val)
{
    DSPW32(s, off, (DSP32(s, off) & ~mask) | (val & mask));
}

USHORT SstHdaFmt(const SST_FMT *f)
{
    USHORT v = 0, bits;
    switch (f->Rate) {
    case 44100: v = 0x4000; break;
    case 96000: v = 0x0800; break;
    case 192000: v = 0x1800; break;
    case 16000: v = 0x0200; break;
    case 32000: v = 0x0A00; break;
    case 8000: v = 0x0500; break;
    default: v = 0;
    }
    bits = f->Valid <= 8 ? 0 : f->Valid <= 16 ? 1 : f->Valid <= 20 ? 2 : f->Valid <= 24 ? 3 : 4;
    if (f->Bits == 32 && f->Valid > 24) bits = 4;
    return (USHORT)(v | (bits << 4) | ((f->Ch - 1) & 15));
}

/* ------------------------------------------------------------------ */
/* cAVS core control                                                   */
/* ------------------------------------------------------------------ */
static BOOLEAN CorePower(SST *s, ULONG mask, BOOLEAN on)
{
    DspUpdate(s, ADSPCS, CS_SPA(mask), on ? CS_SPA(mask) : 0);
    Udelay(1000);
    if (!PollDsp(s, ADSPCS, CS_CPA(mask), on ? CS_CPA(mask) : 0, 20000)) {
        LOGE("sst: cores %x power %s failed (ADSPCS %08x)", mask, on ? "on" : "off", DSP32(s, ADSPCS));
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN CoreReset(SST *s, ULONG mask, BOOLEAN reset)
{
    DspUpdate(s, ADSPCS, CS_CRST(mask), reset ? CS_CRST(mask) : 0);
    if (!PollDsp(s, ADSPCS, CS_CRST(mask), reset ? CS_CRST(mask) : 0, 10000)) {
        LOGE("sst: cores %x %s reset failed (ADSPCS %08x)", mask, reset ? "enter" : "leave", DSP32(s, ADSPCS));
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN CoreStall(SST *s, ULONG mask, BOOLEAN stall)
{
    DspUpdate(s, ADSPCS, CS_CSTALL(mask), stall ? CS_CSTALL(mask) : 0);
    if (!PollDsp(s, ADSPCS, CS_CSTALL(mask), stall ? CS_CSTALL(mask) : 0, 10000)) {
        LOGE("sst: cores %x %sstall failed", mask, stall ? "" : "un");
        return FALSE;
    }
    Udelay(1000);
    return TRUE;
}

static void CoreDisable(SST *s, ULONG mask)
{
    if (ACE(s)) {
        DspUpdate(s, MTL_DSP2CXCTL, 1, 0);
        Udelay(1000);
        PollDsp(s, MTL_DSP2CXCTL, 1UL << 8, 0, 50000);
        return;
    }
    CoreStall(s, mask, TRUE);
    CoreReset(s, mask, TRUE);
    CorePower(s, mask, FALSE);
}

/* ------------------------------------------------------------------ */
/* HD Audio side helpers                                               */
/* ------------------------------------------------------------------ */
static ULONG SdOff(ULONG idx) { return SD_BASE + idx * 0x20; }

static ULONG SdCtl(SST *s, ULONG idx)
{
    ULONG o = SdOff(idx);
    return HDA8(s, o) | ((ULONG)HDA8(s, o + 2) << 16);
}

static void SdCtlW(SST *s, ULONG idx, ULONG v)
{
    ULONG o = SdOff(idx);
    HDAW8(s, o + 2, v >> 16);
    HDAW8(s, o, v & 0xFF);
}

static void SdResetIdx(SST *s, ULONG idx)
{
    ULONG i, v = SdCtl(s, idx);
    SdCtlW(s, idx, v & ~0x1EUL);
    for (i = 0; i < 1000 && (SdCtl(s, idx) & 2); i++) Udelay(1);
    SdCtlW(s, idx, SdCtl(s, idx) | 1);
    for (i = 0; i < 1000 && !(SdCtl(s, idx) & 1); i++) Udelay(1);
    SdCtlW(s, idx, SdCtl(s, idx) & ~1UL);
    for (i = 0; i < 1000 && (SdCtl(s, idx) & 1); i++) Udelay(1);
    HDAW8(s, SdOff(idx) + SD_STS, 0x1C);
}

static void Decouple(SST *s, ULONG idx, BOOLEAN on)
{
    ULONG v;
    if (!s->PpOff) return;
    v = HDA32(s, s->PpOff + PPCTL);
    if (on) v |= 1UL << idx; else v &= ~(1UL << idx);
    HDAW32(s, s->PpOff + PPCTL, v);
}

static void SpibSet(SST *s, ULONG idx, BOOLEAN on, ULONG val)
{
    ULONG v;
    if (!s->SpbOff) return;
    if (!on) HDAW32(s, s->SpbOff + 0x08 + 0x08 * idx, 0);
    v = HDA32(s, s->SpbOff + 0x04);
    if (on) v |= 1UL << idx; else v &= ~(1UL << idx);
    HDAW32(s, s->SpbOff + 0x04, v);
    if (on) HDAW32(s, s->SpbOff + 0x08 + 0x08 * idx, val);
}

/* chunks of contiguous memory with a BDL, for firmware images */
#define CHUNK 0x10000
typedef struct _SGBUF {
    DMA_BUFFER Bdl;
    DMA_BUFFER Ch[32];
    ULONG N, Size;
} SGBUF;

static void SgFree(SGBUF *g)
{
    ULONG i;
    for (i = 0; i < g->N; i++) OsFreeDma(&g->Ch[i]);
    if (g->Bdl.Va) OsFreeDma(&g->Bdl);
    memset(g, 0, sizeof(*g));
}

/* the BDL: at least two entries (the hardware wants two or more), IOC on none */
static BOOLEAN SgAlloc(SGBUF *g, const UCHAR *data, ULONG size)
{
    ULONG i, off = 0;
    PULONG bdl;
    memset(g, 0, sizeof(*g));
    if (!size || size > CHUNK * 32) return FALSE;
    if (!OsAllocDma(&g->Bdl, 4096, FALSE)) return FALSE;
    bdl = (PULONG)g->Bdl.Va;
    while (off < size) {
        ULONG n = MIN(CHUNK, size - off);
        if (size - off <= CHUNK && g->N == 0 && n > 128) n = (n / 2 + 127) & ~127UL;   /* two entries */
        if (!OsAllocDma(&g->Ch[g->N], (n + 4095) & ~4095UL, FALSE)) { SgFree(g); return FALSE; }
        if (data) memcpy(g->Ch[g->N].Va, data + off, n);
        else memset(g->Ch[g->N].Va, 0, n);
        bdl[g->N * 4 + 0] = g->Ch[g->N].Pa;
        bdl[g->N * 4 + 1] = 0;
        bdl[g->N * 4 + 2] = n;
        bdl[g->N * 4 + 3] = 0;
        g->N++;
        off += n;
    }
    for (i = 0; i < g->N; i++) (void)i;
    g->Size = size;
    MemoryBarrier();
    return TRUE;
}

static void SdProgram(SST *s, ULONG idx, SGBUF *g, USHORT fmt, ULONG tag)
{
    ULONG o = SdOff(idx);
    SdResetIdx(s, idx);
    HDAW32(s, o + SD_BDPL, g->Bdl.Pa);
    HDAW32(s, o + SD_BDPU, 0);
    HDAW32(s, o + SD_CBL, g->Size);
    HDAW16(s, o + SD_LVI, (USHORT)(g->N - 1));
    HDAW16(s, o + SD_FMT, fmt);
    SdCtlW(s, idx, (SdCtl(s, idx) & ~0xF00000UL) | (tag << 20));
}

static void SdRun(SST *s, ULONG idx, BOOLEAN run)
{
    ULONG i;
    if (run) SdCtlW(s, idx, SdCtl(s, idx) | 2);
    else {
        SdCtlW(s, idx, SdCtl(s, idx) & ~0x1EUL);
        for (i = 0; i < 1000 && (SdCtl(s, idx) & 2); i++) Udelay(1);
    }
}

/* ------------------------------------------------------------------ */
/* multi-link (extended links: SoundWire 0x00, SSP 0xC0, DMIC 0xC1)    */
/* ------------------------------------------------------------------ */
ULONG SstMlLink(SST *s, ULONG elid)
{
    ULONG n, i;
    if (!s->MlOff) return 0;
    n = HDA32(s, s->MlOff + 0x04) & 0xFF;              /* MLCD: link count */
    for (i = 0; i < n && i < 16; i++) {
        ULONG l = s->MlOff + 0x40 + 0x40 * i, cap = HDA32(s, l);
        if (!(cap & (1UL << 28))) continue;            /* not an alternate (extended) link */
        if (((HDA32(s, l + 0x20) >> 24) & 0xFF) == elid) return l;
    }
    return 0;
}

BOOLEAN SstMlPower(SST *s, ULONG elid, ULONG sub, BOOLEAN on)
{
    ULONG l = SstMlLink(s, elid), t;
    if (!l) return FALSE;
    if (on) HDAW32(s, l + 0x04, HDA32(s, l + 0x04) | (1UL << (16 + sub)));
    else HDAW32(s, l + 0x04, HDA32(s, l + 0x04) & ~(1UL << (16 + sub)));
    for (t = 0; t < 100; t++) {
        Udelay(10);
        if (!!(HDA32(s, l + 0x04) & (1UL << (23 + sub))) == on) return TRUE;
    }
    LOGE("sst: extended link %02x/%u power %s failed (LCTL %08x)", elid, sub, on ? "on" : "off", HDA32(s, l + 0x04));
    return FALSE;
}

/* link DMA of a decoupled stream: PPLCxCTL (stream tag, run), PPLCxFMT */
void SstLinkDmaSetup(SST *s, ULONG stream, ULONG tag, USHORT fmt, BOOLEAN run)
{
    ULONG nstreams = s->Iss + s->Oss, o, v, i;
    if (!s->PpOff) return;
    o = s->PpOff + 0x10 + 0x10 * nstreams + 0x10 * stream;
    v = HDA32(s, o) & ~2UL;
    HDAW32(s, o, v);
    if (!run) {
        HDAW32(s, o, v | 1);                            /* stream reset */
        for (i = 0; i < 50 && !(HDA32(s, o) & 1); i++) Udelay(3);
        HDAW32(s, o, HDA32(s, o) & ~1UL);
        for (i = 0; i < 50 && (HDA32(s, o) & 1); i++) Udelay(3);
        return;
    }
    v = (HDA32(s, o) & ~(0xFUL << 20)) | (tag << 20);
    HDAW32(s, o, v);
    HDAW16(s, o + 4, fmt);
    HDAW32(s, o, v | 2);
}

/* ------------------------------------------------------------------ */
/* IPC                                                                  */
/* ------------------------------------------------------------------ */
void SstMemFromDsp(SST *s, ULONG off, void *dst, ULONG len)
{
    ULONG i;
    PULONG d = (PULONG)dst;
    for (i = 0; i + 4 <= len; i += 4) d[i / 4] = DSP32(s, off + i);
    if (len & 3) {
        ULONG v = DSP32(s, off + (len & ~3UL));
        memcpy((PUCHAR)dst + (len & ~3UL), &v, len & 3);
    }
}

void SstMemToDsp(SST *s, ULONG off, const void *src, ULONG len)
{
    ULONG i;
    for (i = 0; i < len; i += 4) {
        ULONG v = 0;
        memcpy(&v, (const UCHAR *)src + i, MIN(4, len - i));
        DSPW32(s, off + i, v);
    }
}

/* a message from the DSP: TRUE and its header in s->RxHdr; the payload is read by the caller */
BOOLEAN SstRxPending(SST *s, ULONG *pri, ULONG *ext)
{
    ULONG r = DSP32(s, s->RspOff);
    if (r == 0xFFFFFFFF || !(r & s->RspBusy)) return FALSE;
    *pri = r & ~s->RspBusy;
    *ext = DSP32(s, s->RspExt);
    return TRUE;
}

void SstRxDone(SST *s)
{
    ULONG i;
    if (s->Lpe) {                                          /* IPCD: BUSY off, DONE on */
        DSPW32(s, s->RspOff, (DSP32(s, s->RspOff) & ~s->RspBusy) | s->RspDone);
        return;
    }
    /* write-1-to-clear the busy bit: the DSP may send the next message */
    DSPW32(s, s->RspOff, DSP32(s, s->RspOff) | s->RspBusy);
    if (s->P->Gen >= SST_CNL && !ACE(s)) {
        DSPW32(s, s->RspAck, DSP32(s, s->RspAck) | (1UL << 31));
        for (i = 0; i < 100 && (DSP32(s, s->RspAck) & (1UL << 31)); i++) Udelay(10);
    } else if (ACE(s)) {
        DSPW32(s, s->RspAck, DSP32(s, s->RspAck) & ~(1UL << 31));
    }
}

static BOOLEAN IsIpc4Reply(ULONG pri)
{
    /* direction = reply (bit 29) and not a notification (global type 27) */
    return (pri & (1UL << 29)) && ((pri >> 24) & 0x1F) != 27;
}

static void Notification(SST *s, ULONG pri, ULONG ext)
{
    ULONG type = (pri >> 16) & 0xFF;
    (void)ext;
    if (s->P->Fw == SST_FW_SOF3) return;
    if (type == 8) { s->Ready = TRUE; LOGI("sst: firmware ready"); }
    else if (type == 10) { LOGE("sst: firmware exception (%08x %08x)", pri, ext); s->Errors++; }
    else LOGT("sst: notification %u (%08x %08x)", type, pri, ext);
}

/* sends a request and waits for its reply (IPC4) or for the mailbox reply (IPC3) */
BOOLEAN SstIpc(SST *s, ULONG primary, ULONG ext, const void *data, ULONG len, ULONG timeoutMs)
{
    ULONG t, pri, ex;
    BOOLEAN acked = FALSE;
    if (len > s->DownSize) { LOGE("sst: IPC payload %u too large", len); return FALSE; }
    /* the previous request must be taken */
    for (t = 0; t < 20000 && (DSP32(s, s->ReqOff) & s->ReqBusy); t += 10) Udelay(10);
    if (len) SstMemToDsp(s, s->DownOff, data, len);
    DSPW32(s, s->ReqExt, ext);
    DSPW32(s, s->ReqOff, primary | s->ReqBusy);
    s->RxLen = 0;
    for (t = 0; t < timeoutMs * 100; t++) {
        ULONG a = DSP32(s, s->AckOff);
        if (!acked && a != 0xFFFFFFFF && (a & s->AckDone)) {
            acked = TRUE;
            if (s->P->Fw == SST_FW_SOF3) {
                /* IPC3: the reply overwrote the request in the host box */
                SstMemFromDsp(s, s->DownOff, s->Rx, 12);
                s->RxLen = MIN(G32U(s->Rx), SST_MBOX);
                if (s->RxLen > 12) SstMemFromDsp(s, s->DownOff, s->Rx, s->RxLen);
                if (s->Lpe) DSPW32(s, s->AckOff, DSP32(s, s->AckOff) & ~s->AckDone);   /* IPCX DONE: write 0 */
                else DSPW32(s, s->AckOff, DSP32(s, s->AckOff) | s->AckDone);
                if ((LONG)G32U(s->Rx + 8) < 0) {
                    LOGE("sst: IPC3 %08x failed: %d", len >= 8 ? G32U((const UCHAR *)data + 4) : 0, (LONG)G32U(s->Rx + 8));
                    return FALSE;
                }
                return TRUE;
            }
            DSPW32(s, s->AckOff, a | s->AckDone);       /* write-1-to-clear */
        }
        if (SstRxPending(s, &pri, &ex)) {
            if (IsIpc4Reply(pri)) {
                s->RxHdr = ((ULONGLONG)ex << 32) | pri;
                if ((pri & 0xFFFFFF) == 0) {
                    ULONG n = 0;
                    /* LARGE_CONFIG_GET: the size is in the extension */
                    if ((primary & (1UL << 30)) && ((primary >> 24) & 0x1F) == 3) n = MIN(ex & 0xFFFFF, SST_MBOX);
                    if (n) SstMemFromDsp(s, s->UpOff, s->Rx, n);
                    s->RxLen = n;
                }
                SstRxDone(s);
                if (pri & 0xFFFFFF) {
                    LOGE("sst: IPC %08x %08x failed: status %u", primary, ext, pri & 0xFFFFFF);
                    return FALSE;
                }
                return TRUE;
            }
            if (s->P->Fw == SST_FW_SOF3) Ipc3Notify(s);
            else Notification(s, pri, ex);
            SstRxDone(s);
        }
        if (t % 100 == 99 && KeGetCurrentIrql() < DISPATCH_LEVEL) OsSleepMs(1);
        else Udelay(10);
    }
    LOGE("sst: IPC %08x %08x timed out (ack %u, status %08x, error %08x)", primary, ext, acked,
         DSP32(s, s->StatusOff), DSP32(s, s->StatusOff + 4));
    s->Errors++;
    return FALSE;
}

/* polls for messages until FW_READY (IPC4) */
BOOLEAN Ipc4WaitReady(SST *s, ULONG ms)
{
    ULONG t, pri, ex;
    for (t = 0; t < ms && !s->Ready; t++) {
        while (SstRxPending(s, &pri, &ex)) {
            if (!IsIpc4Reply(pri)) Notification(s, pri, ex);
            SstRxDone(s);
        }
        if (s->Ready) break;
        OsSleepMs(1);
    }
    return s->Ready;
}

/* ------------------------------------------------------------------ */
/* firmware image                                                      */
/* ------------------------------------------------------------------ */
static BOOLEAN UuidEq(const UCHAR *m, ULONG d1, USHORT d2, USHORT d3, const UCHAR *d4)
{
    return G32U(m) == d1 && (USHORT)(m[4] | (m[5] << 8)) == d2 && (USHORT)(m[6] | (m[7] << 8)) == d3 &&
           !memcmp(m + 8, d4, 8);
}

/* the ADSP manifest ($AM1): module ids are the positions in its table */
static void ParseManifest(SST *s)
{
    const UCHAR *p = s->Img + s->Payload, *end = s->Img + s->ImgLen, *h = NULL, *m;
    ULONG i, n, hlen;
    static const UCHAR cpr[8] = { 0x94, 0x3C, 0x1F, 0xA2, 0xE8, 0x2F, 0x9D, 0xDA };
    static const UCHAR pkv[8] = { 0xAF, 0xE9, 0xFE, 0x5D, 0xBA, 0xA4, 0xC3, 0x93 };
    static const UCHAR gai[8] = { 0x8E, 0x7B, 0x26, 0x39, 0x21, 0x98, 0x04, 0xB7 };
    s->ModCopier = s->ModPeak = s->ModGain = 0xFFFF;
    for (i = 0; p + i + 64 < end && i < 0x40000; i += 4)
        if (G32U(p + i) == 0x314D4124UL) { h = p + i; break; }      /* "$AM1" */
    if (!h) { LOGE("sst: no ADSP manifest in the image"); return; }
    hlen = G32U(h + 4);
    n = G32U(h + 36);
    OsSprintf(s->FwVer, sizeof(s->FwVer), "%u.%u.%u.%u", h[28] | (h[29] << 8), h[30] | (h[31] << 8),
              h[32] | (h[33] << 8), h[34] | (h[35] << 8));
    m = h + hlen;
    for (i = 0; i < n && m + 116 <= end; i++, m += 116) {
        const UCHAR *u = m + 12;
        if (UuidEq(u, 0x9BA00C83, 0xCA12, 0x4A83, cpr)) s->ModCopier = i;
        else if (UuidEq(u, 0x8A171323, 0x94A3, 0x4E1D, pkv)) s->ModPeak = i;
        else if (UuidEq(u, 0x61BCA9A8, 0x18D0, 0x4A18, gai)) s->ModGain = i;
        LOGV("sst: module %u %.8s", i, (const char *)m + 4);
    }
    LOGI("sst: firmware %.8s %s, %u modules, copier %u", (const char *)h + 8, s->FwVer, n, s->ModCopier);
}

static BOOLEAN LoadImage(SST *s)
{
    char path[96];
    const char *base = s->P->File;
    ULONG over = OsRegGetDword(L"SstFwSof", 0);
    (void)over;
    OsSprintf(path, sizeof(path), "%s", base);
    s->Img = OsLoadFile(path, &s->ImgLen);
    if (!s->Img) {
        LOGE("sst: firmware %s not found (copy it to %%SystemRoot%%\\System32\\drivers\\audiont\\%s)", path, path);
        return FALSE;
    }
    s->Payload = 0;
    if (s->ImgLen > 16 && G32U(s->Img) == 0x31454124UL) s->Payload = G32U(s->Img + 4);   /* "$AE1" */
    if (s->P->Fw == SST_FW_SOF3 && s->ImgLen > 16 && G32U(s->Img) == 0x6E614D58UL)          /* "XMan" */
        s->Payload = G32U(s->Img + 4);                                                       /* full_size */
    if (s->Payload >= s->ImgLen) { LOGE("sst: %s: bad extended manifest", path); return FALSE; }
    LOGI("sst: %s, %u bytes, payload at %x", path, s->ImgLen, s->Payload);
    if (s->P->Fw != SST_FW_SOF3) ParseManifest(s);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* boot                                                                */
/* ------------------------------------------------------------------ */
static void Gating(SST *s, BOOLEAN on)
{
    CARD *c = s->Card;
    ULONG v;
    if (ACE(s)) return;
    v = PciRead32(c->Bus, c->Dev, c->Fn, 0x44);            /* PGCTL.LSRMD */
    PciWrite32(c->Bus, c->Dev, c->Fn, 0x44, on ? (v & ~0x10UL) : (v | 0x10UL));
    v = PciRead32(c->Bus, c->Dev, c->Fn, 0x48);            /* CGCTL.MISCBDCGE */
    PciWrite32(c->Bus, c->Dev, c->Fn, 0x48, on ? (v | 0x40UL) : (v & ~0x40UL));
    v = HDA32(s, VS_EM2);                                  /* EM2.L1SEN */
    HDAW32(s, VS_EM2, on ? (v | (1UL << 13)) : (v & ~(1UL << 13)));
}

/* Skylake / Kaby Lake: the code loader DMA in the DSP BAR */
#define CL_SD    0x80
#define CL_SPBCTL 0x24
#define CL_SPIB  0x28
static BOOLEAN BootCldma(SST *s)
{
    SGBUF ring;
    ULONG left, pos, half = CHUNK, t, i;
    const UCHAR *src = s->Img + s->Payload;
    left = s->ImgLen - s->Payload;
    if (!CorePower(s, 1, TRUE) || !CoreReset(s, 1, FALSE)) return FALSE;
    if (!SgAlloc(&ring, NULL, 2 * half)) return FALSE;
    /* CLDMA stream: stop, reset, BDL (two halves), SPIB */
    DSPW32(s, ADSPIC, DSP32(s, ADSPIC) & ~2UL);
    MMIO8(s->Dsp, CL_SD) = 0;
    for (i = 0; i < 300 && (MMIO8(s->Dsp, CL_SD) & 2); i++) Udelay(3);
    MMIO8(s->Dsp, CL_SD) = 1;
    for (i = 0; i < 300 && !(MMIO8(s->Dsp, CL_SD) & 1); i++) Udelay(3);
    MMIO8(s->Dsp, CL_SD) = 0;
    for (i = 0; i < 300 && (MMIO8(s->Dsp, CL_SD) & 1); i++) Udelay(3);
    ((PULONG)ring.Bdl.Va)[3] = 1;                          /* IOC on both halves */
    ((PULONG)ring.Bdl.Va)[7] = 1;
    DSPW32(s, CL_SD + SD_BDPL, ring.Bdl.Pa);
    DSPW32(s, CL_SD + SD_BDPU, 0);
    DSPW32(s, CL_SD + SD_CBL, 2 * half);
    MMIO16(s->Dsp, CL_SD + SD_LVI) = 1;
    MMIO8(s->Dsp, CL_SD + 2) = (UCHAR)((MMIO8(s->Dsp, CL_SD + 2) & 0x0F) | (1 << 4));   /* stream tag 1 */
    DSPW32(s, CL_SPBCTL, 1);
    if (!CoreStall(s, 1, FALSE)) { SgFree(&ring); return FALSE; }
    if (!PollDsp(s, s->StatusOff, 1, 1, 1000000)) {
        LOGE("sst: ROM init failed: status %08x error %08x", DSP32(s, s->StatusOff), DSP32(s, s->StatusOff + 4));
        CoreDisable(s, 1);
        SgFree(&ring);
        return FALSE;
    }
    /* fill the ring as the DSP takes it; SPIB tells how far valid data goes */
    pos = 0;
    {
        ULONG n = MIN(left, half), k;
        for (k = 0; k < 2 && left; k++) {
            n = MIN(left, half);
            memcpy(ring.Ch[k].Va, src, n);
            src += n; left -= n; pos = (pos + n) % (2 * half);
            if (n < half) break;
        }
        DSPW32(s, CL_SPIB, pos ? pos : 2 * half);
    }
    MemoryBarrier();
    MMIO8(s->Dsp, CL_SD) = 0x1C | 2;                       /* run, interrupts on the stream (status only) */
    for (t = 0; t < 3000; t++) {
        UCHAR st = MMIO8(s->Dsp, CL_SD + SD_STS);
        if ((DSP32(s, s->StatusOff) & 0xFF) == SKL_FW_ENTERED) break;
        if ((st & 4) && left) {                            /* buffer completion: refill the half just played */
            ULONG n = MIN(left, half), off = (DSP32(s, CL_SPIB) % (2 * half)), k = off / half;
            MMIO8(s->Dsp, CL_SD + SD_STS) = 4;
            memcpy((PUCHAR)ring.Ch[k].Va + off % half, src, n);
            src += n; left -= n;
            MemoryBarrier();
            DSPW32(s, CL_SPIB, (off + n) % (2 * half) ? (off + n) % (2 * half) : 2 * half);
        } else if (st & 0x18) {
            LOGE("sst: CLDMA error, status %02x", st);
            break;
        }
        OsSleepMs(1);
    }
    MMIO8(s->Dsp, CL_SD) = 0;
    DSPW32(s, CL_SPBCTL, 0);
    SgFree(&ring);
    if ((DSP32(s, s->StatusOff) & 0xFF) != SKL_FW_ENTERED) {
        LOGE("sst: firmware transfer failed: status %08x error %08x (%u bytes left)", DSP32(s, s->StatusOff),
             DSP32(s, s->StatusOff + 4), left);
        CoreDisable(s, 1);
        return FALSE;
    }
    return TRUE;
}

/* ROM start on cAVS 1.5+ .. 2.5 with ROM_CONTROL / purge naming the host DMA */
static BOOLEAN RomInitCavs(SST *s, ULONG dmaId)
{
    ULONG hdr = 0x01000000UL | 0x4000UL | (dmaId << 9);   /* ROM_CONTROL, PURGE_FW, DMA id */
    ULONG init = s->P->InitMask, host = s->P->HostCores ? s->P->HostCores : 1, i;
    if (s->P->Gen >= SST_TGL) host = 1;
    if (!CorePower(s, host | init, TRUE)) return FALSE;
    /* SSP ports to clock consumer / codec provider (SOF does this before the ROM runs) */
    for (i = 0; i < s->P->Ssp && s->P->Gen >= SST_APL; i++) {
        ULONG base = s->P->Gen == SST_APL ? 0x2000 : 0x10000, o = base + i * 0x1000 + 0x04;
        DSPW32(s, o, DSP32(s, o) | (3UL << 24));
    }
    DSPW32(s, s->ReqExt, 0);
    DSPW32(s, s->ReqOff, hdr | s->ReqBusy);
    if (!CoreReset(s, init, FALSE) || !CoreStall(s, init, FALSE)) return FALSE;
    if (!PollDsp(s, s->AckOff, s->AckDone, s->AckDone, 500000)) {
        LOGE("sst: ROM did not take ROM_CONTROL (ack %08x status %08x)", DSP32(s, s->AckOff), DSP32(s, s->StatusOff));
        CoreDisable(s, host | init);
        return FALSE;
    }
    DSPW32(s, s->AckOff, DSP32(s, s->AckOff) | s->AckDone);
    if ((host | init) & ~init) CorePower(s, (host | init) & ~init, FALSE);
    for (i = 0; i < 3000; i++) {
        ULONG st = DSP32(s, s->StatusOff) & 0xF;
        if (st == FSR_INIT_DONE || st == FSR_FW_ENTERED) return TRUE;
        Udelay(100);
    }
    LOGE("sst: ROM init timed out: status %08x error %08x", DSP32(s, s->StatusOff), DSP32(s, s->StatusOff + 4));
    CoreDisable(s, init);
    return FALSE;
}

static BOOLEAN AcePowerUp(SST *s)
{
    ULONG ctl = s->P->Gen > SST_LNL ? PTL_HFPWRCTL2 : MTL_HFPWRCTL;
    ULONG sts = s->P->Gen > SST_LNL ? PTL_HFPWRSTS2 : MTL_HFPWRSTS;
    if (DSP32(s, MTL_HFDSSCS) & (1UL << 24)) {             /* left on: power cycle */
        CoreDisable(s, 1);
        DspUpdate(s, MTL_HFDSSCS, 1UL << 16, 0);
        Udelay(1000);
        PollDsp(s, MTL_HFDSSCS, 1UL << 24, 0, 50000);
    }
    DspUpdate(s, MTL_HFDSSCS, 1UL << 16, 1UL << 16);
    Udelay(1000);
    if (!PollDsp(s, MTL_HFDSSCS, 1UL << 24, 1UL << 24, 50000)) {
        LOGE("sst: DSP subsystem does not power up (HFDSSCS %08x)", DSP32(s, MTL_HFDSSCS));
        return FALSE;
    }
    DspUpdate(s, ctl, 1, 1);
    Udelay(1000);
    if (!PollDsp(s, sts, 1, 1, 50000)) LOGE("sst: DSP HP domain does not power up");
    DspUpdate(s, MTL_HFPWRCTL, 1UL << 9, 1UL << 9);        /* SoundWire I/O domain stays on */
    return TRUE;
}

static BOOLEAN RomInitAce(SST *s, ULONG dmaId)
{
    ULONG hdr = 0x01000000UL | 0x4000UL | (dmaId << 9), i;
    DSPW32(s, s->ReqOff, hdr | s->ReqBusy);
    DspUpdate(s, MTL_DSP2CXCTL, 3UL << 24, 2UL << 24);     /* owner: host */
    DspUpdate(s, MTL_DSP2CXCTL, 1, 1);
    Udelay(1000);
    if (!PollDsp(s, MTL_DSP2CXCTL, 1UL << 8, 1UL << 8, 50000)) {
        LOGE("sst: primary core does not power up (DSP2CxCTL %08x)", DSP32(s, MTL_DSP2CXCTL));
        return FALSE;
    }
    if (!PollDsp(s, s->AckOff, s->AckDone, s->AckDone, 500000)) {
        LOGE("sst: ROM did not take ROM_CONTROL (ack %08x)", DSP32(s, s->AckOff));
        CoreDisable(s, 1);
        return FALSE;
    }
    DSPW32(s, s->AckOff, DSP32(s, s->AckOff) | s->AckDone);
    for (i = 0; i < 3000; i++) {
        ULONG st = DSP32(s, s->StatusOff) & 0xFFFFFF;
        if (st == FSR_INIT_DONE || st == FSR_FW_ENTERED) return TRUE;
        Udelay(100);
    }
    LOGE("sst: ROM init timed out: status %08x error %08x", DSP32(s, s->StatusOff), DSP32(s, s->StatusOff + 4));
    CoreDisable(s, 1);
    return FALSE;
}

static BOOLEAN BootHostDma(SST *s)
{
    SGBUF img, icc;
    ULONG sd = s->Iss + s->Oss - 1, dmaId = s->Oss - 1, icsd = s->Iss ? s->Iss - 1 : 0, t, attempt;
    BOOLEAN ok = FALSE, iccmax = s->P->Gen >= SST_ICL && !ACE(s) && s->Iss;
    UCHAR ltrp = HDA8(s, VS_LTRP);
    if (!SgAlloc(&img, s->Img + s->Payload, s->ImgLen - s->Payload)) { LOGE("sst: no memory for the image"); return FALSE; }
    memset(&icc, 0, sizeof(icc));
    if (iccmax && SgAlloc(&icc, NULL, 4096)) {
        /* ICCMAX: a running capture stream keeps the link out of L1 while the image loads */
        SdProgram(s, icsd, &icc, 0x0040, 1);
        Decouple(s, icsd, TRUE);
        HDAW8(s, VS_LTRP, (ltrp & ~0x3F) | 95);
        SdRun(s, icsd, TRUE);
    }
    if (s->P->Gen == SST_APL) Decouple(s, sd, FALSE);
    SdProgram(s, sd, &img, 0x0040, dmaId + 1);             /* 48 kHz, 32 bit, mono */
    Decouple(s, sd, TRUE);
    SpibSet(s, sd, TRUE, img.Size);
    for (attempt = 0; attempt < 3 && !ok; attempt++) {
        if (ACE(s)) ok = AcePowerUp(s) && RomInitAce(s, dmaId);
        else ok = RomInitCavs(s, dmaId);
        if (!ok) LOGI("sst: ROM start attempt %u failed", attempt + 1);
    }
    if (ok) {
        SdRun(s, sd, TRUE);
        ok = FALSE;
        for (t = 0; t < 3000; t++) {
            ULONG st = DSP32(s, s->StatusOff) & (ACE(s) ? 0xFFFFFF : 0xFF);
            if (st == FSR_FW_ENTERED) { ok = TRUE; break; }
            OsSleepMs(1);
        }
        SdRun(s, sd, FALSE);
        if (!ok) {
            LOGE("sst: firmware transfer failed: status %08x error %08x LPIB %x", DSP32(s, s->StatusOff),
                 DSP32(s, s->StatusOff + 4), HDA32(s, SdOff(sd) + SD_LPIB));
            CoreDisable(s, s->P->InitMask);
        }
    }
    SpibSet(s, sd, FALSE, 0);
    SdResetIdx(s, sd);
    HDAW32(s, SdOff(sd) + SD_BDPL, 0);
    Decouple(s, sd, FALSE);
    if (icc.N) {
        SdRun(s, icsd, FALSE);
        SdResetIdx(s, icsd);
        Decouple(s, icsd, FALSE);
        HDAW8(s, VS_LTRP, ltrp);
    }
    SgFree(&img);
    SgFree(&icc);
    return ok;
}

static void SetupIpcRegs(SST *s)
{
    if (s->P->Gen <= SST_APL) {
        s->RspOff = 0x40; s->RspExt = 0x44; s->ReqOff = 0x48; s->ReqExt = 0x4C; s->CtlOff = 0x50;
        s->ReqBusy = 1UL << 31; s->AckOff = 0x4C; s->AckDone = 1UL << 30; s->RspBusy = 1UL << 31;
    } else if (!ACE(s)) {
        s->RspOff = 0xC0; s->RspAck = 0xC4; s->RspExt = 0xC8; s->ReqOff = 0xD0; s->AckOff = 0xD4;
        s->ReqExt = 0xD8; s->CtlOff = 0xE8;
        s->ReqBusy = 1UL << 31; s->AckDone = 1UL << 31; s->RspBusy = 1UL << 31;
    } else {
        s->RspOff = 0x73200; s->RspAck = 0x73204; s->ReqOff = 0x73210; s->AckOff = 0x73214; s->CtlOff = 0x73228;
        s->RspExt = 0x73300; s->ReqExt = 0x73380;
        s->ReqBusy = 1UL << 31; s->AckDone = 1UL << 31; s->RspBusy = 1UL << 31;
    }
    if (s->P->Gen == SST_SKL) { s->SramBase = 0x8000; s->SramWin = 0x2000; }
    else if (!ACE(s)) { s->SramBase = 0x80000; s->SramWin = 0x20000; }
    else { s->SramBase = 0x180000; s->SramWin = 0x8000; }
    s->StatusOff = s->SramBase;
    if (s->P->Gen == SST_MTL) s->StatusOff = MTL_FLGPXQWY;
    else if (s->P->Gen >= SST_LNL) s->StatusOff = LNL_HFDSC;
    s->UpOff = s->SramBase + 0x1000;                       /* window 0 after the firmware registers */
    s->UpSize = 0x1000;
    s->DownOff = s->SramBase + s->SramWin;                 /* window 1 */
    s->DownSize = 0x1000;
}

static BOOLEAN Boot(SST *s)
{
    BOOLEAN ok;
    ULONG pp;
    if (!LoadImage(s)) return FALSE;
    /* processing pipe on: streams may be decoupled, the DSP may own gateways */
    pp = HDA32(s, s->PpOff + PPCTL);
    HDAW32(s, s->PpOff + PPCTL, (pp | PPCTL_GPROCEN) & ~(PPCTL_PIE | 0x3FFFFFFFUL));
    DSPW32(s, ADSPIC, 0);                                  /* polled: no DSP interrupts */
    Gating(s, FALSE);
    if (!ACE(s)) CoreDisable(s, (1UL << s->P->Cores) - 1);
    if (s->P->Gen == SST_SKL) ok = BootCldma(s);
    else ok = BootHostDma(s);
    Gating(s, TRUE);
    if (!ok) return FALSE;
    if (s->P->Fw == SST_FW_SOF3) {
        if (!Ipc3WaitReady(s, 3000)) { LOGE("sst: no FW_READY (status %08x)", DSP32(s, s->StatusOff)); return FALSE; }
        return Ipc3Init(s);
    }
    if (!Ipc4WaitReady(s, 3000)) {
        LOGE("sst: no FW_READY (status %08x error %08x)", DSP32(s, s->StatusOff), DSP32(s, s->StatusOff + 4));
        return FALSE;
    }
    return Ipc4Init(s);
}

/* ------------------------------------------------------------------ */
/* endpoints                                                            */
/* ------------------------------------------------------------------ */
SST_EP *SstAddEp(SST *s, UCHAR kind, UCHAR dir, UCHAR port, const char *name)
{
    SST_EP *e;
    if (s->NEp >= SST_MAX_EP) return NULL;
    e = &s->Ep[s->NEp++];
    memset(e, 0, sizeof(*e));
    e->Kind = kind; e->Dir = dir; e->Port = port;
    OsSprintf(e->Name, sizeof(e->Name), "%s", name);
    return e;
}

static void FindDmic(SST *s)
{
    NHLT_FMT f;
    const NHLT_EP *ep;
    SST_EP *e;
    ULONG want = OsRegGetDword(L"SstDmicBits", 0);
    /* SOF and avs prefer the 32-bit blob, the copier converts to 16 bit */
    if (!NhltFind(NHLT_LINK_DMIC, -1, NHLT_DIR_CAPTURE, 48000, want ? want : 32, 2, &f, &ep) &&
        !NhltFind(NHLT_LINK_DMIC, -1, -1, 48000, 0, 0, &f, &ep)) return;
    if (f.Rate != 48000) { LOGI("sst: DMIC endpoint has no 48 kHz format"); return; }
    e = SstAddEp(s, EP_DMIC, 1, 0, "Internal Microphone");
    if (!e) return;
    e->Be.Rate = f.Rate; e->Be.Ch = f.Channels; e->Be.Bits = f.Bits; e->Be.Valid = f.ValidBits;
    e->Blob = f.Blob; e->BlobLen = f.BlobLen;
    e->Line = LINE_AUX;
    LOGI("sst: digital microphones: %u ch %u bit, blob %u bytes", f.Channels, f.Bits, f.BlobLen);
}

static void FindSsp(SST *s)
{
    ULONG mask = NhltSspMask(NHLT_DEV_I2S), port;
    for (port = 0; port < 8; port++) {
        UCHAR dir;
        if (!(mask & (1UL << port))) continue;
        for (dir = 0; dir < 2; dir++) {
            NHLT_FMT f;
            const NHLT_EP *ep;
            SST_EP *e;
            if (!NhltFind(NHLT_LINK_SSP, (int)port, dir, 48000, 0, 2, &f, &ep) || ep->DevType != NHLT_DEV_I2S) continue;
            e = SstAddEp(s, EP_SSP, dir, (UCHAR)port, dir ? "I2S microphone" : "I2S codec");
            if (!e) return;
            e->Be.Rate = f.Rate; e->Be.Ch = f.Channels; e->Be.Bits = f.Bits; e->Be.Valid = f.ValidBits;
            e->Blob = f.Blob; e->BlobLen = f.BlobLen;
            e->Line = LINE_MIC;
            LOGI("sst: SSP%u %s: %u ch %u/%u bit %u Hz", port, dir ? "capture" : "playback", f.Channels, f.ValidBits,
                 f.Bits, f.Rate);
        }
    }
}

/* ------------------------------------------------------------------ */
/* public                                                               */
/* ------------------------------------------------------------------ */
extern BOOLEAN SstCodecsProbe(SST *s);      /* sstcodec.c: I2C codecs and amplifiers */
extern void    SstCodecsPower(SST *s, SST_EP *e, BOOLEAN on);
extern void    SstCodecsShutdown(SST *s);
extern ULONG   SstCodecsDump(SST *s, char *buf, ULONG size);
extern SST_EP *SstCodecsPlayEp(SST *s);

static SST *Of(CARD *c) { return c ? (SST *)c->Sst : NULL; }

BOOLEAN SstProbe(CARD *c, PUCHAR hda, ULONG ppOff, ULONG mlOff, ULONG iss, ULONG oss, BOOLEAN haveCodec)
{
    SST *s;
    const SST_PLAT *p;
    ULONG mode = OsRegGetDword(L"Sst", 1), i, bar, off, n;
    if (c->VendorId != 0x8086 || !mode || !(p = FindPlat(c->DeviceId))) return FALSE;
    if (!ppOff) { LOGI("sst: %s has no processing pipe capability", p->Name); return FALSE; }
    s = (SST *)OsAlloc(sizeof(SST));
    if (!s) return FALSE;
    s->Card = c; s->P = p; s->Hda = hda; s->PpOff = ppOff; s->MlOff = mlOff; s->Iss = iss; s->Oss = oss;
    /* the SPB capability (software position in buffer) */
    off = HDA16(s, 0x14);
    for (n = 0; off && n < 10; n++) {
        ULONG hdr = HDA32(s, off);
        if (hdr == 0xFFFFFFFF) break;
        if (((hdr >> 16) & 0xFFF) == 4) s->SpbOff = off;
        off = hdr & 0xFFFF;
    }
    LOGI("sst: %s audio DSP (%s firmware), PP %x ML %x SPB %x", p->Name,
         p->Fw == SST_FW_AVS ? "Intel cAVS" : p->Fw == SST_FW_SOF4 ? "SOF IPC4" : "SOF IPC3", ppOff, mlOff, s->SpbOff);
    /* endpoints the DSP is needed for */
    NhltInit();
    FindDmic(s);
    FindSsp(s);
    SstCodecsProbe(s);
    SdwProbe(s);
    if (!s->NEp && mode < 2) {
        LOGI("sst: no DSP endpoints (no NHLT microphones / I2S / SoundWire): the DSP stays off");
        SdwShutdown(s);
        SstCodecsShutdown(s);
        OsFree(s);
        return FALSE;
    }
    /* DSP BAR */
    bar = c->Bar[4];
    if ((bar & 1) || !(bar & ~0xFUL)) {
        LOGE("sst: DSP BAR %08x not usable", bar);
        SdwShutdown(s); SstCodecsShutdown(s); OsFree(s);
        return FALSE;
    }
    s->DspPa = bar & ~0xFUL;
    s->DspSize = ACE(s) ? 0x200000 : 0x100000;
    s->Dsp = (PUCHAR)OsMapMmio(s->DspPa, s->DspSize);
    if (!s->Dsp) { LOGE("sst: cannot map the DSP BAR"); SdwShutdown(s); SstCodecsShutdown(s); OsFree(s); return FALSE; }
    SetupIpcRegs(s);
    c->Sst = s;
    if (!Boot(s)) {
        LOGE("sst: DSP did not start; microphones / I2S / SoundWire endpoints unavailable");
        SstShutdown(c);
        return FALSE;
    }
    /* SoundWire links of cAVS / ACE 1.x come up with the firmware running; endpoints without peripherals go */
    SdwStart(s);
    /* playback through the DSP when there is no HD Audio codec with an analog output: I2S codec or
       amplifiers, SoundWire speakers (amplifiers) or headphones, else the first playback endpoint */
    if (!haveCodec && !(s->PlayEp = SstCodecsPlayEp(s)) && !(s->PlayEp = SdwPlayEp(s)))
        for (i = 0; i < s->NEp; i++)
            if (s->Ep[i].Dir == 0) { s->PlayEp = &s->Ep[i]; break; }
    for (i = 0; i < s->NEp; i++)
        if (s->Ep[i].Dir == 1 && s->Ep[i].Line < LINE_COUNT && !s->RecEp[s->Ep[i].Line]) {
            /* with an HD Audio codec, an I2S/SoundWire microphone does not replace its mic input */
            if (haveCodec && s->Ep[i].Line == LINE_MIC && s->Ep[i].Kind != EP_DMIC) s->Ep[i].Line = LINE_AUX;
            if (!s->RecEp[s->Ep[i].Line]) s->RecEp[s->Ep[i].Line] = &s->Ep[i];
        }
    /* pipelines are built now (passive level) and only started / stopped later */
    s->Play.HostDma = s->Oss >= 2 ? 1 : 0;
    s->Rec.HostDma = s->Iss >= 2 ? 1 : 0;
    if (s->PlayEp) {
        BOOLEAN ok = p->Fw == SST_FW_SOF3 ? Ipc3PathCreate(s, &s->Play, s->PlayEp, FALSE)
                                          : Ipc4PathCreate(s, &s->Play, s->PlayEp, FALSE);
        if (!ok) { LOGE("sst: playback path to %s not created", s->PlayEp->Name); s->PlayEp = NULL; }
    }
    for (i = 0; i < LINE_COUNT; i++) {
        const SST_EP *e = s->RecEp[i];
        const char *sh = NULL, *lg = NULL;
        if (!e) continue;
        /* the mixer shows what the input is, not "Aux" */
        if (e->Kind == EP_DMIC || (e->Kind == EP_SDW && i == LINE_AUX)) { sh = "Internal Mic"; lg = "Internal Microphone"; }
        else if (e->Kind == EP_SDW && i == LINE_MIC) { sh = "Headset Mic"; lg = "Headset Microphone"; }
        else if (e->Kind == EP_SSP && i == LINE_AUX) { sh = "I2S Mic"; lg = "I2S Microphone"; }
        if (sh && !c->LineShort[i][0]) {
            OsSprintf(c->LineShort[i], sizeof(c->LineShort[i]), "%s", sh);
            OsSprintf(c->LineLong[i], sizeof(c->LineLong[i]), "%s", lg);
        }
        c->RecSrcMask |= 1UL << i;
        c->Lines[i].Present = TRUE;
        c->CanRecord = TRUE;
    }
    if (s->PlayEp) c->Lines[LINE_MASTER].Present = TRUE;
    LOGI("sst: ready, %u endpoints, playback %s", s->NEp, s->PlayEp ? s->PlayEp->Name : "HD Audio codec");
    return TRUE;
}

void SstShutdown(CARD *c)
{
    SST *s = Of(c);
    if (!s) return;
    if (s->Dsp) {
        if (s->Play.Created) (s->P->Fw == SST_FW_SOF3 ? Ipc3PathDelete : Ipc4PathDelete)(s, &s->Play);
        if (s->Rec.Created) (s->P->Fw == SST_FW_SOF3 ? Ipc3PathDelete : Ipc4PathDelete)(s, &s->Rec);
        SdwShutdown(s);                                 /* the shim links of cAVS need the DSP BAR */
        SstCodecsShutdown(s);
        CoreDisable(s, (1UL << s->P->Cores) - 1);
        if (ACE(s)) {
            DspUpdate(s, MTL_HFDSSCS, 1UL << 16, 0);
            Udelay(1000);
        }
        if (s->PpOff) HDAW32(s, s->PpOff + PPCTL, HDA32(s, s->PpOff + PPCTL) & ~(PPCTL_GPROCEN | 0x3FFFFFFFUL));
        OsUnmapMmio(s->Dsp, s->DspSize);
    } else SdwShutdown(s);
    if (s->RecWide.Va) OsFreeDma(&s->RecWide);
    if (s->Img) OsFree(s->Img);
    OsFree(s);
    c->Sst = NULL;
}

BOOLEAN SstPlayActive(CARD *c)
{
    SST *s = Of(c);
    return s && s->PlayEp && s->Play.Created;
}

static SST_EP *CurRecEp(CARD *c)
{
    SST *s = Of(c);
    ULONG k;
    if (!s) return NULL;
    for (k = 0; k < LINE_COUNT; k++) if ((c->RecSrc & (1UL << k)) && s->RecEp[k]) return s->RecEp[k];
    return NULL;
}

BOOLEAN SstRecActive(CARD *c) { return CurRecEp(c) != NULL; }

/* DSP host streams: the second stream of each direction (the first stays with the codec) */
ULONG SstPlaySd(CARD *c) { SST *s = Of(c); return s->Iss + s->Play.HostDma; }
ULONG SstRecSd(CARD *c) { SST *s = Of(c); return s->Rec.HostDma; }

ULONG SstRecChannels(CARD *c)
{
    SST *s = Of(c);
    SST_EP *e = CurRecEp(c);
    if (!s || !e) return 2;
    return e->Be.Ch > 2 && e->Kind == EP_DMIC ? e->Be.Ch : 2;
}

DMA_BUFFER *SstRecRing(CARD *c)
{
    SST *s = Of(c);
    ULONG ch = SstRecChannels(c);
    if (ch <= 2) return &c->RecRing;
    if (!s->RecWide.Va || s->RecCh != ch) {
        if (s->RecWide.Va) OsFreeDma(&s->RecWide);
        if (!OsAllocDma(&s->RecWide, RING_FRAMES * ch * 2, c->DmaUncached)) return &c->RecRing;
        s->RecCh = ch;
    }
    return &s->RecWide;
}

/* frames [RecDone, toFrame) of the wide ring -> channels 0 and 1 of the engine ring */
void SstRecCopy(CARD *c, ULONG to)
{
    SST *s = Of(c);
    SHORT *src, *dst = (SHORT *)c->RecRing.Va;
    ULONG f, ch;
    if (!s || !s->RecWide.Va || SstRecChannels(c) <= 2) return;
    src = (SHORT *)s->RecWide.Va;
    ch = s->RecCh;
    for (f = s->RecDone; f != to; f = (f + 1) % RING_FRAMES) {
        dst[f * 2] = src[f * ch];
        dst[f * 2 + 1] = src[f * ch + 1];
    }
    s->RecDone = to;
}

BOOLEAN SstStart(CARD *c, BOOLEAN rec)
{
    SST *s = Of(c);
    SST_PATH *p;
    SST_EP *e;
    if (!s) return FALSE;
    p = rec ? &s->Rec : &s->Play;
    e = rec ? CurRecEp(c) : s->PlayEp;
    if (!e) return FALSE;
    if (rec && (!p->Created || p->Ep != e)) {
        /* record source changed: rebuild the capture path (the engine keeps the lock: short IPCs only) */
        if (p->Created) (s->P->Fw == SST_FW_SOF3 ? Ipc3PathDelete : Ipc4PathDelete)(s, p);
        if (!(s->P->Fw == SST_FW_SOF3 ? Ipc3PathCreate : Ipc4PathCreate)(s, p, e, TRUE)) return FALSE;
    }
    if (rec) s->RecDone = 0;
    SstCodecsPower(s, e, TRUE);
    SdwPower(s, e, TRUE);
    Decouple(s, rec ? SstRecSd(c) : SstPlaySd(c), TRUE);
    return (s->P->Fw == SST_FW_SOF3 ? Ipc3PathRun : Ipc4PathRun)(s, p, TRUE, rec);
}

void SstStop(CARD *c, BOOLEAN rec)
{
    SST *s = Of(c);
    SST_PATH *p;
    if (!s) return;
    p = rec ? &s->Rec : &s->Play;
    if (!p->Created) return;
    (s->P->Fw == SST_FW_SOF3 ? Ipc3PathRun : Ipc4PathRun)(s, p, FALSE, rec);
    SstCodecsPower(s, p->Ep, FALSE);
    SdwPower(s, p->Ep, FALSE);
}

ULONG SstDump(CARD *c, char *buf, ULONG size)
{
    SST *s = Of(c);
    ULONG n = 0, i;
    if (!s) return 0;
    n += OsSprintf(buf + n, size - n, "\r\nSmart Sound DSP: %s, %s firmware %s, %s\r\n", s->P->Name,
                   s->P->Fw == SST_FW_AVS ? "Intel" : "SOF", s->FwVer, s->P->File);
    n += OsSprintf(buf + n, size - n, "DSP BAR %08x, status %08x error %08x, IPC errors %u\r\n", s->DspPa,
                   DSP32(s, s->StatusOff), DSP32(s, s->StatusOff + 4), s->Errors);
    for (i = 0; i < s->NEp && n + 120 < size; i++) {
        SST_EP *e = &s->Ep[i];
        static const char *k[] = { "?", "HDA link", "DMIC", "SSP", "SoundWire" };
        n += OsSprintf(buf + n, size - n, "  %s %s port %u: %u ch %u/%u bit %u Hz%s%s%s%s\r\n", k[e->Kind & 7],
                       e->Dir ? "capture" : "playback", e->Port, e->Be.Ch, e->Be.Valid, e->Be.Bits, e->Be.Rate,
                       e == s->PlayEp ? " [playback]" : "", e->Codec ? " +codec" : "", e->Kind == EP_SDW ? " " : "",
                       e->Kind == EP_SDW ? e->Name : "");
    }
    n += SstCodecsDump(s, buf + n, size - n);
    n += SdwDump(s, buf + n, size - n);
    return n;
}
