/*
 * AUDIONT user-mode multimedia driver (audiont.dll)
 *
 * Installable winmm driver for Windows NT 3.1 - 4.0 that talks to the
 * audiont.sys kernel driver through its private device names
 * (\\.\AudntWaveOutN, \\.\AudntWaveInN, \\.\AudntMidiOutN, \\.\AudntAuxN,
 * \\.\AudioNTN).  Compared with mmdrv.dll it allows any number of
 * applications to play at the same time (the kernel mixes them), and it
 * implements the mixer API (mxdMessage) for winmm versions that have it.
 *
 * Exports: DriverProc, wodMessage, widMessage, modMessage, auxMessage,
 *          mxdMessage
 */
#include "ucrt.h"
#include <mmddk.h>
#include "audiontio.h"

/* kernel interface (same codes as ntddwave.h etc.) */
#define SND_CTL(f, m, a) (((0x1d) << 16) | ((a) << 14) | ((f) << 2) | (m))
#define IOCTL_WAVE_QUERY_FORMAT      SND_CTL(0x01, 0, 1)
#define IOCTL_WAVE_SET_FORMAT        SND_CTL(0x02, 0, 2)
#define IOCTL_WAVE_GET_CAPABILITIES  SND_CTL(0x03, 0, 1)
#define IOCTL_WAVE_SET_STATE         SND_CTL(0x04, 0, 2)
#define IOCTL_WAVE_GET_POSITION      SND_CTL(0x06, 0, 2)
#define IOCTL_WAVE_SET_VOLUME        SND_CTL(0x07, 0, 1)
#define IOCTL_WAVE_GET_VOLUME        SND_CTL(0x08, 0, 1)
#define IOCTL_MIDI_GET_CAPABILITIES  SND_CTL(0x81, 0, 1)
#define IOCTL_MIDI_SET_STATE         SND_CTL(0x82, 0, 2)
#define IOCTL_MIDI_SET_VOLUME        SND_CTL(0x84, 0, 1)
#define IOCTL_MIDI_GET_VOLUME        SND_CTL(0x85, 0, 1)
#define IOCTL_MIDI_PLAY              SND_CTL(0x86, 3, 2)
#define IOCTL_AUX_GET_CAPABILITIES   SND_CTL(0x101, 0, 1)
#define IOCTL_AUX_SET_VOLUME         SND_CTL(0x102, 0, 1)
#define IOCTL_AUX_GET_VOLUME         SND_CTL(0x103, 0, 1)
#define DD_STOP   1
#define DD_PLAY   2
#define DD_RECORD 3
#define DD_RESET  4

typedef struct { ULONG Left, Right; } DD_VOLUME;
typedef struct { ULONG SampleCount, ByteCount; } DD_POSITION;

static HINSTANCE g_hInst;
static HANDLE g_Heap;

/* debug trace into the kernel driver log */
static void Trace(const char *fmt, ...)
{
    char buf[200]; va_list ap; int n; HANDLE h; DWORD got;
    va_start(ap, fmt); n = wvsprintfA(buf, fmt, ap); va_end(ap);
    h = CreateFileW(L"\\\\.\\AudioNT0", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DeviceIoControl(h, IOCTL_AUDIONT_USERLOG, buf, n, 0, 0, &got, 0);
    CloseHandle(h);
}
#ifdef AUDRV_TRACE
#define TR(x) Trace x
#else
#define TR(x)
#endif

static void *xalloc(DWORD n) { return HeapAlloc(g_Heap, HEAP_ZERO_MEMORY, n); }
static void  xfree(void *p) { if (p) HeapFree(g_Heap, 0, p); }

/* ------------------------------------------------------------------ */
/* device helpers                                                      */
/* ------------------------------------------------------------------ */
static void DevName(WCHAR *buf, const WCHAR *kind, UINT n)
{
    wsprintfW(buf, L"\\\\.\\%s%u", kind, n);
}

static HANDLE DevOpen(const WCHAR *kind, UINT n, BOOL overlapped)
{
    WCHAR name[64];
    DevName(name, kind, n);
    return CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                       OPEN_EXISTING, overlapped ? FILE_FLAG_OVERLAPPED : 0, 0);
}

static UINT DevCount(const WCHAR *kind)
{
    UINT n;
    for (n = 0; n < 16; n++) {
        HANDLE h = DevOpen(kind, n, FALSE);
        if (h == INVALID_HANDLE_VALUE) break;
        CloseHandle(h);
    }
    return n;
}

/* synchronous IOCTL that also works on overlapped handles */
static BOOL Ioctl(HANDLE h, BOOL ovl, DWORD code, void *in, DWORD inl, void *out, DWORD outl, DWORD *ret)
{
    DWORD got = 0;
    BOOL ok;
    if (!ovl) {
        ok = DeviceIoControl(h, code, in, inl, out, outl, &got, 0);
    } else {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
        ok = DeviceIoControl(h, code, in, inl, out, outl, &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &got, TRUE);
        CloseHandle(ov.hEvent);
    }
    if (ret) *ret = got;
    return ok;
}

static DWORD ErrToMm(void)
{
    switch (GetLastError()) {
    case ERROR_NOT_SUPPORTED: return WAVERR_BADFORMAT;
    case ERROR_BUSY: return MMSYSERR_ALLOCATED;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return MMSYSERR_NOMEM;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return MMSYSERR_BADDEVICEID;
    default: return MMSYSERR_ERROR;
    }
}

static DWORD VolFromDd(DD_VOLUME *v) { return (v->Left >> 16) | (v->Right & 0xFFFF0000UL); }
static void  VolToDd(DWORD vol, DD_VOLUME *v)
{
    ULONG l = vol & 0xFFFF, r = vol >> 16;
    v->Left = (l << 16) | l; v->Right = (r << 16) | r;
}

/* ------------------------------------------------------------------ */
/* streaming instance (wave out / wave in)                             */
/* ------------------------------------------------------------------ */
#define CMD_NONE   0
#define CMD_WRITE  1
#define CMD_STATE  2
#define CMD_RESET  3
#define CMD_EXIT   4
#define CMD_ADD    5

typedef struct _WINST WINST;
typedef struct _IOREQ {
    OVERLAPPED Ov;                 /* must be first */
    WINST     *Inst;
    LPWAVEHDR  Hdr;
    BOOL       Final;
} IOREQ;

