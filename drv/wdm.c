/*
 * AUDIONT - Windows 2000 / XP (x86 and x64) Plug and Play: AUDIONT is the
 * function driver of the sound controllers its INF names (HD Audio, AC'97,
 * VIA) and of the "USBNT\Audio" device the USBNT device bus reports while
 * a USB audio device is connected.  START_DEVICE brings the card up with
 * the interrupt Plug and Play assigned, STOP / REMOVE takes it down; a
 * sleep state takes the card down and the return to S0 brings it back.
 *
 * The wave, MIDI, aux and mixer devices stay the NT 4.0 style devices
 * (\Device\AudntWaveOut0 ...) that audiont.dll, the WinMM driver, opens.
 */
#include "audiont.h"

#define FDO_MAGIC   0x4D445741          /* 'AWDM' */
#define FDO_PCI     1
#define FDO_USB     2

typedef struct _FDO_EXT {
    ULONG           Magic;              /* first, like DEV_EXT.Magic */
    ULONG           Kind;
    PDEVICE_OBJECT  Self;
    PDEVICE_OBJECT  Lower;
    PDEVICE_OBJECT  Pdo;
    ULONG           Bus, Dev, Fn;
    CARD_INT        Int;
    CARD           *Card;
    BOOLEAN         Started;
    BOOLEAN         Sleeping;
} FDO_EXT;

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg);
static LONG g_UsbFdos;
static BOOLEAN g_UsbUp;

/* ------------------------------------------------------------------ */
static NTSTATUS NTAPI SyncDone(PDEVICE_OBJECT d, PIRP irp, PVOID ctx)
{
    (void)d; (void)irp;
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardSync(FDO_EXT *x, PIRP irp)
{
    KEVENT ev;
    NTSTATUS st;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, SyncDone, &ev, TRUE, TRUE, TRUE);
    st = IoCallDriver(x->Lower, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    return st;
}

static NTSTATUS PassDown(FDO_EXT *x, PIRP irp)
{
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(x->Lower, irp);
}

static NTSTATUS Complete(PIRP irp, NTSTATUS st)
{
    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* the interrupt in the translated resources of START_DEVICE */
static void FindInterrupt(PCM_RESOURCE_LIST rl, CARD_INT *out)
{
    ULONG i, j;
    memset(out, 0, sizeof(*out));
    if (!rl) return;
    for (i = 0; i < rl->Count; i++) {
        PCM_PARTIAL_RESOURCE_LIST pl = &rl->List[i].PartialResourceList;
        for (j = 0; j < pl->Count; j++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &pl->PartialDescriptors[j];
            if (d->Type == CmResourceTypeInterrupt && !out->Valid) {
                out->Valid = TRUE;
                out->Vector = d->u.Interrupt.Vector;
                out->Irql = (KIRQL)d->u.Interrupt.Level;
                out->Affinity = d->u.Interrupt.Affinity;
                out->Mode = (d->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
                out->Shared = d->ShareDisposition == CmResourceShareShared;
            }
        }
    }
}

static BOOLEAN PciAddress(PDEVICE_OBJECT pdo, ULONG *bus, ULONG *dev, ULONG *fn)
{
    ULONG b = 0, a = 0, len;
    if (!NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyBusNumber, sizeof(b), &b, &len)) ||
        !NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyAddress, sizeof(a), &a, &len)))
        return FALSE;
    *bus = b; *dev = a >> 16; *fn = a & 0xFFFF;
    return TRUE;
}

NTKERNELAPI NTSTATUS NTAPI ObQueryNameString(PVOID Object, POBJECT_NAME_INFORMATION Info, ULONG Length,
                                            PULONG ReturnLength);

/* FriendlyName of the PDO's device node (what Device Manager shows): the instance key is the parent of
   the key IoOpenDeviceRegistryKey(PLUGPLAY_REGKEY_DEVICE) opens ("<instance>\Device Parameters") */
