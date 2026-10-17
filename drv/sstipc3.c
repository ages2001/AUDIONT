/*
 * AUDIONT - SOF IPC3 (Ice Lake, Jasper Lake, Elkhart Lake; Bay Trail, Cherry
 * Trail and Broadwell through sstatom.c / sstbdw.c).
 *
 * A path is one pipeline:
 *   playback:  host (HD Audio host DMA / host page table) -> buffer -> DAI
 *   capture:   DAI -> buffer -> host
 * scheduled by the DAI, 1 ms period.  The DAI (SSP, DMIC, ALH) is configured
 * with DAI_CONFIG; the DAI component converts between the 16-bit buffer
 * format and the wire format.  PCM_PARAMS names the host stream, TRIG_START /
 * TRIG_STOP run it.
 */
#include "sdw.h"

#define G32U(p) ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8) | ((ULONG)(p)[2] << 16) | ((ULONG)(p)[3] << 24))

#define GLB(x)   ((ULONG)(x) << 28)
#define CMD(x)   ((ULONG)(x) << 16)
#define TPLG     GLB(3)
#define STREAM   GLB(6)
#define FW_READY GLB(7)
#define DAI_MSG  GLB(8)
#define COMP_NEW      (TPLG | CMD(0x001))
#define COMP_FREE     (TPLG | CMD(0x002))
#define COMP_CONNECT  (TPLG | CMD(0x003))
#define PIPE_NEW      (TPLG | CMD(0x010))
#define PIPE_FREE     (TPLG | CMD(0x011))
#define PIPE_COMPLETE (TPLG | CMD(0x013))
#define BUFFER_NEW    (TPLG | CMD(0x020))
#define BUFFER_FREE   (TPLG | CMD(0x021))
#define DAI_CONFIG    (DAI_MSG | CMD(0x001))
#define PCM_PARAMS    (STREAM | CMD(0x001))
#define PCM_FREE      (STREAM | CMD(0x003))
#define TRIG_START    (STREAM | CMD(0x004))
#define TRIG_STOP     (STREAM | CMD(0x005))

#define COMP_HOST    1
#define COMP_DAI     2
#define COMP_BUFFER  12
#define DAI_SSP      1
#define DAI_DMIC     2
#define DAI_HDA      3
#define DAI_ALH      4
#define FRAME_S16    0
#define FRAME_S24    1
#define FRAME_S32    2

static ULONG *Msg(SST *s, ULONG cmd, ULONG size)
{
    ULONG *m = (ULONG *)s->Tx;
    memset(m, 0, size);
    m[0] = size;
    m[1] = cmd;
    return m;
}

static BOOLEAN Send(SST *s, ULONG size)
{
    return SstIpc(s, 0, 0, s->Tx, size, 300);
}

/* ------------------------------------------------------------------ */
/* FW_READY                                                             */
/* ------------------------------------------------------------------ */
static void ParseWindows(SST *s, ULONG off)
{
    UCHAR h[12];
    ULONG guard;
    for (guard = 0; guard < 16; guard++) {
        ULONG size, type;
        SstMemFromDsp(s, off, h, 12);
        size = G32U(h); type = G32U(h + 8);
        if (G32U(h + 4) != FW_READY || size < 12 || size > 2048) break;
        if (type == 1) {                                    /* SOF_IPC_EXT_WINDOW */
            UCHAR w[12 + 4 + 16 * 24];
            ULONG n, i;
            SstMemFromDsp(s, off, w, MIN(size, sizeof(w)));
            n = MIN(G32U(w + 12), 16);
            for (i = 0; i < n; i++) {
                const UCHAR *e = w + 16 + i * 24;
                ULONG rtype = G32U(e + 4), id = G32U(e + 8), sz = G32U(e + 16), eoff = G32U(e + 20);
                ULONG at = s->SramBase + id * s->SramWin + eoff;
                if (rtype == 1) { s->UpOff = at; s->UpSize = MIN(sz, SST_MBOX); }       /* UPBOX: DSP -> host */
                else if (rtype == 0) { s->DownOff = at; s->DownSize = MIN(sz, SST_MBOX); } /* DOWNBOX */
                else if (rtype == 4) s->StreamOff = at;                                 /* STREAM: positions */
                LOGV("sst: window type %u id %u offset %x size %x", rtype, id, eoff, sz);
            }
        }
        off += size;
    }
}

