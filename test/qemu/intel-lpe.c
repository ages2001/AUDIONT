/*
 * Intel LPE audio DSP (Bay Trail / Cherry Trail / Broadwell) as a PCI
 * function ("PCI mode"), running a pretend Sound Open Firmware (IPC3), for
 * driver tests (AUDIONT lpe.c):
 *
 *   -device intel-lpe,gen=byt|cht|bdw,audiodev=snd0[,debug=on]
 *
 * BAR0 is the DSP BAR (IRAM / DRAM / shim / mailbox / DMA controllers),
 * BAR1 the PCI configuration shadow (Broadwell).  Unstalling the DSP with
 * firmware in IRAM sends FW_READY with the mailbox windows; topology,
 * DAI_CONFIG, PCM_PARAMS (the host ring by its page table) and triggers are
 * answered.  A running playback stream is read from the ring at 48 kHz and
 * played; a capture stream gets a 1 kHz tone.  The host-side address of the
 * transfer shows in SAR (playback, DMAC0 channel 0) / DAR (capture, channel
 * 1); the stream position goes to the stream window with a POSITION message
 * every period.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "hw/audio/soundhw.h"
#include "audio/audio.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "sysemu/dma.h"
#include "exec/address-spaces.h"
#include <math.h>

#define TYPE_INTEL_LPE "intel-lpe"
OBJECT_DECLARE_SIMPLE_TYPE(IntelLpe, INTEL_LPE)

#define MBOX_UP    0x000            /* DSP -> host, from the mailbox base */
#define MBOX_DOWN  0x400            /* host -> DSP */
#define MBOX_STRM  0x800            /* stream positions */
#define FIFO       16384

typedef struct {
    bool run, params;
    uint32_t comp;
    uint64_t pt, size, pos, posn;
    uint32_t pages, period, since;
    uint64_t pfn[64];
} LpeStream;

struct IntelLpe {
    PCIDevice parent_obj;
    MemoryRegion bar0, bar1;
    char *gen_str;
    bool debug;
    uint32_t acpi_base;         /* "ACPI mode": the PCI function is not an audio device, the DSP BAR is at
                                   acpi_base and an NVS page at acpi_base + BAR size holds its address */
    MemoryRegion alias, nvs;
    uint8_t nvsmem[4096];
    QEMUSoundCard card;
    bool bdw, cht;
    uint8_t *mem;
    uint32_t size, shim, mbox, iram, dmac0;
    uint8_t pcicfg[0x1000];
    bool booted;
    uint32_t host_comp[2];
    LpeStream st[2];
    QEMUTimer *tick;
    SWVoiceOut *out;
    uint8_t fifo[FIFO];
    uint32_t fifo_rd, fifo_len;
    double ph[2];
    uint32_t msgs;
};

