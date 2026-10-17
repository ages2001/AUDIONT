/*
 * AUDIONT - I2S codecs and amplifiers behind the Smart Sound DSP: ACPI
 * discovery, SSP port assignment, clocks, and a worker thread that powers the
 * chips (their sequences sleep for tens to hundreds of milliseconds; the
 * engine starts streams with its spinlock held).
 *
 * Ports: the NHLT has I2S endpoints per SSP port; a port with a capture
 * endpoint carries the headset codec, a render-only port the amplifiers
 * (registry SstCodecPort / SstAmpPort: port + 1 overrides).  The frame format
 * on the port comes from the NHLT blob (IPC4) or from sstipc3.c's DAI
 * configuration (SOF IPC3: 48 kHz, 2 x 32 bit slots, 24 valid bits).
 */
#include "codec.h"

#define G32(p) ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8) | ((ULONG)(p)[2] << 16) | ((ULONG)(p)[3] << 24))

extern const CODEC_DRV g_DrvRt5682, g_DrvRt5682s, g_DrvRt5663, g_DrvRt5640, g_DrvRt5645, g_DrvRt5651, g_DrvRt5670,
    g_DrvDa7219, g_DrvDa7213, g_DrvNau8825, g_DrvNau8824, g_DrvCs42l42, g_DrvEs8316, g_DrvEs8326, g_DrvCx2072x,
    g_DrvRt286, g_DrvRt274, g_DrvRt5660,
    g_DrvMax98357a, g_DrvMax98373, g_DrvMax98390, g_DrvMax98927, g_DrvRt1011, g_DrvRt1015, g_DrvRt1015p, g_DrvRt1308,
    g_DrvSsm4567, g_DrvNau8318, g_DrvTas2563, g_DrvCs35l41;

/* headset codecs first: an amplifier and a codec never share an id, but the order decides the log */
const CODEC_DRV * const g_CodecDrvs[] = {
    &g_DrvRt5682, &g_DrvRt5682s, &g_DrvRt5663, &g_DrvRt5640, &g_DrvRt5645, &g_DrvRt5651, &g_DrvRt5670, &g_DrvRt5660,
    &g_DrvDa7219, &g_DrvDa7213, &g_DrvNau8825, &g_DrvNau8824, &g_DrvCs42l42, &g_DrvEs8316, &g_DrvEs8326,
    &g_DrvCx2072x, &g_DrvRt286, &g_DrvRt274,
    &g_DrvMax98373, &g_DrvMax98390, &g_DrvMax98927, &g_DrvRt1011, &g_DrvRt1015, &g_DrvRt1308, &g_DrvSsm4567,
    &g_DrvTas2563, &g_DrvCs35l41, &g_DrvMax98357a, &g_DrvRt1015p, &g_DrvNau8318,
    NULL
};

#define MAX_CODECS 8
#define QLEN       16

typedef struct _CODECS {
    SST       *Sst;
    SST_CODEC  C[MAX_CODECS];
    ULONG      N;
    UCHAR      CodecPort, AmpPort;
    /* worker */
    OSLOCK     Lock;
    OSEVENT    Ev, Done;
    struct { UCHAR Port, Rec, On, Vol; ULONG L, R; } Q[QLEN];
    ULONG      Head, Tail;
    BOOLEAN    Thread, Quit;
    LONG       Busy;
} CODECS;

static CODECS *Of(SST *s) { return s ? (CODECS *)s->Codecs : NULL; }


/* ------------------------------------------------------------------ */
/* register access for the drivers                                      */
/* ------------------------------------------------------------------ */
BOOLEAN CRaw(SST_CODEC *c, const UCHAR *w, ULONG wn, UCHAR *r, ULONG rn)
{
    BOOLEAN ok;
    if (c->Spi) {
        if (SpiXfer(c->Spi, w, wn, r, rn) || SpiXfer(c->Spi, w, wn, r, rn)) return TRUE;
        if (c->Errors++ < 8) LOGV("codec: %s at %s: SPI error", c->Drv->Name, SpiName(c->Spi));
        return FALSE;
    }
    if (!c->Bus) return FALSE;
    ok = I2cXfer(c->Bus, c->Addr, w, wn, r, rn);
    if (!ok && !I2cXfer(c->Bus, c->Addr, w, wn, r, rn)) {      /* one retry: arbitration with the BIOS / EC */
        if (c->Errors++ < 8) LOGV("codec: %s at %s:%02x: I2C error", c->Drv->Name, I2cName(c->Bus), c->Addr);
        return FALSE;
    }
    return TRUE;
}

