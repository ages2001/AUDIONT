/*
 * AUDIONT - driver entry, PCI discovery, device objects and IRP dispatch
 */
#include "audiont.h"

CARD *g_Cards[MAX_CARDS];
ULONG g_CardCount;
static PDRIVER_OBJECT g_Driver;

void AudiontSetDriver(PDRIVER_OBJECT drv)
{
    g_Driver = drv;
}

static const char *g_KindName[DEV_KINDS] = { "WaveOut", "WaveIn", "MidiOut", "MMAux", "AudioNT" };
/* "standard" names are seen by mmdrv.dll; private names are used by audiont.dll */
static const char *g_KindNamePriv[DEV_KINDS] = { "AudntWaveOut", "AudntWaveIn", "AudntMidiOut", "AudntAux", "AudioNT" };
BOOLEAN g_StdNames = TRUE;
#define KINDNAME(k) (g_StdNames ? g_KindName[k] : g_KindNamePriv[k])

/* ------------------------------------------------------------------ */
/* device objects                                                      */
/* ------------------------------------------------------------------ */
static void MakeName(WCHAR *dst, const char *prefix, const char *kind, ULONG n)
{
    char tmp[64];
    OsSprintf(tmp, sizeof(tmp), "%s%s%u", prefix, kind, n);
    OsWcsCopyA(dst, tmp, 64);
}

static NTSTATUS CreateDev(CARD *c, ULONG kind)
{
    WCHAR name[64], link[64];
    UNICODE_STRING us, ls;
    PDEVICE_OBJECT dev = NULL;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    ULONG n;
    for (n = 0; n < 32; n++) {
        MakeName(name, "\\Device\\", KINDNAME(kind), n);
        RtlInitUnicodeString(&us, name);
        st = IoCreateDevice(g_Driver, sizeof(DEV_EXT), &us,
                            kind == DEV_CTRL ? FILE_DEVICE_UNKNOWN : FILE_DEVICE_SOUND,
                            0, FALSE, &dev);
        if (st != STATUS_OBJECT_NAME_COLLISION) break;
    }
    if (!NT_SUCCESS(st)) {
        LOGE("card%u: IoCreateDevice %s failed %x", c->Index, KINDNAME(kind), st);
        return st;
    }
    {
        DEV_EXT *ext = dev->DeviceExtension;
        ext->Magic = DEVEXT_MAGIC; ext->Card = c; ext->Kind = kind; ext->Index = n;
    }
    dev->Flags |= DO_DIRECT_IO;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    MakeName(link, "\\DosDevices\\", KINDNAME(kind), n);
    RtlInitUnicodeString(&ls, link);
    st = IoCreateSymbolicLink(&ls, &us);
    if (!NT_SUCCESS(st)) LOGE("symlink %S failed %x", link, st);
    c->Devs[kind] = dev;
    c->DevIndex[kind] = n;
    LOGV("card%u: created %S", c->Index, name);
    return STATUS_SUCCESS;
}

