/*
 * AUDIONT - USB Audio Class 1.0 / 2.0 back-end
 *
 * USB audio devices are driven by the USBNT USB stack, which exports its
 * audio class interface through \Device\UsbNtAudio (usbntaud.h).  USBNT
 * keeps the isochronous transfers going on its own and calls back for
 * every few milliseconds of data in the device's own format and rate.
 *
 * Each USB device becomes a card like the PCI ones: the engine mixes into
 * its 48 kHz / 16-bit stereo ring, and this back-end converts between the
 * ring and the device (sample format, channel count and - through the same
 * windowed-sinc resampler the wave streams use - sample rate).  The frames
 * the device has consumed are the card's clock, so asynchronous devices
 * that run at their own pace are followed exactly.
 *
 * Devices may be plugged in and out at any time.  A new device gets new
 * device objects (WaveOutN, WaveInN, MidiOutN, ...); a device that goes
 * away keeps its card, which then runs on a virtual clock so that open
 * applications see their buffers complete instead of hanging, and the same
 * device (vendor / product id) gets the card back when it returns.
 *
 * Feature unit volume and mute become the master / record hardware
 * volume, selector units the record source, and a USB MIDI output port
 * takes the place of the FM synthesizer on the card's MIDI device.
 */
#include "audiont.h"
#include "usbntaud.h"

typedef struct _USB_CARD {
    CARD           *Card;
    ULONG           Id;                 /* USBNT handle, 0 while unplugged */
    USHORT          Vid, Pid;
    BOOLEAN         Open[2];
    BOOLEAN         Run[2];             /* the engine wants the direction running */
    BOOLEAN         InStart;            /* Fill called from StartPlay: lock already held */
    BOOLEAN         HwVol[2];
    UCHAR           MidiCables;
    UCHAR           NumInputs;
    UA_STREAM_INFO  Si[2];
    SHORT           VolMin[2], VolMax[2];
    ULONG           InputLine[UA_MAX_INPUTS];
    ULONGLONG       Pulled;             /* engine frames handed to the device */
    ULONGLONG       RecWritten;         /* engine frames captured */
    ULONG           VirtMs[2];          /* virtual clock reference */
    RESAMPLER       Rs[2];              /* play: 48 kHz -> device, record: device -> 48 kHz */
    ULONG           Fills, Datas, Starved, Attaches;
    ULONG           FillFrames, FillMax, FillRenders, DataFrames;
} USB_CARD;

static const USBNT_AUDIO_IF *g_Ua;

static PDEVICE_OBJECT g_UaDev;

typedef NTSTATUS (NTAPI *PFN_GETDEVOBJ)(PUNICODE_STRING, ACCESS_MASK, PVOID *, PDEVICE_OBJECT *);

/* ------------------------------------------------------------------ */
/* sample formats                                                      */
/* ------------------------------------------------------------------ */
static ULONG IntToFloatBits(LONG v)
{
    ULONG s = 0, m, p = 0;
    if (v == 0) return 0;
    if (v < 0) { s = 0x80000000UL; v = -v; }
    m = (ULONG)v;
    while ((m >> (p + 1)) != 0) p++;              /* highest set bit */
    /* value = m / 32768 = 1.f * 2^(p-15) */
    return s | ((127 + p - 15) << 23) | ((m << (23 - p)) & 0x7FFFFF);
}

static LONG FloatBitsToInt(ULONG b)
{
    LONG e = (LONG)((b >> 23) & 0xFF);
    ULONG m = (b & 0x7FFFFF) | 0x800000;
    LONG sh = e - 135;                            /* v * 32768 = m * 2^(e-127-23+15) */
    LONG v;
    if (e == 0) return 0;
    if (sh >= 0) v = 32768;
    else if (sh <= -24) v = 0;
    else v = (LONG)(m >> -sh);
    if (v > 32767) v = (b & 0x80000000UL) ? 32768 : 32767;
    return (b & 0x80000000UL) ? -v : v;
}