void Ipc3Notify(SST *s)
{
    UCHAR h[8];
    SstMemFromDsp(s, s->UpOff, h, 8);
    if ((G32U(h + 4) & 0xF0000000UL) == FW_READY && !s->Ready) {
        UCHAR r[108];
        SstMemFromDsp(s, s->UpOff, r, sizeof(r));
        s->Ipc3Abi = G32U(r + 64);
        OsSprintf(s->FwVer, sizeof(s->FwVer), "%u.%u.%u (ABI %u.%u.%u)", r[28] | (r[29] << 8), r[30] | (r[31] << 8),
                  r[32] | (r[33] << 8), s->Ipc3Abi >> 24, (s->Ipc3Abi >> 12) & 0xFFF, s->Ipc3Abi & 0xFFF);
        ParseWindows(s, s->UpOff + sizeof(r));
        s->Ready = TRUE;
        LOGI("sst: SOF firmware %s ready, host box %x dsp box %x", s->FwVer, s->DownOff, s->UpOff);
    } else LOGT("sst: IPC3 message %08x", G32U(h + 4));
}

BOOLEAN Ipc3WaitReady(SST *s, ULONG ms)
{
    ULONG t, pri, ex;
    for (t = 0; t < ms && !s->Ready; t++) {
        while (SstRxPending(s, &pri, &ex)) {
            Ipc3Notify(s);
            SstRxDone(s);
        }
        if (!s->Ready) OsSleepMs(1);
    }
    return s->Ready;
}