static ULONG PutBE(UCHAR *b, ULONG v, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i++) b[i] = (UCHAR)(v >> (8 * (n - 1 - i)));
    return n;
}

BOOLEAN CRd(SST_CODEC *c, ULONG reg, ULONG *val)
{
    UCHAR w[12], r[4];
    ULONG i, v = 0, rb = c->RegBytes, vb = c->ValBytes, pad = c->Spi ? c->SpiPad : 0;
    if (!rb || rb > 4 || !vb || vb > 4 || pad > 4) return FALSE;
    PutBE(w, reg, rb);
    if (c->Spi) w[0] |= c->SpiRead;
    for (i = 0; i < pad; i++) w[rb + i] = 0;
    if (!CRaw(c, w, rb + pad, r, vb)) return FALSE;
    for (i = 0; i < vb; i++) v = (v << 8) | r[i];
    *val = v;
    return TRUE;
}

ULONG CGet(SST_CODEC *c, ULONG reg)
{
    ULONG v;
    return CRd(c, reg, &v) ? v : 0xFFFFFFFFUL;
}

BOOLEAN CWr(SST_CODEC *c, ULONG reg, ULONG val)
{
    UCHAR w[12];
    ULONG i, rb = c->RegBytes, vb = c->ValBytes, pad = c->Spi ? c->SpiPad : 0;
    if (!rb || rb > 4 || !vb || vb > 4 || pad > 4) return FALSE;
    PutBE(w, reg, rb);
    for (i = 0; i < pad; i++) w[rb + i] = 0;
    PutBE(w + rb + pad, val, vb);
    return CRaw(c, w, rb + pad + vb, NULL, 0);
}

BOOLEAN CUpd(SST_CODEC *c, ULONG reg, ULONG mask, ULONG val)
{
    ULONG v;
    if (!CRd(c, reg, &v)) return FALSE;
    if (((v & ~mask) | (val & mask)) == v) return TRUE;
    return CWr(c, reg, (v & ~mask) | (val & mask));
}

BOOLEAN CSeq(SST_CODEC *c, const ULONG *rv, ULONG pairs)
{
    ULONG i;
    BOOLEAN ok = TRUE;
    for (i = 0; i < pairs; i++) ok &= CWr(c, rv[2 * i], rv[2 * i + 1]);
    return ok;
}

void CDelay(ULONG ms) { OsSleepMs(ms); }

void CGpio(SST_CODEC *c, ULONG i, BOOLEAN high)
{
    if (i < c->NGpio) GpioSet(&c->Gpio[i], high);
}

ULONG CGcd(ULONG a, ULONG b)
{
    while (b) { ULONG t = a % b; a = b; b = t; }
    return a;
}

ULONG CVolSteps(ULONG level, ULONG steps)
{
    if (level >= 65535) return steps;
    /* a quarter of the range per 12 dB below the top would need logs; the mixer level is already
       perceptual (the engine squares it), so map it linearly onto the dB steps */
    return (ULONG)(((ULONGLONG)level * steps + 32767) / 65535);
}

/* ------------------------------------------------------------------ */
/* clocks of an SSP port                                                */
/* ------------------------------------------------------------------ */
static ULONG Mclk(SST *s)
{
    switch (s->P->Gen) {
    case SST_SKL: case SST_CNL: case SST_BDW: return 24000000;
    default: return 19200000;
    }
}


/* SOF IPC3 SSP configuration: 2 slots of 25 bits (BCLK 2.4 MHz divides the 19.2, 24 and 38.4 MHz
   SSP clocks); GPIO-only amplifiers (32 / 48 / 64 fs) get 2 x 16 bits where the 38.4 MHz crystal
   makes 1.536 MHz */