static __inline__ UCHAR *PutSample(UCHAR *p, const UA_STREAM_INFO *si, LONG v)
{
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    if (si->Type == UA_FMT_FLOAT) {
        ULONG b = IntToFloatBits(v);
        p[0] = (UCHAR)b; p[1] = (UCHAR)(b >> 8); p[2] = (UCHAR)(b >> 16); p[3] = (UCHAR)(b >> 24);
        return p + 4;
    }
    switch (si->SubSize) {
    case 1:
        p[0] = (UCHAR)(si->Type == UA_FMT_PCM8 ? (v >> 8) + 128 : (v >> 8));
        return p + 1;
    case 2:
        p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8);
        return p + 2;
    case 3:
        p[0] = 0; p[1] = (UCHAR)v; p[2] = (UCHAR)(v >> 8);
        return p + 3;
    default:
        p[0] = 0; p[1] = 0; p[2] = (UCHAR)v; p[3] = (UCHAR)(v >> 8);
        return p + 4;
    }
}

static __inline__ LONG GetSample(const UCHAR *p, const UA_STREAM_INFO *si)
{
    if (si->Type == UA_FMT_FLOAT)
        return FloatBitsToInt(p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24));
    switch (si->SubSize) {
    case 1:  return si->Type == UA_FMT_PCM8 ? ((LONG)p[0] - 128) << 8 : (LONG)(CHAR)p[0] << 8;
    case 2:  return (SHORT)(p[0] | (p[1] << 8));
    case 3:  return (SHORT)(p[1] | (p[2] << 8));
    default: return (SHORT)(p[2] | (p[3] << 8));
    }
}

static void Silence(UCHAR *buf, const UA_STREAM_INFO *si, ULONG frames)
{
    ULONG n = frames * si->Channels * si->SubSize;
    memset(buf, (si->Type == UA_FMT_PCM8) ? 0x80 : 0, n);
}

/* ------------------------------------------------------------------ */
/* stream callbacks (DISPATCH_LEVEL, USBNT poll DPC)                    */
/* ------------------------------------------------------------------ */
static void UA_CALL UsbFill(void *ctx, UCHAR *buf, ULONG frames)
{
    CARD *c = ctx;
    USB_CARD *u = c->Usb;
    const UA_STREAM_INFO *si = &u->Si[UA_PLAY];
    BOOLEAN locked = !u->InStart;
    RESAMPLER *rs = &u->Rs[UA_PLAY];
    const SHORT *ring = c->PlayRing.Va;
    ULONG f, ch;

    if (locked) EngineLock(c);
    u->Fills++;
    u->FillFrames += frames;
    if (frames > u->FillMax) u->FillMax = frames;
    if (!c->Playing || !u->Run[UA_PLAY] || c->Stopping) {
        Silence(buf, si, frames);
        goto out;
    }
    if (locked) {
        /* the engine renders on demand: the device is the clock */
        ULONG need = (ULONG)(((ULONGLONG)frames * ENG_RATE) / (si->Rate ? si->Rate : ENG_RATE)) + 64;
        if (c->WrittenFrames < u->Pulled + need) {
            EngineProcess(c);
            u->FillRenders++;
        }
        if (!c->Playing) {
            Silence(buf, si, frames);
            goto out;
        }
    }
    if (rs->Passthrough) {
        /* device at 48 kHz: straight from the ring, no resampler (and few calls:
           DPC stacks are slow to write in some emulators) */
        BOOLEAN s16 = (BOOLEAN)(si->Type == UA_FMT_PCM && si->SubSize == 2 && si->Channels == 2);
        f = 0;
        while (f < frames) {
            ULONG off = (ULONG)(u->Pulled % RING_FRAMES);
            ULONG n = frames - f, i;
            ULONGLONG avail = (c->WrittenFrames > u->Pulled) ? c->WrittenFrames - u->Pulled : 0;
            const SHORT *p = ring + off * 2;
            if (n > RING_FRAMES - off) n = RING_FRAMES - off;
            if (avail == 0) {
                Silence(buf, si, frames - f);
                u->Starved += frames - f;
                u->Pulled += frames - f;
                break;
            }
            if (n > avail) n = (ULONG)avail;
            if (s16) {
                memcpy(buf, p, n * 4);
                buf += n * 4;
            } else {
                for (i = 0; i < n; i++, p += 2) {
                    if (si->Channels == 1) {
                        buf = PutSample(buf, si, ((LONG)p[0] + p[1]) >> 1);
                    } else {
                        buf = PutSample(buf, si, p[0]);
                        buf = PutSample(buf, si, p[1]);
                        for (ch = 2; ch < si->Channels; ch++) buf = PutSample(buf, si, 0);
                    }
                }
            }
            u->Pulled += n;
            f += n;
        }
        goto out;
    }
    /* other rates: resampled in blocks */
    for (f = 0; f < frames; ) {
        LONG tmp[2 * 128];
        ULONG n = frames - f, i;
        if (n > 128) n = 128;
        RsFromRing(rs, ring, RING_FRAMES, &u->Pulled, c->WrittenFrames, tmp, n, &u->Starved);
        for (i = 0; i < n; i++) {
            LONG l = tmp[i * 2], r = tmp[i * 2 + 1];
            if (si->Channels == 1) {
                buf = PutSample(buf, si, (l + r) >> 1);
            } else {
                buf = PutSample(buf, si, l);
                buf = PutSample(buf, si, r);
                for (ch = 2; ch < si->Channels; ch++) buf = PutSample(buf, si, 0);
            }
        }
        f += n;
    }
out:
    if (locked) EngineUnlock(c);
}

