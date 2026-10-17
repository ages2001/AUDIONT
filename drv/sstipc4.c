/*
 * AUDIONT - IPC4 (Intel cAVS AudioDSP firmware and SOF IPC4): firmware
 * configuration and copier pipelines.
 *
 * A path is two pipelines with one copier each, as in the Linux avs
 * topologies (avs-topology-xml hda / dmic / i2s) and the SOF IPC4 ones:
 *
 *   playback:  [copier host DMA out]  ->  [copier gateway out: SSP / HDA link / ALH]
 *   capture:   [copier gateway in: DMIC / SSP / HDA link / ALH]  ->  [copier host DMA in]
 *
 * The gateway copier carries the NHLT blob of its endpoint (DMIC, SSP); the
 * copiers convert sample formats (16 <-> 24/32 bit) between host and wire.
 * On ACE 2.0+ (Lunar Lake and later) DMIC / SSP / SoundWire gateways use HD
 * Audio link DMA: a DMA configuration TLV names the link stream the host
 * programmed.  SoundWire (ALH) gateways carry the ALH blob of sdw.c (gateway
 * attributes + ALH stream / channel mask per link) and, on ACE 2.0+, one DMA
 * configuration TLV per link (hda-dai.c sdw_hda_dai_hw_params): the link
 * stream, and the link / PDI the SoundWire shim PCMSyCM maps to its tag.
 */
#include "sdw.h"

#define G32U(p) ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8) | ((ULONG)(p)[2] << 16) | ((ULONG)(p)[3] << 24))

/* global messages */
#define GLB_CREATE_PIPELINE     17
#define GLB_DELETE_PIPELINE     18
#define GLB_SET_PIPELINE_STATE  19
/* module messages */
#define MOD_INIT_INSTANCE       0
#define MOD_LARGE_CONFIG_GET    3
#define MOD_LARGE_CONFIG_SET    4
#define MOD_BIND                5
#define MOD_UNBIND              6
#define MOD_DELETE_INSTANCE     11

#define PPL_RESET    2
#define PPL_PAUSED   3
#define PPL_RUNNING  4

/* gateway (node) types */
#define NODE_HOST_OUT   0
#define NODE_HOST_IN    1
#define NODE_LINK_OUT   8
#define NODE_LINK_IN    9
#define NODE_DMIC_IN    11
#define NODE_I2S_OUT    12
#define NODE_I2S_IN     13
#define NODE_ALH_OUT    16
#define NODE_ALH_IN     17

static ULONG GlbMsg(ULONG type) { return type << 24; }
static ULONG ModMsg(ULONG type, ULONG mod, ULONG inst) { return (1UL << 30) | (type << 24) | (inst << 16) | (mod & 0xFFFF); }

/* base firmware runtime parameters (LARGE_CONFIG_GET on module 0) */
static BOOLEAN GetParam(SST *s, ULONG param)
{
    ULONG ext = (1UL << 29) /* init block */ | (param << 20);
    return SstIpc(s, ModMsg(MOD_LARGE_CONFIG_GET, 0, 0), ext, NULL, 0, 200);
}

static BOOLEAN SetParam(SST *s, ULONG param, const void *data, ULONG len)
{
    ULONG ext = (1UL << 29) | (1UL << 28) | (param << 20) | (len & 0xFFFFF);
    return SstIpc(s, ModMsg(MOD_LARGE_CONFIG_SET, 0, 0), ext, data, len, 200);
}

