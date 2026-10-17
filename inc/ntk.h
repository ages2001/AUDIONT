/*
 * AUDIONT - minimal NT kernel definitions
 *
 * Self-contained subset of the NT kernel API that is common to Windows NT
 * 3.1, 3.5, 3.51 and 4.0 on x86.  Structure layouts were checked against the
 * stock NT 3.1 drivers (BEEP.SYS) and match the NT 4.0 layout.
 *
 * Only functions that exist in the NT 3.1 ntoskrnl.exe / hal.dll export
 * tables may be imported statically.  Everything else goes through
 * OsResolve() (see os.c).
 */
#ifndef AUDIONT_NTK_H
#define AUDIONT_NTK_H

#ifdef AUDIONT_WDM
#include "ntkwdm.h"     /* Windows 2000 / XP (x86, x64): the DDK headers */
#else

#define NTAPI    __attribute__((stdcall))
#define FASTCALL __attribute__((fastcall))
#define NULL     ((void *)0)
#define TRUE     1
#define FALSE    0

typedef unsigned char      UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN, KIRQL, *PKIRQL;
typedef char               CHAR, *PCHAR, CCHAR;
typedef unsigned short     USHORT, *PUSHORT, WCHAR, *PWCHAR, *PWSTR;
typedef short              SHORT, *PSHORT, CSHORT;
typedef unsigned long      ULONG, *PULONG, KAFFINITY, *PKAFFINITY;
typedef long               LONG, *PLONG, NTSTATUS;
typedef unsigned long long ULONGLONG;
typedef long long          LONGLONG;
typedef void               VOID, *PVOID;
typedef ULONG              KSPIN_LOCK, *PKSPIN_LOCK;
typedef CCHAR              KPROCESSOR_MODE;
typedef ULONG              ACCESS_MASK;
typedef PVOID              HANDLE, *PHANDLE;
typedef ULONG              SIZE_T, ULONG_PTR;

typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER, PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink, *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

/* ---- status codes ---- */
#define STATUS_SUCCESS                 ((NTSTATUS)0x00000000L)
#define STATUS_PENDING                 ((NTSTATUS)0x00000103L)
#define STATUS_UNSUCCESSFUL            ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED         ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_PARAMETER       ((NTSTATUS)0xC000000DL)
#define STATUS_NO_SUCH_DEVICE          ((NTSTATUS)0xC000000EL)
#define STATUS_INVALID_DEVICE_REQUEST  ((NTSTATUS)0xC0000010L)
#define STATUS_NO_MEMORY               ((NTSTATUS)0xC0000017L)
#define STATUS_ACCESS_DENIED           ((NTSTATUS)0xC0000022L)
#define STATUS_BUFFER_TOO_SMALL        ((NTSTATUS)0xC0000023L)
#define STATUS_OBJECT_NAME_NOT_FOUND   ((NTSTATUS)0xC0000034L)
#define STATUS_INSUFFICIENT_RESOURCES  ((NTSTATUS)0xC000009AL)
#define STATUS_DEVICE_BUSY             ((NTSTATUS)0x80000011L)
#define STATUS_NOT_SUPPORTED           ((NTSTATUS)0xC00000BBL)
#define STATUS_CANCELLED               ((NTSTATUS)0xC0000120L)
#define STATUS_DEVICE_DOES_NOT_EXIST   ((NTSTATUS)0xC00000C0L)
#define STATUS_BUFFER_OVERFLOW         ((NTSTATUS)0x80000005L)
#define NT_SUCCESS(s)                  (((NTSTATUS)(s)) >= 0)

/* ---- IRQL ---- */
#define PASSIVE_LEVEL   0
#define APC_LEVEL       1
#define DISPATCH_LEVEL  2
#define HIGH_LEVEL      31

/* ---- forward declarations ---- */
struct _DEVICE_OBJECT;
struct _DRIVER_OBJECT;
struct _IRP;
struct _KDPC;
struct _KINTERRUPT;
typedef struct _KINTERRUPT *PKINTERRUPT;

