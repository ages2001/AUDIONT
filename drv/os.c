/*
 * AUDIONT - OS abstraction layer
 *
 * Runtime NT version detection, dynamic import resolution, logging,
 * memory, timing, registry and PCI helpers.
 */
#include "os.h"

ULONG g_NtVersion = NTVER_31;
ULONG g_NtBuild   = 0;
#ifdef AUDIONT_DEBUG
ULONG g_DebugLevel = 2;
ULONG g_DebugPorts = 1;
#else
ULONG g_DebugLevel = 1;
ULONG g_DebugPorts = 0;
#endif

#ifndef AUDIONT_WDM
/* import thunks, used to find the ntoskrnl/hal image bases */
extern PVOID ImpIoCreateDevice __asm__("__imp__IoCreateDevice@28");
extern PVOID ImpKeStall        __asm__("__imp__KeStallExecutionProcessor@4");
#endif

static PUCHAR g_NtosBase, g_HalBase;
static WCHAR  g_ParamPath[260];
static ULONG  g_PerfFreq;

typedef VOID (FASTCALL *PFN_IOFCOMPLETE)(PIRP, CCHAR);
typedef VOID (NTAPI *PFN_IOCOMPLETE)(PIRP, CCHAR);
static PFN_IOFCOMPLETE pIofCompleteRequest;
static PFN_IOCOMPLETE  pIoCompleteRequest;