static void UA_CALL UsbData(void *ctx, const UCHAR *buf, ULONG frames)
{
    CARD *c = ctx;
    USB_CARD *u = c->Usb;
    const UA_STREAM_INFO *si = &u->Si[UA_REC];
    RESAMPLER *rs = &u->Rs[UA_REC];
    SHORT *ring = c->RecRing.Va;
    ULONG fb = (ULONG)si->Channels * si->SubSize, f;

    EngineLock(c);
    u->Datas++;
    u->DataFrames += frames;
    if (c->Recording && u->Run[UA_REC] && !c->Stopping && rs->Passthrough) {
        BOOLEAN s16 = (BOOLEAN)(si->Type == UA_FMT_PCM && si->SubSize == 2 && si->Channels == 2);
        f = 0;
        while (f < frames) {
            ULONG off = (ULONG)(u->RecWritten % RING_FRAMES);
            ULONG n = frames - f, i;
            SHORT *d = ring + off * 2;
            if (n > RING_FRAMES - off) n = RING_FRAMES - off;
            if (s16) {
                memcpy(d, buf, n * 4);
                buf += n * 4;
            } else {
                for (i = 0; i < n; i++, buf += fb, d += 2) {
                    LONG l = GetSample(buf, si);
                    d[0] = (SHORT)l;
                    d[1] = (SHORT)((si->Channels > 1) ? GetSample(buf + si->SubSize, si) : l);
                }
            }
            u->RecWritten += n;
            f += n;
        }
        EngineProcess(c);
    } else if (c->Recording && u->Run[UA_REC] && !c->Stopping) {
        for (f = 0; f < frames; ) {
            LONG tmp[2 * 128];
            ULONG n = frames - f, k;
            if (n > 128) n = 128;
            for (k = 0; k < n; k++, buf += fb) {
                tmp[k * 2] = GetSample(buf, si);
                tmp[k * 2 + 1] = (si->Channels > 1) ? GetSample(buf + si->SubSize, si) : tmp[k * 2];
            }
            RsToRing(rs, tmp, n, ring, RING_FRAMES, &u->RecWritten);
            f += n;
        }
        EngineProcess(c);
    }
    EngineUnlock(c);
}

static const UA_STREAM_OPS *OpsFor(CARD *c)
{
    static UA_STREAM_OPS ops[MAX_CARDS];
    ULONG i = c->Index % MAX_CARDS;
    ops[i].Context = c;
    ops[i].Fill = UsbFill;
    ops[i].Data = UsbData;
    return &ops[i];
}

/* ------------------------------------------------------------------ */
/* HWOPS                                                               */
/* ------------------------------------------------------------------ */
static BOOLEAN Live(USB_CARD *u, ULONG dir)
{
    return u->Id != 0 && u->Open[dir];
}