typedef NTSTATUS (NTAPI *PDRIVER_DISPATCH)(struct _DEVICE_OBJECT *, struct _IRP *);
typedef VOID     (NTAPI *PDRIVER_UNLOAD)(struct _DRIVER_OBJECT *);
typedef VOID     (NTAPI *PDRIVER_CANCEL)(struct _DEVICE_OBJECT *, struct _IRP *);
typedef VOID     (NTAPI *PKDEFERRED_ROUTINE)(struct _KDPC *, PVOID, PVOID, PVOID);
typedef BOOLEAN  (NTAPI *PKSERVICE_ROUTINE)(PKINTERRUPT, PVOID);
typedef BOOLEAN  (NTAPI *PKSYNCHRONIZE_ROUTINE)(PVOID);

/* ---- dispatcher objects (sizes padded generously) ---- */
typedef struct _KDPC   { ULONG Opaque[16]; } KDPC, *PKDPC;    /* real: 0x20 */
typedef struct _KTIMER { ULONG Opaque[16]; } KTIMER, *PKTIMER; /* real: 0x28 */
typedef struct _KEVENT { ULONG Opaque[8];  } KEVENT, *PKEVENT; /* real: 0x10 */

typedef enum _EVENT_TYPE { NotificationEvent, SynchronizationEvent } EVENT_TYPE;

typedef struct _IO_STATUS_BLOCK {
    NTSTATUS Status;
    ULONG    Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

/* ---- MDL ---- */
typedef struct _MDL {
    struct _MDL *Next;
    CSHORT Size;
    CSHORT MdlFlags;
    PVOID  Process;
    PVOID  MappedSystemVa;
    PVOID  StartVa;
    ULONG  ByteCount;
    ULONG  ByteOffset;
} MDL, *PMDL;
#define MDL_MAPPED_TO_SYSTEM_VA      0x0001
#define MDL_SOURCE_IS_NONPAGED_POOL  0x0004

/* ---- DEVICE_OBJECT (only the leading, version-stable part is used) ---- */
typedef struct _DEVICE_OBJECT {
    CSHORT Type;                         /* 00 */
    USHORT Size;                         /* 02 */
    LONG   ReferenceCount;               /* 04 */
    struct _DRIVER_OBJECT *DriverObject; /* 08 */
    struct _DEVICE_OBJECT *NextDevice;   /* 0c */
    struct _DEVICE_OBJECT *AttachedDevice; /* 10 */
    struct _IRP *CurrentIrp;             /* 14 */
    PVOID  Timer;                        /* 18 */
    ULONG  Flags;                        /* 1c */
    ULONG  Characteristics;              /* 20 */
    PVOID  Vpb;                          /* 24 */
    PVOID  DeviceExtension;              /* 28 */
    ULONG  DeviceType;                   /* 2c */
    CCHAR  StackSize;                    /* 30 */
} DEVICE_OBJECT, *PDEVICE_OBJECT;

#define DO_BUFFERED_IO         0x00000004
#define DO_EXCLUSIVE           0x00000008
#define DO_DIRECT_IO           0x00000010
#define DO_DEVICE_INITIALIZING 0x00000080

#define FILE_DEVICE_SOUND      0x0000001d
#define FILE_DEVICE_UNKNOWN    0x00000022

/* ---- DRIVER_OBJECT ---- */
#define IRP_MJ_CREATE          0x00
#define IRP_MJ_CLOSE           0x02
#define IRP_MJ_READ            0x03
#define IRP_MJ_WRITE           0x04
#define IRP_MJ_DEVICE_CONTROL  0x0e
#define IRP_MJ_SHUTDOWN        0x10
#define IRP_MJ_CLEANUP         0x12
#define IRP_MJ_MAXIMUM_FUNCTION 0x1b

typedef struct _DRIVER_OBJECT {
    CSHORT Type;                    /* 00 */
    CSHORT Size;                    /* 02 */
    PDEVICE_OBJECT DeviceObject;    /* 04 */
    ULONG  Flags;                   /* 08 */
    PVOID  DriverStart;             /* 0c */
    ULONG  DriverSize;              /* 10 */
    PVOID  DriverSection;           /* 14 */
    PVOID  DriverExtension;         /* 18 */
    UNICODE_STRING DriverName;      /* 1c */
    PUNICODE_STRING HardwareDatabase; /* 24 */
    PVOID  FastIoDispatch;          /* 28 */
    PVOID  DriverInit;              /* 2c */
    PVOID  DriverStartIo;           /* 30 */
    PDRIVER_UNLOAD DriverUnload;    /* 34 */
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1]; /* 38 */
} DRIVER_OBJECT, *PDRIVER_OBJECT;