void SstSspFormat(SST *s, UCHAR port, ULONG *mclk, ULONG *bclk, UCHAR *slot, UCHAR *valid)
{
    CODECS *x = Of(s);
    ULONG i;
    BOOLEAN plainAmp = FALSE;
    *mclk = Mclk(s);
    *slot = 25; *valid = 24;
    for (i = 0; x && i < x->N; i++)
        if (x->C[i].Port == port && x->C[i].Drv->Kind == CODEC_AMP && !x->C[i].Drv->RegBytes) plainAmp = TRUE;
    if (plainAmp && (s->P->Gen == SST_ICL || s->P->Gen == SST_TGL)) { *slot = 16; *valid = 16; }
    *bclk = 48000 * 2 * *slot;
}

/* the NHLT SSP blob: SSCR0 (data size, network mode, slots), SSPSP (I2S frame width), MDIVC (MCLK on) */
static void ClocksFromBlob(SST *s, const SST_EP *e, SST_CODEC *c)
{
    const UCHAR *b = e->Blob;
    ULONG n = e->BlobLen / 4, ssc0, sspsp, mdivc, ver, base, ds, slots, frame;
    if (!b || n < 21) return;
    ver = G32(b + 4);
    base = (ver == 0xEE000105UL || ver == 0xEE000200UL) ? 2 : 0;       /* version, size before ts_group */
    if (n < base + 21) return;
    ssc0 = G32(b + 4 * (base + 9));
    sspsp = G32(b + 4 * (base + 12));
    mdivc = G32(b + 4 * (ver == 0xEE000200UL ? 20 : base + 19));
    ds = (ssc0 & 0xF) + 1 + ((ssc0 & (1UL << 20)) ? 16 : 0);
    slots = (ssc0 & (1UL << 31)) ? ((ssc0 >> 24) & 7) + 1 : 2;
    frame = 2 * ((sspsp >> 16) & 0x3F);
    if (!(ssc0 & (1UL << 31)) && frame >= 2 * ds && frame <= 128) {
        c->Slots = 2;
        c->Slot = (UCHAR)(frame / 2);
    } else {
        c->Slots = (UCHAR)slots;
        c->Slot = (UCHAR)(ds <= 16 ? 16 : ds <= 24 ? (ds == 24 ? 24 : 32) : 32);
        if (ds == 25) c->Slot = 25;
    }
    c->Valid = (UCHAR)(ds >= 24 ? 24 : 16);
    if (e->Be.Valid && e->Be.Valid < c->Valid) c->Valid = (UCHAR)e->Be.Valid;
    c->Rate = e->Be.Rate ? e->Be.Rate : 48000;
    c->Bclk = c->Rate * c->Slots * c->Slot;
    c->Mclk = (mdivc & 3) ? Mclk(s) : 0;
    LOGV("codec: SSP%u blob: ssc0 %08x sspsp %08x mdivc %08x -> %u x %u bit (%u valid), BCLK %u, MCLK %u",
         e->Port, ssc0, sspsp, mdivc, c->Slots, c->Slot, c->Valid, c->Bclk, c->Mclk);
}

static void Clocks(SST *s, SST_CODEC *c)
{
    ULONG i;
    c->Rate = 48000; c->Slots = 2;
    if (s->P->Fw == SST_FW_SOF3) {           /* sstipc3.c configures the SSP the same way */
        SstSspFormat(s, c->Port, &c->Mclk, &c->Bclk, &c->Slot, &c->Valid);
        return;
    }
    c->Slot = 32; c->Valid = 24;
    c->Bclk = c->Rate * 64;
    c->Mclk = 0;
    for (i = 0; i < s->NEp; i++)
        if (s->Ep[i].Kind == EP_SSP && s->Ep[i].Port == c->Port && s->Ep[i].Blob) {
            ClocksFromBlob(s, &s->Ep[i], c);
            if (s->Ep[i].Dir == 0) break;    /* the render blob wins */
        }
}