/* frames of the virtual clock since the last call (unplugged device) */
static ULONG VirtFrames(USB_CARD *u, ULONG dir)
{
    ULONG now = OsTimeMs(), ms = now - u->VirtMs[dir];
    u->VirtMs[dir] = now;
    if (ms > 1000) ms = 1000;
    return ms * (ENG_RATE / 1000);
}

static NTSTATUS UsbInit(CARD *c) { (void)c; return STATUS_SUCCESS; }

static BOOLEAN UsbStartPlay(CARD *c)
{
    USB_CARD *u = c->Usb;
    u->Run[UA_PLAY] = TRUE;
    u->Pulled = 0;
    RsReset(&u->Rs[UA_PLAY]);
    u->VirtMs[UA_PLAY] = OsTimeMs();
    if (Live(u, UA_PLAY)) {
        int r;
        u->InStart = TRUE;
        r = g_Ua->Start(u->Id, UA_PLAY);
        u->InStart = FALSE;
        if (r) LOGE("usb%u: playback start failed (%d)", c->Index, r);
    }
    return TRUE;
}

static void UsbStopPlay(CARD *c)
{
    USB_CARD *u = c->Usb;
    u->Run[UA_PLAY] = FALSE;
    if (Live(u, UA_PLAY)) g_Ua->Stop(u->Id, UA_PLAY);
}

static ULONGLONG UsbPlayCount(CARD *c)
{
    USB_CARD *u = c->Usb;
    if (!Live(u, UA_PLAY)) u->Pulled += VirtFrames(u, UA_PLAY);
    return u->Pulled;
}

static ULONG UsbPlayPos(CARD *c)
{
    return (ULONG)(UsbPlayCount(c) % RING_FRAMES);
}

static BOOLEAN UsbStartRec(CARD *c)
{
    USB_CARD *u = c->Usb;
    u->Run[UA_REC] = TRUE;
    u->RecWritten = 0;
    RsReset(&u->Rs[UA_REC]);
    u->VirtMs[UA_REC] = OsTimeMs();
    if (Live(u, UA_REC)) {
        int r = g_Ua->Start(u->Id, UA_REC);
        if (r) LOGE("usb%u: capture start failed (%d)", c->Index, r);
    }
    return TRUE;
}

static void UsbStopRec(CARD *c)
{
    USB_CARD *u = c->Usb;
    u->Run[UA_REC] = FALSE;
    if (Live(u, UA_REC)) g_Ua->Stop(u->Id, UA_REC);
}

static ULONG UsbRecPos(CARD *c)
{
    USB_CARD *u = c->Usb;
    if (!Live(u, UA_REC)) {
        /* unplugged: silence at the nominal rate keeps wave-in buffers moving */
        ULONG n = VirtFrames(u, UA_REC), i;
        SHORT *ring = c->RecRing.Va;
        if (n > RING_FRAMES / 2) n = RING_FRAMES / 2;
        for (i = 0; i < n; i++) {
            SHORT *d = ring + (ULONG)(u->RecWritten % RING_FRAMES) * 2;
            d[0] = d[1] = 0;
            u->RecWritten++;
        }
    }
    return (ULONG)(u->RecWritten % RING_FRAMES);
}

/* 0..65535 slider -> 1/256 dB inside the unit's range */
static SHORT VolDb256(USB_CARD *u, ULONG dir, USHORT v)
{
    LONG db = -(LONG)(((ULONGLONG)VolToDb100(v) * 256) / 100);
    if (db < u->VolMin[dir]) db = u->VolMin[dir];
    if (db > u->VolMax[dir]) db = u->VolMax[dir];
    if (db > 0) db = 0;                           /* never above unity by default */
    if (u->VolMax[dir] < 0 && db > u->VolMax[dir]) db = u->VolMax[dir];
    return (SHORT)db;
}