/* ------------------------------------------------------------------ */
/* memory primitives (NT 3.1 ntoskrnl exports no memcpy/memset)        */
/* ------------------------------------------------------------------ */
#ifdef __x86_64__
void *memcpy(void *d, const void *s, OS_SIZE n)
{
    void *r = d;
    __asm__ __volatile__("cld; rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}
#else
void *memcpy(void *d, const void *s, OS_SIZE n)
{
    void *r = d;
    __asm__ __volatile__("cld; rep movsl; movl %3,%%ecx; rep movsb"
        : "+D"(d), "+S"(s), "=c"(n) : "r"(n & 3), "2"(n >> 2) : "memory");
    return r;
}
#endif

void *memset(void *d, int c, OS_SIZE n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memmove(void *d, const void *s, OS_SIZE n)
{
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n; sp += n;
    while (n--) *--dp = *--sp;
    return d;
}

int memcmp(const void *a, const void *b, OS_SIZE n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

ULONG OsWcslen(const WCHAR *s) { ULONG n = 0; while (s[n]) n++; return n; }
void  OsWcsCopyA(WCHAR *d, const char *s, ULONG max)
{
    ULONG i = 0;
    if (!max) return;
    while (s[i] && i + 1 < max) { d[i] = (UCHAR)s[i]; i++; }
    d[i] = 0;
}

PVOID OsAlloc(ULONG size)
{
    PVOID p = ExAllocatePool(NonPagedPool, size);
    if (p) memset(p, 0, size);
    return p;
}
void OsFree(PVOID p) { if (p) ExFreePool(p); }

/* i486+ (EFLAGS.AC can be toggled): WBINVD exists */
static BOOLEAN Cpu486(void)
{
#ifdef AUDIONT_WDM
    return TRUE;                        /* Windows 2000 needs a Pentium */
#else
    ULONG a, b;
    __asm__ __volatile__(
        "pushfl\n\tpopl %0\n\tmovl %0, %1\n\txorl $0x40000, %0\n\t"
        "pushl %0\n\tpopfl\n\tpushfl\n\tpopl %0\n\tpushl %1\n\tpopfl"
        : "=&r"(a), "=&r"(b) :: "cc");
    return ((a ^ b) & 0x40000) != 0;
#endif
}

BOOLEAN OsAllocDma(DMA_BUFFER *b, ULONG size, BOOLEAN uncached)
{
    PHYSICAL_ADDRESS hi;
    ULONG fl;
    size = (size + 4095) & ~4095UL;
    hi.QuadPart = 0x7FFFFFFF;          /* stay well below 4 GB for 32-bit DMA engines */
    b->Uncached = FALSE;
    b->Alloc = b->Va = MmAllocateContiguousMemory(size, hi);
    if (!b->Va) { b->Pa = 0; b->Size = 0; return FALSE; }
    b->Pa = MmGetPhysicalAddress(b->Va).u.LowPart;
    b->Size = size;
    fl = IntSave();                    /* stay on this CPU while its cache is flushed */
    memset(b->Va, 0, size);
    if (uncached && Cpu486()) {
        /* write back and drop every cached line of the buffer before the uncached alias
           is used (same scheme as MmAllocateContiguousMemorySpecifyCache in later NT) */
        __asm__ __volatile__("wbinvd" ::: "memory");
    }
    IntRestore(fl);
    if (uncached) {
        PVOID uc = OsMapMmio(b->Pa, size);
        if (uc) { b->Va = uc; b->Uncached = TRUE; }
        else LOGE("dma: uncached mapping of %08x failed, using cached memory", b->Pa);
    }
    return TRUE;
}

void OsFreeDma(DMA_BUFFER *b)
{
    if (b->Uncached && b->Va) OsUnmapMmio(b->Va, b->Size);
    if (b->Alloc) MmFreeContiguousMemory(b->Alloc);
    b->Va = b->Alloc = NULL; b->Pa = 0; b->Size = 0; b->Uncached = FALSE;
}

PVOID OsMapMmio(ULONG pa, ULONG size)
{
    PHYSICAL_ADDRESS p; p.QuadPart = pa;
    return MmMapIoSpace(p, size, MmNonCached);
}
void OsUnmapMmio(PVOID va, ULONG size) { if (va) MmUnmapIoSpace(va, size); }

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
#define LOGRING_SIZE 32768
static char   g_LogRing[LOGRING_SIZE];
static ULONG  g_LogHead;      /* total bytes ever written */
static KSPIN_LOCK g_LogLock;
static BOOLEAN g_ComInit;

static void ComInit(void)
{
    outb(0x3F9, 0x00); outb(0x3FB, 0x80); outb(0x3F8, 0x01); outb(0x3F9, 0x00);
    outb(0x3FB, 0x03); outb(0x3FA, 0xC7); outb(0x3FC, 0x0B);
    g_ComInit = TRUE;
}

static void LogEmit(const char *s, int n)
{
    int i;
    ULONG fl = IntSave();
    for (i = 0; i < n; i++) {
        g_LogRing[g_LogHead % LOGRING_SIZE] = s[i];
        g_LogHead++;
        if (g_DebugPorts & 1) outb(0xE9, (UCHAR)s[i]);
        if (g_DebugPorts & 2) {
            ULONG t = 100000;
            if (!g_ComInit) ComInit();
            while (!(inb(0x3FD) & 0x20) && --t) ;
            outb(0x3F8, (UCHAR)s[i]);
        }
    }
    IntRestore(fl);
}

ULONG OsLogSnapshot(char *dst, ULONG size)
{
    ULONG fl = IntSave(), n, start, i;
    n = g_LogHead < LOGRING_SIZE ? g_LogHead : LOGRING_SIZE;
    if (n > size) n = size;
    start = g_LogHead - n;
    for (i = 0; i < n; i++) dst[i] = g_LogRing[(start + i) % LOGRING_SIZE];
    IntRestore(fl);
    return n;
}

static int FmtNum(char *buf, int pos, int size, ULONG v, int base, int width, int zero,
                  int neg, int upper, int left)
{
    char tmp[16]; int n = 0, len;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    if (neg) tmp[n++] = '-';
    len = n;
    if (!left) while (width > len && pos < size - 1) { buf[pos++] = zero ? '0' : ' '; width--; }
    if (neg && zero && !left) { /* keep simple: sign after padding */ }
    while (n && pos < size - 1) buf[pos++] = tmp[--n];
    if (left) while (width > len && pos < size - 1) { buf[pos++] = ' '; width--; }
    return pos;
}

int OsFormat(char *buf, int size, const char *fmt, __builtin_va_list ap)
{
    int pos = 0;
    if (size <= 0) return 0;
    while (*fmt && pos < size - 1) {
        int width = 0, zero = 0, left = 0, prec = -1;
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            LONG v = __builtin_va_arg(ap, LONG);
            pos = FmtNum(buf, pos, size, v < 0 ? (ULONG)-v : (ULONG)v, 10, width, zero, v < 0, 0, left);
            break; }
        case 'u': pos = FmtNum(buf, pos, size, __builtin_va_arg(ap, ULONG), 10, width, zero, 0, 0, left); break;
        case 'x': pos = FmtNum(buf, pos, size, __builtin_va_arg(ap, ULONG), 16, width, zero, 0, 0, left); break;
        case 'X': pos = FmtNum(buf, pos, size, __builtin_va_arg(ap, ULONG), 16, width, zero, 0, 1, left); break;
        case 'p': {
            ULONG_PTR v = (ULONG_PTR)__builtin_va_arg(ap, PVOID);
#ifdef __x86_64__
            pos = FmtNum(buf, pos, size, (ULONG)(v >> 32), 16, 8, 1, 0, 0, 0);
#endif
            pos = FmtNum(buf, pos, size, (ULONG)v, 16, 8, 1, 0, 0, 0);
            break; }
        case 'c': buf[pos++] = (char)__builtin_va_arg(ap, int); break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            int len = 0;
            if (!s) s = "(null)";
            while ((prec < 0 || len < prec) && s[len]) len++;
            if (!left) while (width > len && pos < size - 1) { buf[pos++] = ' '; width--; }
            { int k; for (k = 0; k < len && pos < size - 1; k++) buf[pos++] = (s[k] >= 32 && s[k] < 127) ? s[k] : '.'; }
            if (left) while (width > len && pos < size - 1) { buf[pos++] = ' '; width--; }
            break; }
        case 'S': {
            const WCHAR *s = __builtin_va_arg(ap, const WCHAR *);
            if (!s) s = L"(null)";
            while (*s && pos < size - 1) { buf[pos++] = (*s < 128) ? (char)*s : '?'; s++; }
            break; }
        case '%': buf[pos++] = '%'; break;
        case 0: fmt--; break;
        default: buf[pos++] = '?'; break;
        }
        fmt++;
    }
    buf[pos] = 0;
    return pos;
}

int OsSprintf(char *buf, int size, const char *fmt, ...)
{
    __builtin_va_list ap; int n;
    __builtin_va_start(ap, fmt);
    n = OsFormat(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

void OsLog(ULONG level, const char *fmt, ...)
{
    char buf[256]; int n;
    __builtin_va_list ap;
    if (level > g_DebugLevel) return;
    n = OsSprintf(buf, 16, "[AUDIONT] ");
    __builtin_va_start(ap, fmt);
    n += OsFormat(buf + n, sizeof(buf) - n - 2, fmt, ap);
    __builtin_va_end(ap);
    if (n > 0 && buf[n - 1] != '\n') { buf[n++] = '\n'; buf[n] = 0; }
    LogEmit(buf, n);
    if (g_DebugPorts & 4) DbgPrint("%s", buf);
}

/* ------------------------------------------------------------------ */
/* dynamic import resolution                                           */
/* ------------------------------------------------------------------ */
#ifdef AUDIONT_WDM
PVOID OsResolve(int hal, const char *name)
{
    WCHAR w[64]; UNICODE_STRING us;
    (void)hal;
    OsWcsCopyA(w, name, 64);
    RtlInitUnicodeString(&us, w);
    return MmGetSystemRoutineAddress(&us);
}
#else
static int IsValidAddr(PVOID p);

static PUCHAR FindImageBase(PVOID inside)
{
    ULONG a = (ULONG)inside & ~0xFFFUL, i;
    for (i = 0; i < 4096; i++, a -= 0x1000) {   /* up to 16 MB back */
        PUCHAR p = (PUCHAR)a;
        if (!IsValidAddr(p)) continue;
        if (p[0] == 'M' && p[1] == 'Z') {
            ULONG lfanew = *(PULONG)(p + 0x3C);
            if (lfanew < 0x1000 && IsValidAddr(p + lfanew) &&
                *(PULONG)(p + lfanew) == 0x00004550)
                return p;
        }
    }
    return NULL;
}

static PVOID FindExport(PUCHAR base, const char *name)
{
    PUCHAR nt; ULONG dirRva, dirSize, nNames, i;
    PULONG funcs, names; PUSHORT ords;
    if (!base) return NULL;
    nt = base + *(PULONG)(base + 0x3C);
    dirRva  = *(PULONG)(nt + 0x18 + 96);
    dirSize = *(PULONG)(nt + 0x18 + 100);
    if (!dirRva) return NULL;
    nNames = *(PULONG)(base + dirRva + 0x18);
    funcs  = (PULONG)(base + *(PULONG)(base + dirRva + 0x1C));
    names  = (PULONG)(base + *(PULONG)(base + dirRva + 0x20));
    ords   = (PUSHORT)(base + *(PULONG)(base + dirRva + 0x24));
    for (i = 0; i < nNames; i++) {
        const char *n = (const char *)(base + names[i]);
        const char *q = name;
        while (*n && *n == *q) { n++; q++; }
        if (*n == 0 && *q == 0) {
            ULONG rva = funcs[ords[i]];
            if (rva >= dirRva && rva < dirRva + dirSize) return NULL; /* forwarder */
            return base + rva;
        }
    }
    return NULL;
}

PVOID OsResolve(int hal, const char *name)
{
    return FindExport(hal ? g_HalBase : g_NtosBase, name);
}

typedef BOOLEAN (NTAPI *PFN_MMISVALID)(PVOID);
static PFN_MMISVALID pMmIsAddressValid;
static int IsValidAddr(PVOID p)
{
    if (!pMmIsAddressValid) return 1;
    return pMmIsAddressValid(p);
}
#endif

/* ------------------------------------------------------------------ */
/* locks, IRPs, time                                                   */
/* ------------------------------------------------------------------ */
void OsLockAcquire(OSLOCK *l) { KIRQL o; KeAcquireSpinLock(&l->Lock, &o); l->OldIrql = o; }
void OsLockRelease(OSLOCK *l) { KeReleaseSpinLock(&l->Lock, l->OldIrql); }

void OsCompleteIrp(PIRP irp, NTSTATUS st, ULONG info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
#ifdef AUDIONT_WDM
    IoCompleteRequest(irp, IO_SOUND_INCREMENT);
#else
    if (pIofCompleteRequest) pIofCompleteRequest(irp, IO_SOUND_INCREMENT);
    else pIoCompleteRequest(irp, IO_SOUND_INCREMENT);
#endif
}

PVOID OsMdlAddress(PMDL mdl)
{
    if (!mdl) return NULL;
    if (mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL))
        return mdl->MappedSystemVa;
#ifdef AUDIONT_WDM
    return MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
#else
    return MmMapLockedPages(mdl, 0);
#endif
}

void OsStallUs(ULONG us)
{
    while (us > 50000) { KeStallExecutionProcessor(50000); us -= 50000; }
    if (us) KeStallExecutionProcessor(us);
}

void OsSleepMs(ULONG ms)
{
    if (KeGetCurrentIrql() < DISPATCH_LEVEL) {
        LARGE_INTEGER t; t.QuadPart = -(LONGLONG)ms * 10000;
        KeDelayExecutionThread(0, FALSE, &t);
    } else OsStallUs(ms * 1000);
}

void OsEventInit(OSEVENT *e) { KeInitializeEvent(&e->Ev, SynchronizationEvent, FALSE); }
void OsEventSet(OSEVENT *e) { KeSetEvent(&e->Ev, 0, FALSE); }
BOOLEAN OsEventWait(OSEVENT *e, ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    return KeWaitForSingleObject(&e->Ev, 0 /* Executive */, 0, FALSE, &t) == 0;
}

typedef struct { void (*Fn)(void *); void *Ctx; } OS_THREAD;
static VOID NTAPI ThreadStart(PVOID p)
{
    OS_THREAD t = *(OS_THREAD *)p;
    OsFree(p);
    t.Fn(t.Ctx);
    PsTerminateSystemThread(0);
}

BOOLEAN OsThreadStart(void (*fn)(void *), void *ctx)
{
    HANDLE h;
    OS_THREAD *t = (OS_THREAD *)OsAlloc(sizeof(OS_THREAD));
    if (!t) return FALSE;
    t->Fn = fn; t->Ctx = ctx;
    if (PsCreateSystemThread(&h, 0x1F03FF /* THREAD_ALL_ACCESS */, NULL, NULL, NULL, ThreadStart, t) != 0) {
        OsFree(t);
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

void OsThreadExit(void) { PsTerminateSystemThread(0); }

ULONG OsTimeMs(void)
{
    LARGE_INTEGER c = KeQueryPerformanceCounter(NULL);
    if (!g_PerfFreq) {
        LARGE_INTEGER f; KeQueryPerformanceCounter(&f);
        g_PerfFreq = f.u.LowPart ? f.u.LowPart : 1193182;
    }
    return (ULONG)(((ULONGLONG)c.QuadPart * 1000ULL) / g_PerfFreq);
}

/* ------------------------------------------------------------------ */
/* registry                                                            */
/* ------------------------------------------------------------------ */
static HANDLE OpenParams(BOOLEAN create)
{
    OBJECT_ATTRIBUTES oa; UNICODE_STRING us; HANDLE h = NULL; ULONG disp;
    NTSTATUS st;
    if (!g_ParamPath[0]) return NULL;
    RtlInitUnicodeString(&us, g_ParamPath);
    oa.Length = sizeof(oa); oa.RootDirectory = NULL; oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    if (create) st = ZwCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp);
    else        st = ZwOpenKey(&h, KEY_READ, &oa);
    return NT_SUCCESS(st) ? h : NULL;
}

static ULONG RegQuery(const WCHAR *name, ULONG type, PVOID data, ULONG len)
{
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 512];
    PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    UNICODE_STRING us; ULONG rl = 0, n = 0; HANDLE h;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return 0;
    h = OpenParams(FALSE);
    if (!h) return 0;
    RtlInitUnicodeString(&us, name);
    if (NT_SUCCESS(ZwQueryValueKey(h, &us, KeyValuePartialInformation, kv, sizeof(buf), &rl)) &&
        kv->Type == type) {
        n = MIN(kv->DataLength, len);
        memcpy(data, kv->Data, n);
    }
    ZwClose(h);
    return n;
}

/* a whole file below <SystemRoot>\System32\drivers\audiont\ into nonpaged memory
   (PASSIVE_LEVEL; OsFree releases it) */
UCHAR *OsLoadFile(const char *rel, ULONG *len)
{
    WCHAR path[160];
    static const char pre[] = "\\SystemRoot\\System32\\drivers\\audiont\\";
    OBJECT_ATTRIBUTES oa; UNICODE_STRING us; IO_STATUS_BLOCK io; HANDLE h = NULL;
    FILE_STANDARD_INFORMATION fi;
    LARGE_INTEGER off;
    ULONG i, n = 0, size;
    UCHAR *buf;
    NTSTATUS st;
    for (i = 0; pre[i] && n < 150; i++) path[n++] = (WCHAR)pre[i];
    for (i = 0; rel[i] && n < 158; i++) path[n++] = (WCHAR)rel[i];
    path[n] = 0;
    RtlInitUnicodeString(&us, path);
    oa.Length = sizeof(oa); oa.RootDirectory = NULL; oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor = NULL; oa.SecurityQualityOfService = NULL;
    st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                      FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) { LOGV("file %s: %08x", rel, st); return NULL; }
    memset(&fi, 0, sizeof(fi));
    st = ZwQueryInformationFile(h, &io, &fi, sizeof(fi), FileStandardInformation);
    if (!NT_SUCCESS(st) || fi.EndOfFile.QuadPart <= 0 || fi.EndOfFile.QuadPart > 16 * 1024 * 1024) { ZwClose(h); return NULL; }
    size = (ULONG)fi.EndOfFile.QuadPart;
    buf = (UCHAR *)OsAlloc(size);
    if (!buf) { ZwClose(h); return NULL; }
    off.QuadPart = 0;
    st = ZwReadFile(h, NULL, NULL, NULL, &io, buf, size, &off, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st) || io.Information != size) { OsFree(buf); return NULL; }
    *len = size;
    return buf;
}