static void SetFriendlyName(PDEVICE_OBJECT pdo, const char *name)
{
    HANDLE h = NULL, inst = NULL;
    PVOID obj = NULL;
    ULONG len = 0, n;
    POBJECT_NAME_INFORMATION ni;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING vn;
    WCHAR w[96];

    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !NT_SUCCESS(IoOpenDeviceRegistryKey(pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &h)))
        return;
    ni = (POBJECT_NAME_INFORMATION)ExAllocatePoolWithTag(PagedPool, 1024, 0x61647561UL);
    if (ni && NT_SUCCESS(ObReferenceObjectByHandle(h, KEY_READ, NULL, KernelMode, &obj, NULL))) {
        if (NT_SUCCESS(ObQueryNameString(obj, ni, 1024 - sizeof(WCHAR), &len)) && ni->Name.Buffer) {
            USHORT c = (USHORT)(ni->Name.Length / sizeof(WCHAR));
            while (c > 0 && ni->Name.Buffer[c - 1] != L'\\') c--;
            if (c > 1) {
                ni->Name.Length = (USHORT)((c - 1) * sizeof(WCHAR));
                InitializeObjectAttributes(&oa, &ni->Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                if (!NT_SUCCESS(ZwOpenKey(&inst, KEY_ALL_ACCESS, &oa))) inst = NULL;
            }
        }
        ObDereferenceObject(obj);
    }
    if (ni) ExFreePool(ni);
    ZwClose(h);
    if (!inst) return;
    for (n = 0; name[n] && n + 1 < sizeof(w) / sizeof(w[0]); n++) w[n] = (WCHAR)(UCHAR)name[n];
    w[n] = 0;
    RtlInitUnicodeString(&vn, L"FriendlyName");
    ZwSetValueKey(inst, &vn, 0, REG_SZ, w, (n + 1) * sizeof(WCHAR));
    ZwClose(inst);
}

static void CardUp(FDO_EXT *x)
{
    x->Card = CardProbe(x->Bus, x->Dev, x->Fn, x->Int.Valid ? &x->Int : NULL);
    if (x->Card && OsRegGetDword(L"DeviceNames", 1)) {
        char name[96];
        CardDeviceName(x->Card, name, sizeof(name));
        SetFriendlyName(x->Pdo, name);
        LOGI("pnp: %u:%u.%u is \"%s\" in Device Manager", x->Bus, x->Dev, x->Fn, name);
    }
    if (!x->Card)
        LOGI("pnp: %u:%u.%u has no usable codec (HDMI only or not supported); device left idle",
             x->Bus, x->Dev, x->Fn);
}

static void CardDown(FDO_EXT *x)
{
    if (x->Card) {
        CardRemove(x->Card);
        x->Card = NULL;
    }
}

static void UsbUp(void)
{
    if (!g_UsbUp && OsRegGetDword(L"UsbAudio", 1)) g_UsbUp = UsbAudInit();
}

static void UsbDown(void)
{
    if (g_UsbUp) { UsbAudShutdown(); g_UsbUp = FALSE; }
}

/* ------------------------------------------------------------------ */
static NTSTATUS FdoPnp(FDO_EXT *x, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = ForwardSync(x, irp);
        if (NT_SUCCESS(st)) {
            if (x->Kind == FDO_USB) {
                UsbUp();
            } else if (PciAddress(x->Pdo, &x->Bus, &x->Dev, &x->Fn)) {
                FindInterrupt(sp->Parameters.StartDevice.AllocatedResourcesTranslated, &x->Int);
                LOGV("pnp: %u:%u.%u started, interrupt %s vector %u irql %u %s", x->Bus, x->Dev, x->Fn,
                     x->Int.Valid ? "yes" : "none", x->Int.Vector, x->Int.Irql, x->Int.Shared ? "shared" : "");
                CardUp(x);
            } else {
                LOGE("pnp: no PCI address");
                st = STATUS_NO_SUCH_DEVICE;
            }
            x->Started = NT_SUCCESS(st);
        }
        return Complete(irp, st);
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        if (x->Card && x->Card->OpenHandles > 0) {
            LOGI("pnp: card%u in use, removal refused", x->Card->Index);
            return Complete(irp, STATUS_DEVICE_BUSY);
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(x, irp);
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(x, irp);
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        if (x->Kind == FDO_PCI) CardDown(x);
        x->Started = FALSE;
        irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(x, irp);
    case IRP_MN_REMOVE_DEVICE:
        if (x->Kind == FDO_PCI) CardDown(x);
        else if (InterlockedDecrement(&g_UsbFdos) == 0) UsbDown();
        x->Started = FALSE;
        irp->IoStatus.Status = STATUS_SUCCESS;
        st = PassDown(x, irp);
        IoDetachDevice(x->Lower);
        x->Magic = 0;
        IoDeleteDevice(x->Self);
        return st;
    default:
        return PassDown(x, irp);
    }
}

