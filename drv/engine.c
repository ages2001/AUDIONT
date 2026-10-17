/*
 * AUDIONT - audio engine
 *
 * The hardware always runs at 48 kHz, 16-bit stereo on a looping DMA ring.
 * A periodic timer DPC (and the period interrupt, when one is connected)
 * reads the hardware position and renders mixed audio a fixed latency
 * ahead of it.  Any number of wave-out streams (up to MAX_PLAY_STREAMS)
 * and the MIDI synthesizer are mixed in software; captured audio is
 * distributed to up to MAX_REC_STREAMS wave-in streams.
 *
 * IRPs are completed when their last sample has actually been played,
 * so WHDR_DONE timing and waveOutGetPosition are accurate.
 */
#include "audiont.h"

#define IRP_VA(i)       ((i)->IRP_DC_FIELD[0])
#define IRP_LEN(i)      (*(ULONG *)&(i)->IRP_DC_FIELD[1])
#define IRP_END(i)      (*(ULONG *)&(i)->IRP_DC_FIELD[2])
#define IRP_FLAGS(i)    (*(ULONG *)&(i)->IRP_DC_FIELD[3])
#define IRPF_QUEUED     1
#define IRPF_CANCELOWN  2     /* cancel routine already owns completion */

#define IDLE_STOP_MS    3000

static VOID NTAPI CancelRoutine(PDEVICE_OBJECT dev, PIRP irp);

/* ------------------------------------------------------------------ */
/* locking and IRP bookkeeping                                         */
/* ------------------------------------------------------------------ */
void EngineLock(CARD *c) { OsLockAcquire(&c->Lock); }

void EngineUnlock(CARD *c)
{
    LIST_ENTRY local;
    InitializeListHead(&local);
    while (!IsListEmpty(&c->CompleteList))
        InsertTailList(&local, RemoveHeadList(&c->CompleteList));
    OsLockRelease(&c->Lock);
    while (!IsListEmpty(&local)) {
        PIRP irp = CONTAINING_RECORD(RemoveHeadList(&local), IRP, IRP_LE_FIELD);
        OsCompleteIrp(irp, irp->IoStatus.Status, irp->IoStatus.Information);
    }
}

void IrpQueue(CARD *c, PLIST_ENTRY list, PIRP irp)
{
    KIRQL ci;
    (void)c;
    IRP_FLAGS(irp) = IRPF_QUEUED;
    IoMarkIrpPending(irp);
    InsertTailList(list, &irp->IRP_LE_FIELD);
    IoAcquireCancelSpinLock(&ci);
    irp->CancelRoutine = CancelRoutine;
    IoReleaseCancelSpinLock(ci);
}

/* remove from its list and schedule completion (lock held) */
void IrpFinish(CARD *c, PIRP irp, NTSTATUS st, ULONG info)
{
    KIRQL ci;
    if (IRP_FLAGS(irp) & IRPF_QUEUED) {
        RemoveEntryList(&irp->IRP_LE_FIELD);
        IRP_FLAGS(irp) = 0;
    }
    IoAcquireCancelSpinLock(&ci);
    if (irp->Cancel && irp->CancelRoutine == NULL) {
        /* IoCancelIrp already took the cancel routine: it will complete the IRP */
        IRP_FLAGS(irp) = IRPF_CANCELOWN;
        IoReleaseCancelSpinLock(ci);
        return;
    }
    irp->CancelRoutine = NULL;
    IoReleaseCancelSpinLock(ci);
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    InsertTailList(&c->CompleteList, &irp->IRP_LE_FIELD);
}

static VOID NTAPI CancelRoutine(PDEVICE_OBJECT dev, PIRP irp)
{
    DEV_EXT *ext = dev->DeviceExtension;
    CARD *c = ext->Card;
    ULONG i;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    EngineLock(c);
    if (IRP_FLAGS(irp) & (IRPF_QUEUED | IRPF_CANCELOWN)) {
        /* if this was the IRP being consumed, forget it */
        for (i = 0; i < MAX_PLAY_STREAMS; i++)
            if (c->Play[i].Cur == irp) { c->Play[i].Cur = NULL; c->Play[i].CurOffset = 0; }
        if (IRP_FLAGS(irp) & IRPF_QUEUED) RemoveEntryList(&irp->IRP_LE_FIELD);
        IRP_FLAGS(irp) = 0;
        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        InsertTailList(&c->CompleteList, &irp->IRP_LE_FIELD);
        LOGV("irp %p cancelled", irp);
    }
    EngineUnlock(c);
}

/* ------------------------------------------------------------------ */
/* timer / interrupt plumbing                                          */
/* ------------------------------------------------------------------ */
static void TimerArm(CARD *c)
{
    LARGE_INTEGER due;
    due.QuadPart = -100000;        /* 10 ms */
    KeSetTimer(&c->Timer, due, &c->TimerDpc);
}