struct _WINST {
    BOOL        Input;
    HANDLE      Dev;
    HANDLE      Thread;
    DWORD       ThreadId;
    HANDLE      CmdEvent, DoneEvent;
    CRITICAL_SECTION Lock;
    DWORD       Cmd, Arg, Result;
    LPWAVEHDR   ArgHdr;
    LONG        Pending;           /* IOREQs in flight (I/O thread only) */
    DWORD_PTR   Callback, Instance;
    DWORD       Flags;
    HWAVE       hWave;
    WAVEFORMATEX Fmt;
    /* loop handling */
    LPWAVEHDR   Loop[64];
    UINT        LoopCount;         /* headers collected since BEGINLOOP */
    DWORD       LoopsLeft;
    BOOL        InLoop, BreakLoop;
    LONG        Queued;            /* headers handed to us and not yet returned */
};

static void Notify(WINST *w, UINT msg, DWORD_PTR p1)
{
    TR(("notify msg %x p1 %x cb %x flags %x", msg, p1, w->Callback, w->Flags));
    DriverCallback(w->Callback, HIWORD(w->Flags), (HDRVR)w->hWave, msg, w->Instance, p1, 0);
}

static VOID CALLBACK IoDone(DWORD err, DWORD bytes, LPOVERLAPPED ov)
{
    IOREQ *r = (IOREQ *)ov;
    WINST *w = r->Inst;
    LPWAVEHDR h = r->Hdr;
    (void)err;
    w->Pending--;
    if (r->Final) {
        if (w->Input) h->dwBytesRecorded = bytes;
        h->dwFlags &= ~WHDR_INQUEUE;
        h->dwFlags |= WHDR_DONE;
        InterlockedDecrement(&w->Queued);
        Notify(w, w->Input ? WIM_DATA : WOM_DONE, (DWORD_PTR)h);
    }
    xfree(r);
}

static BOOL Submit(WINST *w, LPWAVEHDR h, BOOL final)
{
    IOREQ *r = xalloc(sizeof(IOREQ));
    BOOL ok;
    if (!r) return FALSE;
    r->Inst = w; r->Hdr = h; r->Final = final;
    w->Pending++;
    if (w->Input) ok = ReadFileEx(w->Dev, h->lpData, h->dwBufferLength, &r->Ov, IoDone);
    else          ok = WriteFileEx(w->Dev, h->lpData, h->dwBufferLength, &r->Ov, IoDone);
    if (!ok) {
        /* report it as done so the application does not hang */
        w->Pending--;
        xfree(r);
        if (final) {
            h->dwFlags &= ~WHDR_INQUEUE; h->dwFlags |= WHDR_DONE;
            InterlockedDecrement(&w->Queued);
            Notify(w, w->Input ? WIM_DATA : WOM_DONE, (DWORD_PTR)h);
        }
    }
    return ok;
}

/* playback header with loop semantics */
static void PlayHeader(WINST *w, LPWAVEHDR h)
{
    UINT i, rep;
    if (h->dwFlags & WHDR_BEGINLOOP) {
        w->InLoop = TRUE; w->BreakLoop = FALSE;
        w->LoopsLeft = h->dwLoops ? h->dwLoops : 1;
        w->LoopCount = 0;
    }
    if (!w->InLoop || w->LoopsLeft <= 1) {
        Submit(w, h, TRUE);
        if (h->dwFlags & WHDR_ENDLOOP) w->InLoop = FALSE;
        return;
    }
    /* inside a loop: first pass is not final */
    if (w->LoopCount < 64) w->Loop[w->LoopCount++] = h;
    Submit(w, h, FALSE);
    if (h->dwFlags & WHDR_ENDLOOP) {
        for (rep = 2; rep <= w->LoopsLeft && !w->BreakLoop; rep++)
            for (i = 0; i < w->LoopCount; i++)
                Submit(w, w->Loop[i], rep == w->LoopsLeft);
        if (w->BreakLoop)                   /* make sure every header is returned once */
            for (i = 0; i < w->LoopCount; i++) Submit(w, w->Loop[i], TRUE);
        w->InLoop = FALSE;
    }
}

static void DrainApcs(WINST *w, DWORD maxMs)
{
    DWORD t0 = GetTickCount();
    while (w->Pending > 0 && GetTickCount() - t0 < maxMs) SleepEx(10, TRUE);
}

static void DoCmd(WINST *w)
{
    DWORD st;
    switch (w->Cmd) {
    case CMD_WRITE:
        PlayHeader(w, w->ArgHdr);
        w->Result = MMSYSERR_NOERROR;
        break;
    case CMD_ADD:
        Submit(w, w->ArgHdr, TRUE);
        w->Result = MMSYSERR_NOERROR;
        break;
    case CMD_STATE:
        st = w->Arg;
        w->Result = Ioctl(w->Dev, TRUE, IOCTL_WAVE_SET_STATE, &st, 4, 0, 0, 0) ? MMSYSERR_NOERROR : ErrToMm();
        if (st == DD_STOP && w->Input) SleepEx(0, TRUE);
        break;
    case CMD_RESET:
        st = DD_RESET;
        w->BreakLoop = TRUE;
        w->Result = Ioctl(w->Dev, TRUE, IOCTL_WAVE_SET_STATE, &st, 4, 0, 0, 0) ? MMSYSERR_NOERROR : ErrToMm();
        DrainApcs(w, 5000);
        w->InLoop = FALSE;
        break;
    }
}

static DWORD WINAPI IoThread(LPVOID p)
{
    WINST *w = p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        DWORD r = WaitForSingleObjectEx(w->CmdEvent, INFINITE, TRUE);
        if (r == WAIT_IO_COMPLETION) continue;
        if (w->Cmd == CMD_EXIT) { DrainApcs(w, 2000); SetEvent(w->DoneEvent); break; }
        DoCmd(w);
        SetEvent(w->DoneEvent);
    }
    return 0;
}

static DWORD Send(WINST *w, DWORD cmd, DWORD arg, LPWAVEHDR hdr)
{
    DWORD r;
    if (GetCurrentThreadId() == w->ThreadId) {     /* called from our own callback */
        w->Cmd = cmd; w->Arg = arg; w->ArgHdr = hdr;
        DoCmd(w);
        return w->Result;
    }
    EnterCriticalSection(&w->Lock);
    w->Cmd = cmd; w->Arg = arg; w->ArgHdr = hdr;
    SetEvent(w->CmdEvent);
    WaitForSingleObject(w->DoneEvent, INFINITE);
    r = w->Result;
    LeaveCriticalSection(&w->Lock);
    return r;
}