static VOID NTAPI ResumeWork(PDEVICE_OBJECT dev, PVOID ctx)
{
    FDO_EXT *x = dev->DeviceExtension;
    if (x->Sleeping && x->Started) {
        x->Sleeping = FALSE;
        CardUp(x);
        LOGI("pnp: %u:%u.%u back after sleep", x->Bus, x->Dev, x->Fn);
    }
    IoFreeWorkItem((PIO_WORKITEM)ctx);
}

static NTSTATUS NTAPI PowerUpDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    FDO_EXT *x = dev->DeviceExtension;
    (void)ctx;
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    if (x->Sleeping) {
        PIO_WORKITEM wi = IoAllocateWorkItem(dev);
        if (wi) IoQueueWorkItem(wi, ResumeWork, DelayedWorkQueue, wi);
    }
    PoStartNextPowerIrp(irp);
    return STATUS_SUCCESS;
}

static NTSTATUS FdoPower(FDO_EXT *x, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    if (x->Kind == FDO_PCI && x->Started && sp->MinorFunction == IRP_MN_SET_POWER &&
        sp->Parameters.Power.Type == SystemPowerState) {
        SYSTEM_POWER_STATE s = sp->Parameters.Power.State.SystemState;
        if (s > PowerSystemWorking && s < PowerSystemShutdown && !x->Sleeping && x->Card) {
            x->Sleeping = TRUE;
            CardDown(x);                    /* the codec loses its state in S3 */
        } else if (s == PowerSystemWorking && x->Sleeping) {
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, PowerUpDone, NULL, TRUE, TRUE, TRUE);
            return PoCallDriver(x->Lower, irp);
        }
    }
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(x->Lower, irp);
}

/* ------------------------------------------------------------------ */
static NTSTATUS NTAPI WdmDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    FDO_EXT *x = dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);

    if (x && x->Magic == FDO_MAGIC) {
        switch (sp->MajorFunction) {
        case IRP_MJ_PNP:
            return FdoPnp(x, irp);
        case IRP_MJ_POWER:
            return FdoPower(x, irp);
        case IRP_MJ_SYSTEM_CONTROL:
            return PassDown(x, irp);
        case IRP_MJ_CREATE:
        case IRP_MJ_CLOSE:
        case IRP_MJ_CLEANUP:
            return Complete(irp, STATUS_SUCCESS);
        default:
            return Complete(irp, STATUS_INVALID_DEVICE_REQUEST);
        }
    }
    if (sp->MajorFunction == IRP_MJ_PNP || sp->MajorFunction == IRP_MJ_POWER ||
        sp->MajorFunction == IRP_MJ_SYSTEM_CONTROL) {
        if (sp->MajorFunction == IRP_MJ_POWER) PoStartNextPowerIrp(irp);
        return Complete(irp, irp->IoStatus.Status);
    }
    return AudiontDispatch(dev, irp);
}