BOOLEAN Ipc4Init(SST *s)
{
    ULONG o;
    s->MaxPpl = 8;
    if (GetParam(s, 7)) {                                 /* FIRMWARE_CONFIG: TLVs */
        for (o = 0; o + 8 <= s->RxLen; ) {
            ULONG t = G32U(s->Rx + o), l = G32U(s->Rx + o + 4);
            const UCHAR *v = s->Rx + o + 8;
            if (o + 8 + l > s->RxLen) break;
            if (t == 0 && l >= 8 && !s->FwVer[0])
                OsSprintf(s->FwVer, sizeof(s->FwVer), "%u.%u.%u.%u", v[0] | (v[1] << 8), v[2] | (v[3] << 8),
                          v[4] | (v[5] << 8), v[6] | (v[7] << 8));
            if (t == 6 && l >= 4 && G32U(v) >= 256) s->DownSize = MIN(G32U(v), SST_MBOX);
            if (t == 7 && l >= 4 && G32U(v) >= 256) s->UpSize = MIN(G32U(v), SST_MBOX);
            if (t == 9 && l >= 4 && G32U(v)) s->MaxPpl = G32U(v);
            o += 8 + ((l + 3) & ~3UL);
        }
        LOGI("sst: firmware config: %u pipelines, mailboxes %u / %u", s->MaxPpl, s->DownSize, s->UpSize);
    } else LOGI("sst: no firmware config (continuing)");
    if (GetParam(s, 8) && s->RxLen >= 16) {               /* HARDWARE_CONFIG */
        for (o = 0; o + 8 <= s->RxLen; ) {
            ULONG t = G32U(s->Rx + o), l = G32U(s->Rx + o + 4);
            if (t == 1 && l >= 4) LOGI("sst: %u DSP cores", G32U(s->Rx + o + 8));
            o += 8 + ((l + 3) & ~3UL);
        }
    }
    if (s->ModCopier == 0xFFFF) {
        /* not in the manifest table: the copier is module 1 in every known image after BASEFW */
        s->ModCopier = 1;
        LOGI("sst: copier module id assumed 1");
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* formats                                                              */
/* ------------------------------------------------------------------ */
typedef struct _AFMT {
    ULONG Rate, Bits, Map, Cfg, Interleave, Packed;
} AFMT;

static void Fmt(AFMT *a, const SST_FMT *f)
{
    static const ULONG cfg[9] = { 0, 0, 1, 3, 5, 7, 8, 8, 12 };
    ULONG i, map = 0xFFFFFFFFUL;
    for (i = 0; i < f->Ch && i < 8; i++) map = (map & ~(0xFUL << (4 * i))) | (i << (4 * i));
    a->Rate = f->Rate;
    a->Bits = f->Bits;
    a->Map = map;
    a->Cfg = cfg[f->Ch <= 8 ? f->Ch : 8];
    a->Interleave = 0;
    a->Packed = f->Ch | ((ULONG)f->Valid << 8) | (0UL << 16);
}

static ULONG Ms(const SST_FMT *f) { return f->Rate / 1000 * f->Ch * (f->Bits / 8); }

/* copier INIT_INSTANCE payload; returns its length */
static ULONG LinkStreamFor(SST *s, BOOLEAN rec, ULONG i);

/* sof_ipc4_dma_config_tlv: HD Audio link DMA (pre-allocated by the host), channel = stream index in its
   direction, stream id = tag; one device / channel mask mapping (SoundWire: link << 8 | PDI) */
static ULONG DmaTlv(UCHAR *at, ULONG chan, ULONG device, ULONG mask)
{
    ULONG *t = (ULONG *)at;
    memset(t, 0, 8 + 148);
    t[0] = 0x1000;                                        /* GTW_DMA_CONFIG_ID */
    t[1] = 148;
    at[8] = 1;                                            /* dma_method HDA */
    at[9] = 1;                                            /* pre-allocated */
    t[3] = chan;                                          /* dma channel */
    t[4] = chan + 1;                                      /* stream id */
    if (mask) { t[5] = 1; t[6] = device; t[7] = mask; }   /* dma_stream_channel_map */
    return 8 + 148;
}

static ULONG CopierCfg(SST *s, UCHAR *b, const SST_FMT *in, const SST_FMT *out, ULONG node, ULONG dmaBuf,
                       const UCHAR *blob, ULONG blobLen, ULONG linkDma, const SST_EP *gw, BOOLEAN rec)
{
    ULONG *w = (ULONG *)b, n;
    AFMT a;
    memset(b, 0, 96);
    w[0] = 10000;                                         /* cpc */
    w[1] = Ms(in);                                        /* ibs */
    w[2] = Ms(out);                                       /* obs */
    w[3] = 0;                                             /* is_pages */
    Fmt(&a, in);  memcpy(&w[4], &a, sizeof(a));           /* audio format (in) */
    Fmt(&a, out); memcpy(&w[10], &a, sizeof(a));          /* out format */
    w[16] = 0;                                            /* feature mask */
    w[17] = node;
    w[18] = dmaBuf;
    n = 19 * 4;
    if (blob && blobLen >= 4) {
        if (n + blobLen + 160 > s->DownSize) { LOGE("sst: gateway blob of %u bytes too large", blobLen); blobLen = 4; }
        w[19] = blobLen / 4;
        memcpy(b + 20 * 4, blob, blobLen);
        n = 20 * 4 + blobLen;
    } else {
        w[19] = 1;                                        /* gateway attributes only */
        w[20] = 0;
        n = 21 * 4;
    }
    if (linkDma != 0xFFFFFFFFUL && gw && gw->Kind == EP_SDW) {
        /* one TLV per SoundWire link of the gateway, in the order of the ALH blob mappings */
        UCHAR lk[4], pdi[4], ch[4];
        ULONG k, ns = SdwEpStreams(s, gw, lk, pdi, ch, 4), t;
        for (k = 0; k < ns && n + 8 + 148 <= s->DownSize; k++) {
            ULONG st = LinkStreamFor(s, rec, k);
            t = DmaTlv(b + n, rec ? st : st - s->Iss, ((ULONG)lk[k] << 8) | pdi[k], (1UL << ch[k]) - 1);
            n += t;
            w[19] += t / 4;
        }
    } else if (linkDma != 0xFFFFFFFFUL) {
        /* ACE 2.0+: DMA configuration TLV (link DMA chosen by the host) */
        ULONG t = DmaTlv(b + n, linkDma, 0, 0);
        n += t;
        w[19] += t / 4;
    }
    return n;
}

static BOOLEAN CreatePipeline(SST *s, UCHAR id)
{
    ULONG pri = GlbMsg(GLB_CREATE_PIPELINE) | (4UL /* pages */) | (0UL << 11) | ((ULONG)id << 16);
    return SstIpc(s, pri, 0, NULL, 0, 200);
}

static void DeletePipeline(SST *s, UCHAR id)
{
    SstIpc(s, GlbMsg(GLB_DELETE_PIPELINE) | ((ULONG)id << 16), 0, NULL, 0, 200);
}

static BOOLEAN SetState(SST *s, UCHAR id, ULONG state)
{
    return SstIpc(s, GlbMsg(GLB_SET_PIPELINE_STATE) | state | ((ULONG)id << 16), 0, NULL, 0, 200);
}

static BOOLEAN InitCopier(SST *s, UCHAR inst, UCHAR ppl, const UCHAR *cfg, ULONG len)
{
    ULONG ext = ((len + 3) / 4) | ((ULONG)ppl << 16);    /* param block size (dwords), pipeline, core 0 */
    return SstIpc(s, ModMsg(MOD_INIT_INSTANCE, s->ModCopier, inst), ext, cfg, len, 300);
}

static BOOLEAN Bind(SST *s, UCHAR src, UCHAR dst)
{
    ULONG ext = s->ModCopier | ((ULONG)dst << 16);        /* dst module, dst instance, queues 0 */
    return SstIpc(s, ModMsg(MOD_BIND, s->ModCopier, src), ext, NULL, 0, 200);
}

/* ------------------------------------------------------------------ */
/* ACE link handling                                                    */
/* ------------------------------------------------------------------ */
#define ELID_SDW  0x00
#define ELID_SSP  0xC0
#define ELID_DMIC 0xC1

static ULONG EpElid(SST_EP *e) { return e->Kind == EP_DMIC ? ELID_DMIC : e->Kind == EP_SSP ? ELID_SSP : ELID_SDW; }

/* link stream used for a gateway on ACE 2+: the last streams, one per direction; aggregated SoundWire
   streams (amplifiers on two links) take the next ones (playback: the last output stream, which only
   carries the firmware image at boot) */
static ULONG LinkStreamFor(SST *s, BOOLEAN rec, ULONG i)
{
    if (rec) return s->Iss > i ? s->Iss - 1 - i : 0;
    return s->Iss + s->Oss - 2 + i;
}

static void LinkDmaGateway(SST *s, SST_PATH *p, BOOLEAN rec, BOOLEAN on)
{
    SST_EP *e = p->Ep;
    ULONG l = SstMlLink(s, EpElid(e)), tag;
    SST_FMT f = e->Be;
    if (s->P->Gen < SST_LNL || e->Kind == EP_HDA || !l) return;
    if (e->Kind == EP_SDW) {
        /* per link: LOSIDV of the SoundWire extended link, link DMA, PCMSyCM PDI -> stream tag */
        UCHAR lk[4], pdi[4], ch[4];
        ULONG k, ns = SdwEpStreams(s, e, lk, pdi, ch, 4);
        for (k = 0; k < ns; k++) {
            ULONG st = LinkStreamFor(s, rec, k);
            tag = (rec ? st : st - s->Iss) + 1;
            f.Ch = ch[k];
            if (on) {
                if (!rec) HDAW16(s, l + 0x08, (USHORT)(HDA16(s, l + 0x08) | (1U << tag)));
                SdwLinkStream(s, lk[k], pdi[k], ch[k], tag, rec);
                SstLinkDmaSetup(s, st, tag, SstHdaFmt(&f), TRUE);
            } else {
                SstLinkDmaSetup(s, st, tag, 0, FALSE);
                SdwLinkStream(s, lk[k], pdi[k], ch[k], 0, rec);
                if (!rec) HDAW16(s, l + 0x08, (USHORT)(HDA16(s, l + 0x08) & ~(1U << tag)));
            }
        }
        return;
    }
    tag = (rec ? p->LinkDma : p->LinkDma - s->Iss) + 1;
    if (e->Kind == EP_DMIC && f.Bits == 16) { f.Bits = 32; f.Valid = 32; f.Ch = (USHORT)MAX(1, f.Ch / 2); }
    /* LOSIDV (link output stream id valid): output streams only (hda-dai.c) */
    if (on) {
        if (!rec) HDAW16(s, l + 0x08, (USHORT)(HDA16(s, l + 0x08) | (1U << tag)));
        SstLinkDmaSetup(s, p->LinkDma, tag, SstHdaFmt(&f), TRUE);
    } else {
        SstLinkDmaSetup(s, p->LinkDma, tag, 0, FALSE);
        if (!rec) HDAW16(s, l + 0x08, (USHORT)(HDA16(s, l + 0x08) & ~(1U << tag)));
    }
}

/* ------------------------------------------------------------------ */
/* paths                                                                */
/* ------------------------------------------------------------------ */
BOOLEAN Ipc4PathCreate(SST *s, SST_PATH *p, SST_EP *e, BOOLEAN rec)
{
    SST_FMT host, be = e->Be;
    ULONG node, len, linkDma = 0xFFFFFFFFUL, hostDma = p->HostDma;
    UCHAR *b = s->Tx;
    memset(p, 0, sizeof(*p));
    p->HostDma = hostDma;
    p->Ep = e;
    host.Rate = 48000; host.Bits = 16; host.Valid = 16;
    host.Ch = (rec && e->Kind == EP_DMIC && be.Ch > 2) ? be.Ch : 2;
    if (!rec && be.Ch < 2) be.Ch = 2;
    p->Fe = host;
    /* pipeline / instance ids: playback 0/1, capture 2/3 */
    p->PplFe = rec ? 3 : 0; p->PplBe = rec ? 2 : 1;
    p->InstFe = rec ? 3 : 0; p->InstBe = rec ? 2 : 1;
    switch (e->Kind) {
    case EP_DMIC: node = (NODE_DMIC_IN << 8) | 0; break;
    case EP_SSP:  node = ((rec ? NODE_I2S_IN : NODE_I2S_OUT) << 8) | ((ULONG)e->Port << 4); break;
    case EP_HDA:  node = ((rec ? NODE_LINK_IN : NODE_LINK_OUT) << 8) | e->Port; break;
    case EP_SDW:  node = ((rec ? NODE_ALH_IN : NODE_ALH_OUT) << 8) | (e->Alh & 0xFF); break;
    default: return FALSE;
    }
    if (s->P->Gen >= SST_MTL && e->Kind != EP_HDA) {
        /* ACE: the DMIC / SSP link must be powered (and offloaded to the DSP on ACE 2+, lnl.c); sdw.c powers
           the SoundWire links itself (no offload bit for them in Linux) */
        ULONG l = SstMlLink(s, EpElid(e));
        if (e->Kind != EP_SDW) {
            SstMlPower(s, EpElid(e), e->Kind == EP_SSP ? e->Port : 0, TRUE);
            if (l && s->P->Gen >= SST_LNL) HDAW32(s, l + 0x04, HDA32(s, l + 0x04) | (1UL << 4));    /* OFLEN */
        }
        if (s->P->Gen >= SST_LNL) {
            p->LinkDma = LinkStreamFor(s, rec, 0);
            linkDma = rec ? p->LinkDma : p->LinkDma - s->Iss;
        }
    }
    if (!CreatePipeline(s, p->PplFe)) return FALSE;
    if (!CreatePipeline(s, p->PplBe)) { DeletePipeline(s, p->PplFe); return FALSE; }
    p->Created = TRUE;
    if (!rec) {
        len = CopierCfg(s, b, &host, &be, (NODE_HOST_OUT << 8) | p->HostDma, Ms(&host) * 4, NULL, 0, 0xFFFFFFFFUL,
                        NULL, FALSE);
        if (!InitCopier(s, p->InstFe, p->PplFe, b, len)) goto fail;
        len = CopierCfg(s, b, &be, &be, node, Ms(&be) * 2, e->Blob, e->BlobLen, linkDma, e, FALSE);
        if (!InitCopier(s, p->InstBe, p->PplBe, b, len)) goto fail;
        if (!Bind(s, p->InstFe, p->InstBe)) goto fail;
    } else {
        len = CopierCfg(s, b, &be, &be, node, Ms(&be) * 2, e->Blob, e->BlobLen, linkDma, e, TRUE);
        if (!InitCopier(s, p->InstBe, p->PplBe, b, len)) goto fail;
        len = CopierCfg(s, b, &be, &host, (NODE_HOST_IN << 8) | p->HostDma, Ms(&host) * 4, NULL, 0, 0xFFFFFFFFUL,
                        NULL, TRUE);
        if (!InitCopier(s, p->InstFe, p->PplFe, b, len)) goto fail;
        if (!Bind(s, p->InstBe, p->InstFe)) goto fail;
    }
    SetState(s, p->PplFe, PPL_RESET);
    SetState(s, p->PplBe, PPL_RESET);
    LOGI("sst: %s path to %s: host %u ch 16 bit, gateway %08x %u ch %u/%u bit", rec ? "capture" : "playback",
         e->Name, host.Ch, node, be.Ch, be.Valid, be.Bits);
    return TRUE;
fail:
    LOGE("sst: %s path to %s failed", rec ? "capture" : "playback", e->Name);
    Ipc4PathDelete(s, p);
    return FALSE;
}

void Ipc4PathDelete(SST *s, SST_PATH *p)
{
    if (!p->Created) return;
    if (p->Running) Ipc4PathRun(s, p, FALSE, p->PplFe == 3);
    SetState(s, p->PplFe, PPL_RESET);
    SetState(s, p->PplBe, PPL_RESET);
    DeletePipeline(s, p->PplFe);                          /* deletes its modules too */
    DeletePipeline(s, p->PplBe);
    p->Created = FALSE;
}

/* start: both pipelines PAUSED, then RUNNING from the sink (the side data flows into) */
BOOLEAN Ipc4PathRun(SST *s, SST_PATH *p, BOOLEAN run, BOOLEAN rec)
{
    UCHAR sink = rec ? p->PplFe : p->PplBe, src = rec ? p->PplBe : p->PplFe;
    BOOLEAN ok;
    if (!p->Created) return FALSE;
    if (run) {
        LinkDmaGateway(s, p, rec, TRUE);
        ok = SetState(s, sink, PPL_PAUSED) && SetState(s, src, PPL_PAUSED) &&
             SetState(s, sink, PPL_RUNNING) && SetState(s, src, PPL_RUNNING);
        p->Running = ok;
        return ok;
    }
    SetState(s, src, PPL_PAUSED);
    SetState(s, sink, PPL_PAUSED);
    SetState(s, src, PPL_RESET);
    SetState(s, sink, PPL_RESET);
    LinkDmaGateway(s, p, rec, FALSE);
    p->Running = FALSE;
    return TRUE;
}