static void lpe_log(IntelLpe *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void lpe_log(IntelLpe *s, const char *fmt, ...)
{
    va_list ap;
    if (!s->debug) {
        return;
    }
    va_start(ap, fmt);
    fprintf(stderr, "intel-lpe: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static uint32_t rd32(IntelLpe *s, uint32_t o) { return o + 4 <= s->size ? ldl_le_p(s->mem + o) : 0; }
static void wr32(IntelLpe *s, uint32_t o, uint32_t v) { if (o + 4 <= s->size) stl_le_p(s->mem + o, v); }

/* IPC bits: Atom 64-bit registers (BUSY 63, DONE 62 -> high dword 31 / 30), Broadwell 32-bit */
static uint32_t ipcx_hi(IntelLpe *s) { return s->shim + 0x38 + (s->bdw ? 0 : 4); }
static uint32_t ipcd_hi(IntelLpe *s) { return s->shim + 0x40 + (s->bdw ? 0 : 4); }

static void dsp_send(IntelLpe *s, const void *msg, uint32_t len)
{
    memcpy(s->mem + s->mbox + MBOX_UP, msg, len);
    wr32(s, ipcd_hi(s), (rd32(s, ipcd_hi(s)) & ~(1u << 30)) | (1u << 31));
}

static void fw_ready(IntelLpe *s)
{
    uint8_t r[108 + 16 + 3 * 24];
    uint32_t o = 108;
    memset(r, 0, sizeof(r));
    stl_le_p(r, 108); stl_le_p(r + 4, 0x70000000);
    stw_le_p(r + 28, 2); stw_le_p(r + 30, 2);
    stl_le_p(r + 64, (3u << 24) | (23u << 12));
    stl_le_p(r + o, 16 + 3 * 24); stl_le_p(r + o + 4, 0x70000000); stl_le_p(r + o + 8, 1); stl_le_p(r + o + 12, 3);
    /* windows: hdr, type, id, flags, size, offset (from the mailbox) */
    stl_le_p(r + o + 16, 24); stl_le_p(r + o + 20, 0); stl_le_p(r + o + 32, 0x400); stl_le_p(r + o + 36, MBOX_DOWN);
    stl_le_p(r + o + 40, 24); stl_le_p(r + o + 44, 1); stl_le_p(r + o + 56, 0x400); stl_le_p(r + o + 60, MBOX_UP);
    stl_le_p(r + o + 64, 24); stl_le_p(r + o + 68, 4); stl_le_p(r + o + 80, 0x200); stl_le_p(r + o + 84, MBOX_STRM);
    dsp_send(s, r, sizeof(r));
    s->booted = true;
    lpe_log(s, "FW_READY (IRAM %08x %08x, DRAM %08x)", rd32(s, s->iram), rd32(s, s->iram + 0x400),
            rd32(s, s->bdw ? 0x10 : 0x100010));
}

/* in "ACPI mode" the function is not a bus master: DMA goes straight to memory */
static void lpe_dma(IntelLpe *s, uint64_t a, void *buf, uint32_t n, bool write)
{
    if (s->acpi_base) {
        address_space_rw(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED, buf, n, write);
    } else if (write) {
        pci_dma_write(PCI_DEVICE(s), a, buf, n);
    } else {
        pci_dma_read(PCI_DEVICE(s), a, buf, n);
    }
}

static void decode_pt(IntelLpe *s, LpeStream *st)
{
    uint8_t t[200];
    uint32_t i;
    lpe_dma(s, st->pt, t, sizeof(t), false);
    for (i = 0; i < st->pages && i < 64; i++) {
        uint32_t idx = (i * 5) >> 1, v = ldl_le_p(t + idx);
        st->pfn[i] = (i & 1) ? (v >> 4) & 0xFFFFF : v & 0xFFFFF;
    }
}

static uint64_t ring_addr(LpeStream *st, uint64_t off)
{
    return (st->pfn[(off >> 12) % 64] << 12) + (off & 0xFFF);
}

static void ipc_cmd(IntelLpe *s)
{
    uint32_t box = s->mbox + MBOX_DOWN, cmd = rd32(s, box + 4), id = rd32(s, box + 8), rsize = 12, d;
    s->msgs++;
    switch (cmd) {
    case 0x30010000: {                            /* COMP_NEW */
        uint32_t type = rd32(s, box + 12), dir = rd32(s, box + 64);
        if (type == 1 && dir < 2) {
            s->host_comp[dir] = id;
        }
        lpe_log(s, "comp %u type %u dir %u", id, type, dir);
        break;
    }
    case 0x80010000:                              /* DAI_CONFIG */
        lpe_log(s, "dai config type %u index %u: mclk %u bclk %u slot %u valid %u", rd32(s, box + 8),
                rd32(s, box + 12), rd32(s, box + 52 + 8), rd32(s, box + 52 + 16), lduw_le_p(s->mem + box + 52 + 36),
                rd32(s, box + 52 + 32));
        break;
    case 0x60010000: {                            /* PCM_PARAMS */
        uint32_t q = box + 24;
        d = rd32(s, q + 32) & 1;
        s->st[d].comp = id;
        s->st[d].pt = rd32(s, q + 8);
        s->st[d].pages = rd32(s, q + 12);
        s->st[d].size = rd32(s, q + 16);
        s->st[d].period = rd32(s, q + 56) ? rd32(s, q + 56) : 2048;
        s->st[d].params = true;
        s->st[d].pos = s->st[d].posn = 0;
        decode_pt(s, &s->st[d]);
        wr32(s, box + 12, id);
        wr32(s, box + 16, d * 0x80);              /* posn_offset in the stream window */
        rsize = 20;
        lpe_log(s, "pcm params comp %u dir %u: page table %08" PRIx64 " %u pages, %" PRIu64 " bytes, first page %"
                PRIx64 ", period %u, no position %u", id, d, s->st[d].pt, s->st[d].pages, s->st[d].size,
                s->st[d].pfn[0] << 12, s->st[d].period, lduw_le_p(s->mem + q + 60));
        break;
    }
    case 0x60040000: case 0x60050000:
        for (d = 0; d < 2; d++) {
            if (s->st[d].comp == id && s->st[d].params) {
                s->st[d].run = cmd == 0x60040000;
            }
        }
        lpe_log(s, "trigger %s comp %u", cmd == 0x60040000 ? "start" : "stop", id);
        break;
    case 0x60030000:
        for (d = 0; d < 2; d++) {
            if (s->st[d].comp == id) {
                s->st[d].params = s->st[d].run = false;
            }
        }
        break;
    default:
        break;
    }
    wr32(s, box, rsize);
    wr32(s, box + 4, cmd);
    wr32(s, box + 8, 0);
    /* reply: BUSY off, DONE on */
    wr32(s, ipcx_hi(s), (rd32(s, ipcx_hi(s)) & ~(1u << 31)) | (1u << 30));
}

static void out_cb(void *opaque, int avail)
{
    IntelLpe *s = opaque;
    uint8_t buf[4096];
    while (avail > 0 && s->fifo_len) {
        int n = MIN(MIN(avail, (int)sizeof(buf)), (int)s->fifo_len), i;
        for (i = 0; i < n; i++) {
            buf[i] = s->fifo[(s->fifo_rd + i) % FIFO];
        }
        n = AUD_write(s->out, buf, n);
        if (!n) {
            break;
        }
        s->fifo_rd = (s->fifo_rd + n) % FIFO;
        s->fifo_len -= n;
        avail -= n;
    }
}

/* ring access split at the end of the ring and at page ends */
static void ring_rw(IntelLpe *s, LpeStream *st, uint8_t *buf, uint32_t n, bool write)
{
    uint64_t off = st->pos;
    while (n) {
        uint32_t c = MIN(n, 4096 - (uint32_t)(off & 0xFFF));
        c = MIN(c, (uint32_t)(st->size - off));
        lpe_dma(s, ring_addr(st, off), buf, c, write);
        buf += c;
        n -= c;
        off = (off + c) % st->size;
    }
}

static void stream_tick(IntelLpe *s, int d)
{
    LpeStream *st = &s->st[d];
    uint8_t buf[192];
    uint32_t n = sizeof(buf), i;
    uint64_t posn_at = s->mbox + MBOX_STRM + d * 0x80;
    if (!st->run || !st->size) {
        return;
    }
    if (d == 0) {
        ring_rw(s, st, buf, n, false);
        if (s->fifo_len + n <= FIFO) {
            for (i = 0; i < n; i++) {
                s->fifo[(s->fifo_rd + s->fifo_len + i) % FIFO] = buf[i];
            }
            s->fifo_len += n;
        }
    } else {
        int16_t *p = (int16_t *)buf;
        for (i = 0; i < 48; i++) {
            p[2 * i] = (int16_t)(sin(s->ph[0]) * 12000);
            p[2 * i + 1] = (int16_t)(sin(s->ph[1]) * 12000);
            s->ph[0] += 2 * M_PI * 1000.0 / 48000.0;
            s->ph[1] += 2 * M_PI * 1500.0 / 48000.0;
            if (s->ph[0] > 2 * M_PI) s->ph[0] -= 2 * M_PI;
            if (s->ph[1] > 2 * M_PI) s->ph[1] -= 2 * M_PI;
        }
        ring_rw(s, st, buf, n, true);
    }
    st->pos = (st->pos + n) % st->size;
    st->posn += n;
    /* DMA controller registers: SAR of channel 0 (playback), DAR of channel 1 (capture) */
    wr32(s, s->dmac0 + (d ? 0x58 + 0x08 : 0), (uint32_t)ring_addr(st, st->pos));
    /* position: sof_ipc_stream_posn.host_posn, a message every period */
    stl_le_p(s->mem + posn_at + 28, (uint32_t)st->posn);
    stl_le_p(s->mem + posn_at + 32, (uint32_t)(st->posn >> 32));
    st->since += n;
    if (st->since >= st->period && !(rd32(s, ipcd_hi(s)) & (1u << 31))) {
        uint8_t m[12 + 64];
        memset(m, 0, sizeof(m));
        stl_le_p(m, sizeof(m)); stl_le_p(m + 4, 0x60080000 | st->comp);
        stl_le_p(m + 12, st->comp);
        dsp_send(s, m, sizeof(m));
        st->since = 0;
    }
}

static void lpe_tick(void *opaque)
{
    IntelLpe *s = opaque;
    stream_tick(s, 0);
    stream_tick(s, 1);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
}

static uint64_t bar0_read(void *opaque, hwaddr a, unsigned size)
{
    IntelLpe *s = opaque;
    uint64_t v = 0;
    if (a + size <= s->size) {
        memcpy(&v, s->mem + a, size);
    }
    return v;
}

static void bar0_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IntelLpe *s = opaque;
    uint32_t a = addr & ~3u, old, v;
    if (addr + size > s->size) {
        return;
    }
    old = rd32(s, a);
    memcpy(s->mem + addr, &val, size);
    v = rd32(s, a);
    if (a == s->shim) {                           /* CSR */
        bool stall = s->bdw ? (v & 0x400) : (v & 4), rst = s->bdw ? (v & 2) : (v & 1);
        bool was = s->bdw ? (old & 0x400) : (old & 4);
        if (was && !stall && !rst) {
            if (rd32(s, s->iram) || rd32(s, s->iram + 0x400)) {
                fw_ready(s);
            } else {
                lpe_log(s, "unstalled without firmware in IRAM");
            }
        }
        if (rst) {
            s->booted = false;
        }
    } else if (a == ipcx_hi(s) && (v & (1u << 31)) && !(old & (1u << 31)) && s->booted) {
        ipc_cmd(s);
    }
}

static const MemoryRegionOps bar0_ops = {
    .read = bar0_read, .write = bar0_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t bar1_read(void *opaque, hwaddr a, unsigned size)
{
    IntelLpe *s = opaque;
    uint64_t v = 0;
    memcpy(&v, s->pcicfg + a, size);
    if (a == 0x84) {
        v &= ~3ull;                               /* PMCS: D0 at once */
    }
    return v;
}

static void bar1_write(void *opaque, hwaddr a, uint64_t val, unsigned size)
{
    IntelLpe *s = opaque;
    memcpy(s->pcicfg + a, &val, size);
}

static const MemoryRegionOps bar1_ops = {
    .read = bar1_read, .write = bar1_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpe_realize(PCIDevice *pd, Error **errp)
{
    IntelLpe *s = INTEL_LPE(pd);
    struct audsettings as = { .freq = 48000, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .endianness = 0 };
    const char *g = s->gen_str ? s->gen_str : "byt";
    uint16_t dev;
    if (!strcmp(g, "bdw")) {
        s->bdw = true; dev = 0x9CB6; s->size = 0x100000; s->shim = 0xFB000; s->mbox = 0x9E000; s->iram = 0xA0000;
        s->dmac0 = 0xFE000;
    } else {
        s->cht = !strcmp(g, "cht");
        dev = s->cht ? 0x22A8 : 0x0F28;
        s->size = 0x200000; s->shim = 0x140000; s->mbox = 0x144000; s->iram = 0xC0000; s->dmac0 = 0x98000;
    }
    pci_set_word(pd->config + PCI_DEVICE_ID, s->acpi_base ? 0xFFFE : dev);
    if (s->acpi_base) {
        pci_config_set_class(pd->config, 0xFF00);
    }
    pci_config_set_interrupt_pin(pd->config, 1);
    s->mem = g_malloc0(s->size);
    memory_region_init_io(&s->bar0, OBJECT(s), &bar0_ops, s, "intel-lpe-dsp", s->size);
    pci_register_bar(pd, 0, 0, &s->bar0);
    memory_region_init_io(&s->bar1, OBJECT(s), &bar1_ops, s, "intel-lpe-pci", 0x1000);
    pci_register_bar(pd, 1, 0, &s->bar1);
    if (s->acpi_base) {
        memory_region_init_alias(&s->alias, OBJECT(s), "intel-lpe-acpi", &s->bar0, 0, s->size);
        memory_region_add_subregion(get_system_memory(), s->acpi_base, &s->alias);
        stl_le_p(s->nvsmem + 0x10, s->acpi_base);
        stl_le_p(s->nvsmem + 0x14, 0);
        memory_region_init_ram_ptr(&s->nvs, OBJECT(s), "intel-lpe-nvs", 4096, s->nvsmem);
        memory_region_add_subregion(get_system_memory(), s->acpi_base + s->size, &s->nvs);
    }
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, lpe_tick, s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
    if (AUD_register_card("intel-lpe", &s->card, errp)) {
        s->out = AUD_open_out(&s->card, NULL, "intel-lpe.out", s, out_cb, &as);
        if (s->out) {
            AUD_set_active_out(s->out, true);
        }
    }
}

static Property lpe_props[] = {
    DEFINE_AUDIO_PROPERTIES(IntelLpe, card),
    DEFINE_PROP_STRING("gen", IntelLpe, gen_str),
    DEFINE_PROP_BOOL("debug", IntelLpe, debug, false),
    DEFINE_PROP_UINT32("acpi-base", IntelLpe, acpi_base, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void lpe_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    k->realize = lpe_realize;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = 0x0F28;
    k->class_id = 0x0401;
    device_class_set_props(dc, lpe_props);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo lpe_info = {
    .name = TYPE_INTEL_LPE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IntelLpe),
    .class_init = lpe_class_init,
    .interfaces = (InterfaceInfo[]) { { INTERFACE_CONVENTIONAL_PCI_DEVICE }, { } },
};

static void lpe_register(void)
{
    type_register_static(&lpe_info);
}

type_init(lpe_register)