static void UsbApplyLine(CARD *c, ULONG line)
{
    USB_CARD *u = c->Usb;
    ULONG dir;
    LINE_STATE *l = &c->Lines[line];
    if (line == LINE_MASTER) dir = UA_PLAY;
    else if (line == LINE_RECORD) dir = UA_REC;
    else return;
    if (!u->HwVol[dir] || !u->Id) return;
    g_Ua->SetVolume(u->Id, dir, VolDb256(u, dir, l->Left), VolDb256(u, dir, l->Right),
                    (BOOLEAN)(l->Mute || (l->Left == 0 && l->Right == 0)));
}

static void UsbApplyRecSrc(CARD *c)
{
    USB_CARD *u = c->Usb;
    ULONG i;
    if (!u->Id || u->NumInputs < 2) return;
    for (i = 0; i < u->NumInputs; i++) {
        if ((1UL << u->InputLine[i]) == c->RecSrc) {
            g_Ua->SelectInput(u->Id, i);
            return;
        }
    }
}

static ULONG UsbDump(CARD *c, char *buf, ULONG size)
{
    USB_CARD *u = c->Usb;
    UA_STATS ps, rs;
    ULONG n = 0;
    memset(&ps, 0, sizeof(ps));
    memset(&rs, 0, sizeof(rs));
    if (u->Id) {
        g_Ua->GetStats(u->Id, UA_PLAY, &ps);
        g_Ua->GetStats(u->Id, UA_REC, &rs);
    }
    return n + OsSprintf(buf + n, size - n,
        "USB audio %04x:%04x id %x%s\r\n"
        "play: %u Hz %u ch %u/%u bit, latency %u ms, packets %u, late %u, errors %u, feedback %u (%u), gap max %u ms (%u > 30 ms), poll gap max %u ms\r\n"
        "rec : %u Hz %u ch %u/%u bit, packets %u, errors %u\r\n"
        "fills %u (%u frames, max %u, %u renders), captures %u (%u frames), starved frames %u, attaches %u, pulled %u, captured %u\r\n",
        u->Vid, u->Pid, u->Id, u->Id ? "" : " (unplugged)",
        u->Si[0].Rate, u->Si[0].Channels, u->Si[0].Bits, u->Si[0].SubSize * 8, u->Si[0].LatencyMs,
        ps.Packets, ps.Late, ps.Errors, ps.Feedback, ps.FeedbackCount, ps.MaxGapMs, ps.Gaps, ps.TickMaxGapMs,
        u->Si[1].Rate, u->Si[1].Channels, u->Si[1].Bits, u->Si[1].SubSize * 8, rs.Packets, rs.Errors,
        u->Fills, u->FillFrames, u->FillMax, u->FillRenders, u->Datas, u->DataFrames, u->Starved, u->Attaches,
        (ULONG)u->Pulled, (ULONG)u->RecWritten);
}

static const HWOPS g_UsbOps = {
    "usb", UsbInit, NULL,
    UsbStartPlay, UsbStopPlay, UsbPlayPos, NULL,
    UsbStartRec, UsbStopRec, UsbRecPos, NULL,
    NULL, UsbApplyLine, UsbApplyRecSrc, UsbDump,
    UsbPlayCount
};

/* ------------------------------------------------------------------ */
/* MIDI                                                                */
/* ------------------------------------------------------------------ */
BOOLEAN UsbMidiOut(CARD *c, const UCHAR *p, ULONG n)
{
    USB_CARD *u = c->Usb;
    if (c->Type != HW_USB || !u || !u->MidiCables) return FALSE;
    if (p && n && u->Id) g_Ua->MidiOut(u->Id, 0, p, n);
    return TRUE;
}

void UsbMidiReset(CARD *c)
{
    UCHAR m[16 * 6];
    ULONG ch;
    if (c->Type != HW_USB || !c->Usb || !c->Usb->MidiCables) return;
    for (ch = 0; ch < 16; ch++) {
        m[ch * 6 + 0] = (UCHAR)(0xB0 | ch); m[ch * 6 + 1] = 0x7B; m[ch * 6 + 2] = 0;   /* all notes off */
        m[ch * 6 + 3] = (UCHAR)(0xB0 | ch); m[ch * 6 + 4] = 0x79; m[ch * 6 + 5] = 0;   /* reset controllers */
    }
    UsbMidiOut(c, m, sizeof(m));
}