BOOLEAN Ipc3Init(SST *s)
{
    (void)s;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* topology                                                             */
/* ------------------------------------------------------------------ */
static BOOLEAN PipeNew(SST *s, ULONG comp, ULONG pipe, ULONG sched)
{
    ULONG *m = Msg(s, PIPE_NEW, 48);
    m[2] = comp; m[3] = pipe; m[4] = sched; m[5] = 0 /* core */;
    m[6] = 1000;                                           /* period us */
    m[7] = 0;                                              /* priority */
    m[8] = 5000;                                           /* period mips */
    m[9] = 0;                                              /* frames per sched */
    m[10] = 1000;                                          /* xrun limit us */
    m[11] = 1;                                             /* timer domain */
    return Send(s, 48);
}

/* sof_ipc_comp (28 bytes) + sof_ipc_comp_config (36 bytes) */
static ULONG *CompHdr(SST *s, ULONG id, ULONG type, ULONG pipe, ULONG size, ULONG psink, ULONG psrc)
{
    ULONG *m = Msg(s, COMP_NEW, size);
    m[2] = id; m[3] = type; m[4] = pipe; m[5] = 0; m[6] = 0;     /* core, ext data length */
    m[7] = 36; m[8] = 0;                                         /* config header */
    m[9] = psink; m[10] = psrc; m[11] = 0; m[12] = FRAME_S16; m[13] = 1 /* xrun stop */;
    return m;
}

static BOOLEAN Host(SST *s, ULONG id, ULONG pipe, BOOLEAN rec)
{
    ULONG *m = CompHdr(s, id, COMP_HOST, pipe, 19 * 4, rec ? 0 : 2, rec ? 2 : 0);
    m[16] = rec ? 1 : 0;                                   /* direction */
    m[17] = 0;                                             /* no_irq */
    m[18] = 0;                                             /* dmac config */
    return Send(s, 19 * 4);
}

static BOOLEAN Dai(SST *s, ULONG id, ULONG pipe, BOOLEAN rec, ULONG type, ULONG index)
{
    ULONG *m = CompHdr(s, id, COMP_DAI, pipe, 20 * 4, rec ? 2 : 0, rec ? 0 : 2);
    m[16] = rec ? 1 : 0;
    m[17] = index;
    m[18] = type;
    m[19] = 0;
    return Send(s, 20 * 4);
}

static BOOLEAN Buffer(SST *s, ULONG id, ULONG pipe, ULONG size)
{
    ULONG *m = Msg(s, BUFFER_NEW, 11 * 4);
    m[2] = id; m[3] = COMP_BUFFER; m[4] = pipe; m[5] = 0; m[6] = 0;
    m[7] = size;
    m[8] = 0x01 | 0x20;                                    /* RAM | DMA */
    m[9] = 0; m[10] = 0;
    return Send(s, 11 * 4);
}

static BOOLEAN Connect(SST *s, ULONG src, ULONG sink)
{
    ULONG *m = Msg(s, COMP_CONNECT, 16);
    m[2] = src; m[3] = sink;
    return Send(s, 16);
}

static BOOLEAN Free(SST *s, ULONG cmd, ULONG id)
{
    ULONG *m = Msg(s, cmd, 12);
    m[2] = id;
    return Send(s, 12);
}

/* SoundWire: the firmware's ALH DAI index is the Linux DAI index (link << 8 | pin) minus
   INTEL_ALH_DAI_INDEX_BASE (2, ipc3-topology.c); aggregated amplifiers: the first link only */
static ULONG AlhDaiIndex(const SST_EP *e) { return (((ULONG)e->Port << 8) | e->Pdi) - 2; }
static ULONG AlhStreamId(const SST_EP *e) { return e->Port * 16UL + e->Pdi + 3 + (e->Pdi >= 2 ? 2 : 0); }

/* sof_ipc_dai_config: hdr(8) type index format(2) group(1) flags(1) reserved[8] + params */
static BOOLEAN DaiConfig(SST *s, SST_EP *e)
{
    ULONG size = 52 + 164, *m;
    UCHAR *p;
    m = Msg(s, DAI_CONFIG, size);
    p = (UCHAR *)m;
    if (e->Kind == EP_DMIC) {
        ULONG *d = (ULONG *)(p + 52);
        m[2] = DAI_DMIC; m[3] = 0;
        *(USHORT *)(p + 16) = 6;                           /* PDM */
        d[0] = 164;                                         /* params size with four PDM controllers */
        d[1] = 1;                                           /* driver ipc version */
        d[2] = 500000;                                      /* pdm clock min */
        d[3] = 4800000;                                     /* pdm clock max */
        d[4] = 48000;                                       /* fifo fs */
        d[5] = 0;
        *(USHORT *)&d[6] = 16;                              /* fifo bits */
        *((USHORT *)&d[6] + 1) = 0;
        *(USHORT *)&d[7] = 40;                              /* duty min */
        *((USHORT *)&d[7] + 1) = 60;                        /* duty max */
        d[8] = 1;                                           /* one PDM controller (2 mics) */
        d[9] = 0; d[10] = 0; d[11] = 400;                   /* wake up, clock on, unmute ramp */
        /* pdm[0] at d[17]: hdr, id, enable a, enable b */
        d[17] = 24;
        *(USHORT *)&d[18] = 0;
        *((USHORT *)&d[18] + 1) = 1;
        *(USHORT *)&d[19] = 1;
        e->Be.Ch = 2; e->Be.Bits = 16; e->Be.Valid = 16;
    } else if (e->Kind == EP_SSP) {
        ULONG *d = (ULONG *)(p + 52);
        m[2] = DAI_SSP; m[3] = e->Port;
        *(USHORT *)(p + 16) = 1 | (4 << 12);               /* I2S, codec bit clock and frame consumer */
        d[0] = 60;
        *((USHORT *)&d[1] + 1) = 0;                         /* mclk id */
        ULONG mclk, bclk;
        UCHAR slot, valid;
        SstSspFormat(s, e->Port, &mclk, &bclk, &slot, &valid);  /* the codecs are set up for the same */
        d[2] = mclk;
        d[3] = 48000;                                       /* fsync */
        d[4] = bclk;
        d[5] = 2; d[6] = 3; d[7] = 3;                       /* tdm slots, rx / tx slot masks */
        d[8] = valid;                                       /* valid bits */
        *(USHORT *)&d[9] = slot;                            /* slot width */
        d[10] = 0;                                          /* mclk direction */
        e->Be.Ch = 2; e->Be.Bits = slot > 16 ? 32 : 16; e->Be.Valid = valid;
    } else if (e->Kind == EP_SDW) {
        /* sof_ipc_dai_alh_params: hdr, stream id (the ALH stream of the PDI), rate, channels */
        ULONG *d = (ULONG *)(p + 52);
        m[2] = DAI_ALH; m[3] = AlhDaiIndex(e);
        d[0] = 68; d[1] = AlhStreamId(e); d[2] = 48000; d[3] = e->Be.Ch;
    } else return FALSE;
    return Send(s, size);
}

BOOLEAN Ipc3PathCreate(SST *s, SST_PATH *p, SST_EP *e, BOOLEAN rec)
{
    ULONG base = rec ? 20 : 10, pipe = rec ? 2 : 1, hostDma = p->HostDma;
    ULONG type = e->Kind == EP_DMIC ? DAI_DMIC : e->Kind == EP_SSP ? DAI_SSP : e->Kind == EP_SDW ? DAI_ALH : 0;
    ULONG index = e->Kind == EP_SSP ? e->Port : e->Kind == EP_SDW ? AlhDaiIndex(e) : 0;
    memset(p, 0, sizeof(*p));
    p->HostDma = hostDma;
    p->Ep = e;
    p->Fe.Rate = 48000; p->Fe.Ch = 2; p->Fe.Bits = 16; p->Fe.Valid = 16;
    if (!type) return FALSE;
    p->Pipe[0] = base;
    p->Comp[0] = base + 1;                                  /* host */
    p->Comp[1] = base + 2;                                  /* buffer */
    p->Comp[2] = base + 3;                                  /* dai */
    p->NComp = 3;
    if (!PipeNew(s, base, pipe, base + 3)) return FALSE;
    p->Created = TRUE;
    if (!Host(s, base + 1, pipe, rec) || !Buffer(s, base + 2, pipe, 2 * 48 * 2 * 2 * 2) ||
        !Dai(s, base + 3, pipe, rec, type, index))
        goto fail;
    if (rec ? !(Connect(s, base + 3, base + 2) && Connect(s, base + 2, base + 1))
            : !(Connect(s, base + 1, base + 2) && Connect(s, base + 2, base + 3)))
        goto fail;
    if (!DaiConfig(s, e)) goto fail;
    {
        ULONG *m = Msg(s, PIPE_COMPLETE, 12);
        m[2] = base;
        if (!Send(s, 12)) goto fail;
    }
    LOGI("sst: IPC3 %s pipeline %u to %s created", rec ? "capture" : "playback", pipe, e->Name);
    return TRUE;
fail:
    LOGE("sst: IPC3 %s pipeline to %s failed", rec ? "capture" : "playback", e->Name);
    Ipc3PathDelete(s, p);
    return FALSE;
}

void Ipc3PathDelete(SST *s, SST_PATH *p)
{
    if (!p->Created) return;
    if (p->Running) Ipc3PathRun(s, p, FALSE, FALSE);
    Free(s, PIPE_FREE, p->Pipe[0]);                         /* frees its components and buffers */
    p->Created = FALSE;
}

BOOLEAN Ipc3PathRun(SST *s, SST_PATH *p, BOOLEAN run, BOOLEAN rec)
{
    ULONG *m;
    if (!p->Created) return FALSE;
    if (run) {
        /* sof_ipc_pcm_params: hdr, comp, flags, reserved[2], sof_ipc_stream_params (84 bytes) */
        ULONG size = 24 + 84, *q;
        m = Msg(s, PCM_PARAMS, size);
        m[2] = p->Comp[0];
        q = &m[6];
        q[0] = 84;                                          /* params hdr.size */
        q[1] = 28;                                          /* host buffer (unused with HD Audio host DMA) */
        q[8] = rec ? 1 : 0;                                 /* direction */
        q[9] = FRAME_S16;
        q[10] = 0;                                          /* interleaved */
        q[11] = 48000;
        ((USHORT *)&q[12])[0] = (USHORT)(p->HostDma + 1);   /* stream tag */
        ((USHORT *)&q[12])[1] = 2;                          /* channels */
        ((USHORT *)&q[13])[0] = 2;                          /* valid bytes */
        ((USHORT *)&q[13])[1] = 2;                          /* container bytes */
        q[14] = PERIOD_BYTES;
        ((USHORT *)&q[15])[0] = s->Lpe ? 0 : 1;             /* position messages: only without DPIB */
        ((USHORT *)&q[17])[0] = 3; ((USHORT *)&q[17])[1] = 4;   /* channel map FL FR */
        if (p->HostBufPa) {                                 /* DSPs with their own DMA: page table of the ring */
            q[2] = p->HostBufPa; q[3] = p->HostBufPages; q[4] = p->HostBufSize;
        }
        if (!Send(s, size)) return FALSE;
        p->PosnOff = s->RxLen >= 20 ? G32U(s->Rx + 16) : 0;     /* sof_ipc_pcm_params_reply.posn_offset */
        m = Msg(s, TRIG_START, 12);
        m[2] = p->Comp[0];
        p->Running = Send(s, 12);
        (void)rec;
        return p->Running;
    }
    m = Msg(s, TRIG_STOP, 12);
    m[2] = p->Comp[0];
    Send(s, 12);
    m = Msg(s, PCM_FREE, 12);
    m[2] = p->Comp[0];
    Send(s, 12);
    p->Running = FALSE;
    return TRUE;
}