static void TimerEnsure(CARD *c)
{
    if (!c->TimerRunning && !c->Stopping) {
        c->TimerRunning = TRUE;
        c->LastTickMs = OsTimeMs();
        TimerArm(c);
    }
}

static VOID NTAPI TimerDpcRoutine(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    CARD *c = ctx;
    (void)dpc; (void)a1; (void)a2;
    EngineLock(c);
    c->TickCount++;
    EngineProcess(c);
    if (!c->Stopping && (c->Playing || c->Recording))
        TimerArm(c);
    else
        c->TimerRunning = FALSE;
    EngineUnlock(c);
}

static VOID NTAPI IsrDpcRoutine(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    CARD *c = ctx;
    (void)dpc; (void)a1; (void)a2;
    EngineLock(c);
    EngineProcess(c);
    EngineUnlock(c);
}

static BOOLEAN NTAPI IsrRoutine(PKINTERRUPT intr, PVOID ctx)
{
    CARD *c = ctx;
    (void)intr;
    if (!c->Ops->Isr || !c->Ops->Isr(c)) return FALSE;
    c->IntCount++;
    KeInsertQueueDpc(&c->IsrDpc, NULL, NULL);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* init / shutdown                                                     */
/* ------------------------------------------------------------------ */
NTSTATUS EngineInit(CARD *c)
{
    ULONG ms;
    OsLockInit(&c->Lock);
    InitializeListHead(&c->CompleteList);
    for (ms = 0; ms < MAX_PLAY_STREAMS; ms++) {
        InitializeListHead(&c->Play[ms].Queue);
        InitializeListHead(&c->Play[ms].Done);
    }
    for (ms = 0; ms < MAX_REC_STREAMS; ms++) InitializeListHead(&c->Rec[ms].Queue);
    if (!OsAllocDma(&c->PlayRing, RING_BYTES, c->DmaUncached)) return STATUS_INSUFFICIENT_RESOURCES;
    if (!OsAllocDma(&c->RecRing, RING_BYTES, c->DmaUncached)) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeDpc(&c->TimerDpc, TimerDpcRoutine, c);
    KeInitializeDpc(&c->IsrDpc, IsrDpcRoutine, c);
    KeInitializeTimer(&c->Timer);
    ms = OsRegGetDword(L"LatencyMs", 60);
    if (ms < 20) ms = 20;
    if (ms > 150) ms = 150;
    c->LatencyFrames = ms * (ENG_RATE / 1000);
    LOGV("card%u: play ring va=%p pa=%08x, rec ring pa=%08x, latency %u ms",
         c->Index, c->PlayRing.Va, c->PlayRing.Pa, c->RecRing.Pa, ms);
    return STATUS_SUCCESS;
}

NTSTATUS EngineStart(CARD *c)
{
    ULONG mode = OsRegGetDword(L"InterruptMode", 1);   /* 0 = polling only */
    NTSTATUS st;
    c->UseInterrupt = FALSE;
    if (mode && c->Int.Valid && c->Ops->Isr) {
        /* Windows 2000 / XP: the interrupt Plug and Play assigned */
        c->IntLock = 0;
        st = IoConnectInterrupt(&c->Interrupt, IsrRoutine, c, &c->IntLock, c->Int.Vector, c->Int.Irql,
                                c->Int.Irql, c->Int.Mode, c->Int.Shared, c->Int.Affinity, FALSE);
        if (NT_SUCCESS(st)) {
            c->UseInterrupt = TRUE;
            c->Vector = c->Int.Vector; c->IrqlLevel = c->Int.Irql;
        } else {
            c->Interrupt = NULL;
            LOGI("card%u: IoConnectInterrupt(vector %u) failed %x, using timer only", c->Index, c->Int.Vector, st);
        }
    }
#ifndef AUDIONT_WDM
    else if (mode && c->Irq && c->Irq < 16 && c->Ops->Isr) {
        KIRQL irql; KAFFINITY aff; ULONG vec = 0;
        c->IntLock = 0;
        if (g_NtVersion >= NTVER_35)
            vec = HalGetInterruptVector(PCIBus, c->Bus, c->Irq, c->Irq, &irql, &aff);
        if (!vec)
            vec = HalGetInterruptVector(Isa, 0, c->Irq, c->Irq, &irql, &aff);
        if (vec) {
            st = IoConnectInterrupt(&c->Interrupt, IsrRoutine, c, &c->IntLock, vec, irql, irql,
                                    LevelSensitive, TRUE, aff, FALSE);
            if (NT_SUCCESS(st)) {
                c->UseInterrupt = TRUE;
                c->Vector = vec; c->IrqlLevel = irql;
            } else {
                c->Interrupt = NULL;
                LOGI("card%u: IoConnectInterrupt(irq %u) failed %x, using timer only", c->Index, c->Irq, st);
            }
        }
    }
#endif
    LOGI("card%u: %s mode (irq %u)", c->Index, c->UseInterrupt ? "interrupt" : "polling", c->Irq);
    return STATUS_SUCCESS;
}

void EngineShutdown(CARD *c)
{
    EngineLock(c);
    c->Stopping = TRUE;
    if (c->Playing) { c->Ops->StopPlay(c); c->Playing = FALSE; }
    if (c->Recording) { c->Ops->StopRec(c); c->Recording = FALSE; }
    EngineUnlock(c);
    KeCancelTimer(&c->Timer);
    KeRemoveQueueDpc(&c->TimerDpc);
    if (c->Interrupt) { IoDisconnectInterrupt(c->Interrupt); c->Interrupt = NULL; }
    KeRemoveQueueDpc(&c->IsrDpc);
}

/* ------------------------------------------------------------------ */
/* playback                                                            */
/* ------------------------------------------------------------------ */
static void StreamHistAdd(PLAY_STREAM *s, ULONGLONG outFrame)
{
    POSMAP *h = &s->Hist[s->HistHead % POSHIST];
    POSMAP *prev = &s->Hist[(s->HistHead + POSHIST - 1) % POSHIST];
    if (s->HistHead && prev->StreamBytes == s->BytesRendered) {
        /* nothing new: don't overwrite useful history while starved */
        return;
    }
    h->OutFrame = outFrame;
    h->StreamBytes = s->BytesRendered;
    s->HistHead++;
}

/* bytes of the stream that have actually been heard */
static ULONG StreamPlayedBytes(CARD *c, PLAY_STREAM *s)
{
    ULONG i, n = MIN(s->HistHead, POSHIST);
    ULONGLONG played = c->PlayedFrames;
    POSMAP *best = NULL, *next = NULL;
    for (i = 0; i < n; i++) {
        POSMAP *h = &s->Hist[(s->HistHead - 1 - i) % POSHIST];
        if (h->OutFrame <= played) { best = h; break; }
        next = h;
    }
    if (!best) {
        /* nothing heard yet, or history overflowed: oldest entry is the best bound */
        if (n && next && next->OutFrame > played && s->HistHead > POSHIST) return next->StreamBytes;
        if (!n) return s->BytesRendered;
        return 0;
    }
    if (next) {
        /* interpolate inside the chunk */
        ULONGLONG span = next->OutFrame - best->OutFrame;
        ULONG bytes = next->StreamBytes - best->StreamBytes;
        if (span) {
            ULONG part = (ULONG)(((ULONGLONG)bytes * (played - best->OutFrame)) / span);
            part -= part % s->Conv.Fmt.BlockAlign;
            return best->StreamBytes + part;
        }
    }
    return best->StreamBytes;
}

static void StreamRender(CARD *c, PLAY_STREAM *s, LONG *mix, ULONG n, ULONGLONG frameBase)
{
    ULONG produced = 0;
    LONG vl = CtrlGain(c, LINE_WAVE, 0), vr = CtrlGain(c, LINE_WAVE, 1);
    if (!s->InUse || s->Paused) return;
    while (produced < n) {
        PIRP irp = s->Cur;
        ULONG used = 0, got, len;
        if (!irp) {
            if (IsListEmpty(&s->Queue)) break;
            irp = CONTAINING_RECORD(s->Queue.Flink, IRP, IRP_LE_FIELD);
            s->Cur = irp; s->CurOffset = 0;
        }
        len = IRP_LEN(irp);
        got = ConvPlay(&s->Conv, (PUCHAR)IRP_VA(irp) + s->CurOffset, len - s->CurOffset, &used,
                       mix + produced * 2, n - produced, vl, vr);
        produced += got;
        s->CurOffset += used;
        s->BytesRendered += used;
        if (s->CurOffset >= len) {
            /* fully consumed: heard once the output reaches this frame (+2 for interpolation) */
            RemoveEntryList(&irp->IRP_LE_FIELD);
            IRP_END(irp) = (ULONG)(frameBase + produced + 2);
            InsertTailList(&s->Done, &irp->IRP_LE_FIELD);
            s->Cur = NULL; s->CurOffset = 0;
        } else if (!got && !used) {
            break;
        }
    }
    StreamHistAdd(s, frameBase + produced);
}

static BOOLEAN StreamHasData(PLAY_STREAM *s)
{
    return s->InUse && !s->Paused && (s->Cur || !IsListEmpty(&s->Queue));
}

static void MixChunk(CARD *c, SHORT *out, ULONG n)
{
    ULONG i;
    LONG *mix = c->MixBuf;
    LONG gl = CtrlGain(c, LINE_MASTER, 0), gr = CtrlGain(c, LINE_MASTER, 1);
    memset(mix, 0, n * 2 * sizeof(LONG));
    for (i = 0; i < MAX_PLAY_STREAMS; i++)
        if (c->Play[i].InUse) StreamRender(c, &c->Play[i], mix, n, c->WrittenFrames);
    if (c->Synth) SynthRender(c, mix, n);
    for (i = 0; i < n; i++) {
        LONG l = mix[i * 2], r = mix[i * 2 + 1];
        if (gl != 32768) l = (LONG)(((LONGLONG)l * gl) >> 15);
        if (gr != 32768) r = (LONG)(((LONGLONG)r * gr) >> 15);
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[i * 2] = (SHORT)l;
        out[i * 2 + 1] = (SHORT)r;
    }
}

static void CompleteHeard(CARD *c)
{
    ULONG i;
    ULONG played = (ULONG)c->PlayedFrames;
    for (i = 0; i < MAX_PLAY_STREAMS; i++) {
        PLAY_STREAM *s = &c->Play[i];
        while (!IsListEmpty(&s->Done)) {
            PIRP irp = CONTAINING_RECORD(s->Done.Flink, IRP, IRP_LE_FIELD);
            if ((LONG)(played - IRP_END(irp)) < 0 && c->Playing) break;
            IrpFinish(c, irp, STATUS_SUCCESS, IRP_LEN(irp));
        }
    }
}

static BOOLEAN AnyPlayData(CARD *c)
{
    ULONG i;
    for (i = 0; i < MAX_PLAY_STREAMS; i++)
        if (StreamHasData(&c->Play[i]) || !IsListEmpty(&c->Play[i].Done)) return TRUE;
    return c->Synth && SynthActive(c);
}

static void RenderAhead(CARD *c)
{
    ULONGLONG target = c->PlayedFrames + c->LatencyFrames;
    ULONGLONG limit = c->PlayedFrames + RING_FRAMES - RING_PERIOD_FRAMES;
    if (target > limit) target = limit;
    while (c->WrittenFrames < target) {
        ULONG off = (ULONG)(c->WrittenFrames % RING_FRAMES);
        ULONG n = (ULONG)MIN((ULONGLONG)MIX_CHUNK_FRAMES, target - c->WrittenFrames);
        if (n > RING_FRAMES - off) n = RING_FRAMES - off;
        MixChunk(c, (SHORT *)c->PlayRing.Va + off * 2, n);
        c->WrittenFrames += n;
    }
}

static void PlayProcess(CARD *c, ULONG elapsedMs)
{
    if (c->Ops->PlayCount) {
        /* the back-end counts consumed frames itself (USB) */
        ULONGLONG n = c->Ops->PlayCount(c);
        if (n > c->PlayedFrames) c->PlayedFrames = n;
        c->LastPlayPos = (ULONG)(n % RING_FRAMES);
    } else {
        ULONG pos = c->Ops->PlayPos(c) % RING_FRAMES;
        ULONG delta = (pos + RING_FRAMES - c->LastPlayPos) % RING_FRAMES;
        ULONG expect = elapsedMs * (ENG_RATE / 1000);
        /* a DPC delayed by more than one ring length: account for the lost wraps */
        while (expect > delta + RING_FRAMES / 2 + RING_FRAMES / 4 && expect < 60 * ENG_RATE) {
            delta += RING_FRAMES;
            if (delta > expect) break;
        }
        c->LastPlayPos = pos;
        c->PlayedFrames += delta;
    }
    if (c->Ops->PlayTick) c->Ops->PlayTick(c);

    if (c->WrittenFrames < c->PlayedFrames + 16) {
        if (c->WrittenFrames + 64 < c->PlayedFrames || AnyPlayData(c)) {
            c->Underruns++;
            LOGV("card%u: underrun (played %u written %u)", c->Index,
                 (ULONG)c->PlayedFrames, (ULONG)c->WrittenFrames);
        }
        c->WrittenFrames = c->PlayedFrames + 32;
    }
    RenderAhead(c);
    CompleteHeard(c);

    if (AnyPlayData(c)) c->IdleMs = 0;
    else c->IdleMs += elapsedMs;
    if (c->IdleMs >= IDLE_STOP_MS) {
        LOGV("card%u: idle, stopping playback DMA", c->Index);
        c->Ops->StopPlay(c);
        c->Playing = FALSE;
        CompleteHeard(c);
    }
}

void EngineKick(CARD *c)
{
    if (c->Stopping) return;
    c->IdleMs = 0;
    if (!c->Playing) {
        memset(c->PlayRing.Va, 0, RING_BYTES);
        c->PlayedFrames = 0;
        c->WrittenFrames = 0;
        c->LastPlayPos = 0;
        RenderAhead(c);
        if (!c->Ops->StartPlay(c)) {
            LOGE("card%u: StartPlay failed", c->Index);
            return;
        }
        c->LastPlayPos = c->Ops->PlayPos(c) % RING_FRAMES;
        c->Playing = TRUE;
        c->LastTickMs = OsTimeMs();
        LOGV("card%u: playback DMA started", c->Index);
    }
    TimerEnsure(c);
}

/* ------------------------------------------------------------------ */
/* recording                                                           */
/* ------------------------------------------------------------------ */
static BOOLEAN AnyRecRunning(CARD *c)
{
    ULONG i;
    for (i = 0; i < MAX_REC_STREAMS; i++)
        if (c->Rec[i].InUse && c->Rec[i].Running) return TRUE;
    return FALSE;
}

static void RecDistribute(CARD *c, const SHORT *src, ULONG frames)
{
    ULONG i;
    for (i = 0; i < MAX_REC_STREAMS; i++) {
        REC_STREAM *s = &c->Rec[i];
        const SHORT *p = src;
        ULONG left = frames;
        if (!s->InUse || !s->Running) continue;
        while (left) {
            PIRP irp; ULONG prod = 0, used, len;
            if (IsListEmpty(&s->Queue)) break;       /* overrun: drop */
            irp = CONTAINING_RECORD(s->Queue.Flink, IRP, IRP_LE_FIELD);
            len = IRP_LEN(irp);
            used = ConvRecord(&s->Conv, p, left, (PUCHAR)IRP_VA(irp) + s->CurOffset,
                              len - s->CurOffset, &prod);
            s->CurOffset += prod;
            s->BytesRecorded += prod;
            p += used * 2;
            left -= used;
            if (s->CurOffset >= len) {
                IrpFinish(c, irp, STATUS_SUCCESS, s->CurOffset);
                s->CurOffset = 0;
            } else if (!used) break;
        }
    }
}

static void RecProcess(CARD *c)
{
    ULONG pos = c->Ops->RecPos(c) % RING_FRAMES;
    const SHORT *ring = c->RecRing.Va;
    if (c->Ops->RecTick) c->Ops->RecTick(c);
    if (pos != c->LastRecPos) {
        if (pos > c->LastRecPos) {
            RecDistribute(c, ring + c->LastRecPos * 2, pos - c->LastRecPos);
            c->RecFrames += pos - c->LastRecPos;
        } else {
            RecDistribute(c, ring + c->LastRecPos * 2, RING_FRAMES - c->LastRecPos);
            RecDistribute(c, ring, pos);
            c->RecFrames += RING_FRAMES - c->LastRecPos + pos;
        }
        c->LastRecPos = pos;
    }
    if (!AnyRecRunning(c)) {
        c->Ops->StopRec(c);
        c->Recording = FALSE;
        LOGV("card%u: record DMA stopped", c->Index);
    }
}

static void RecKick(CARD *c)
{
    if (c->Stopping || c->Recording || !c->CanRecord) return;
    memset(c->RecRing.Va, 0, RING_BYTES);
    c->LastRecPos = 0;
    if (!c->Ops->StartRec(c)) { LOGE("card%u: StartRec failed", c->Index); return; }
    c->LastRecPos = c->Ops->RecPos(c) % RING_FRAMES;
    c->Recording = TRUE;
    LOGV("card%u: record DMA started", c->Index);
    TimerEnsure(c);
}

/* ------------------------------------------------------------------ */
void EngineProcess(CARD *c)
{
    ULONG now = OsTimeMs();
    ULONG elapsed = now - c->LastTickMs;
    c->LastTickMs = now;
    if (c->Stopping) return;
    if (c->Playing) PlayProcess(c, elapsed);
    if (c->Recording) RecProcess(c);
}

/* ------------------------------------------------------------------ */
/* wave out device                                                     */
/* ------------------------------------------------------------------ */
static PLAY_STREAM *PlayFind(CARD *c, PVOID fo)
{
    ULONG i;
    for (i = 0; i < MAX_PLAY_STREAMS; i++)
        if (c->Play[i].InUse && c->Play[i].FileObject == fo) return &c->Play[i];
    return NULL;
}

static void PlayFlush(CARD *c, PLAY_STREAM *s, NTSTATUS st)
{
    while (!IsListEmpty(&s->Done)) {
        PIRP irp = CONTAINING_RECORD(s->Done.Flink, IRP, IRP_LE_FIELD);
        IrpFinish(c, irp, st, IRP_LEN(irp));
    }
    while (!IsListEmpty(&s->Queue)) {
        PIRP irp = CONTAINING_RECORD(s->Queue.Flink, IRP, IRP_LE_FIELD);
        IrpFinish(c, irp, st, irp == s->Cur ? s->CurOffset : 0);
    }
    s->Cur = NULL; s->CurOffset = 0;
}

static void PlayResetPos(PLAY_STREAM *s)
{
    s->BytesRendered = 0;
    s->HistHead = 0;
    ConvReset(&s->Conv);
}

static void FillWaveOutCaps(CARD *c, WAVEOUTCAPSW *caps)
{
    char name[40];
    memset(caps, 0, sizeof(*caps));
    caps->wMid = MM_MICROSOFT; caps->wPid = 1;
    caps->vDriverVersion = AUDIONT_VERSION;
    CardNameFor(c, DEV_WAVEOUT, name, sizeof(name));
    OsWcsCopyA(caps->szPname, name, MAXPNAMELEN);
    caps->dwFormats = 0xFFF;
    caps->wChannels = 2;
    caps->dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME | WAVECAPS_SAMPLEACCURATE;
}

NTSTATUS WaveOutIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp)
{
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    ULONG code = IrpCode(sp), inl = IrpInLen(sp), outl = IrpOutLen(sp);
    PVOID fo = sp->FileObject;
    PLAY_STREAM *s;
    PCM_FORMAT f;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG info = 0, i;

    switch (code) {
    case IOCTL_WAVE_GET_CAPABILITIES: {
        WAVEOUTCAPSW caps;
        FillWaveOutCaps(c, &caps);
        info = MIN(outl, sizeof(caps));
        memcpy(buf, &caps, info);
        irp->IoStatus.Information = info;
        return STATUS_SUCCESS;
    }
    case IOCTL_WAVE_QUERY_FORMAT:
        return ConvValidFormat(buf, inl, NULL) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    case IOCTL_WAVE_SET_FORMAT:
        if (!ConvValidFormat(buf, inl, &f)) return STATUS_NOT_SUPPORTED;
        EngineLock(c);
        s = PlayFind(c, fo);
        if (!s) {
            for (i = 0; i < MAX_PLAY_STREAMS; i++)
                if (!c->Play[i].InUse) { s = &c->Play[i]; break; }
            if (!s) { EngineUnlock(c); return STATUS_DEVICE_BUSY; }
            s->InUse = TRUE; s->FileObject = fo; s->Paused = FALSE;
            s->Cur = NULL; s->CurOffset = 0;
        } else {
            PlayFlush(c, s, STATUS_SUCCESS);
        }
        ConvSetup(&s->Conv, &f, FALSE);
        PlayResetPos(s);
        EngineUnlock(c);
        LOGV("waveout: format %u Hz %u ch %u bit", f.Rate, f.Channels, f.Bits);
        return STATUS_SUCCESS;
    case IOCTL_WAVE_SET_STATE:
        if (inl < 4) return STATUS_INVALID_PARAMETER;
        EngineLock(c);
        s = PlayFind(c, fo);
        if (s) {
            switch (*(PULONG)buf) {
            case WAVE_DD_STOP:  s->Paused = TRUE; break;
            case WAVE_DD_PLAY:  s->Paused = FALSE; if (StreamHasData(s)) EngineKick(c); break;
            case WAVE_DD_RESET:
                PlayFlush(c, s, STATUS_SUCCESS);
                PlayResetPos(s);
                s->Paused = FALSE;
                break;
            default: st = STATUS_INVALID_PARAMETER;
            }
        }
        EngineUnlock(c);
        return st;
    case IOCTL_WAVE_GET_STATE:
        if (outl < 4) return STATUS_BUFFER_TOO_SMALL;
        EngineLock(c);
        s = PlayFind(c, fo);
        *(PULONG)buf = !s ? WAVE_DD_IDLE : s->Paused ? WAVE_DD_STOPPED :
                       (StreamHasData(s) || !IsListEmpty(&s->Done)) ? WAVE_DD_PLAYING : WAVE_DD_IDLE;
        EngineUnlock(c);
        irp->IoStatus.Information = 4;
        return STATUS_SUCCESS;
    case IOCTL_WAVE_GET_POSITION: {
        WAVE_DD_POSITION pos;
        if (outl < sizeof(pos)) return STATUS_BUFFER_TOO_SMALL;
        EngineLock(c);
        s = PlayFind(c, fo);
        if (s) {
            if (c->Playing) EngineProcess(c);
            pos.ByteCount = StreamPlayedBytes(c, s);
            pos.SampleCount = pos.ByteCount / s->Conv.Fmt.BlockAlign;
        } else pos.ByteCount = pos.SampleCount = 0;
        EngineUnlock(c);
        memcpy(buf, &pos, sizeof(pos));
        irp->IoStatus.Information = sizeof(pos);
        return STATUS_SUCCESS;
    }
    case IOCTL_WAVE_SET_VOLUME:
        if (inl < sizeof(WAVE_DD_VOLUME)) return STATUS_INVALID_PARAMETER;
        c->Lines[LINE_WAVE].Left  = (USHORT)(((WAVE_DD_VOLUME *)buf)->Left >> 16);
        c->Lines[LINE_WAVE].Right = (USHORT)(((WAVE_DD_VOLUME *)buf)->Right >> 16);
        c->ChangeCount++;
        return STATUS_SUCCESS;
    case IOCTL_WAVE_GET_VOLUME: {
        WAVE_DD_VOLUME v;
        if (outl < sizeof(v)) return STATUS_BUFFER_TOO_SMALL;
        v.Left  = (ULONG)c->Lines[LINE_WAVE].Left << 16 | c->Lines[LINE_WAVE].Left;
        v.Right = (ULONG)c->Lines[LINE_WAVE].Right << 16 | c->Lines[LINE_WAVE].Right;
        memcpy(buf, &v, sizeof(v));
        irp->IoStatus.Information = sizeof(v);
        return STATUS_SUCCESS;
    }
    case IOCTL_SOUND_GET_CHANGED_VOLUME:
        return STATUS_NOT_SUPPORTED;
    case IOCTL_WAVE_BREAK_LOOP:
    case IOCTL_WAVE_SET_LOW_PRIORITY:
        return STATUS_SUCCESS;
    case IOCTL_WAVE_PLAY:
        return WaveOutWrite(c, irp, sp);
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS WaveOutWrite(CARD *c, PIRP irp, PIO_STACK_LOCATION sp)
{
    PLAY_STREAM *s;
    ULONG len = (sp->MajorFunction == IRP_MJ_WRITE) ? sp->Parameters.Write.Length
                                                     : IrpOutLen(sp);
    PVOID va = OsMdlAddress(irp->MdlAddress);
    if (!len) return STATUS_SUCCESS;
    if (!va) return STATUS_INSUFFICIENT_RESOURCES;
    EngineLock(c);
    s = PlayFind(c, sp->FileObject);
    if (!s) { EngineUnlock(c); return STATUS_INVALID_DEVICE_REQUEST; }
    IRP_VA(irp) = va;
    IRP_LEN(irp) = len;
    IrpQueue(c, &s->Queue, irp);
    if (!s->Paused) EngineKick(c);
    EngineUnlock(c);
    return STATUS_PENDING;
}

/* ------------------------------------------------------------------ */
/* wave in device                                                      */
/* ------------------------------------------------------------------ */
static REC_STREAM *RecFind(CARD *c, PVOID fo)
{
    ULONG i;
    for (i = 0; i < MAX_REC_STREAMS; i++)
        if (c->Rec[i].InUse && c->Rec[i].FileObject == fo) return &c->Rec[i];
    return NULL;
}

static void RecFlush(CARD *c, REC_STREAM *s)
{
    BOOLEAN first = TRUE;
    while (!IsListEmpty(&s->Queue)) {
        PIRP irp = CONTAINING_RECORD(s->Queue.Flink, IRP, IRP_LE_FIELD);
        IrpFinish(c, irp, STATUS_SUCCESS, first ? s->CurOffset : 0);
        first = FALSE;
    }
    s->CurOffset = 0;
}

NTSTATUS WaveInIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp)
{
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    ULONG code = IrpCode(sp), inl = IrpInLen(sp), outl = IrpOutLen(sp), i;
    PVOID fo = sp->FileObject;
    REC_STREAM *s;
    PCM_FORMAT f;
    NTSTATUS st = STATUS_SUCCESS;

    switch (code) {
    case IOCTL_WAVE_GET_CAPABILITIES: {
        WAVEINCAPSW caps; char name[40];
        memset(&caps, 0, sizeof(caps));
        caps.wMid = MM_MICROSOFT; caps.wPid = 2;
        caps.vDriverVersion = AUDIONT_VERSION;
        CardNameFor(c, DEV_WAVEIN, name, sizeof(name));
        OsWcsCopyA(caps.szPname, name, MAXPNAMELEN);
        caps.dwFormats = 0xFFF;
        caps.wChannels = 2;
        irp->IoStatus.Information = MIN(outl, sizeof(caps));
        memcpy(buf, &caps, irp->IoStatus.Information);
        return STATUS_SUCCESS;
    }
    case IOCTL_WAVE_QUERY_FORMAT:
        return ConvValidFormat(buf, inl, NULL) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    case IOCTL_WAVE_SET_FORMAT:
        if (!ConvValidFormat(buf, inl, &f)) return STATUS_NOT_SUPPORTED;
        EngineLock(c);
        s = RecFind(c, fo);
        if (!s) {
            for (i = 0; i < MAX_REC_STREAMS; i++)
                if (!c->Rec[i].InUse) { s = &c->Rec[i]; break; }
            if (!s) { EngineUnlock(c); return STATUS_DEVICE_BUSY; }
            s->InUse = TRUE; s->FileObject = fo; s->Running = FALSE;
        } else {
            RecFlush(c, s);
        }
        ConvSetup(&s->Conv, &f, TRUE);
        s->BytesRecorded = 0; s->CurOffset = 0;
        EngineUnlock(c);
        return STATUS_SUCCESS;
    case IOCTL_WAVE_SET_STATE:
        if (inl < 4) return STATUS_INVALID_PARAMETER;
        EngineLock(c);
        s = RecFind(c, fo);
        if (s) {
            switch (*(PULONG)buf) {
            case WAVE_DD_RECORD: s->Running = TRUE; RecKick(c); break;
            case WAVE_DD_STOP:
                s->Running = FALSE;
                /* return the partially filled buffer, as the NT drivers do */
                if (!IsListEmpty(&s->Queue) && s->CurOffset) {
                    PIRP h = CONTAINING_RECORD(s->Queue.Flink, IRP, IRP_LE_FIELD);
                    IrpFinish(c, h, STATUS_SUCCESS, s->CurOffset);
                    s->CurOffset = 0;
                }
                break;
            case WAVE_DD_RESET:
                s->Running = FALSE;
                RecFlush(c, s);
                s->BytesRecorded = 0;
                ConvReset(&s->Conv);
                break;
            default: st = STATUS_INVALID_PARAMETER;
            }
        }
        EngineUnlock(c);
        return st;
    case IOCTL_WAVE_GET_STATE:
        if (outl < 4) return STATUS_BUFFER_TOO_SMALL;
        EngineLock(c);
        s = RecFind(c, fo);
        *(PULONG)buf = !s ? WAVE_DD_IDLE : s->Running ? WAVE_DD_RECORDING : WAVE_DD_STOPPED;
        EngineUnlock(c);
        irp->IoStatus.Information = 4;
        return STATUS_SUCCESS;
    case IOCTL_WAVE_GET_POSITION: {
        WAVE_DD_POSITION pos;
        if (outl < sizeof(pos)) return STATUS_BUFFER_TOO_SMALL;
        EngineLock(c);
        s = RecFind(c, fo);
        if (s) {
            if (c->Recording) EngineProcess(c);
            pos.ByteCount = s->BytesRecorded;
            pos.SampleCount = pos.ByteCount / s->Conv.Fmt.BlockAlign;
        } else pos.ByteCount = pos.SampleCount = 0;
        EngineUnlock(c);
        memcpy(buf, &pos, sizeof(pos));
        irp->IoStatus.Information = sizeof(pos);
        return STATUS_SUCCESS;
    }
    case IOCTL_WAVE_SET_VOLUME:
        if (inl < sizeof(WAVE_DD_VOLUME)) return STATUS_INVALID_PARAMETER;
        EngineLock(c);                    /* codec access must not interleave with the tick */
        c->Lines[LINE_RECORD].Left  = (USHORT)(((WAVE_DD_VOLUME *)buf)->Left >> 16);
        c->Lines[LINE_RECORD].Right = (USHORT)(((WAVE_DD_VOLUME *)buf)->Right >> 16);
        if (c->Ops->ApplyLine) c->Ops->ApplyLine(c, LINE_RECORD);
        c->ChangeCount++;
        EngineUnlock(c);
        return STATUS_SUCCESS;
    case IOCTL_WAVE_GET_VOLUME: {
        WAVE_DD_VOLUME v;
        if (outl < sizeof(v)) return STATUS_BUFFER_TOO_SMALL;
        v.Left  = (ULONG)c->Lines[LINE_RECORD].Left << 16 | c->Lines[LINE_RECORD].Left;
        v.Right = (ULONG)c->Lines[LINE_RECORD].Right << 16 | c->Lines[LINE_RECORD].Right;
        memcpy(buf, &v, sizeof(v));
        irp->IoStatus.Information = sizeof(v);
        return STATUS_SUCCESS;
    }
    case IOCTL_WAVE_SET_LOW_PRIORITY:
        return STATUS_SUCCESS;
    case IOCTL_WAVE_RECORD:
        return WaveInRead(c, irp, sp);
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS WaveInRead(CARD *c, PIRP irp, PIO_STACK_LOCATION sp)
{
    REC_STREAM *s;
    ULONG len = (sp->MajorFunction == IRP_MJ_READ) ? sp->Parameters.Read.Length : IrpOutLen(sp);
    PVOID va = OsMdlAddress(irp->MdlAddress);
    if (!c->CanRecord) return STATUS_NOT_SUPPORTED;
    if (!len) return STATUS_SUCCESS;
    if (!va) return STATUS_INSUFFICIENT_RESOURCES;
    EngineLock(c);
    s = RecFind(c, sp->FileObject);
    if (!s) { EngineUnlock(c); return STATUS_INVALID_DEVICE_REQUEST; }
    IRP_VA(irp) = va;
    IRP_LEN(irp) = len;
    IrpQueue(c, &s->Queue, irp);
    EngineUnlock(c);
    return STATUS_PENDING;
}

/* ------------------------------------------------------------------ */
void StreamCleanup(CARD *c, PVOID fo)
{
    PLAY_STREAM *p;
    REC_STREAM *r;
    EngineLock(c);
    while ((p = PlayFind(c, fo)) != NULL) {
        PlayFlush(c, p, STATUS_CANCELLED);
        ConvFree(&p->Conv);
        p->InUse = FALSE; p->FileObject = NULL;
    }
    while ((r = RecFind(c, fo)) != NULL) {
        RecFlush(c, r);
        ConvFree(&r->Conv);
        r->InUse = FALSE; r->Running = FALSE; r->FileObject = NULL;
    }
    EngineUnlock(c);
}

void EngineStopAll(CARD *c)
{
    ULONG i;
    EngineLock(c);
    for (i = 0; i < MAX_PLAY_STREAMS; i++) if (c->Play[i].InUse) PlayFlush(c, &c->Play[i], STATUS_CANCELLED);
    for (i = 0; i < MAX_REC_STREAMS; i++) if (c->Rec[i].InUse) RecFlush(c, &c->Rec[i]);
    if (c->Playing) { c->Ops->StopPlay(c); c->Playing = FALSE; }
    if (c->Recording) { c->Ops->StopRec(c); c->Recording = FALSE; }
    EngineUnlock(c);
}
