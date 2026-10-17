/*
 * I2C register file: a stand-in for audio codecs / amplifiers in driver tests
 * (AUDIONT).  Register addresses and values are big endian on the wire, with
 * auto increment; registers that were never written read as their preset
 * (the "regs" property) or 0.
 *
 *   -device i2c-regfile,bus=i2c0.0,address=0x1a,reg-bytes=2,val-bytes=2,
 *           regs=0xff:0x6530;0x00:0x0000[,name=rt5682][,trace=on]
 *
 * "writes" counts the register writes, "reads" the reads; trace=on logs them
 * to stderr.  A register can be made to clear bits after a write with
 * "selfclear=reg:mask;..." (busy / calibration bits the driver polls).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_I2C_REGFILE "i2c-regfile"
OBJECT_DECLARE_SIMPLE_TYPE(I2cRegfile, I2C_REGFILE)

#define MAXREG 512

struct I2cRegfile {
    I2CSlave parent_obj;
    uint32_t reg_bytes, val_bytes;
    char *regs_str, *selfclear_str, *fixed_str, *name;
    bool trace;
    uint32_t writes, reads;
    /* sparse register file */
    uint32_t addr[MAXREG], val[MAXREG], n;
    uint32_t sc_reg[16], sc_mask[16], nsc;
    uint32_t fx_reg[16], fx_val[16], nfx;   /* "fixed=reg:val;...": writes do not change these */
    /* transfer state */
    uint32_t cur, acc;
    int nbytes;         /* bytes of the register address or value received */
    bool have_reg;
    int rd_pos;
};

static uint32_t *slot(I2cRegfile *s, uint32_t reg, bool create)
{
    uint32_t i;
    for (i = 0; i < s->n; i++) {
        if (s->addr[i] == reg) {
            return &s->val[i];
        }
    }
    if (!create || s->n == MAXREG) {
        return NULL;
    }
    s->addr[s->n] = reg;
    s->val[s->n] = 0;
    return &s->val[s->n++];
}

static uint32_t rd(I2cRegfile *s, uint32_t reg)
{
    uint32_t *p = slot(s, reg, false);
    return p ? *p : 0;
}

static void wr(I2cRegfile *s, uint32_t reg, uint32_t v)
{
    uint32_t i, *p = slot(s, reg, true);
    for (i = 0; i < s->nsc; i++) {
        if (s->sc_reg[i] == reg) {
            v &= ~s->sc_mask[i];
        }
    }
    for (i = 0; i < s->nfx; i++) {
        if (s->fx_reg[i] == reg) {
            v = s->fx_val[i];
        }
    }
    if (p) {
        *p = v;
    }
    s->writes++;
    if (s->trace) {
        fprintf(stderr, "i2c-regfile %s: W %0*x = %0*x\n", s->name ? s->name : "", s->reg_bytes * 2, reg,
                s->val_bytes * 2, v);
    }
}

static int rf_event(I2CSlave *i2c, enum i2c_event ev)
{
    I2cRegfile *s = I2C_REGFILE(i2c);
    switch (ev) {
    case I2C_START_SEND:
        s->have_reg = false;
        s->nbytes = 0;
        s->acc = 0;
        break;
    case I2C_START_RECV:
        s->rd_pos = 0;
        break;
    default:
        break;
    }
    return 0;
}

static int rf_send(I2CSlave *i2c, uint8_t data)
{
    I2cRegfile *s = I2C_REGFILE(i2c);
    s->acc = (s->acc << 8) | data;
    if (++s->nbytes < (int)(s->have_reg ? s->val_bytes : s->reg_bytes)) {
        return 0;
    }
    if (!s->have_reg) {
        s->cur = s->acc;
        s->have_reg = true;
    } else {
        wr(s, s->cur, s->acc);
        s->cur++;
    }
    s->acc = 0;
    s->nbytes = 0;
    return 0;
}

static uint8_t rf_recv(I2CSlave *i2c)
{
    I2cRegfile *s = I2C_REGFILE(i2c);
    uint32_t v = rd(s, s->cur);
    uint8_t b = (uint8_t)(v >> (8 * (s->val_bytes - 1 - s->rd_pos)));
    if (s->rd_pos == 0) {
        s->reads++;
        if (s->trace) {
            fprintf(stderr, "i2c-regfile %s: R %0*x -> %0*x\n", s->name ? s->name : "", s->reg_bytes * 2, s->cur,
                    s->val_bytes * 2, v);
        }
    }
    if (++s->rd_pos == (int)s->val_bytes) {
        s->rd_pos = 0;
        s->cur++;
    }
    return b;
}

static void parse_pairs(const char *str, uint32_t *a, uint32_t *b, uint32_t max, uint32_t *n, Error **errp)
{
    const char *p = str;
    *n = 0;
    while (p && *p && *n < max) {
        char *end;
        a[*n] = strtoul(p, &end, 0);
        if (*end != ':') {
            error_setg(errp, "i2c-regfile: bad pair at '%s'", p);
            return;
        }
        b[*n] = strtoul(end + 1, &end, 0);
        (*n)++;
        p = *end == ';' ? end + 1 : NULL;
    }
}

static void rf_realize(DeviceState *dev, Error **errp)
{
    I2cRegfile *s = I2C_REGFILE(dev);
    if (s->reg_bytes < 1 || s->reg_bytes > 4 || s->val_bytes < 1 || s->val_bytes > 4) {
        error_setg(errp, "i2c-regfile: reg-bytes / val-bytes must be 1..4");
        return;
    }
    if (s->regs_str) {
        parse_pairs(s->regs_str, s->addr, s->val, MAXREG, &s->n, errp);
    }
    if (s->fixed_str) {
        parse_pairs(s->fixed_str, s->fx_reg, s->fx_val, 16, &s->nfx, errp);
    }
    if (s->selfclear_str) {
        parse_pairs(s->selfclear_str, s->sc_reg, s->sc_mask, 16, &s->nsc, errp);
    }
}

static Property rf_props[] = {
    DEFINE_PROP_UINT32("reg-bytes", I2cRegfile, reg_bytes, 1),
    DEFINE_PROP_UINT32("val-bytes", I2cRegfile, val_bytes, 1),
    DEFINE_PROP_STRING("regs", I2cRegfile, regs_str),
    DEFINE_PROP_STRING("selfclear", I2cRegfile, selfclear_str),
    DEFINE_PROP_STRING("fixed", I2cRegfile, fixed_str),
    DEFINE_PROP_STRING("name", I2cRegfile, name),
    DEFINE_PROP_BOOL("trace", I2cRegfile, trace, false),
    DEFINE_PROP_UINT32("writes", I2cRegfile, writes, 0),
    DEFINE_PROP_UINT32("reads", I2cRegfile, reads, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void rf_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    dc->realize = rf_realize;
    device_class_set_props(dc, rf_props);
    k->event = rf_event;
    k->recv = rf_recv;
    k->send = rf_send;
}

static const TypeInfo rf_info = {
    .name = TYPE_I2C_REGFILE,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(I2cRegfile),
    .class_init = rf_class_init,
};

static void rf_register(void)
{
    type_register_static(&rf_info);
}

type_init(rf_register)