static DWORD WaveOpen(BOOL input, UINT dev, DWORD_PTR *pUser, LPWAVEOPENDESC d, DWORD flags)
{
    WINST *w;
    HANDLE h;
    DWORD len;
    const WCHAR *kind = input ? L"AudntWaveIn" : L"AudntWaveOut";
    if (!d || !d->lpFormat) return MMSYSERR_INVALPARAM;
    len = d->lpFormat->wFormatTag == WAVE_FORMAT_PCM ? sizeof(PCMWAVEFORMAT)
          : sizeof(WAVEFORMATEX) + ((LPWAVEFORMATEX)d->lpFormat)->cbSize;
    h = DevOpen(kind, dev, TRUE);
    if (h == INVALID_HANDLE_VALUE) return MMSYSERR_BADDEVICEID;
    if (flags & WAVE_FORMAT_QUERY) {
        BOOL ok = Ioctl(h, TRUE, IOCTL_WAVE_QUERY_FORMAT, d->lpFormat, len, 0, 0, 0);
        CloseHandle(h);
        return ok ? MMSYSERR_NOERROR : WAVERR_BADFORMAT;
    }
    if (!Ioctl(h, TRUE, IOCTL_WAVE_SET_FORMAT, d->lpFormat, len, 0, 0, 0)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        return e == ERROR_BUSY ? MMSYSERR_ALLOCATED : WAVERR_BADFORMAT;
    }
    w = xalloc(sizeof(WINST));
    if (!w) { CloseHandle(h); return MMSYSERR_NOMEM; }
    w->Input = input; w->Dev = h;
    w->Callback = d->dwCallback; w->Instance = d->dwInstance; w->Flags = flags; w->hWave = d->hWave;
    memcpy(&w->Fmt, d->lpFormat, len < sizeof(w->Fmt) ? len : sizeof(w->Fmt));
    InitializeCriticalSection(&w->Lock);
    w->CmdEvent = CreateEventW(0, FALSE, FALSE, 0);
    w->DoneEvent = CreateEventW(0, FALSE, FALSE, 0);
    w->Thread = CreateThread(0, 0, IoThread, w, 0, &w->ThreadId);
    if (!w->Thread || !w->CmdEvent || !w->DoneEvent) {
        CloseHandle(h); xfree(w); return MMSYSERR_NOMEM;
    }
    *pUser = (DWORD_PTR)w;
    Notify(w, input ? WIM_OPEN : WOM_OPEN, 0);
    return MMSYSERR_NOERROR;
}

static DWORD WaveClose(WINST *w)
{
    if (w->Queued) return WAVERR_STILLPLAYING;
    Send(w, CMD_EXIT, 0, 0);
    WaitForSingleObject(w->Thread, 5000);
    CloseHandle(w->Thread);
    CloseHandle(w->Dev);
    CloseHandle(w->CmdEvent); CloseHandle(w->DoneEvent);
    DeleteCriticalSection(&w->Lock);
    Notify(w, w->Input ? WIM_CLOSE : WOM_CLOSE, 0);
    xfree(w);
    return MMSYSERR_NOERROR;
}

static DWORD WaveGetPos(WINST *w, LPMMTIME t, DWORD size)
{
    DD_POSITION p;
    if (!t || size < sizeof(MMTIME)) return MMSYSERR_INVALPARAM;
    if (!Ioctl(w->Dev, TRUE, IOCTL_WAVE_GET_POSITION, 0, 0, &p, sizeof(p), 0)) return ErrToMm();
    switch (t->wType) {
    case TIME_SAMPLES: t->u.sample = p.SampleCount; break;
    case TIME_MS:
        if (w->Fmt.nSamplesPerSec) {
            t->u.ms = (DWORD)(((unsigned __int64)p.SampleCount * 1000) / w->Fmt.nSamplesPerSec);
            break;
        }
        /* fall through */
    default: t->wType = TIME_BYTES; t->u.cb = p.ByteCount; break;
    }
    return MMSYSERR_NOERROR;
}

static DWORD DevVolume(const WCHAR *kind, UINT dev, DWORD code, DWORD *vol)
{
    HANDLE h = DevOpen(kind, dev, FALSE);
    DD_VOLUME v;
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return MMSYSERR_BADDEVICEID;
    if (code == IOCTL_WAVE_SET_VOLUME || code == IOCTL_MIDI_SET_VOLUME || code == IOCTL_AUX_SET_VOLUME) {
        VolToDd(*vol, &v);
        ok = Ioctl(h, FALSE, code, &v, sizeof(v), 0, 0, 0);
    } else {
        ok = Ioctl(h, FALSE, code, 0, 0, &v, sizeof(v), 0);
        if (ok) *vol = VolFromDd(&v);
    }
    CloseHandle(h);
    return ok ? MMSYSERR_NOERROR : MMSYSERR_NOTSUPPORTED;
}

static DWORD DevCaps(const WCHAR *kind, UINT dev, DWORD code, void *caps, DWORD size)
{
    HANDLE h = DevOpen(kind, dev, FALSE);
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return MMSYSERR_BADDEVICEID;
    ok = Ioctl(h, FALSE, code, 0, 0, caps, size, 0);
    CloseHandle(h);
    return ok ? MMSYSERR_NOERROR : MMSYSERR_ERROR;
}

/* ------------------------------------------------------------------ */
/* messages every driver entry point accepts */
#define COMMON_MSGS(msg) \
    if ((msg) == DRVM_INIT || (msg) == DRVM_EXIT || (msg) == DRVM_ENABLE || (msg) == DRVM_DISABLE) \
        return MMSYSERR_NOERROR