/* ------------------------------------------------------------------ */
/* arrival / removal (USBNT worker thread, PASSIVE_LEVEL)               */
/* ------------------------------------------------------------------ */
static void OpenStreams(CARD *c, const UA_INFO *in)
{
    USB_CARD *u = c->Usb;
    ULONG dir;
    for (dir = 0; dir < 2; dir++) {
        UA_STREAM_INFO si;
        int r;
        u->Open[dir] = FALSE;
        if (!in->HasDir[dir]) continue;
        memset(&si, 0, sizeof(si));
        r = g_Ua->Open(in->Id, dir, ENG_RATE, ENG_CH, 16, OpsFor(c), &si);
        if (r) {
            LOGE("usb%u: cannot open the %s stream (%d)", c->Index, dir ? "capture" : "playback", r);
            continue;
        }
        EngineLock(c);
        u->Si[dir] = si;
        EngineUnlock(c);
        /* the resampler allocates its filter table: done before the stream can run */
        if (dir == UA_PLAY) RsInit(&u->Rs[dir], ENG_RATE, si.Rate);
        else RsInit(&u->Rs[dir], si.Rate, ENG_RATE);
        u->Open[dir] = TRUE;
        LOGI("usb%u: %s %u Hz, %u ch, %u bit%s, %u ms queued", c->Index, dir ? "capture " : "playback",
             si.Rate, si.Channels, si.Bits, si.Type == UA_FMT_FLOAT ? " float" : "", si.LatencyMs);
    }
}

static ULONG InputLineOf(USHORT type, ULONG used)
{
    ULONG want;
    if ((type >= 0x0201 && type <= 0x0206) || type == 0x0402 || type == 0x0403) want = LINE_MIC;
    else if (type == 0x0603 || type == 0x0601 || type == 0x0602) want = LINE_LINEIN;
    else if (type == 0x0703 || type == 0x0704) want = LINE_CD;
    else want = LINE_AUX;
    if (used & (1UL << want)) {
        static const ULONG alt[4] = { LINE_LINEIN, LINE_AUX, LINE_CD, LINE_MIC };
        ULONG i;
        for (i = 0; i < 4; i++) if (!(used & (1UL << alt[i]))) return alt[i];
    }
    return want;
}

BOOLEAN UsbConnected(CARD *c)
{
    return c->Usb && c->Usb->Id != 0;
}

