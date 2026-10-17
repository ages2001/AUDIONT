/*
 * Synopsys DesignWare I2C master as an Intel LPSS PCI function (test model
 * for the USBNT HID over I2C driver).
 *
 * Transfers are carried out when a command is written to IC_DATA_CMD: a
 * START (or repeated START) with the target address, then the byte written
 * or read; a STOP with bit 9 or when the TX FIFO runs empty without the
 * hold feature. NACKs set TX_ABRT with the abort source; the TX FIFO is
 * flushed and further commands are ignored until IC_CLR_TX_ABRT is read.
 * The LPSS private registers (resets at 0x204) gate the controller: while
 * the function is in reset the DesignWare registers read 0.
 *
 * -device dw-i2c-pci,id=i2c0[,device-id=0x9d61][,fifo=64][,keep-in-reset=on]
 * the bus is "i2c0.0"
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "qemu/fifo8.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qom/object.h"

#define TYPE_DW_I2C_PCI "dw-i2c-pci"
OBJECT_DECLARE_SIMPLE_TYPE(DwI2cPci, DW_I2C_PCI)

#define FIFO_MAX 256

struct DwI2cPci {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    I2CBus *bus;

    uint32_t con, tar, ss_h, ss_l, fs_h, fs_l, sda_hold, intr_mask, raw, abrt_src;
    uint32_t enable, rx_tl, tx_tl;
    uint32_t resets;
    uint16_t device_id;
    uint32_t fifo;
    bool in_reset_default;
    uint8_t rx[FIFO_MAX];
    uint32_t rx_n;
    bool active;        /* a transfer is open on the bus */
    bool reading;
    bool aborted;
    uint32_t xfers, naks;
};

#define R_INTR_TX_ABRT  (1 << 6)
#define R_INTR_ACTIVITY (1 << 8)
#define R_INTR_STOP_DET (1 << 9)
#define R_INTR_START    (1 << 10)
#define R_INTR_TX_EMPTY (1 << 4)
#define R_INTR_RX_FULL  (1 << 2)

static bool lpss_on(DwI2cPci *s)
{
    return (s->resets & 3) == 3;
}

static void dw_stop(DwI2cPci *s)
{
    if (s->active) {
        i2c_end_transfer(s->bus);
        s->active = false;
    }
    s->raw |= R_INTR_STOP_DET;
}

static void dw_abort(DwI2cPci *s, uint32_t src)
{
    if (s->active) {
        i2c_end_transfer(s->bus);
        s->active = false;
    }
    s->abrt_src = src;
    s->raw |= R_INTR_TX_ABRT | R_INTR_STOP_DET;
    s->aborted = true;
    s->naks++;
}

static void dw_cmd(DwI2cPci *s, uint32_t v)
{
    bool rd = v & 0x100;
    bool stop = v & 0x200;
    bool restart = v & 0x400;

    if (!(s->enable & 1) || s->aborted) {
        return;
    }
    if (s->active && (restart || rd != s->reading)) {
        /* repeated start: the bus keeps the device, it sees a new start */
        if (!(s->con & 0x20)) {
            i2c_end_transfer(s->bus);
            s->active = false;
        }
    }
    if (!s->active || restart || rd != s->reading) {
        if (i2c_start_transfer(s->bus, s->tar & 0x7f, rd)) {
            s->active = false;
            dw_abort(s, 1);                 /* ABRT_7B_ADDR_NOACK */
            return;
        }
        s->active = true;
        s->reading = rd;
        s->raw |= R_INTR_START | R_INTR_ACTIVITY;
        s->xfers++;
    }
    if (rd) {
        uint8_t b = i2c_recv(s->bus);
        if (s->rx_n < s->fifo) {
            s->rx[s->rx_n++] = b;
        } else {
            s->raw |= 1 << 1;               /* RX_OVER */
        }
    } else if (i2c_send(s->bus, v & 0xff)) {
        dw_abort(s, 1 << 3);                /* ABRT_TXDATA_NOACK */
        return;
    }
    if (stop) {
        if (rd) {
            i2c_nack(s->bus);
        }
        dw_stop(s);
    }
}

