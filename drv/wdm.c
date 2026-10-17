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

static void CardUp(FDO_EXT *x)
{
    x->Card = CardProbe(x->Bus, x->Dev, x->Fn, x->Int.Valid ? &x->Int : NULL);
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

static VOID NTAPI Unload(PDRIVER_OBJECT drv)
{
    (void)drv;
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
    return STATUS_SUCCESS;
}