/* ------------------------------------------------------------------ */
/* discovery                                                            */
/* ------------------------------------------------------------------ */
typedef struct { CODECS *X; ULONG Seen; } SCAN;

static BOOLEAN HidMatch(const AML_DEV *d, const CODEC_DRV *drv, char *hid)
{
    const char *h;
    for (h = drv->Hids; *h; h += strlen_s(h) + 1)
        if (AmlHasId(d, h)) {
            ULONG n = strlen_s(h);
            memcpy(hid, h, n < 11 ? n + 1 : 11);
            hid[11] = 0;
            return TRUE;
        }
    return FALSE;
}

static BOOLEAN ScanDev(const AML_DEV *d, void *ctx)
{
    SCAN *sc = (SCAN *)ctx;
    CODECS *x = sc->X;
    ULONG k, i;
    for (k = 0; g_CodecDrvs[k]; k++) {
        const CODEC_DRV *drv = g_CodecDrvs[k];
        SST_CODEC *c;
        AML_RES r;
        char hid[12];
        ULONGLONG uid = 0;
        if (!HidMatch(d, drv, hid)) continue;
        if (x->N >= MAX_CODECS) return FALSE;
        AmlResources(d, &r);
        AmlNamedInt(d, "_UID", &uid);
        if (drv->RegBytes && !r.NI2c) { LOGV("codec: %s %s has no I2C resource", hid, d->Path); return TRUE; }
        /* the same device in DSDT and an SSDT, or two names for one chip */
        for (i = 0; i < x->N; i++) {
            SST_CODEC *o = &x->C[i];
            if (drv->RegBytes && o->Bus && o->Addr == r.I2c[0].Addr && !memcmp(I2cName(o->Bus), r.I2c[0].Ctrl, 4))
                return TRUE;
            if (!drv->RegBytes && o->Drv == drv && !memcmp(o->Path, d->Path, 39)) return TRUE;
        }
        c = &x->C[x->N];
        memset(c, 0, sizeof(*c));
        c->Drv = drv;
        c->Sst = x->Sst;
        c->RegBytes = drv->RegBytes;
        c->ValBytes = drv->ValBytes;
        memcpy(c->Hid, hid, sizeof(c->Hid));
        memcpy(c->Path, d->Path, sizeof(c->Path));
        c->Priv[15] = (ULONG)uid;
        if (drv->RegBytes) {
            c->Bus = I2cOpen(r.I2c[0].Ctrl);
            c->Addr = r.I2c[0].Addr;
            if (!c->Bus) { LOGI("codec: %s %s: I2C controller %s unusable", hid, d->Path, r.I2c[0].Ctrl); return TRUE; }
        }
        for (i = 0; i < r.NGpio && c->NGpio < CODEC_MAX_GPIO; i++)
            if (!r.Gpio[i].Int) GpioResolve(&r.Gpio[i], &c->Gpio[c->NGpio++]);
        c->Mic = OsRegGetDword(L"SstCodecMic", 0);
        if (!drv->Probe(c)) {
            LOGV("codec: %s %s at %s:%02x does not answer", hid, d->Path, I2cName(c->Bus), c->Addr);
            return TRUE;
        }
        LOGI("codec: %s (%s) at %s:%02x, %u GPIO", drv->Name, d->Path, I2cName(c->Bus), c->Addr, c->NGpio);
        x->N++;
        return TRUE;
    }
    return TRUE;
}