/* ---- IO_STACK_LOCATION (4-byte packed on x86, like the DDK) ---- */
#pragma pack(push, 4)
typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;            /* 00 */
    UCHAR MinorFunction;            /* 01 */
    UCHAR Flags;                    /* 02 */
    UCHAR Control;                  /* 03 */
    union {
        struct { ULONG Length; ULONG Key; LARGE_INTEGER ByteOffset; } Read;
        struct { ULONG Length; ULONG Key; LARGE_INTEGER ByteOffset; } Write;
        struct {
            ULONG OutputBufferLength;
            ULONG InputBufferLength;
            ULONG IoControlCode;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
        struct { PVOID A1, A2, A3, A4; } Others;
    } Parameters;                   /* 04 */
    PDEVICE_OBJECT DeviceObject;    /* 14 */
    PVOID FileObject;               /* 18 */
    PVOID CompletionRoutine;        /* 1c */
    PVOID Context;                  /* 20 */
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
#pragma pack(pop)

#define SL_PENDING_RETURNED 0x01

/* ---- IRP ---- */
typedef struct _IRP {
    CSHORT Type;                    /* 00 */
    USHORT Size;                    /* 02 */
    PMDL   MdlAddress;              /* 04 */
    ULONG  Flags;                   /* 08 */
    union {
        struct _IRP *MasterIrp;
        LONG  IrpCount;
        PVOID SystemBuffer;
    } AssociatedIrp;                /* 0c */
    LIST_ENTRY ThreadListEntry;     /* 10 */
    IO_STATUS_BLOCK IoStatus;       /* 18 */
    KPROCESSOR_MODE RequestorMode;  /* 20 */
    BOOLEAN PendingReturned;        /* 21 */
    CHAR   StackCount;              /* 22 */
    CHAR   CurrentLocation;         /* 23 */
    BOOLEAN Cancel;                 /* 24 */
    KIRQL  CancelIrql;              /* 25 */
    CCHAR  ApcEnvironment;          /* 26 */
    UCHAR  AllocationFlags;         /* 27 */
    PIO_STATUS_BLOCK UserIosb;      /* 28 */
    PKEVENT UserEvent;              /* 2c */
    PVOID  Overlay[2];              /* 30 */
    PDRIVER_CANCEL CancelRoutine;   /* 38 */
    PVOID  UserBuffer;              /* 3c */
    PVOID  DriverContext[4];        /* 40  Tail.Overlay.DriverContext */
    PVOID  Thread;                  /* 50 */
    PCHAR  AuxiliaryBuffer;         /* 54 */
    LIST_ENTRY ListEntry;           /* 58  Tail.Overlay.ListEntry */
    PIO_STACK_LOCATION CurrentStackLocation; /* 60 */
    PVOID  OriginalFileObject;      /* 64 */
} IRP, *PIRP;

#define NTK_OFS(t, f) ((ULONG)&((t *)0)->f)
_Static_assert(NTK_OFS(IO_STACK_LOCATION, Parameters.DeviceIoControl.IoControlCode) == 0x0c, "iosl");
_Static_assert(NTK_OFS(IO_STACK_LOCATION, FileObject) == 0x18, "iosl fo");
_Static_assert(sizeof(IO_STACK_LOCATION) == 0x24, "iosl size");
_Static_assert(NTK_OFS(IRP, CurrentStackLocation) == 0x60, "irp csl");
_Static_assert(NTK_OFS(IRP, IoStatus) == 0x18, "irp ios");
_Static_assert(NTK_OFS(IRP, ListEntry) == 0x58, "irp le");
_Static_assert(NTK_OFS(IRP, CancelRoutine) == 0x38, "irp cr");
_Static_assert(NTK_OFS(DRIVER_OBJECT, MajorFunction) == 0x38, "drv mf");
_Static_assert(NTK_OFS(DEVICE_OBJECT, DeviceExtension) == 0x28, "dev ext");
#define IoGetCurrentIrpStackLocation(irp) ((irp)->CurrentStackLocation)
#define IRP_LE_FIELD ListEntry           /* Tail.Overlay.ListEntry in the DDK */
#define IRP_DC_FIELD DriverContext
#define IoMarkIrpPending(irp) ((irp)->CurrentStackLocation->Control |= SL_PENDING_RETURNED)
#define IO_NO_INCREMENT        0
#define IO_SOUND_INCREMENT     8

/* ---- CTL_CODE ---- */
#define CTL_CODE(dev, fn, meth, acc) (((dev) << 16) | ((acc) << 14) | ((fn) << 2) | (meth))
#define METHOD_BUFFERED    0
#define METHOD_IN_DIRECT   1
#define METHOD_OUT_DIRECT  2
#define METHOD_NEITHER     3
#define FILE_ANY_ACCESS    0
#define FILE_READ_ACCESS   1
#define FILE_WRITE_ACCESS  2

/* ---- memory / bus ---- */
typedef enum _POOL_TYPE { NonPagedPool = 0, PagedPool = 1 } POOL_TYPE;
typedef enum _KINTERRUPT_MODE { LevelSensitive = 0, Latched = 1 } KINTERRUPT_MODE;
typedef enum _INTERFACE_TYPE {
    Internal = 0, Isa = 1, Eisa = 2, MicroChannel = 3, TurboChannel = 4, PCIBus = 5
} INTERFACE_TYPE;
typedef enum _BUS_DATA_TYPE { Cmos = 0, EisaConfiguration = 1, Pos = 2,
                              CbusConfiguration = 3, PCIConfiguration = 4 } BUS_DATA_TYPE;
typedef enum _MEMORY_CACHING_TYPE { MmNonCached = 0, MmCached = 1 } MEMORY_CACHING_TYPE;

/* ---- registry ---- */
typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
#define OBJ_CASE_INSENSITIVE 0x00000040
#define KEY_READ             0x20019
#define KEY_ALL_ACCESS       0xF003F
#define REG_SZ               1
#define REG_BINARY           3
#define REG_DWORD            4
#define REG_OPTION_NON_VOLATILE 0

typedef enum _KEY_VALUE_INFORMATION_CLASS {
    KeyValueBasicInformation, KeyValueFullInformation, KeyValuePartialInformation
} KEY_VALUE_INFORMATION_CLASS;
typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataLength;
    UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;

/* ---- statically imported functions (all present in NT 3.1) ---- */
/* ntoskrnl.exe */
NTSTATUS NTAPI IoCreateDevice(PDRIVER_OBJECT, ULONG, PUNICODE_STRING, ULONG, ULONG, BOOLEAN, PDEVICE_OBJECT *);
VOID     NTAPI IoDeleteDevice(PDEVICE_OBJECT);
NTSTATUS NTAPI IoCreateSymbolicLink(PUNICODE_STRING, PUNICODE_STRING);
NTSTATUS NTAPI IoDeleteSymbolicLink(PUNICODE_STRING);
VOID     NTAPI IoAcquireCancelSpinLock(PKIRQL);
VOID     NTAPI IoReleaseCancelSpinLock(KIRQL);
NTSTATUS NTAPI IoConnectInterrupt(PKINTERRUPT *, PKSERVICE_ROUTINE, PVOID, PKSPIN_LOCK,
                                  ULONG, KIRQL, KIRQL, KINTERRUPT_MODE, BOOLEAN, KAFFINITY, BOOLEAN);
VOID     NTAPI IoDisconnectInterrupt(PKINTERRUPT);
VOID     NTAPI KeInitializeDpc(PKDPC, PKDEFERRED_ROUTINE, PVOID);
BOOLEAN  NTAPI KeInsertQueueDpc(PKDPC, PVOID, PVOID);
BOOLEAN  NTAPI KeRemoveQueueDpc(PKDPC);
VOID     NTAPI KeInitializeTimer(PKTIMER);
BOOLEAN  NTAPI KeSetTimer(PKTIMER, LARGE_INTEGER, PKDPC);
BOOLEAN  NTAPI KeCancelTimer(PKTIMER);
VOID     NTAPI KeInitializeSpinLock(PKSPIN_LOCK);
VOID     NTAPI KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK);
VOID     NTAPI KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK);
BOOLEAN  NTAPI KeSynchronizeExecution(PKINTERRUPT, PKSYNCHRONIZE_ROUTINE, PVOID);
VOID     NTAPI KeInitializeEvent(PKEVENT, EVENT_TYPE, BOOLEAN);
LONG     NTAPI KeSetEvent(PKEVENT, LONG, BOOLEAN);
NTSTATUS NTAPI KeWaitForSingleObject(PVOID, ULONG, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);
typedef VOID (NTAPI *PKSTART_ROUTINE)(PVOID);
NTSTATUS NTAPI PsCreateSystemThread(PHANDLE, ULONG, POBJECT_ATTRIBUTES, HANDLE, PVOID, PKSTART_ROUTINE, PVOID);
NTSTATUS NTAPI PsTerminateSystemThread(NTSTATUS);
VOID     NTAPI KeQuerySystemTime(PLARGE_INTEGER);
VOID     NTAPI KeQueryTickCount(PLARGE_INTEGER);
PVOID    NTAPI ExAllocatePool(POOL_TYPE, ULONG);
VOID     NTAPI ExFreePool(PVOID);
PVOID    NTAPI MmAllocateContiguousMemory(ULONG, PHYSICAL_ADDRESS);
VOID     NTAPI MmFreeContiguousMemory(PVOID);
PHYSICAL_ADDRESS NTAPI MmGetPhysicalAddress(PVOID);
PVOID    NTAPI MmMapIoSpace(PHYSICAL_ADDRESS, ULONG, MEMORY_CACHING_TYPE);
VOID     NTAPI MmUnmapIoSpace(PVOID, ULONG);
PVOID    NTAPI MmMapLockedPages(PMDL, KPROCESSOR_MODE);
VOID     NTAPI RtlInitUnicodeString(PUNICODE_STRING, const WCHAR *);
NTSTATUS NTAPI ZwOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
NTSTATUS NTAPI ZwCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
NTSTATUS NTAPI ZwQueryValueKey(HANDLE, PUNICODE_STRING, KEY_VALUE_INFORMATION_CLASS, PVOID, ULONG, PULONG);
NTSTATUS NTAPI ZwSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
NTSTATUS NTAPI ZwClose(HANDLE);
/* files (firmware images) */
#define FILE_READ_DATA          0x0001
#define SYNCHRONIZE             0x00100000
#define GENERIC_READ            0x80000000
#define FILE_SHARE_READ         0x00000001
#define FILE_OPEN               0x00000001
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define FILE_NON_DIRECTORY_FILE 0x00000040
#define FILE_ATTRIBUTE_NORMAL   0x00000080
typedef enum _FILE_INFORMATION_CLASS { FileStandardInformation = 5 } FILE_INFORMATION_CLASS;
typedef struct _FILE_STANDARD_INFORMATION {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} FILE_STANDARD_INFORMATION;
NTSTATUS NTAPI ZwCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                            ULONG, ULONG, ULONG, PVOID, ULONG);
NTSTATUS NTAPI ZwReadFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
NTSTATUS NTAPI ZwQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
ULONG    __attribute__((cdecl)) DbgPrint(const char *, ...);
NTSTATUS NTAPI IoRegisterShutdownNotification(PDEVICE_OBJECT);
VOID     NTAPI IoUnregisterShutdownNotification(PDEVICE_OBJECT);
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
/* hal.dll */
ULONG    NTAPI HalGetInterruptVector(INTERFACE_TYPE, ULONG, ULONG, ULONG, PKIRQL, PKAFFINITY);
ULONG    NTAPI HalGetBusData(BUS_DATA_TYPE, ULONG, ULONG, PVOID, ULONG);
VOID     NTAPI KeStallExecutionProcessor(ULONG);
VOID     NTAPI KeAcquireSpinLock(PKSPIN_LOCK, PKIRQL);
VOID     NTAPI KeReleaseSpinLock(PKSPIN_LOCK, KIRQL);
VOID     NTAPI KeRaiseIrql(KIRQL, PKIRQL);
VOID     NTAPI KeLowerIrql(KIRQL);
KIRQL    NTAPI KeGetCurrentIrql(VOID);
LARGE_INTEGER NTAPI KeQueryPerformanceCounter(PLARGE_INTEGER);