static uint64_t dw_read(void *opaque, hwaddr addr, unsigned size)
{
    DwI2cPci *s = opaque;
    uint32_t v = 0;

    if (addr >= 0x200 && addr < 0x300) {
        switch (addr) {
        case 0x204: return s->resets;
        case 0x2fc: return 0;               /* capabilities: I2C */
        default: return 0;
        }
    }
    if (!lpss_on(s)) {
        return 0;
    }
    switch (addr) {
    case 0x00: v = s->con; break;
    case 0x04: v = s->tar; break;
    case 0x10:
        if (s->rx_n) {
            v = s->rx[0];
            memmove(s->rx, s->rx + 1, --s->rx_n);
        }
        break;
    case 0x14: v = s->ss_h; break;
    case 0x18: v = s->ss_l; break;
    case 0x1c: v = s->fs_h; break;
    case 0x20: v = s->fs_l; break;
    case 0x2c: v = s->raw & s->intr_mask; break;
    case 0x30: v = s->intr_mask; break;
    case 0x34:
        v = s->raw | R_INTR_TX_EMPTY | (s->rx_n > s->rx_tl ? R_INTR_RX_FULL : 0);
        break;
    case 0x38: v = s->rx_tl; break;
    case 0x3c: v = s->tx_tl; break;
    case 0x40: v = 0; s->raw = 0; s->aborted = false; break;
    case 0x54: v = 0; s->raw &= ~R_INTR_TX_ABRT; s->aborted = false; break;
    case 0x5c: s->raw &= ~R_INTR_ACTIVITY; break;
    case 0x60: s->raw &= ~R_INTR_STOP_DET; break;
    case 0x64: s->raw &= ~R_INTR_START; break;
    case 0x44: case 0x48: case 0x4c: case 0x50: case 0x58: case 0x68: case 0xa8: break;
    case 0x6c: v = s->enable; break;
    case 0x70:                              /* status: TFNF, TFE, RFNE */
        v = 0x2 | 0x4 | (s->rx_n ? 0x8 : 0) | (s->rx_n >= s->fifo ? 0x10 : 0) |
            (s->active ? 0x21 : 0);
        break;
    case 0x74: v = 0; break;
    case 0x78: v = s->rx_n; break;
    case 0x7c: v = s->sda_hold; break;
    case 0x80: v = s->abrt_src; break;
    case 0x9c: v = s->enable & 1; break;
    case 0xf4: v = ((s->fifo - 1) << 16) | ((s->fifo - 1) << 8) | (2 << 2) | 2; break;
    case 0xf8: v = 0x3132302a; break;       /* "120*" */
    case 0xfc: v = 0x44570140; break;
    default: break;
    }
    return v;
}

static void dw_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    DwI2cPci *s = opaque;
    uint32_t v = val;

    if (addr >= 0x200 && addr < 0x300) {
        if (addr == 0x204) {
            s->resets = v & 7;
            if (!lpss_on(s)) {
                s->enable = 0;
                s->raw = 0;
                s->rx_n = 0;
                s->con = 0x7f;
                if (s->active) {
                    i2c_end_transfer(s->bus);
                    s->active = false;
                }
            }
        }
        return;
    }
    if (!lpss_on(s)) {
        return;
    }
    switch (addr) {
    case 0x00: if (!(s->enable & 1)) s->con = v; break;
    case 0x04: if (!(s->enable & 1)) s->tar = v; break;
    case 0x10: dw_cmd(s, v); break;
    case 0x14: s->ss_h = v; break;
    case 0x18: s->ss_l = v; break;
    case 0x1c: s->fs_h = v; break;
    case 0x20: s->fs_l = v; break;
    case 0x30: s->intr_mask = v; break;
    case 0x38: s->rx_tl = v; break;
    case 0x3c: s->tx_tl = v; break;
    case 0x6c:
        if ((v & 2) || !(v & 1)) {
            if (s->active) {
                dw_stop(s);
            }
            if (!(v & 1)) {
                s->rx_n = 0;
                s->aborted = false;
            }
        }
        s->enable = v & 1;
        break;
    case 0x7c: s->sda_hold = v; break;
    default: break;
    }
}

static const MemoryRegionOps dw_ops = {
    .read = dw_read,
    .write = dw_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dw_reset(DeviceState *d)
{
    DwI2cPci *s = DW_I2C_PCI(d);

    s->resets = s->in_reset_default ? 0 : 7;
    s->con = 0x7f;
    s->ss_h = s->ss_l = s->fs_h = s->fs_l = 0;
    s->enable = 0;
    s->rx_n = 0;
    s->raw = 0;
    s->aborted = false;
    s->active = false;
}

static void dw_realize(PCIDevice *pd, Error **errp)
{
    DwI2cPci *s = DW_I2C_PCI(pd);

    if (s->fifo < 2 || s->fifo > FIFO_MAX) {
        s->fifo = 64;
    }
    pci_config_set_device_id(pd->config, s->device_id);
    memory_region_init_io(&s->mmio, OBJECT(s), &dw_ops, s, "dw-i2c", 0x1000);
    pci_register_bar(pd, 0, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, &s->mmio);
    s->bus = i2c_init_bus(DEVICE(s), NULL);
}

static Property dw_props[] = {
    DEFINE_PROP_UINT16("device-id", DwI2cPci, device_id, 0x9d61),
    DEFINE_PROP_UINT32("fifo", DwI2cPci, fifo, 64),
    DEFINE_PROP_BOOL("keep-in-reset", DwI2cPci, in_reset_default, true),
    DEFINE_PROP_UINT32("xfers", DwI2cPci, xfers, 0),
    DEFINE_PROP_UINT32("naks", DwI2cPci, naks, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void dw_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = dw_realize;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = 0x9d61;
    k->class_id = 0x0c80;
    dc->reset = dw_reset;
    dc->desc = "DesignWare I2C (Intel LPSS)";
    device_class_set_props(dc, dw_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo dw_info = {
    .name = TYPE_DW_I2C_PCI,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(DwI2cPci),
    .class_init = dw_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void dw_register(void)
{
    type_register_static(&dw_info);
}

type_init(dw_register)