/* ports: a capture endpoint marks the codec port */
static void Ports(SST *s, CODECS *x)
{
    ULONG play = 0, rec = 0, i, p;
    ULONG cp = OsRegGetDword(L"SstCodecPort", 0), ap = OsRegGetDword(L"SstAmpPort", 0);
    for (i = 0; i < s->NEp; i++)
        if (s->Ep[i].Kind == EP_SSP) {
            if (s->Ep[i].Dir) rec |= 1UL << s->Ep[i].Port;
            else play |= 1UL << s->Ep[i].Port;
        }
    x->CodecPort = x->AmpPort = 0xFF;
    for (p = 0; p < 8 && x->CodecPort == 0xFF; p++) if ((rec & play) & (1UL << p)) x->CodecPort = (UCHAR)p;
    for (p = 0; p < 8 && x->CodecPort == 0xFF; p++) if (rec & (1UL << p)) x->CodecPort = (UCHAR)p;
    for (p = 0; p < 8 && x->AmpPort == 0xFF; p++) if ((play & ~rec) & (1UL << p)) x->AmpPort = (UCHAR)p;
    if (x->CodecPort == 0xFF && x->AmpPort != 0xFF) {
        /* only render ports: with a headset codec the first one is its */
        BOOLEAN headset = FALSE, amp = FALSE;
        for (i = 0; i < x->N; i++) { headset |= x->C[i].Drv->Kind == CODEC_HEADSET; amp |= x->C[i].Drv->Kind == CODEC_AMP; }
        if (headset) {
            x->CodecPort = x->AmpPort;
            x->AmpPort = 0xFF;
            for (p = x->CodecPort + 1; p < 8 && x->AmpPort == 0xFF; p++) if (play & (1UL << p)) x->AmpPort = (UCHAR)p;
            if (x->AmpPort == 0xFF && amp) x->AmpPort = x->CodecPort;
        }
    }
    if (x->AmpPort == 0xFF) x->AmpPort = x->CodecPort;
    if (x->CodecPort == 0xFF) {
        /* no NHLT I2S endpoints (SOF IPC3 boards): the usual wiring of the generation */
        switch (s->P->Gen) {
        case SST_SKL: x->CodecPort = 1; x->AmpPort = 0; break;
        case SST_APL: x->CodecPort = 2; x->AmpPort = 1; break;
        case SST_BYT: case SST_CHT: x->CodecPort = x->AmpPort = s->P->Ssp == 1 ? 0 : 2; break;   /* BYT-CR: SSP0 */
        case SST_BDW: x->CodecPort = x->AmpPort = 0; break;
        default: x->CodecPort = 0; x->AmpPort = 1; break;
        }
    }
    if (cp) x->CodecPort = (UCHAR)(cp - 1);
    if (ap) x->AmpPort = (UCHAR)(ap - 1);
}