/* ---- port / register I/O (inline, no HAL import needed on x86) ---- */
static __inline__ UCHAR  inb(USHORT p) { UCHAR v;  __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static __inline__ USHORT inw(USHORT p) { USHORT v; __asm__ __volatile__("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static __inline__ ULONG  inl(USHORT p) { ULONG v;  __asm__ __volatile__("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static __inline__ void outb(USHORT p, UCHAR v)  { __asm__ __volatile__("outb %0,%1" :: "a"(v), "Nd"(p)); }
static __inline__ void outw(USHORT p, USHORT v) { __asm__ __volatile__("outw %0,%1" :: "a"(v), "Nd"(p)); }
static __inline__ void outl(USHORT p, ULONG v)  { __asm__ __volatile__("outl %0,%1" :: "a"(v), "Nd"(p)); }
#define MMIO8(b,o)  (*(volatile UCHAR  *)((PUCHAR)(b) + (o)))
#define MMIO16(b,o) (*(volatile USHORT *)((PUCHAR)(b) + (o)))
#define MMIO32(b,o) (*(volatile ULONG  *)((PUCHAR)(b) + (o)))

static __inline__ ULONG IntSave(void)  { ULONG f; __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(f) :: "memory"); return f; }
static __inline__ void  IntRestore(ULONG f) { __asm__ __volatile__("pushl %0; popfl" :: "r"(f) : "memory", "cc"); }
#define MemoryBarrier() __asm__ __volatile__("lock; addl $0,(%%esp)" ::: "memory")

/* ---- list helpers ---- */
static __inline__ void InitializeListHead(PLIST_ENTRY h) { h->Flink = h->Blink = h; }
static __inline__ int  IsListEmpty(PLIST_ENTRY h) { return h->Flink == h; }
static __inline__ void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e)
{ PLIST_ENTRY b = h->Blink; e->Flink = h; e->Blink = b; b->Flink = e; h->Blink = e; }
static __inline__ void InsertHeadList(PLIST_ENTRY h, PLIST_ENTRY e)
{ PLIST_ENTRY f = h->Flink; e->Flink = f; e->Blink = h; f->Blink = e; h->Flink = e; }
static __inline__ void RemoveEntryList(PLIST_ENTRY e)
{ PLIST_ENTRY f = e->Flink, b = e->Blink; b->Flink = f; f->Blink = b; e->Flink = e->Blink = e; }
static __inline__ PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h)
{ PLIST_ENTRY e = h->Flink; RemoveEntryList(e); return e; }
#define CONTAINING_RECORD(addr, type, field) ((type *)((PCHAR)(addr) - (ULONG)(&((type *)0)->field)))

#endif /* AUDIONT_WDM */
#endif
