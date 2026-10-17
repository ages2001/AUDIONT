/*
 * AUDIONT - OS abstraction layer
 */
#ifndef AUDIONT_OS_H
#define AUDIONT_OS_H

#include "ntk.h"

/* NT version as detected at runtime */
#define NTVER_31   0x030A
#define NTVER_35   0x0332
#define NTVER_351  0x0333
#define NTVER_40   0x0400
#define NTVER_50   0x0500    /* Windows 2000 (5.1 XP, 5.2 XP x64) */

extern ULONG g_NtVersion;      /* NTVER_xx */
extern ULONG g_NtBuild;        /* build number (0 if unknown) */
extern ULONG g_DebugLevel;     /* 0 = errors only, 1 = info, 2 = verbose, 3 = trace */
extern ULONG g_DebugPorts;     /* bit0 = port E9 (QEMU/Bochs), bit1 = COM1, bit2 = DbgPrint */

/* logging */
void  OsLog(ULONG level, const char *fmt, ...);
#define LOGE(...)  OsLog(0, __VA_ARGS__)
#define LOGI(...)  OsLog(1, __VA_ARGS__)
#define LOGV(...)  OsLog(2, __VA_ARGS__)
#define LOGT(...)  OsLog(3, __VA_ARGS__)
int   OsFormat(char *buf, int size, const char *fmt, __builtin_va_list ap);
int   OsSprintf(char *buf, int size, const char *fmt, ...);
ULONG OsLogSnapshot(char *dst, ULONG size);   /* copy in-memory log ring */

/* init */
NTSTATUS OsInit(PDRIVER_OBJECT drv, PUNICODE_STRING regPath);

/* dynamic import resolution */
PVOID OsResolve(int hal, const char *name);

/* memory */
#ifdef AUDIONT_WDM
typedef size_t OS_SIZE;
#else
typedef unsigned int OS_SIZE;
#endif
void *memcpy(void *d, const void *s, OS_SIZE n);
void *memset(void *d, int c, OS_SIZE n);
void *memmove(void *d, const void *s, OS_SIZE n);
int   memcmp(const void *a, const void *b, OS_SIZE n);
PVOID OsAlloc(ULONG size);               /* nonpaged, zeroed */
void  OsFree(PVOID p);

typedef struct _DMA_BUFFER {
    PVOID  Va;           /* CPU view (uncached alias when Uncached) */
    ULONG  Pa;           /* physical address, always < 4 GB */
    ULONG  Size;
    PVOID  Alloc;        /* address returned by the allocator */
    BOOLEAN Uncached;
} DMA_BUFFER;
/* physically contiguous, page aligned, < 2 GB; 'uncached' maps it uncached for
   controllers that do not snoop the CPU caches */
BOOLEAN OsAllocDma(DMA_BUFFER *b, ULONG size, BOOLEAN uncached);
void    OsFreeDma(DMA_BUFFER *b);
PVOID   OsMapMmio(ULONG pa, ULONG size);
void    OsUnmapMmio(PVOID va, ULONG size);

/* time */
void  OsStallUs(ULONG us);
void  OsSleepMs(ULONG ms);               /* PASSIVE_LEVEL only; stalls otherwise */
ULONG OsTimeMs(void);                    /* monotonic milliseconds */

/* worker threads and events (passive level work queued from DISPATCH_LEVEL) */
typedef struct _OSEVENT { KEVENT Ev; } OSEVENT;
void    OsEventInit(OSEVENT *e);                 /* auto-reset */
void    OsEventSet(OSEVENT *e);
BOOLEAN OsEventWait(OSEVENT *e, ULONG ms);       /* PASSIVE_LEVEL; FALSE on timeout */
BOOLEAN OsThreadStart(void (*fn)(void *), void *ctx);
void    OsThreadExit(void);

/* spinlock wrappers */
typedef struct _OSLOCK { KSPIN_LOCK Lock; KIRQL OldIrql; } OSLOCK;
static __inline__ void OsLockInit(OSLOCK *l) { l->Lock = 0; }
void OsLockAcquire(OSLOCK *l);
void OsLockRelease(OSLOCK *l);
static __inline__ void OsLockAcquireDpc(OSLOCK *l) { KeAcquireSpinLockAtDpcLevel(&l->Lock); }
static __inline__ void OsLockReleaseDpc(OSLOCK *l) { KeReleaseSpinLockFromDpcLevel(&l->Lock); }

static __inline__ void OsAtomicInc(LONG *p) { __asm__ __volatile__("lock; incl %0" : "+m"(*p) :: "memory"); }
static __inline__ void OsAtomicDec(LONG *p) { __asm__ __volatile__("lock; decl %0" : "+m"(*p) :: "memory"); }

/* IRP completion (IoCompleteRequest / IofCompleteRequest) */
void OsCompleteIrp(PIRP irp, NTSTATUS st, ULONG info);
PVOID OsMdlAddress(PMDL mdl);

/* registry (service Parameters key) */
ULONG OsRegGetDword(const WCHAR *name, ULONG def);
BOOLEAN OsRegSetDword(const WCHAR *name, ULONG val);
BOOLEAN OsRegSetBinary(const WCHAR *name, PVOID data, ULONG len);
ULONG OsRegGetBinary(const WCHAR *name, PVOID data, ULONG len);

/* PCI configuration space (mechanism #1, no HAL dependency) */
ULONG  PciRead32(ULONG bus, ULONG dev, ULONG fn, ULONG off);
USHORT PciRead16(ULONG bus, ULONG dev, ULONG fn, ULONG off);
UCHAR  PciRead8(ULONG bus, ULONG dev, ULONG fn, ULONG off);
void   PciWrite32(ULONG bus, ULONG dev, ULONG fn, ULONG off, ULONG v);
void   PciWrite16(ULONG bus, ULONG dev, ULONG fn, ULONG off, USHORT v);
void   PciWrite8(ULONG bus, ULONG dev, ULONG fn, ULONG off, UCHAR v);

/* misc */
#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
ULONG OsWcslen(const WCHAR *s);
void  OsWcsCopyA(WCHAR *d, const char *s, ULONG max);   /* ASCII -> UTF-16 */
ULONG PciFindCap(ULONG bus, ULONG dev, ULONG fn, UCHAR id);   /* 0 if absent */

#endif