static void DeleteDevs(CARD *c)
{
    ULONG k;
    for (k = 0; k < DEV_KINDS; k++) {
        if (c->Devs[k]) {
            WCHAR link[64]; UNICODE_STRING ls;
            MakeName(link, "\\DosDevices\\", KINDNAME(k), c->DevIndex[k]);
            RtlInitUnicodeString(&ls, link);
            IoDeleteSymbolicLink(&ls);
            if (k == DEV_CTRL) IoUnregisterShutdownNotification(c->Devs[k]);
            /* a handle still open on it (the mixer) must not reach the freed card */
            ((DEV_EXT *)c->Devs[k]->DeviceExtension)->Magic = 0;
            IoDeleteDevice(c->Devs[k]);
            c->Devs[k] = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */
NTSTATUS NTAPI AudiontDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    DEV_EXT *ext = dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    CARD *c = ext->Card;
    NTSTATUS st = STATUS_SUCCESS;

    irp->IoStatus.Information = 0;
    if (ext->Magic != DEVEXT_MAGIC) {
        OsCompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    switch (sp->MajorFunction) {
    case IRP_MJ_CREATE:
        if (ext->Kind == DEV_WAVEIN && !c->CanRecord) st = STATUS_NO_SUCH_DEVICE;
        else if (ext->Kind == DEV_MIDIOUT) {
            EngineLock(c); c->SynthOpen++; EngineUnlock(c);
        }
        if (NT_SUCCESS(st) && ext->Kind != DEV_CTRL) OsAtomicInc(&c->OpenHandles);
        break;
    case IRP_MJ_CLOSE:
        if (ext->Kind != DEV_CTRL) OsAtomicDec(&c->OpenHandles);
        break;
    case IRP_MJ_CLEANUP:
        if (ext->Kind == DEV_WAVEOUT || ext->Kind == DEV_WAVEIN)
            StreamCleanup(c, sp->FileObject);
        else if (ext->Kind == DEV_MIDIOUT) {
            EngineLock(c);
            if (c->SynthOpen) c->SynthOpen--;
            SynthReset(c);
            EngineUnlock(c);
            if (!c->Synth) UsbMidiReset(c);
        }
        break;
    case IRP_MJ_WRITE:
        st = (ext->Kind == DEV_WAVEOUT) ? WaveOutWrite(c, irp, sp) : STATUS_INVALID_DEVICE_REQUEST;
        break;
    case IRP_MJ_READ:
        st = (ext->Kind == DEV_WAVEIN) ? WaveInRead(c, irp, sp) : STATUS_INVALID_DEVICE_REQUEST;
        break;
    case IRP_MJ_DEVICE_CONTROL:
        switch (ext->Kind) {
        case DEV_WAVEOUT: st = WaveOutIoctl(c, irp, sp); break;
        case DEV_WAVEIN:  st = WaveInIoctl(c, irp, sp); break;
        case DEV_MIDIOUT: st = MidiOutIoctl(c, irp, sp); break;
        case DEV_AUX:     st = AuxIoctl(c, irp, sp); break;
        case DEV_CTRL:    st = CtrlIoctl(c, irp, sp); break;
        default:          st = STATUS_INVALID_DEVICE_REQUEST;
        }
        if (st != STATUS_PENDING)
            LOGT("ioctl dev%u code %x -> %x info %u", ext->Kind, IrpCode(sp), st, irp->IoStatus.Information);
        break;
    case IRP_MJ_SHUTDOWN: {
        ULONG i;
        LOGI("system shutdown: stopping audio DMA");
        for (i = 0; i < g_CardCount; i++) {
            if (!g_Cards[i]) continue;
            EngineStopAll(g_Cards[i]);
            CtrlSave(g_Cards[i]);
        }
        break;
    }
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
    }
    if (st != STATUS_PENDING) OsCompleteIrp(irp, st, irp->IoStatus.Information);
    return st;
}

/* ------------------------------------------------------------------ */
/* PCI discovery                                                       */
/* ------------------------------------------------------------------ */
static void ReadPci(CARD *c)
{
    ULONG i;
    c->VendorId  = PciRead16(c->Bus, c->Dev, c->Fn, 0x00);
    c->DeviceId  = PciRead16(c->Bus, c->Dev, c->Fn, 0x02);
    c->Revision  = PciRead8(c->Bus, c->Dev, c->Fn, 0x08);
    c->SubVendor = PciRead16(c->Bus, c->Dev, c->Fn, 0x2C);
    c->SubDevice = PciRead16(c->Bus, c->Dev, c->Fn, 0x2E);
    c->Irq       = PciRead8(c->Bus, c->Dev, c->Fn, 0x3C);
    if (c->Irq == 0xFF) c->Irq = 0;
    for (i = 0; i < 6; i++) c->Bar[i] = PciRead32(c->Bus, c->Dev, c->Fn, 0x10 + i * 4);
}

/* creates the device objects of a card that is fully initialised */
void CardCreateDevs(CARD *c)
{
    ULONG k;
    for (k = 0; k < DEV_KINDS; k++) {
        if (k == DEV_WAVEIN && !c->CanRecord) continue;
        if (k == DEV_WAVEOUT && c->Type == HW_USB && !c->Lines[LINE_WAVE].Present) continue;
        if (k == DEV_AUX && c->Type == HW_USB && !c->Lines[LINE_WAVE].Present) continue;
        if (k == DEV_MIDIOUT && !c->Synth && !(c->Type == HW_USB && UsbMidiOut(c, NULL, 0))) continue;
        CreateDev(c, k);
    }
    if (c->Devs[DEV_CTRL]) IoRegisterShutdownNotification(c->Devs[DEV_CTRL]);
}

/* the name a card goes by: the codec part for HDA/AC'97 ("Realtek ALC1220"),
   the product string for USB ("Logitech USB Headset"), the controller when the
   codec is not identified */
static ULONG CardBaseName(CARD *c, char *buf, ULONG size)
{
    const char *n = c->Desc;
    ULONG i, k;
    if (c->Type != HW_USB && c->Codec[0]) {
        BOOLEAN generic = FALSE;
        for (i = 0; c->Codec[i]; i++)
            if (c->Codec[i] == 'c' && c->Codec[i + 1] == 'o' && c->Codec[i + 2] == 'd' && c->Codec[i + 3] == 'e' &&
                c->Codec[i + 4] == 'c') generic = TRUE;
        if (!generic) n = c->Codec;
    }
    if (!n[0]) n = c->Type == HW_USB ? "USB Audio Device" : "AUDIONT";
    for (k = 0; k + 1 < size && n[k]; k++) buf[k] = n[k];
    buf[k] = 0;
    for (i = 1; i < k; i++)                  /* "Conexant CX20561 (Hermosa)": no code names */
        if (buf[i] == '(' && buf[i - 1] == ' ' && c->Type != HW_USB) { buf[i - 1] = 0; k = i - 1; break; }
    return k;
}

/* copies src cut to max characters, at a word boundary when that keeps most of it */
static ULONG CopyCut(char *d, const char *src, ULONG max)
{
    ULONG n = 0, i;
    while (src[n]) n++;
    if (n > max) {
        for (i = max; i > max / 2 && src[i] != ' '; i--) ;
        n = src[i] == ' ' && i > max / 2 ? i : max;
        while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '-' || src[n - 1] == ',')) n--;
    }
    for (i = 0; i < n; i++) d[i] = src[i];
    d[n] = 0;
    return n;
}

/* product name of a device for the capabilities structures (at most 31 characters):
   "Realtek ALC1220", "Realtek ALC1220 In", "Realtek ALC1220 Synth", "... Volume";
   two cards with the same name get " #2", " #3" */
ULONG CardNameFor(CARD *c, ULONG kind, char *buf, ULONG size)
{
    char base[64], other[64], dup[8], out[40];
    const char *suf = "";
    ULONG i, n, max;
    CardBaseName(c, base, sizeof(base));
    dup[0] = 0;
    for (i = 0; i < MAX_CARDS; i++) {
        CARD *x = g_Cards[i];
        ULONG k;
        if (!x || x == c) continue;
        CardBaseName(x, other, sizeof(other));
        for (k = 0; base[k] && base[k] == other[k]; k++) ;
        if (base[k] == other[k]) { OsSprintf(dup, sizeof(dup), " #%u", c->Index + 1); break; }
    }
    if (kind == DEV_MIDIOUT) suf = c->Synth ? " Synth" : " MIDI";
    else if (kind == DEV_AUX) suf = " Volume";
    else if (kind == DEV_WAVEIN && (c->Type != HW_USB || c->Lines[LINE_WAVE].Present)) suf = " In";
    else if (kind == DEV_CTRL) suf = " Mixer";
    for (n = 0; suf[n]; n++) ;
    for (i = 0; dup[i]; i++) ;
    max = 31 - n - i;
    n = CopyCut(out, base, max);
    OsSprintf(out + n, sizeof(out) - n, "%s%s", suf, dup);
    return OsSprintf(buf, size, "%s", out);
}

static void FreeCard(CARD *c)
{
    if (!c) return;
    DeleteDevs(c);
    if (c->Type == HW_USB) UsbAudFreeCard(c);
    SynthDestroy(c);
    OsFreeDma(&c->PlayRing);
    OsFreeDma(&c->RecRing);
    OsFreeDma(&c->Aux);
    if (c->Mmio) OsUnmapMmio(c->Mmio, c->MmioSize);
    OsFree(c->Ac97);
    OsFree(c->Hda);
    OsFree(c);
}

/* 64-bit BARs placed above 4 GB by the firmware are moved below 4 GB */
#define RL_CFG32(b, d, f, o)     PciRead32(b, d, f, o)
#define RL_CFGW32(b, d, f, o, v) PciWrite32(b, d, f, o, v)
#define RL_MAP(pa, size)         OsMapMmio(pa, size)
#define RL_UNMAP(va, size)       OsUnmapMmio(va, size)
#define RL_LOG(...)              LOGI(__VA_ARGS__)
static UCHAR RlCmos(UCHAR reg)
{
    UCHAR v;
    ULONG fl = IntSave();
    outb(0x70, (UCHAR)(reg | 0x80));
    v = inb(0x71);
    outb(0x70, 0x0D);                   /* NMI back on, index to status D */
    IntRestore(fl);
    return v;
}
#define RL_CMOS(r)               RlCmos(r)
#include "pcireloc.c"

/* can the kernel map physical addresses above 4 GB?  x64 always; 32-bit Windows
   2000 / XP with the PAE kernel; NT 3.x / 4.0 never */
static BOOLEAN CanMapHigh(void)
{
#if defined(_WIN64)
    return TRUE;
#elif defined(AUDIONT_WDM)
    ULONG cr4;
    __asm__ __volatile__("movl %%cr4, %0" : "=r"(cr4));
    return (cr4 & 0x20) != 0;
#else
    return FALSE;
#endif
}

/* RelocateBars: 0 never, 1 only the sound controllers, 2 every device in the system
   when the kernel cannot reach it above 4 GB (default), 3 every device always */
void AudiontRelocateAll(void)
{
    ULONG mode = OsRegGetDword(L"RelocateBars", 2);
    if (mode < 2) return;
    if (mode == 2 && CanMapHigh()) {
        LOGI("pcireloc: the kernel maps above 4 GB (PAE / x64), devices stay where they are");
        return;
    }
    PciRelocateAll();
}

/* bring the function to D0 and make its DMA cache-coherent */
static void PciPrepare(CARD *c)
{
    ULONG pm = PciFindCap(c->Bus, c->Dev, c->Fn, 0x01);
    ULONG pe = PciFindCap(c->Bus, c->Dev, c->Fn, 0x10);
    ULONG i;
    if (pm) {
        USHORT csr = PciRead16(c->Bus, c->Dev, c->Fn, pm + 4);
        if (csr & 3) {
            /* D3hot -> D0 may reset the configuration space (No_Soft_Reset = 0):
               the BARs and the interrupt line read in D3 are written back */
            LOGI("card%u: controller in power state D%u, switching to D0", c->Index, csr & 3);
            PciWrite16(c->Bus, c->Dev, c->Fn, pm + 4, (USHORT)(csr & 0x7FFC));
            OsSleepMs(10);
            for (i = 0; i < 6; i++)
                if (PciRead32(c->Bus, c->Dev, c->Fn, 0x10 + i * 4) != c->Bar[i])
                    PciWrite32(c->Bus, c->Dev, c->Fn, 0x10 + i * 4, c->Bar[i]);
            if (c->Irq && PciRead8(c->Bus, c->Dev, c->Fn, 0x3C) != c->Irq)
                PciWrite8(c->Bus, c->Dev, c->Fn, 0x3C, c->Irq);
            csr = PciRead16(c->Bus, c->Dev, c->Fn, pm + 4);
            if (csr & 3) LOGE("card%u: still in D%u", c->Index, csr & 3);
        }
    }
    for (i = 0; i < 5; i++) {
        if ((c->Bar[i] & 7) == 4 && c->Bar[i + 1]) {
            ULONG nb;
            if (OsRegGetDword(L"RelocateBars", 1) && (nb = PciRelocateBar(c->Bus, c->Dev, c->Fn, 0x10 + i * 4)) != 0) {
                c->Bar[i] = nb; c->Bar[i + 1] = 0;
            } else LOGE("card%u: BAR%u at %08x%08x is above 4 GB and could not be moved", c->Index, i,
                        c->Bar[i + 1], c->Bar[i] & ~0xFUL);
        }
        if ((c->Bar[i] & 7) == 4) i++;
    }
    if (pe && !c->DmaUncached) {
        /* PCI Express: "Enable No Snoop" off, every DMA transfer snoops the CPU caches */
        USHORT dc = PciRead16(c->Bus, c->Dev, c->Fn, pe + 8);
        if (dc & 0x0800) {
            PciWrite16(c->Bus, c->Dev, c->Fn, pe + 8, (USHORT)(dc & ~0x0800));
            LOGV("card%u: PCIe no-snoop disabled (devctl %04x)", c->Index, dc);
        }
    }
}

/* tears a card down completely (its devices disappear) */
void CardDestroy(CARD *c)
{
    EngineStopAll(c);
    EngineShutdown(c);
    if (c->Ops->Shutdown) c->Ops->Shutdown(c);
    FreeCard(c);
}

/* the common end of a card's probe: controls, synthesizer, devices, interrupt / timer */
static CARD *CardFinish(CARD *c, ULONG slot)
{
    ULONG k;
    CtrlLoad(c);
    for (k = 0; k < LINE_COUNT; k++)
        if (c->Lines[k].Present && c->Lines[k].HwVolume && c->Ops->ApplyLine) c->Ops->ApplyLine(c, k);
    if (c->Ops->ApplyRecSrc && c->RecSrc) c->Ops->ApplyRecSrc(c);
    SynthCreate(c);
    CardCreateDevs(c);
    EngineStart(c);
    g_Cards[slot] = c;
    if (slot >= g_CardCount) g_CardCount = slot + 1;
    LOGI("card%u ready: codec \"%s\", waveout%u%s, midiout%u", c->Index, c->Codec,
         c->DevIndex[DEV_WAVEOUT], c->CanRecord ? ", wavein" : " (no record)",
         c->DevIndex[DEV_MIDIOUT]);
    return c;
}

/* the card in PCI function bus:dev.fn, NULL if it is not a usable sound controller;
   Int: the interrupt Plug and Play gave it (Windows 2000 / XP), NULL to find it from the IRQ line */
CARD *CardProbe(ULONG bus, ULONG dev, ULONG fn, const CARD_INT *Int)
{
    CARD *c;
    NTSTATUS st;
    ULONG slot;
    USHORT cmd, cmdOrig;
    for (slot = 0; slot < MAX_CARDS && g_Cards[slot]; slot++) ;
    if (slot >= MAX_CARDS) return NULL;
    c = OsAlloc(sizeof(CARD));
    if (!c) return NULL;
    if (Int) c->Int = *Int;
    c->Index = slot;
    c->Bus = bus; c->Dev = dev; c->Fn = fn;
    ReadPci(c);
    if (!LpeProbe(c) && !HdaProbe(c) && !Ac97Probe(c)) {
        LOGI("pci %u:%u.%u %04x:%04x class %06x: not a supported sound controller", bus, dev, fn, c->VendorId,
             c->DeviceId, PciRead32(bus, dev, fn, 8) >> 8);
        OsFree(c);
        return NULL;
    }
    LOGI("card%u: %s [%04x:%04x sub %04x:%04x rev %02x] at %u:%u.%u irq %u",
         c->Index, c->Desc, c->VendorId, c->DeviceId, c->SubVendor, c->SubDevice,
         c->Revision, bus, dev, fn, c->Irq);

    if (c->Type != HW_HDA) c->DmaUncached = OsRegGetDword(L"UncachedDma", 2) == 1;
    PciPrepare(c);
    cmdOrig = cmd = PciRead16(bus, dev, fn, 0x04);
    cmd |= 0x0007;        /* I/O, memory, bus master */
    cmd &= ~0x0400;       /* make sure INTx is not disabled */
    PciWrite16(bus, dev, fn, 0x04, cmd);

    CtrlInit(c);
    st = EngineInit(c);
    if (NT_SUCCESS(st)) st = c->Ops->Init(c);
    if (!NT_SUCCESS(st)) {
        LOGE("card%u: initialisation failed (%x)", c->Index, st);
        /* stop whatever DMA the back-end started before its buffers are freed, and give
           the function back as it was (e.g. the HDMI-only controller of a graphics card) */
        if (c->Ops->Shutdown) c->Ops->Shutdown(c);
        PciWrite16(bus, dev, fn, 0x04, cmdOrig);
        FreeCard(c);
        return NULL;
    }
    return CardFinish(c, slot);
}

/* an audio DSP that only ACPI describes (Bay Trail / Cherry Trail / Broadwell in ACPI mode) */
CARD *CardProbeAcpi(USHORT devId, ULONG mem0, ULONG mem1, BOOLEAN flag)
{
    CARD *c;
    NTSTATUS st;
    ULONG slot;
    for (slot = 0; slot < MAX_CARDS && g_Cards[slot]; slot++) ;
    if (slot >= MAX_CARDS) return NULL;
    c = OsAlloc(sizeof(CARD));
    if (!c) return NULL;
    c->Index = slot;
    c->Bus = 0xFF; c->Dev = 0xFF; c->Fn = 0xFF;
    c->VendorId = 0x8086; c->DeviceId = devId;
    c->Bar[0] = mem0; c->Bar[1] = mem1;
    c->HwFlags = flag ? 1 : 0;
    if (!LpeProbe(c)) { OsFree(c); return NULL; }
    LOGI("card%u: %s [ACPI, %04x] at %08x", c->Index, c->Desc, devId, mem0);
    CtrlInit(c);
    st = EngineInit(c);
    if (NT_SUCCESS(st)) st = c->Ops->Init(c);
    if (!NT_SUCCESS(st)) {
        LOGE("card%u: initialisation failed (%x)", c->Index, st);
        if (c->Ops->Shutdown) c->Ops->Shutdown(c);
        FreeCard(c);
        return NULL;
    }
    return CardFinish(c, slot);
}

/* Plug and Play took the function away: the card goes, its slot is free again */
void CardRemove(CARD *c)
{
    ULONG i;
    for (i = 0; i < MAX_CARDS; i++) if (g_Cards[i] == c) g_Cards[i] = NULL;
    EngineStopAll(c);
    CtrlSave(c);
    CardDestroy(c);
}

static void ScanPci(void)
{
    ULONG bus, dev, fn, maxbus = OsRegGetDword(L"MaxPciBus", 255);
    for (bus = 0; bus <= maxbus; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (fn = 0; fn < 8; fn++) {
                ULONG id = PciRead32(bus, dev, fn, 0);
                ULONG cls;
                if (id == 0xFFFFFFFF || id == 0) { if (fn == 0) break; continue; }
                cls = PciRead32(bus, dev, fn, 8) >> 8;
                LOGT("pci %u:%u.%u %08x class %06x", bus, dev, fn, id, cls);
                if ((cls >> 16) == 0x04 || id == 0x30581106 || id == 0x30591106)
                    CardProbe(bus, dev, fn, NULL);
                if (fn == 0 && !(PciRead8(bus, dev, 0, 0x0E) & 0x80)) break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
void AudiontUnloadAll(void)
{
    ULONG i;
    UsbAudShutdown();
    for (i = 0; i < g_CardCount; i++) {
        CARD *c = g_Cards[i];
        if (!c) continue;
        EngineStopAll(c);
        EngineShutdown(c);
        if (c->Ops->Shutdown) c->Ops->Shutdown(c);
        CtrlSave(c);
        FreeCard(c);
        g_Cards[i] = NULL;
    }
    g_CardCount = 0;
    LOGI("unloaded");
}

#ifndef AUDIONT_WDM
static VOID NTAPI DriverUnload(PDRIVER_OBJECT drv)
{
    (void)drv;
    AudiontUnloadAll();
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    BOOLEAN usb;
    AudiontSetDriver(drv);
    if (!NT_SUCCESS(OsInit(drv, reg))) return STATUS_UNSUCCESSFUL;
    drv->MajorFunction[IRP_MJ_CREATE] = AudiontDispatch;
    drv->MajorFunction[IRP_MJ_CLOSE] = AudiontDispatch;
    drv->MajorFunction[IRP_MJ_CLEANUP] = AudiontDispatch;
    drv->MajorFunction[IRP_MJ_READ] = AudiontDispatch;
    drv->MajorFunction[IRP_MJ_WRITE] = AudiontDispatch;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AudiontDispatch;
    drv->MajorFunction[IRP_MJ_SHUTDOWN] = AudiontDispatch;
    drv->DriverUnload = DriverUnload;

    g_StdNames = OsRegGetDword(L"StandardNames", 1) ? TRUE : FALSE;
    AudiontRelocateAll();                    /* every BAR above 4 GB goes below 4 GB */
    ScanPci();
    LpeScanAcpi();                           /* Bay Trail / Cherry Trail / Broadwell DSPs in ACPI mode */
    /* USB audio devices come and go through USBNT; stay loaded for them */
    usb = OsRegGetDword(L"UsbAudio", 1) ? UsbAudInit() : FALSE;
    if (!g_CardCount && !usb) {
        LOGI("no supported AC'97 / HD Audio controller found");
        return STATUS_NO_SUCH_DEVICE;
    }
    return STATUS_SUCCESS;
}
#endif /* !AUDIONT_WDM */