DWORD APIENTRY wodMessage(UINT dev, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2)
{
    WINST *w = (WINST *)user;
    LPWAVEHDR h = (LPWAVEHDR)p1;
    TR(("wod dev %u msg %u user %x p1 %x p2 %x tid %x", dev, msg, user, p1, p2, GetCurrentThreadId()));
    COMMON_MSGS(msg);
    switch (msg) {
    case WODM_GETNUMDEVS: return DevCount(L"AudntWaveOut");
    case WODM_GETDEVCAPS: return DevCaps(L"AudntWaveOut", dev, IOCTL_WAVE_GET_CAPABILITIES, (void *)p1, p2);
    case WODM_OPEN:       return WaveOpen(FALSE, dev, (DWORD_PTR *)user, (LPWAVEOPENDESC)p1, p2);
    case WODM_CLOSE:      return WaveClose(w);
    case WODM_WRITE:
        if (!h || !(h->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
        if (h->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
        h->dwFlags &= ~WHDR_DONE;
        h->dwFlags |= WHDR_INQUEUE;
        h->lpNext = 0;
        InterlockedIncrement(&w->Queued);
        return Send(w, CMD_WRITE, 0, h);
    case WODM_PAUSE:      return Send(w, CMD_STATE, DD_STOP, 0);
    case WODM_RESTART:    return Send(w, CMD_STATE, DD_PLAY, 0);
    case WODM_RESET:      return Send(w, CMD_RESET, 0, 0);
    case WODM_BREAKLOOP:  if (w) w->BreakLoop = TRUE; return MMSYSERR_NOERROR;
    case WODM_GETPOS:     return WaveGetPos(w, (LPMMTIME)p1, p2);
    case WODM_GETVOLUME:  return DevVolume(L"AudntWaveOut", dev, IOCTL_WAVE_GET_VOLUME, (DWORD *)p1);
    case WODM_SETVOLUME:  { DWORD v = (DWORD)p1; return DevVolume(L"AudntWaveOut", dev, IOCTL_WAVE_SET_VOLUME, &v); }
    case WODM_PREPARE: case WODM_UNPREPARE:
    case WODM_GETPITCH: case WODM_SETPITCH: case WODM_GETPLAYBACKRATE: case WODM_SETPLAYBACKRATE:
    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}

DWORD APIENTRY widMessage(UINT dev, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2)
{
    WINST *w = (WINST *)user;
    LPWAVEHDR h = (LPWAVEHDR)p1;
    TR(("wid dev %u msg %u user %x p1 %x p2 %x", dev, msg, user, p1, p2));
    COMMON_MSGS(msg);
    switch (msg) {
    case WIDM_GETNUMDEVS: return DevCount(L"AudntWaveIn");
    case WIDM_GETDEVCAPS: return DevCaps(L"AudntWaveIn", dev, IOCTL_WAVE_GET_CAPABILITIES, (void *)p1, p2);
    case WIDM_OPEN:       return WaveOpen(TRUE, dev, (DWORD_PTR *)user, (LPWAVEOPENDESC)p1, p2);
    case WIDM_CLOSE:      return WaveClose(w);
    case WIDM_ADDBUFFER:
        if (!h || !(h->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
        if (h->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
        h->dwFlags &= ~WHDR_DONE;
        h->dwFlags |= WHDR_INQUEUE;
        h->dwBytesRecorded = 0;
        InterlockedIncrement(&w->Queued);
        return Send(w, CMD_ADD, 0, h);
    case WIDM_START:      return Send(w, CMD_STATE, DD_RECORD, 0);
    case WIDM_STOP:       return Send(w, CMD_STATE, DD_STOP, 0);
    case WIDM_RESET:      return Send(w, CMD_RESET, 0, 0);
    case WIDM_GETPOS:     return WaveGetPos(w, (LPMMTIME)p1, p2);
    default:              return MMSYSERR_NOTSUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* MIDI out                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    HANDLE Dev;
    DWORD_PTR Callback, Instance;
    DWORD Flags;
    HMIDIOUT hMidi;
} MINST;

static void MNotify(MINST *m, UINT msg, DWORD_PTR p1)
{
    DriverCallback(m->Callback, HIWORD(m->Flags), (HDRVR)m->hMidi, msg, m->Instance, p1, 0);
}

static DWORD MidiSend(MINST *m, const BYTE *b, DWORD n)
{
    return Ioctl(m->Dev, FALSE, IOCTL_MIDI_PLAY, (void *)b, n, 0, 0, 0) ? MMSYSERR_NOERROR : MMSYSERR_ERROR;
}

DWORD APIENTRY modMessage(UINT dev, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2)
{
    MINST *m = (MINST *)user;
    TR(("mod dev %u msg %u", dev, msg));
    COMMON_MSGS(msg);
    if (dev >= 16) dev = 0;          /* loaded as the MIDI mapper: route to the synthesizer */
    switch (msg) {
    case MODM_GETNUMDEVS: return DevCount(L"AudntMidiOut");
    case MODM_GETDEVCAPS: return DevCaps(L"AudntMidiOut", dev, IOCTL_MIDI_GET_CAPABILITIES, (void *)p1, p2);
    case MODM_OPEN: {
        LPMIDIOPENDESC d = (LPMIDIOPENDESC)p1;
        HANDLE h = DevOpen(L"AudntMidiOut", dev, FALSE);
        if (h == INVALID_HANDLE_VALUE) return MMSYSERR_BADDEVICEID;
        m = xalloc(sizeof(MINST));
        if (!m) { CloseHandle(h); return MMSYSERR_NOMEM; }
        m->Dev = h; m->Callback = d->dwCallback; m->Instance = d->dwInstance; m->Flags = p2;
        m->hMidi = (HMIDIOUT)d->hMidi;
        *(DWORD_PTR *)user = (DWORD_PTR)m;
        MNotify(m, MOM_OPEN, 0);
        return MMSYSERR_NOERROR;
    }
    case MODM_CLOSE:
        CloseHandle(m->Dev);
        MNotify(m, MOM_CLOSE, 0);
        xfree(m);
        return MMSYSERR_NOERROR;
    case MODM_DATA: {
        BYTE b[3]; DWORD n;
        b[0] = (BYTE)p1; b[1] = (BYTE)(p1 >> 8); b[2] = (BYTE)(p1 >> 16);
        if (b[0] >= 0xF8) n = 1;
        else if (b[0] < 0x80) n = 2;                       /* running status */
        else if ((b[0] & 0xF0) == 0xC0 || (b[0] & 0xF0) == 0xD0 || b[0] == 0xF1 || b[0] == 0xF3) n = 2;
        else if (b[0] == 0xF6) n = 1;
        else n = 3;
        return MidiSend(m, b, n);
    }
    case MODM_LONGDATA: {
        LPMIDIHDR h = (LPMIDIHDR)p1;
        if (!h || !(h->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;
        MidiSend(m, (BYTE *)h->lpData, h->dwBufferLength);
        h->dwFlags |= MHDR_DONE;
        MNotify(m, MOM_DONE, (DWORD_PTR)h);
        return MMSYSERR_NOERROR;
    }
    case MODM_RESET: {
        DWORD st = DD_RESET;
        Ioctl(m->Dev, FALSE, IOCTL_MIDI_SET_STATE, &st, 4, 0, 0, 0);
        return MMSYSERR_NOERROR;
    }
    case MODM_GETVOLUME: return DevVolume(L"AudntMidiOut", dev, IOCTL_MIDI_GET_VOLUME, (DWORD *)p1);
    case MODM_SETVOLUME: { DWORD v = (DWORD)p1; return DevVolume(L"AudntMidiOut", dev, IOCTL_MIDI_SET_VOLUME, &v); }
    case MODM_CACHEPATCHES: case MODM_CACHEDRUMPATCHES: return MMSYSERR_NOERROR;
    default: return MMSYSERR_NOTSUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* aux                                                                 */
/* ------------------------------------------------------------------ */
DWORD APIENTRY auxMessage(UINT dev, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)user;
    COMMON_MSGS(msg);
    switch (msg) {
    case AUXDM_GETNUMDEVS: return DevCount(L"AudntAux");
    case AUXDM_GETDEVCAPS: return DevCaps(L"AudntAux", dev, IOCTL_AUX_GET_CAPABILITIES, (void *)p1, p2);
    case AUXDM_GETVOLUME:  return DevVolume(L"AudntAux", dev, IOCTL_AUX_GET_VOLUME, (DWORD *)p1);
    case AUXDM_SETVOLUME:  { DWORD v = (DWORD)p1; return DevVolume(L"AudntAux", dev, IOCTL_AUX_SET_VOLUME, &v); }
    default: return MMSYSERR_NOTSUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* mixer                                                               */
/* ------------------------------------------------------------------ */
/*
 * Line layout (ids):
 *   destination 0 "Speakers" (line id 0x0000)   controls: volume, mute
 *     sources: Wave, MIDI Synth, CD, Line In, Microphone, Aux, PC Speaker
 *   destination 1 "Recording" (line id 0x0001)  controls: mux, volume (record gain)
 *     sources: Microphone, Line In, CD, Aux, Stereo Mix (record sources)
 * source line id = 0x1000 * (dest + 1) + source index
 * control id     = line id * 4 + kind  (kind 0 volume, 1 mute, 2 mux)
 */
static const WORD g_OutSrcLine[] = { AT_LINE_WAVE, AT_LINE_SYNTH, AT_LINE_CD, AT_LINE_LINEIN, AT_LINE_MIC, AT_LINE_AUX, AT_LINE_PCSPK };
static const WORD g_InSrcLine[]  = { AT_LINE_MIC, AT_LINE_LINEIN, AT_LINE_CD, AT_LINE_AUX, AT_LINE_MASTER };
static const DWORD g_OutSrcType[] = { MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT, MIXERLINE_COMPONENTTYPE_SRC_SYNTHESIZER,
    MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC, MIXERLINE_COMPONENTTYPE_SRC_LINE, MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE,
    MIXERLINE_COMPONENTTYPE_SRC_AUXILIARY, MIXERLINE_COMPONENTTYPE_SRC_PCSPEAKER };
static const DWORD g_InSrcType[] = { MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE, MIXERLINE_COMPONENTTYPE_SRC_LINE,
    MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC, MIXERLINE_COMPONENTTYPE_SRC_AUXILIARY, MIXERLINE_COMPONENTTYPE_SRC_ANALOG };
static const WCHAR *g_LineName[AT_LINE_COUNT] = { L"Volume Control", L"Wave", L"MIDI Synth", L"CD Audio",
    L"Line In", L"Microphone", L"Aux", L"PC Speaker", L"Recording", L"-" };

typedef struct {
    UINT  Dev;
    DWORD_PTR Callback, Instance;
    DWORD Flags;
    HMIXER hMixer;
} XINST;
static XINST *g_Mixers[16];

static BOOL GetLines(UINT dev, AUDIONT_LINES *ls)
{
    HANDLE h = DevOpen(L"AudioNT", dev, FALSE);
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = Ioctl(h, FALSE, IOCTL_AUDIONT_GET_LINES, 0, 0, ls, sizeof(*ls), 0);
    CloseHandle(h);
    return ok;
}

/* card names and device numbers (zeroed names when the driver is older) */
static BOOL GetInfo(UINT dev, AUDIONT_INFO *in)
{
    HANDLE h = DevOpen(L"AudioNT", dev, FALSE);
    DWORD got = 0;
    BOOL ok;
    memset(in, 0, sizeof(*in));
    in->WaveOutDev = in->WaveInDev = dev;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = Ioctl(h, FALSE, IOCTL_AUDIONT_GET_INFO, 0, 0, in, sizeof(*in), &got);
    CloseHandle(h);
    if (got < sizeof(*in)) memset(in->Name, 0, sizeof(in->Name) + sizeof(in->DevName));
    return ok;
}

/* the mixer has a recording destination (wave in target) when the card can
   record, even without a record source selector (USB microphones) */
static UINT Dests(UINT dev, AUDIONT_LINES *ls)
{
    AUDIONT_INFO in;
    if (ls->RecSrcMask) return 2;
    GetInfo(dev, &in);
    return in.CanRecord ? 2 : 1;
}

static void AnsiToW(WCHAR *d, const char *s, UINT max)
{
    UINT i;
    for (i = 0; i + 1 < max && s[i]; i++) d[i] = (WCHAR)(UCHAR)s[i];
    d[i] = 0;
}

static BOOL CtlIoctl(UINT dev, DWORD code, void *in, DWORD inl)
{
    HANDLE h = DevOpen(L"AudioNT", dev, FALSE);
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = Ioctl(h, FALSE, code, in, inl, 0, 0, 0);
    CloseHandle(h);
    return ok;
}

/* list of present sources for a destination; returns count */
static UINT Sources(AUDIONT_LINES *ls, UINT dest, UINT *idx)
{
    UINT n = 0, i;
    if (dest == 0) {
        for (i = 0; i < sizeof(g_OutSrcLine) / sizeof(g_OutSrcLine[0]); i++)
            if (ls->Line[g_OutSrcLine[i]].Present) idx[n++] = i;
    } else {
        for (i = 0; i < sizeof(g_InSrcLine) / sizeof(g_InSrcLine[0]); i++)
            if (ls->RecSrcMask & (1UL << g_InSrcLine[i])) idx[n++] = i;
    }
    return n;
}

/* the card's own line names (all empty with an older driver) */
static void GetLineNames(UINT dev, AUDIONT_LINENAMES *ln)
{
    HANDLE h = DevOpen(L"AudioNT", dev, FALSE);
    memset(ln, 0, sizeof(*ln));
    if (h == INVALID_HANDLE_VALUE) return;
    if (!Ioctl(h, FALSE, IOCTL_AUDIONT_GET_LINENAMES, 0, 0, ln, sizeof(*ln), 0)) memset(ln, 0, sizeof(*ln));
    CloseHandle(h);
}

static void FillLine(UINT dev, AUDIONT_LINES *ls, LPMIXERLINEW ml, UINT dest, int srcPos)
{
    UINT idx[8], n = Sources(ls, dest, idx);
    AUDIONT_INFO in;
    GetInfo(dev, &in);
    ml->dwDestination = dest;
    ml->dwSource = srcPos < 0 ? 0 : (DWORD)srcPos;
    ml->fdwLine = MIXERLINE_LINEF_ACTIVE;
    ml->dwUser = 0;
    ml->cChannels = 2;
    ml->Target.dwType = dest == 0 ? MIXERLINE_TARGETTYPE_WAVEOUT : MIXERLINE_TARGETTYPE_WAVEIN;
    /* must equal the wave device capabilities so winmm can pair mixer line and device */
    ml->Target.dwDeviceID = dest == 0 ? in.WaveOutDev : in.WaveInDev;
    ml->Target.wMid = MM_MICROSOFT; ml->Target.wPid = dest == 0 ? 1 : 2; ml->Target.vDriverVersion = 0x0100;
    if (in.DevName[dest][0]) AnsiToW(ml->Target.szPname, in.DevName[dest], MAXPNAMELEN);
    else u_wcscpy(ml->Target.szPname, dest == 0 ? L"AUDIONT Wave Out" : L"AUDIONT Wave In");
    if (srcPos < 0) {
        ml->dwLineID = dest;
        ml->dwComponentType = dest == 0 ? MIXERLINE_COMPONENTTYPE_DST_SPEAKERS : MIXERLINE_COMPONENTTYPE_DST_WAVEIN;
        ml->cConnections = n;
        ml->cControls = (dest == 1 && !ls->RecSrcMask) ? (ls->Line[AT_LINE_RECORD].Present ? 1 : 0) : 2;
        u_wcscpy(ml->szShortName, dest == 0 ? L"Volume" : L"Recording");
        u_wcscpy(ml->szName, dest == 0 ? L"Volume Control" : L"Recording Control");
    } else {
        UINT s = idx[srcPos];
        WORD line = dest == 0 ? g_OutSrcLine[s] : g_InSrcLine[s];
        ml->dwLineID = 0x1000 * (dest + 1) + s;
        ml->fdwLine |= MIXERLINE_LINEF_SOURCE;
        ml->dwComponentType = dest == 0 ? g_OutSrcType[s] : g_InSrcType[s];
        ml->cConnections = 0;
        ml->cControls = dest == 0 ? 2 : 0;
        ml->Target.dwType = MIXERLINE_TARGETTYPE_UNDEFINED;
        if (dest == 0 && line == AT_LINE_WAVE) ml->Target.dwType = MIXERLINE_TARGETTYPE_WAVEOUT;
        if (dest == 0 && line == AT_LINE_SYNTH) ml->Target.dwType = MIXERLINE_TARGETTYPE_MIDIOUT;
        AUDIONT_LINENAMES ln;
        u_wcscpy(ml->szShortName, dest == 1 && line == AT_LINE_MASTER ? L"Stereo Mix" : g_LineName[line]);
        u_wcscpy(ml->szName, ml->szShortName);
        GetLineNames(dev, &ln);
        if (line < AT_LINE_COUNT && ln.Short[line][0]) {
            ln.Short[line][15] = 0; ln.Long[line][31] = 0;
            AnsiToW(ml->szShortName, ln.Short[line], MIXER_SHORT_NAME_CHARS);
            AnsiToW(ml->szName, ln.Long[line][0] ? ln.Long[line] : ln.Short[line], MIXER_LONG_NAME_CHARS);
        }
    }
}

/* resolve a line id into (dest, source index, AUDIONT line); FALSE if invalid */
static BOOL ResolveLine(AUDIONT_LINES *ls, DWORD id, UINT *dest, int *src, UINT *atline)
{
    UINT idx[8], n, i, d;
    if (id < 2) { *dest = id; *src = -1; *atline = id == 0 ? AT_LINE_MASTER : AT_LINE_RECORD; return TRUE; }
    d = (id >> 12) - 1;
    if (d > 1) return FALSE;
    n = Sources(ls, d, idx);
    for (i = 0; i < n; i++)
        if (idx[i] == (id & 0xFFF)) {
            *dest = d; *src = i;
            *atline = d == 0 ? g_OutSrcLine[idx[i]] : g_InSrcLine[idx[i]];
            return TRUE;
        }
    return FALSE;
}

static void FillControl(DWORD lineId, UINT dest, int src, UINT kind, LPMIXERCONTROLW mc, AUDIONT_LINES *ls)
{
    UINT idx[8], n = Sources(ls, 1, idx), i;
    memset(mc, 0, sizeof(*mc));
    mc->cbStruct = sizeof(*mc);
    mc->dwControlID = lineId * 4 + kind;
    if (kind == 0) {
        mc->dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
        mc->Bounds.dwMinimum = 0; mc->Bounds.dwMaximum = 0xFFFF;
        mc->Metrics.cSteps = 0xFFFF;
        u_wcscpy(mc->szShortName, L"Volume");
        u_wcscpy(mc->szName, src < 0 && dest == 1 ? L"Recording Level" : L"Volume");
    } else if (kind == 1) {
        mc->dwControlType = MIXERCONTROL_CONTROLTYPE_MUTE;
        mc->fdwControl = MIXERCONTROL_CONTROLF_UNIFORM;
        mc->Bounds.dwMaximum = 1;
        u_wcscpy(mc->szShortName, L"Mute");
        u_wcscpy(mc->szName, L"Mute");
    } else {
        mc->dwControlType = MIXERCONTROL_CONTROLTYPE_MUX;
        mc->fdwControl = MIXERCONTROL_CONTROLF_UNIFORM | MIXERCONTROL_CONTROLF_MULTIPLE;
        mc->cMultipleItems = n;
        mc->Bounds.dwMaximum = n ? n - 1 : 0;
        mc->Metrics.cSteps = n;
        u_wcscpy(mc->szShortName, L"Input");
        u_wcscpy(mc->szName, L"Recording Source");
    }
    (void)i;
}

static UINT CtlKinds(AUDIONT_LINES *ls, UINT dest, int src, UINT *kinds)
{
    if (src < 0 && dest == 0) { kinds[0] = 0; kinds[1] = 1; return 2; }
    if (src < 0 && dest == 1 && !ls->RecSrcMask) { kinds[0] = 0; return ls->Line[AT_LINE_RECORD].Present ? 1 : 0; }
    if (src < 0 && dest == 1) { kinds[0] = 2; kinds[1] = 0; return 2; }
    if (dest == 0) { kinds[0] = 0; kinds[1] = 1; return 2; }
    return 0;
}

static void MixerNotify(UINT dev, DWORD ctlId)
{
    UINT i;
    for (i = 0; i < 16; i++) {
        XINST *x = g_Mixers[i];
        if (x && x->Dev == dev)
            DriverCallback(x->Callback, HIWORD(x->Flags), (HDRVR)x->hMixer, MM_MIXM_CONTROL_CHANGE,
                           x->Instance, ctlId, 0);
    }
}

DWORD APIENTRY mxdMessage(UINT dev, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2)
{
    AUDIONT_LINES ls;
    COMMON_MSGS(msg);
    switch (msg) {
    case MXDM_GETNUMDEVS: return DevCount(L"AudioNT");
    case MXDM_GETDEVCAPS: {
        LPMIXERCAPSW c = (LPMIXERCAPSW)p1;
        MIXERCAPSW mc;
        AUDIONT_INFO in;
        if (!GetLines(dev, &ls)) return MMSYSERR_BADDEVICEID;
        GetInfo(dev, &in);
        memset(&mc, 0, sizeof(mc));
        mc.wMid = MM_MICROSOFT; mc.wPid = 5; mc.vDriverVersion = 0x0100;
        if (in.DevName[4][0]) AnsiToW(mc.szPname, in.DevName[4], MAXPNAMELEN);
        else if (dev) wsprintfW(mc.szPname, L"AUDIONT Mixer %u", dev + 1);
        else u_wcscpy(mc.szPname, L"AUDIONT Mixer");
        mc.cDestinations = Dests(dev, &ls);
        memcpy(c, &mc, p2 < sizeof(mc) ? p2 : sizeof(mc));
        return MMSYSERR_NOERROR;
    }
    case MXDM_OPEN: {
        LPMIXEROPENDESC d = (LPMIXEROPENDESC)p1;
        XINST *x;
        UINT i;
        if (!GetLines(dev, &ls)) return MMSYSERR_BADDEVICEID;
        x = xalloc(sizeof(XINST));
        if (!x) return MMSYSERR_NOMEM;
        x->Dev = dev; x->Callback = d->dwCallback; x->Instance = d->dwInstance; x->Flags = p2;
        x->hMixer = d->hmx;
        for (i = 0; i < 16; i++) if (!g_Mixers[i]) { g_Mixers[i] = x; break; }
        *(DWORD_PTR *)user = (DWORD_PTR)x;
        return MMSYSERR_NOERROR;
    }
    case MXDM_CLOSE: {
        XINST *x = (XINST *)user;
        UINT i;
        for (i = 0; i < 16; i++) if (g_Mixers[i] == x) g_Mixers[i] = 0;
        xfree(x);
        return MMSYSERR_NOERROR;
    }
    case MXDM_GETLINEINFO: {
        LPMIXERLINEW ml = (LPMIXERLINEW)p1;
        UINT dest, at, idx[8];
        int src;
        if (!GetLines(dev, &ls)) return MMSYSERR_BADDEVICEID;
        switch (p2 & MIXER_GETLINEINFOF_QUERYMASK) {
        case MIXER_GETLINEINFOF_DESTINATION:
            if (ml->dwDestination >= Dests(dev, &ls)) return MIXERR_INVALLINE;
            FillLine(dev, &ls, ml, ml->dwDestination, -1);
            return MMSYSERR_NOERROR;
        case MIXER_GETLINEINFOF_SOURCE:
            if (ml->dwDestination > 1) return MIXERR_INVALLINE;
            if (ml->dwSource >= Sources(&ls, ml->dwDestination, idx)) return MIXERR_INVALLINE;
            FillLine(dev, &ls, ml, ml->dwDestination, (int)ml->dwSource);
            return MMSYSERR_NOERROR;
        case MIXER_GETLINEINFOF_LINEID:
            if (!ResolveLine(&ls, ml->dwLineID, &dest, &src, &at)) return MIXERR_INVALLINE;
            FillLine(dev, &ls, ml, dest, src);
            return MMSYSERR_NOERROR;
        case MIXER_GETLINEINFOF_COMPONENTTYPE: {
            UINT d, i, n;
            for (d = 0; d < Dests(dev, &ls); d++) {
                MIXERLINEW t;
                FillLine(dev, &ls, &t, d, -1);
                if (t.dwComponentType == ml->dwComponentType) { FillLine(dev, &ls, ml, d, -1); return MMSYSERR_NOERROR; }
                n = Sources(&ls, d, idx);
                for (i = 0; i < n; i++) {
                    FillLine(dev, &ls, &t, d, (int)i);
                    if (t.dwComponentType == ml->dwComponentType) { FillLine(dev, &ls, ml, d, (int)i); return MMSYSERR_NOERROR; }
                }
            }
            return MIXERR_INVALLINE;
        }
        case MIXER_GETLINEINFOF_TARGETTYPE:
            if (ml->Target.dwType == MIXERLINE_TARGETTYPE_WAVEOUT) { FillLine(dev, &ls, ml, 0, -1); return MMSYSERR_NOERROR; }
            if (ml->Target.dwType == MIXERLINE_TARGETTYPE_WAVEIN && Dests(dev, &ls) > 1) { FillLine(dev, &ls, ml, 1, -1); return MMSYSERR_NOERROR; }
            return MIXERR_INVALLINE;
        }
        return MMSYSERR_NOTSUPPORTED;
    }
    case MXDM_GETLINECONTROLS: {
        LPMIXERLINECONTROLSW lc = (LPMIXERLINECONTROLSW)p1;
        UINT dest, at, kinds[4], nk, i;
        int src;
        if (!GetLines(dev, &ls)) return MMSYSERR_BADDEVICEID;
        switch (p2 & MIXER_GETLINECONTROLSF_QUERYMASK) {
        case MIXER_GETLINECONTROLSF_ALL:
            if (!ResolveLine(&ls, lc->dwLineID, &dest, &src, &at)) return MIXERR_INVALLINE;
            nk = CtlKinds(&ls, dest, src, kinds);
            if (lc->cControls < nk) return MMSYSERR_INVALPARAM;
            for (i = 0; i < nk; i++) FillControl(lc->dwLineID, dest, src, kinds[i], &lc->pamxctrl[i], &ls);
            lc->cControls = nk;
            return MMSYSERR_NOERROR;
        case MIXER_GETLINECONTROLSF_ONEBYID: {
            DWORD line = lc->dwControlID / 4, kind = lc->dwControlID % 4;
            if (!ResolveLine(&ls, line, &dest, &src, &at)) return MIXERR_INVALCONTROL;
            nk = CtlKinds(&ls, dest, src, kinds);
            for (i = 0; i < nk; i++) if (kinds[i] == kind) break;
            if (i == nk) return MIXERR_INVALCONTROL;
            lc->dwLineID = line;
            FillControl(line, dest, src, kind, lc->pamxctrl, &ls);
            return MMSYSERR_NOERROR;
        }
        case MIXER_GETLINECONTROLSF_ONEBYTYPE: {
            DWORD want = lc->dwControlType;
            if (!ResolveLine(&ls, lc->dwLineID, &dest, &src, &at)) return MIXERR_INVALLINE;
            nk = CtlKinds(&ls, dest, src, kinds);
            for (i = 0; i < nk; i++) {
                FillControl(lc->dwLineID, dest, src, kinds[i], lc->pamxctrl, &ls);
                if (lc->pamxctrl->dwControlType == want) return MMSYSERR_NOERROR;
            }
            return MIXERR_INVALCONTROL;
        }
        }
        return MMSYSERR_NOTSUPPORTED;
    }
    case MXDM_GETCONTROLDETAILS:
    case MXDM_SETCONTROLDETAILS: {
        LPMIXERCONTROLDETAILS cd = (LPMIXERCONTROLDETAILS)p1;
        DWORD line = cd->dwControlID / 4, kind = cd->dwControlID % 4;
        UINT dest, at, idx[8], n, i;
        int src;
        AUDIONT_SET_LINE sl;
        if (!GetLines(dev, &ls)) return MMSYSERR_BADDEVICEID;
        if (!ResolveLine(&ls, line, &dest, &src, &at)) return MIXERR_INVALCONTROL;
        if (msg == MXDM_GETCONTROLDETAILS && (p2 & MIXER_GETCONTROLDETAILSF_QUERYMASK) == MIXER_GETCONTROLDETAILSF_LISTTEXT) {
            LPMIXERCONTROLDETAILS_LISTTEXTW lt = (LPMIXERCONTROLDETAILS_LISTTEXTW)cd->paDetails;
            MIXERLINEW t;
            if (kind != 2) return MMSYSERR_NOTSUPPORTED;
            n = Sources(&ls, 1, idx);
            for (i = 0; i < n && i < cd->cMultipleItems; i++) {
                FillLine(dev, &ls, &t, 1, (int)i);
                lt[i].dwParam1 = t.dwLineID; lt[i].dwParam2 = t.dwComponentType;
                u_wcscpy(lt[i].szName, t.szName);
            }
            return MMSYSERR_NOERROR;
        }
        sl.Line = at; sl.Left = ls.Line[at].Left; sl.Right = ls.Line[at].Right; sl.Mute = ls.Line[at].Mute;
        if (kind == 0) {
            LPMIXERCONTROLDETAILS_UNSIGNED u = (LPMIXERCONTROLDETAILS_UNSIGNED)cd->paDetails;
            if (msg == MXDM_GETCONTROLDETAILS) {
                u[0].dwValue = sl.Left;
                if (cd->cChannels > 1) u[1].dwValue = sl.Right;
                return MMSYSERR_NOERROR;
            }
            sl.Left = (WORD)u[0].dwValue;
            sl.Right = (WORD)(cd->cChannels > 1 ? u[1].dwValue : u[0].dwValue);
        } else if (kind == 1) {
            LPMIXERCONTROLDETAILS_BOOLEAN b = (LPMIXERCONTROLDETAILS_BOOLEAN)cd->paDetails;
            if (msg == MXDM_GETCONTROLDETAILS) { b[0].fValue = sl.Mute; return MMSYSERR_NOERROR; }
            sl.Mute = b[0].fValue ? 1 : 0;
        } else {
            LPMIXERCONTROLDETAILS_BOOLEAN b = (LPMIXERCONTROLDETAILS_BOOLEAN)cd->paDetails;
            n = Sources(&ls, 1, idx);
            if (msg == MXDM_GETCONTROLDETAILS) {
                for (i = 0; i < n && i < cd->cMultipleItems; i++)
                    b[i].fValue = (ls.RecSrc & (1UL << g_InSrcLine[idx[i]])) ? 1 : 0;
                return MMSYSERR_NOERROR;
            }
            for (i = 0; i < n && i < cd->cMultipleItems; i++) {
                DWORD mask = 1UL << g_InSrcLine[idx[i]];
                if (b[i].fValue && !(ls.RecSrc & mask)) {
                    if (!CtlIoctl(dev, IOCTL_AUDIONT_SET_RECSRC, &mask, 4)) return MMSYSERR_ERROR;
                    MixerNotify(dev, cd->dwControlID);
                    break;
                }
            }
            return MMSYSERR_NOERROR;
        }
        if (!CtlIoctl(dev, IOCTL_AUDIONT_SET_LINE, &sl, sizeof(sl))) return MMSYSERR_ERROR;
        MixerNotify(dev, cd->dwControlID);
        return MMSYSERR_NOERROR;
    }
    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
LRESULT APIENTRY DriverProc(DWORD_PTR id, HDRVR hdrv, UINT msg, LPARAM p1, LPARAM p2)
{
    TR(("DriverProc id %x msg %x", id, msg));
    switch (msg) {
    case DRV_LOAD: case DRV_FREE: case DRV_ENABLE: case DRV_DISABLE: case DRV_OPEN: case DRV_CLOSE:
        return 1;
    case DRV_QUERYCONFIGURE:
        return 1;
    case DRV_CONFIGURE: {
        WCHAR path[MAX_PATH + 16];
        STARTUPINFOW si; PROCESS_INFORMATION pi;
        GetSystemDirectoryW(path, MAX_PATH);
        u_wcscat(path, L"\\audmix.exe");
        memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
        if (CreateProcessW(path, 0, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        return DRVCNF_OK;
    }
    case DRV_INSTALL: case DRV_REMOVE:
        return DRVCNF_RESTART;
    default:
        return DefDriverProc(id, hdrv, msg, p1, p2);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; g_Heap = GetProcessHeap(); }
    return TRUE;
}