static NTSTATUS NTAPI AddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT fdo;
    FDO_EXT *x;
    WCHAR hw[64];
    ULONG len = 0, kind = FDO_PCI;
    NTSTATUS st;

    memset(hw, 0, sizeof(hw));
    IoGetDeviceProperty(pdo, DevicePropertyHardwareID, sizeof(hw) - sizeof(WCHAR), hw, &len);
    if (hw[0] == 'U' && hw[1] == 'S' && hw[2] == 'B' && hw[3] == 'N' && hw[4] == 'T' && hw[5] == '\\')
        kind = FDO_USB;                     /* USBNT\Audio: USB audio devices through USBNT */
    st = IoCreateDevice(drv, sizeof(FDO_EXT), NULL, FILE_DEVICE_SOUND, FILE_DEVICE_SECURE_OPEN, FALSE, &fdo);
    if (!NT_SUCCESS(st)) return st;
    x = fdo->DeviceExtension;
    memset(x, 0, sizeof(*x));
    x->Magic = FDO_MAGIC;
    x->Kind = kind;
    x->Self = fdo;
    x->Pdo = pdo;
    x->Lower = IoAttachDeviceToDeviceStack(fdo, pdo);
    if (!x->Lower) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }
    if (kind == FDO_USB) InterlockedIncrement(&g_UsbFdos);
    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* the WinMM driver: audiont.dll must be in Drivers32, or Windows sees no sound device ("No  */
/* audio devices") although the device is installed - which is what an installation       */
/* through Device Manager / the Found New Hardware wizard with the INF alone gave (SETUP     */
/* registers it).  The driver registers it when audiont.dll is there and StandardNames is   */
/* 0 (SETUP /mmdrv sets 1: mmdrv.dll then).  At boot the SOFTWARE hive comes later than the  */
/* driver: a thread tries again.                                                            */
/* ------------------------------------------------------------------ */
static volatile LONG g_RegBusy, g_Unloading;

static HANDLE OpenKeyPath(const WCHAR *path, ACCESS_MASK acc, NTSTATUS *st)
{
    OBJECT_ATTRIBUTES oa; UNICODE_STRING us; HANDLE h = NULL;
    RtlInitUnicodeString(&us, path);
    oa.Length = sizeof(oa); oa.RootDirectory = NULL; oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor = NULL; oa.SecurityQualityOfService = NULL;
    *st = ZwOpenKey(&h, acc, &oa);
    return NT_SUCCESS(*st) ? h : NULL;
}

static BOOLEAN FileThere(const WCHAR *path)
{
    OBJECT_ATTRIBUTES oa; UNICODE_STRING us; IO_STATUS_BLOCK io; HANDLE h = NULL;
    RtlInitUnicodeString(&us, path);
    oa.Length = sizeof(oa); oa.RootDirectory = NULL; oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor = NULL; oa.SecurityQualityOfService = NULL;
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &io, NULL, 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) return FALSE;
    ZwClose(h);
    return TRUE;
}

/* value Name of key K is "audiont.dll" (1), empty / missing (0) or something else (2) */
static int Drv32Value(HANDLE k, const WCHAR *name)
{
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 128];
    PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    static const char want[] = "audiont.dll";
    UNICODE_STRING us; ULONG rl = 0, i, n;
    const WCHAR *v;
    RtlInitUnicodeString(&us, name);
    if (!NT_SUCCESS(ZwQueryValueKey(k, &us, KeyValuePartialInformation, kv, sizeof(buf) - 2, &rl))) return 0;
    if (kv->Type != REG_SZ && kv->Type != REG_EXPAND_SZ) return 2;
    v = (const WCHAR *)kv->Data;
    n = kv->DataLength / sizeof(WCHAR);
    while (n && !v[n - 1]) n--;
    if (n == 0) return 0;
    if (n != sizeof(want) - 1) return 2;
    for (i = 0; i < n; i++) {
        WCHAR c = v[i];
        if (c >= 'A' && c <= 'Z') c = (WCHAR)(c + 32);
        if (c != (WCHAR)want[i]) return 2;
    }
    return 1;
}