static CARD *NewCard(const UA_INFO *in, ULONG index)
{
    CARD *c;
    USB_CARD *u;
    ULONG i, k;
    if (index >= MAX_CARDS) {
        LOGE("usb: no free card slot for \"%s\"", in->Name);
        return NULL;
    }
    c = OsAlloc(sizeof(CARD));
    u = OsAlloc(sizeof(USB_CARD));
    if (!c || !u) { OsFree(c); OsFree(u); return NULL; }
    c->Usb = u;
    u->Card = c;
    c->Index = index;
    c->Type = HW_USB;
    c->Ops = &g_UsbOps;
    c->VendorId = in->Vid;
    c->DeviceId = in->Pid;
    u->Vid = in->Vid;
    u->Pid = in->Pid;
    OsSprintf(c->Desc, sizeof(c->Desc), "%s", in->Name[0] ? in->Name : "USB Audio");
    OsSprintf(c->Codec, sizeof(c->Codec), "USB Audio %u.0, %s speed, %04x:%04x", in->Uac,
              in->Speed == 2 ? "high" : in->Speed == 3 ? "super" : "full", in->Vid, in->Pid);
    c->CanRecord = in->HasDir[UA_REC];
    u->MidiCables = in->MidiOutCables;

    CtrlInit(c);
    c->Lines[LINE_MASTER].Present = in->HasDir[UA_PLAY];
    c->Lines[LINE_WAVE].Present = in->HasDir[UA_PLAY];
    c->Lines[LINE_SYNTH].Present = FALSE;
    c->Lines[LINE_MASTER].HwVolume = u->HwVol[UA_PLAY] = in->HasVolume[UA_PLAY];
    c->Lines[LINE_RECORD].Present = in->HasDir[UA_REC];
    c->Lines[LINE_RECORD].HwVolume = u->HwVol[UA_REC] = in->HasVolume[UA_REC];
    for (i = 0; i < 2; i++) {
        u->VolMin[i] = in->VolMin[i];
        u->VolMax[i] = in->VolMax[i];
    }
    /* capture sources behind a selector unit */
    u->NumInputs = in->NumInputs;
    for (i = 0, k = 0; i < in->NumInputs && i < UA_MAX_INPUTS; i++) {
        u->InputLine[i] = InputLineOf(in->InputType[i], k);
        k |= 1UL << u->InputLine[i];
    }
    if (in->NumInputs > 1) {
        c->RecSrcMask = k;
        c->RecSrc = 1UL << u->InputLine[0];
        for (i = 0; i < in->NumInputs; i++) c->Lines[u->InputLine[i]].Present = TRUE;
    }
    if (!NT_SUCCESS(EngineInit(c))) {
        LOGE("usb: no memory for the rings of \"%s\"", in->Name);
        OsFreeDma(&c->PlayRing);
        OsFreeDma(&c->RecRing);
        OsFree(u);
        OsFree(c);
        return NULL;
    }
    CtrlLoad(c);
    /* PCM playback without a MIDI port of its own: the FM synthesizer plays on it */
    if (in->HasDir[UA_PLAY] && !u->MidiCables) {
        SynthCreate(c);
        c->Lines[LINE_SYNTH].Present = c->Synth != NULL;
    }
    return c;
}

static VOID UA_CALL UaArrival(void *ctx, const UA_INFO *in)
{
    CARD *c = NULL;
    USB_CARD *u;
    ULONG i;
    BOOLEAN fresh = FALSE;
    (void)ctx;

    LOGI("usb: \"%s\" %04x:%04x arrived (UAC %u, play %u, capture %u, MIDI %u)", in->Name, in->Vid, in->Pid,
         in->Uac, in->HasDir[0], in->HasDir[1], in->MidiOutCables);
    /* the same device coming back gets its card (and device names) back */
    for (i = 0; i < g_CardCount; i++) {
        CARD *x = g_Cards[i];
        if (x && x->Type == HW_USB && !x->Usb->Id && x->Usb->Vid == in->Vid && x->Usb->Pid == in->Pid) {
            c = x;
            break;
        }
    }
    if (!c) {
        /* a different device: it takes over the card of an unplugged one nobody has open,
           so the device list stays the same length and the names stay in place */
        ULONG idx = g_CardCount;
        for (i = 0; i < g_CardCount; i++) {
            CARD *x = g_Cards[i];
            if (x && x->Type == HW_USB && !x->Usb->Id && x->OpenHandles <= 0 && !x->Playing && !x->Recording) {
                LOGI("card%u: \"%s\" replaced by \"%s\"", x->Index, x->Desc, in->Name);
                idx = x->Index;
                g_Cards[i] = NULL;
                CardDestroy(x);
                break;
            }
        }
        c = NewCard(in, idx);
        if (!c) return;
        fresh = TRUE;
    }
    u = c->Usb;
    u->Attaches++;
    OpenStreams(c, in);
    EngineLock(c);
    u->Id = in->Id;
    if (!fresh) {
        /* streams that were running on the virtual clock move to the device */
        if (c->Playing && u->Open[UA_PLAY]) {
            u->InStart = TRUE;
            g_Ua->Start(u->Id, UA_PLAY);
            u->InStart = FALSE;
        }
        if (c->Recording && u->Open[UA_REC]) g_Ua->Start(u->Id, UA_REC);
    }
    EngineUnlock(c);
    if (u->HwVol[UA_PLAY]) UsbApplyLine(c, LINE_MASTER);
    if (u->HwVol[UA_REC]) UsbApplyLine(c, LINE_RECORD);
    if (c->RecSrc) UsbApplyRecSrc(c);
    if (fresh) {
        CardCreateDevs(c);
        EngineStart(c);
        g_Cards[c->Index] = c;
        if (c->Index == g_CardCount) g_CardCount++;
        LOGI("card%u ready: \"%s\" (%s), waveout%u%s, midiout%u%s", c->Index, c->Desc, c->Codec,
             c->DevIndex[DEV_WAVEOUT], c->CanRecord ? ", wavein" : "", c->DevIndex[DEV_MIDIOUT],
             u->MidiCables ? " (USB MIDI)" : "");
    } else {
        LOGI("card%u: \"%s\" is back", c->Index, c->Desc);
    }
}