ULONG OsRegGetDword(const WCHAR *name, ULONG def)
{
    ULONG v;
    return RegQuery(name, REG_DWORD, &v, 4) == 4 ? v : def;
}

ULONG OsRegGetBinary(const WCHAR *name, PVOID data, ULONG len)
{
    return RegQuery(name, REG_BINARY, data, len);
}

static BOOLEAN RegSet(const WCHAR *name, ULONG type, PVOID data, ULONG len)
{
    UNICODE_STRING us; HANDLE h; NTSTATUS st;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return FALSE;
    h = OpenParams(TRUE);
    if (!h) return FALSE;
    RtlInitUnicodeString(&us, name);
    st = ZwSetValueKey(h, &us, 0, type, data, len);
    ZwClose(h);
    return NT_SUCCESS(st);
}
BOOLEAN OsRegSetDword(const WCHAR *name, ULONG val) { return RegSet(name, REG_DWORD, &val, 4); }
BOOLEAN OsRegSetBinary(const WCHAR *name, PVOID d, ULONG l) { return RegSet(name, REG_BINARY, d, l); }

/* ------------------------------------------------------------------ */
/* PCI configuration mechanism #1                                      */
/* ------------------------------------------------------------------ */
static ULONG PciAddr(ULONG b, ULONG d, ULONG f, ULONG o)
{
    return 0x80000000UL | (b << 16) | (d << 11) | (f << 8) | (o & 0xFC);
}
ULONG PciRead32(ULONG b, ULONG d, ULONG f, ULONG o)
{
    ULONG fl = IntSave(), v;
    outl(0xCF8, PciAddr(b, d, f, o)); v = inl(0xCFC);
    IntRestore(fl); return v;
}
USHORT PciRead16(ULONG b, ULONG d, ULONG f, ULONG o)
{
    ULONG fl = IntSave(); USHORT v;
    outl(0xCF8, PciAddr(b, d, f, o)); v = inw(0xCFC + (o & 2));
    IntRestore(fl); return v;
}
UCHAR PciRead8(ULONG b, ULONG d, ULONG f, ULONG o)
{
    ULONG fl = IntSave(); UCHAR v;
    outl(0xCF8, PciAddr(b, d, f, o)); v = inb(0xCFC + (o & 3));
    IntRestore(fl); return v;
}
void PciWrite32(ULONG b, ULONG d, ULONG f, ULONG o, ULONG v)
{
    ULONG fl = IntSave();
    outl(0xCF8, PciAddr(b, d, f, o)); outl(0xCFC, v);
    IntRestore(fl);
}
void PciWrite16(ULONG b, ULONG d, ULONG f, ULONG o, USHORT v)
{
    ULONG fl = IntSave();
    outl(0xCF8, PciAddr(b, d, f, o)); outw(0xCFC + (o & 2), v);
    IntRestore(fl);
}
void PciWrite8(ULONG b, ULONG d, ULONG f, ULONG o, UCHAR v)
{
    ULONG fl = IntSave();
    outl(0xCF8, PciAddr(b, d, f, o)); outb(0xCFC + (o & 3), v);
    IntRestore(fl);
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */
NTSTATUS OsInit(PDRIVER_OBJECT drv, PUNICODE_STRING regPath)
{
    ULONG n;
#ifdef AUDIONT_WDM
    ULONG maj = 5, min = 0;
    (void)drv;
    g_LogLock = 0;
    PsGetVersion(&maj, &min, &g_NtBuild, NULL);
    g_NtVersion = (maj << 8) | min;
#else
    PULONG pBuild;
    (void)drv;
    g_LogLock = 0;

    g_NtosBase = FindImageBase(ImpIoCreateDevice);
    pMmIsAddressValid = (PFN_MMISVALID)FindExport(g_NtosBase, "MmIsAddressValid");
    g_HalBase  = FindImageBase(ImpKeStall);

    pIofCompleteRequest = (PFN_IOFCOMPLETE)OsResolve(0, "IofCompleteRequest");
    pIoCompleteRequest  = (PFN_IOCOMPLETE)OsResolve(0, "IoCompleteRequest");

    pBuild = (PULONG)OsResolve(0, "NtBuildNumber");
    if (pBuild) {
        g_NtBuild = *pBuild & 0xFFFF;
        if (g_NtBuild >= 1381)      g_NtVersion = NTVER_40;
        else if (g_NtBuild >= 1057) g_NtVersion = NTVER_351;
        else if (g_NtBuild >= 807)  g_NtVersion = NTVER_35;
        else                        g_NtVersion = NTVER_31;
    } else {
        g_NtBuild = 511;
        g_NtVersion = NTVER_31;
    }
#endif

    /* "<service key>\Parameters" */
    n = regPath ? regPath->Length / 2 : 0;
    if (n > 200) n = 200;
    if (n) {
        memcpy(g_ParamPath, regPath->Buffer, n * 2);
        memcpy(g_ParamPath + n, L"\\Parameters", 24);
    }
    g_DebugLevel = OsRegGetDword(L"DebugLevel", g_DebugLevel);
    g_DebugPorts = OsRegGetDword(L"DebugPorts", g_DebugPorts);

#ifdef AUDIONT_WDM
    LOGI("AUDIONT " __DATE__ " " __TIME__ " for Windows 2000 / XP (%s), build %u ver %x",
         sizeof(void *) == 8 ? "x64" : "x86", g_NtBuild, g_NtVersion);
    (void)g_NtosBase; (void)g_HalBase; (void)pIofCompleteRequest; (void)pIoCompleteRequest;
    return STATUS_SUCCESS;
#endif
    LOGI("AUDIONT " __DATE__ " " __TIME__ " ntos=%p hal=%p build=%u ver=%x complete=%s",
         g_NtosBase, g_HalBase, g_NtBuild, g_NtVersion,
         pIofCompleteRequest ? "Iof" : "Io");
    if (!g_NtosBase || !g_HalBase || (!pIofCompleteRequest && !pIoCompleteRequest)) {
        LOGE("kernel image lookup failed");
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

ULONG PciFindCap(ULONG b, ULONG d, ULONG f, UCHAR id)
{
    ULONG p, guard;
    if (!(PciRead16(b, d, f, 0x06) & 0x10)) return 0;      /* no capability list */
    p = PciRead8(b, d, f, 0x34) & 0xFC;
    for (guard = 0; p >= 0x40 && guard < 48; guard++) {
        if (PciRead8(b, d, f, p) == id) return p;
        p = PciRead8(b, d, f, p + 1) & 0xFC;
    }
    return 0;
}