static SST_EP *EpOf(SST *s, UCHAR port, UCHAR dir)
{
    ULONG i;
    for (i = 0; i < s->NEp; i++)
        if (s->Ep[i].Kind == EP_SSP && s->Ep[i].Port == port && s->Ep[i].Dir == dir) return &s->Ep[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* worker                                                               */
/* ------------------------------------------------------------------ */
static void RunStream(CODECS *x, UCHAR port, BOOLEAN rec, BOOLEAN on)
{
    ULONG i, k;
    /* on: headset codec, then amplifiers (unmute last); off: the reverse */
    for (k = 0; k < x->N; k++) {
        SST_CODEC *c = &x->C[on ? k : x->N - 1 - k];
        BOOLEAN *st = rec ? &c->RecOn : &c->PlayOn;
        i = on ? k : x->N - 1 - k;
        (void)i;
        if (!c->Ready || c->Port != port || !c->Drv->Stream) continue;
        if (!(c->Caps & (rec ? CC_REC : CC_PLAY)) || *st == on) continue;
        c->Drv->Stream(c, rec, on);
        *st = on;
    }
}

static void Worker(void *ctx)
{
    CODECS *x = (CODECS *)ctx;
    for (;;) {
        OsEventWait(&x->Ev, 1000);
        for (;;) {
            UCHAR port, rec, on, vol;
            ULONG l, r, i;
            OsLockAcquire(&x->Lock);
            if (x->Head == x->Tail) { OsLockRelease(&x->Lock); break; }
            port = x->Q[x->Head].Port; rec = x->Q[x->Head].Rec; on = x->Q[x->Head].On; vol = x->Q[x->Head].Vol;
            l = x->Q[x->Head].L; r = x->Q[x->Head].R;
            x->Head = (x->Head + 1) % QLEN;
            OsLockRelease(&x->Lock);
            if (vol) {
                for (i = 0; i < x->N; i++)
                    if (x->C[i].Ready && x->C[i].Drv->Volume) x->C[i].Drv->Volume(&x->C[i], l, r, !l && !r);
            } else RunStream(x, port, rec, on);
        }
        if (x->Quit) break;
    }
    OsEventSet(&x->Done);
}

static void Queue(CODECS *x, UCHAR port, UCHAR rec, UCHAR on, UCHAR vol, ULONG l, ULONG r)
{
    ULONG next;
    if (!x->Thread) {
        /* no worker (thread creation failed): only at passive level */
        if (KeGetCurrentIrql() >= DISPATCH_LEVEL) return;
        if (vol) return;
        RunStream(x, port, rec, on);
        return;
    }
    OsLockAcquire(&x->Lock);
    next = (x->Tail + 1) % QLEN;
    if (next != x->Head) {
        x->Q[x->Tail].Port = port; x->Q[x->Tail].Rec = rec; x->Q[x->Tail].On = on; x->Q[x->Tail].Vol = vol;
        x->Q[x->Tail].L = l; x->Q[x->Tail].R = r;
        x->Tail = next;
    }
    OsLockRelease(&x->Lock);
    OsEventSet(&x->Ev);
}

/* ------------------------------------------------------------------ */
/* sst.c interface                                                      */
/* ------------------------------------------------------------------ */
BOOLEAN SstCodecsProbe(SST *s)
{
    CODECS *x;
    SCAN sc;
    ULONG i, amps = 0, k;
    if (!OsRegGetDword(L"SstCodecs", 1)) return FALSE;
    x = (CODECS *)OsAlloc(sizeof(CODECS));
    if (!x) return FALSE;
    x->Sst = s;
    sc.X = x; sc.Seen = 0;
    AmlForEachDevice(ScanDev, &sc);
    if (!x->N) { OsFree(x); I2cCloseAll(); GpioCloseAll(); return FALSE; }
    s->Codecs = x;
    Ports(s, x);
    /* amplifier order on a port: ACPI _UID, then discovery order */
    for (i = 0; i < x->N; i++) x->C[i].Port = x->C[i].Drv->Kind == CODEC_AMP ? x->AmpPort : x->CodecPort;
    for (i = 0; i < x->N; i++) {
        SST_CODEC *c = &x->C[i];
        ULONG idx = 0, cnt = 0;
        if (c->Drv->Kind != CODEC_AMP) continue;
        for (k = 0; k < x->N; k++) {
            SST_CODEC *o = &x->C[k];
            if (o->Drv->Kind != CODEC_AMP || o->Port != c->Port) continue;
            cnt++;
            if (o->Priv[15] < c->Priv[15] || (o->Priv[15] == c->Priv[15] && k < i)) idx++;
        }
        c->Index = (UCHAR)idx; c->Count = (UCHAR)cnt;
        amps++;
    }
    /* Maxim names its first amplifier on a port "Right" (Linux sof_maxim_common) */
    for (i = 0; i < x->N; i++) {
        SST_CODEC *c = &x->C[i];
        if (c->Drv->Kind == CODEC_AMP && c->Count == 2 && c->Drv->Name[0] == 'M' && c->Drv->Name[1] == 'A' &&
            c->Drv->RegBytes) c->Index ^= 1;
    }
    for (i = 0; i < x->N; i++) {
        SST_CODEC *c = &x->C[i];
        Clocks(s, c);
        c->Priv[15] = 0;
        if (!c->Drv->Init(c)) { LOGE("codec: %s did not initialise", c->Drv->Name); continue; }
        c->Ready = TRUE;
        if (c->Drv->Volume) c->Drv->Volume(c, 65535, 65535, FALSE);
        LOGI("codec: %s on SSP%u%s, %u x %u bit, BCLK %u Hz%s%s", c->Drv->Name, c->Port,
             c->Drv->Kind == CODEC_AMP ? " (amplifier)" : "", c->Slots, c->Slot, c->Bclk,
             (c->Caps & CC_PLAY) ? ", playback" : "", (c->Caps & CC_REC) ? ", capture" : "");
    }
    /* endpoints: SOF IPC3 needs no NHLT blob, so the codec ports get endpoints without one */
    for (i = 0; i < x->N; i++) {
        SST_CODEC *c = &x->C[i];
        UCHAR d;
        if (!c->Ready) continue;
        for (d = 0; d < 2; d++) {
            SST_EP *e;
            if (!(c->Caps & (d ? CC_REC : CC_PLAY))) continue;
            e = EpOf(s, c->Port, d);
            if (!e && s->P->Fw == SST_FW_SOF3) {
                e = SstAddEp(s, EP_SSP, d, c->Port, "");
                if (e) {
                    e->Be.Rate = 48000; e->Be.Ch = 2; e->Be.Bits = 32; e->Be.Valid = 24;
                    e->Line = d ? LINE_MIC : 0;
                }
            }
            if (!e) {
                LOGI("codec: %s: no %s endpoint on SSP%u in the NHLT", c->Drv->Name, d ? "capture" : "playback", c->Port);
                continue;
            }
            if (!e->Codec || (e->Codec->Drv->Kind == CODEC_AMP && c->Drv->Kind == CODEC_HEADSET)) e->Codec = c;
            OsSprintf(e->Name, sizeof(e->Name), "%s", d ? "Headset Microphone" :
                      e->Codec->Drv->Kind == CODEC_AMP ? "Speakers" : "Headphones");
        }
    }
    OsLockInit(&x->Lock);
    OsEventInit(&x->Ev);
    OsEventInit(&x->Done);
    x->Thread = OsThreadStart(Worker, x);
    LOGI("codec: %u chips (%u amplifiers), codec port SSP%u, amplifier port SSP%u", x->N, amps, x->CodecPort, x->AmpPort);
    return TRUE;
}

/* the playback endpoint when there is no HD Audio codec: the speakers (amplifiers) when there are some */
SST_EP *SstCodecsPlayEp(SST *s)
{
    CODECS *x = Of(s);
    SST_EP *e;
    ULONG pp = OsRegGetDword(L"SstPlayPort", 0);
    if (!x) return NULL;
    if (pp && (e = EpOf(s, (UCHAR)(pp - 1), 0))) return e;
    if ((e = EpOf(s, x->AmpPort, 0)) && e->Codec) return e;
    if ((e = EpOf(s, x->CodecPort, 0)) && e->Codec) return e;
    return NULL;
}

void SstCodecsPower(SST *s, SST_EP *e, BOOLEAN on)
{
    CODECS *x = Of(s);
    if (!x || !e || e->Kind != EP_SSP) return;
    Queue(x, e->Port, e->Dir, (UCHAR)on, 0, 0, 0);
}

void SstCodecsVolume(SST *s, ULONG l, ULONG r)
{
    CODECS *x = Of(s);
    if (x) Queue(x, 0, 0, 0, 1, l, r);
}

void SstCodecsShutdown(SST *s)
{
    CODECS *x = Of(s);
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
    }
    I2cCloseAll();
    GpioCloseAll();
    OsFree(x);
    s->Codecs = NULL;
}

ULONG SstCodecsDump(SST *s, char *buf, ULONG size)
{
    CODECS *x = Of(s);
    ULONG n = 0, i;
    if (!x) return 0;
    for (i = 0; i < x->N && n + 160 < size; i++) {
        SST_CODEC *c = &x->C[i];
        n += OsSprintf(buf + n, size - n, "  %s %s (%s) at %s:%02x, SSP%u%s, %u x %u bit %u Hz, BCLK %u, MCLK %u%s%s%s, "
                       "%u I2C errors\r\n", c->Drv->Kind == CODEC_AMP ? "amplifier" : "codec", c->Drv->Name, c->Hid,
                       I2cName(c->Bus), c->Addr, c->Port, c->Drv->Kind == CODEC_AMP && c->Count > 1 ?
                       (c->Index ? " right" : " left") : "", c->Slots, c->Slot, c->Rate, c->Bclk, c->Mclk,
                       c->Ready ? "" : ", not initialised", c->PlayOn ? ", playing" : "", c->RecOn ? ", recording" : "",
                       c->Errors);
    }
    return n;
}