static VOID UA_CALL UaRemoval(void *ctx, ULONG id)
{
    ULONG i;
    (void)ctx;
    for (i = 0; i < g_CardCount; i++) {
        CARD *c = g_Cards[i];
        USB_CARD *u;
        if (!c || c->Type != HW_USB || c->Usb->Id != id) continue;
        u = c->Usb;
        EngineLock(c);
        u->Id = 0;
        u->Open[0] = u->Open[1] = FALSE;
        u->VirtMs[0] = u->VirtMs[1] = OsTimeMs();
        EngineUnlock(c);
        LOGI("card%u: \"%s\" unplugged, running on the virtual clock", c->Index, c->Desc);
    }
}

/* ------------------------------------------------------------------ */
BOOLEAN UsbAudInit(void)
{
    static UA_CLIENT client;
    PFN_GETDEVOBJ getDev = (PFN_GETDEVOBJ)OsResolve(0, "IoGetDeviceObjectPointer");
    UNICODE_STRING name;
    PVOID fo = NULL;
    PDEVICE_OBJECT dev = NULL;
    const USBNT_AUDIO_EXT *ext;
    NTSTATUS st;

    if (!getDev) return FALSE;
    RtlInitUnicodeString(&name, USBNT_AUDIO_DEVICE_NAME);
    st = getDev(&name, 0x0001 /* FILE_READ_DATA */, &fo, &dev);
    if (!NT_SUCCESS(st) || !dev) {
        LOGV("usb: USBNT audio interface not present (%x)", st);
        return FALSE;
    }
    ext = dev->DeviceExtension;
    if (!ext || ext->Magic != USBNT_AUDIO_EXT_MAGIC || !ext->If || ext->If->Version < 1 ||
        ext->If->Size < sizeof(USBNT_AUDIO_IF)) {
        LOGE("usb: unknown USBNT audio interface (ext %p magic %x if %p version %u size %u/%u)", ext,
             ext ? ext->Magic : 0, ext ? ext->If : NULL, ext && ext->If ? ext->If->Version : 0,
             ext && ext->If ? ext->If->Size : 0, sizeof(USBNT_AUDIO_IF));
        return FALSE;
    }
    g_UaDev = dev;
    g_Ua = ext->If;
    client.Context = NULL;
    client.Arrival = UaArrival;
    client.Removal = UaRemoval;
    if (g_Ua->Register(&client)) {
        g_Ua = NULL;
        return FALSE;
    }
    LOGI("usb: USB audio through USBNT");
    return TRUE;
}

void UsbAudShutdown(void)
{
    ULONG i;
    if (!g_Ua) return;
    g_Ua->Unregister();
    for (i = 0; i < g_CardCount; i++) {
        CARD *c = g_Cards[i];
        if (c && c->Type == HW_USB && c->Usb->Id) {
            ULONG id = c->Usb->Id;
            EngineLock(c);
            c->Usb->Id = 0;
            EngineUnlock(c);
            g_Ua->Stop(id, UA_PLAY);
            g_Ua->Stop(id, UA_REC);
            g_Ua->Close(id, UA_PLAY);
            g_Ua->Close(id, UA_REC);
        }
    }
    g_Ua = NULL;                        /* UsbAudInit registers again */
}

void UsbAudFreeCard(CARD *c)
{
    if (!c->Usb) return;
    RsFree(&c->Usb->Rs[0]);
    RsFree(&c->Usb->Rs[1]);
    OsFree(c->Usb);
    c->Usb = NULL;
}