/* FALSE: the key is not there yet (SOFTWARE hive not loaded) */
static BOOLEAN RegisterWinmm(const WCHAR *keyPath, const WCHAR *dll)
{
    static const WCHAR *types[] = { L"wave", L"midi", L"aux", L"mixer" };
    static const WCHAR val[] = L"audiont.dll";
    WCHAR name[16];
    UNICODE_STRING us;
    NTSTATUS st;
    HANDLE k;
    ULONG t, i, j, free;
    if (!FileThere(dll)) return TRUE;
    k = OpenKeyPath(keyPath, KEY_READ | KEY_SET_VALUE, &st);
    if (!k) return st != STATUS_OBJECT_NAME_NOT_FOUND && st != STATUS_OBJECT_PATH_NOT_FOUND;
    for (t = 0; t < 4; t++) {
        free = 99;
        for (i = 0; i < 10; i++) {
            ULONG n = 0;
            for (j = 0; types[t][j]; j++) name[n++] = types[t][j];
            if (i) name[n++] = (WCHAR)('0' + i);
            name[n] = 0;
            j = (ULONG)Drv32Value(k, name);
            if (j == 1) break;
            if (j == 0 && free == 99) free = i;
        }
        if (i < 10 || free == 99) continue;              /* there already / no free slot */
        {
            ULONG n = 0;
            for (j = 0; types[t][j]; j++) name[n++] = types[t][j];
            if (free) name[n++] = (WCHAR)('0' + free);
            name[n] = 0;
        }
        RtlInitUnicodeString(&us, name);
        if (NT_SUCCESS(ZwSetValueKey(k, &us, 0, REG_SZ, (PVOID)val, sizeof(val))))
            LOGI("registered the WinMM driver: %S\\%S = audiont.dll", keyPath + 18, name);
    }
    ZwClose(k);
    return TRUE;
}

static BOOLEAN RegisterAll(void)
{
    BOOLEAN ok = RegisterWinmm(L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
                               L"\\SystemRoot\\System32\\audiont.dll");
#ifdef _WIN64
    /* 32 bit programs on x64 */
    ok &= RegisterWinmm(L"\\Registry\\Machine\\SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
                        L"\\SystemRoot\\SysWOW64\\audiont.dll");
#endif
    return ok;
}

static void RegThread(void *ctx)
{
    ULONG i;
    (void)ctx;
    for (i = 0; i < 300 && !g_Unloading; i++) {           /* up to 5 minutes after boot */
        OsSleepMs(1000);
        if (RegisterAll()) break;
    }
    InterlockedExchange((PLONG)&g_RegBusy, 0);
}

static void RegisterWinmmDriver(void)
{
    if (OsRegGetDword(L"StandardNames", 0) || OsRegGetDword(L"RegisterWinmm", 1) == 0) return;
    if (RegisterAll()) return;
    InterlockedExchange((PLONG)&g_RegBusy, 1);
    if (!OsThreadStart(RegThread, NULL)) InterlockedExchange((PLONG)&g_RegBusy, 0);
}

static VOID NTAPI Unload(PDRIVER_OBJECT drv)
{
    (void)drv;
    InterlockedExchange((PLONG)&g_Unloading, 1);
    while (g_RegBusy) OsSleepMs(50);
    UsbDown();
    AudiontUnloadAll();
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    ULONG i;
    AudiontSetDriver(drv);
    if (!NT_SUCCESS(OsInit(drv, reg))) return STATUS_UNSUCCESSFUL;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = WdmDispatch;
    drv->DriverExtension->AddDevice = AddDevice;
    drv->DriverUnload = Unload;
    /* audiont.dll opens the private device names; mmdrv.dll is not used on 2000 / XP */
    g_StdNames = OsRegGetDword(L"StandardNames", 0) ? TRUE : FALSE;
    AudiontRelocateAll();                    /* every BAR above 4 GB goes below 4 GB (32-bit kernel) */
    RegisterWinmmDriver();
    return STATUS_SUCCESS;
}
