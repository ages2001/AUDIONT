/*
 * Intel Smart Sound (cAVS / ACE) audio DSP model for the intel-hda device.
 * Test vehicle for drivers: it implements the host-visible protocol, not the
 * DSP:
 *   - HD Audio extended capabilities: SPB, processing pipe (PPCTL, link DMA
 *     PPLCx), multi-link (HDA link; DMIC and SSP extended links on ACE),
 *     vendor registers (EM2, LTRP, SDxDPIB)
 *   - BAR4: core power / reset / stall (ADSPCS) or ACE (HFDSSCS, HFPWRCTL,
 *     DSP2CxCTL), the ROM handshake (ROM_CONTROL purge + DMA id), code loading
 *     through a decoupled host output stream or the Skylake CLDMA, FW_READY
 *   - IPC4 (create / delete / set state of pipelines, copier instances, bind,
 *     FIRMWARE_CONFIG / HARDWARE_CONFIG) and SOF IPC3 (topology, DAI config,
 *     PCM params, triggers) through the IPC registers and SRAM mailboxes
 *   - streaming: a host input gateway fed by a DMIC / I2S / ALH gateway gets a
 *     1 kHz tone (1.5 kHz on the second channel); a host output gateway is
 *     played to the audio device of the controller
 *   - SoundWire: the Intel shim (cAVS / ACE 1.x in BAR4; ACE 2.x: multi-link
 *     SoundWire extended link + per-link shims in the HD Audio BAR at 0x30000)
 *     and Cadence masters (command / response FIFO, PING slave status, bank
 *     switch through SCP_FrameCtrl, data port registers stored), peripherals
 *     from the sdw= property (link:mfg:part:class:uid,...) as register files
 *     with their DevId; an ALH capture gateway gets the tone only when its
 *     stream is wired end to end (ALH / PCMSyCHM or PCMSyCM + link DMA, PDI,
 *     master port enabled in the active bank, a peripheral source port enabled
 *     at the same block offset), otherwise silence
 *
 * dsp=kbl (Kaby Lake-LP, Intel firmware, CLDMA), apl, cnl (Intel firmware,
 * host DMA), icl (SOF IPC3), tgl (SOF IPC4), mtl, lnl, ptl (ACE, SOF IPC4)
 */

#define DSPG_SKL 1
#define DSPG_APL 2
#define DSPG_CNL 3
#define DSPG_ICL 4
#define DSPG_TGL 5
#define DSPG_MTL 6
#define DSPG_LNL 7
#define DSPG_PTL 8

typedef struct HDADspCpr {
    bool used;
    int ppl;
    uint32_t node;
    uint32_t in_ch, in_bits, out_ch, out_bits;
    uint32_t cfg[200];          /* the INIT_INSTANCE payload (gateway blob, TLVs) */
    uint32_t ncfg;
} HDADspCpr;

/* SoundWire */
#define SDW_NLINKS 4
#define SDW_NPER   4
#define SDW_NREGS  4096
typedef struct HDASdwPer {
    int link;
    uint8_t devid[6];
    uint16_t part;
    uint8_t devnum;
    bool attached;
    uint8_t page1, page2, bank;
    uint32_t n;
    uint32_t addr[SDW_NREGS];
    uint8_t val[SDW_NREGS];
} HDASdwPer;

typedef struct HDASdwLink {
    uint8_t mem[0x8000];        /* cAVS: Cadence registers; ACE 2.x: shim2 + Cadence + vendor shim */
    uint32_t resp[32];
    int nresp;
    int bank;
    bool reset_done;
    uint32_t cmds;
} HDASdwLink;

typedef struct HDADsp {
    IntelHDAState *d;
    int gen;
    bool ace, ipc3;
    MemoryRegion bar;
    uint32_t barsize;
    uint8_t *mem;
    uint32_t req, reqx, ack, ackbit, rsp, rspx, rspack, sram, win, status, up, down;
    int state;                    /* 0 off, 1 ROM running, 2 image taken, 3 ready */
    bool rom_pending;
    uint32_t img_bytes;
    /* CLDMA */
    bool cl_run;
    uint32_t cl_pos, cl_idle, cl_avail, cl_spib;
    QEMUTimer *tick;
    /* HD Audio extended registers */
    uint32_t ppctl, spbctl, spib[8], pplc_ctl[8], pplc_fmt[8];
    uint32_t ml_lctl[4], ml_losidv[4];
    uint32_t ml_lsync, ml_lsdiid[4];
    uint32_t em2, ltrp;
    /* IPC4 */
    int ppl_state[16];
    bool ppl_used[16];
    HDADspCpr cpr[32];
    /* IPC3 */
    uint32_t c3_host_comp[2], c3_tag[2];
    bool c3_run[2];
    uint32_t c3_dai_type[2], c3_dai_index[2], c3_alh[2];
    /* SoundWire */
    bool sdw_ext;
    uint32_t sdw_shim, sdw_alh, sdw_link, sdw_lsize, sdw_cbase, sdw_ip;
    HDASdwLink sdwl[SDW_NLINKS];
    HDASdwPer *per[SDW_NPER];
    int nper;
    MemoryRegion sdwwin;
    int cap_ok_logged, play_ok_logged;
    /* audio */
    SWVoiceOut *out;
    uint8_t fifo[16384];
    uint32_t fifo_rd, fifo_len;
    double ph[8];
    uint32_t msgs;
} HDADsp;

static void dsp_log(HDADsp *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void dsp_log(HDADsp *s, const char *fmt, ...)
{
    va_list ap;
    if (!s->d->debug) {
        return;
    }
    va_start(ap, fmt);
    fprintf(stderr, "intel-hda-dsp: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static uint32_t dsp_rd(HDADsp *s, uint32_t off)
{
    return off + 4 <= s->barsize ? ldl_le_p(s->mem + off) : 0;
}

static void dsp_wr(HDADsp *s, uint32_t off, uint32_t v)
{
    if (off + 4 <= s->barsize) {
        stl_le_p(s->mem + off, v);
    }
}

static void dsp_send(HDADsp *s, uint32_t primary, uint32_t ext)
{
    dsp_wr(s, s->rspx, ext);
    dsp_wr(s, s->rsp, primary | (1u << 31));
}

/* ------------------------------------------------------------------ */
/* streams of the host side                                             */
/* ------------------------------------------------------------------ */
static bool dsp_decoupled(HDADsp *s, int idx)
{
    return (s->ppctl & (1u << 30)) && (s->ppctl & (1u << idx));
}

/* DMA of a decoupled host stream (like intel_hda_xfer, by index) */
static uint32_t dsp_xfer(HDADsp *s, int idx, uint8_t *buf, uint32_t len, bool to_mem)
{
    IntelHDAState *d = s->d;
    IntelHDAStream *st = d->st + idx;
    uint32_t left = len, n, guard;
    if (!(st->ctl & 2) || !st->bpl || !st->bentries) {
        return 0;
    }
    for (guard = 0; left && guard < 64; guard++) {
        n = left;
        if (n > st->bsize - st->lpib) {
            n = st->bsize - st->lpib;
        }
        if (n > st->bpl[st->be].len - st->bp) {
            n = st->bpl[st->be].len - st->bp;
        }
        pci_dma_rw(&d->pci, st->bpl[st->be].addr + st->bp, buf, n,
                   to_mem ? DMA_DIRECTION_FROM_DEVICE : DMA_DIRECTION_TO_DEVICE,
                   MEMTXATTRS_UNSPECIFIED);
        st->lpib += n;
        st->bp += n;
        buf += n;
        left -= n;
        if (st->bp == st->bpl[st->be].len) {
            if (st->bpl[st->be].flags & 1) {
                st->ctl |= 1 << 26;
                intel_hda_update_irq(d);
            }
            st->bp = 0;
            if (++st->be == st->bentries) {
                st->be = 0;
                st->lpib = 0;
            }
        }
    }
    if (d->dp_lbase & 1) {
        stl_le_pci_dma(&d->pci, intel_hda_addr(d->dp_lbase & ~1, d->dp_ubase) + 8 * idx, st->lpib,
                       MEMTXATTRS_UNSPECIFIED);
    }
    return len - left;
}

static void dsp_fw_ready(HDADsp *s)
{
    s->state = 3;
    if (s->ipc3) {
        uint8_t r[108 + 16 + 2 * 24 + 8];
        uint32_t *w = (uint32_t *)r, o = 108;
        memset(r, 0, sizeof(r));
        w[0] = 108; w[1] = 0x70000000;
        stw_le_p(r + 28, 2); stw_le_p(r + 30, 2); stw_le_p(r + 32, 0);
        stl_le_p(r + 64, (3u << 24) | (23u << 12));
        stl_le_p(r + o, 16 + 2 * 24); stl_le_p(r + o + 4, 0x70000000); stl_le_p(r + o + 8, 1);
        stl_le_p(r + o + 12, 2);
        /* DOWNBOX: window 1; UPBOX: window 0 after the registers */
        stl_le_p(r + o + 16, 24); stl_le_p(r + o + 20, 0); stl_le_p(r + o + 24, 1);
        stl_le_p(r + o + 32, 0x1000); stl_le_p(r + o + 36, 0);
        stl_le_p(r + o + 40, 24); stl_le_p(r + o + 44, 1); stl_le_p(r + o + 48, 0);
        stl_le_p(r + o + 56, 0x1000); stl_le_p(r + o + 60, 0x1000);
        memcpy(s->mem + s->sram + 0x1000, r, sizeof(r));
        s->up = s->sram + 0x1000;
        s->down = s->sram + s->win;
        dsp_send(s, 0x70000000, 0);
    } else {
        dsp_send(s, 0x3B080000, 0);
    }
    dsp_log(s, "FW_READY sent");
}

static bool dsp_owns(IntelHDAState *d, int idx)
{
    return d->dsp && dsp_decoupled(d->dsp, idx);
}

/* the image through a decoupled host output stream */
static void dsp_code_load(HDADsp *s, int idx)
{
    IntelHDAState *d = s->d;
    IntelHDAStream *st = d->st + idx;
    uint32_t total = st->cbl, i, sum = 0, first = 0;
    uint8_t *buf;
    if ((s->spbctl & (1u << idx)) && s->spib[idx] && s->spib[idx] < total) {
        total = s->spib[idx];
    }
    if (!st->bpl || total > 16 * 1024 * 1024) {
        return;
    }
    buf = g_malloc(total);
    for (i = 0; i < total; ) {
        uint32_t n = dsp_xfer(s, idx, buf + i, total - i, false);
        if (!n) {
            break;
        }
        i += n;
    }
    first = total >= 4 ? ldl_le_p(buf) : 0;
    for (i = 0; i < total; i++) {
        sum += buf[i];
    }
    g_free(buf);
    s->img_bytes = total;
    dsp_log(s, "code loader: %u bytes, first dword %08x, sum %08x", total, first, sum);
    if (first != 0x44504324) {                     /* "$CPD": the ROM checks the CSE manifest */
        dsp_log(s, "ROM: no CSE manifest, image rejected");
        dsp_wr(s, s->status, 0x40000000 | 1);
        dsp_wr(s, s->status + 4, 0x2D);
        return;
    }
    dsp_wr(s, s->status, 5);                       /* FW_ENTERED */
    s->state = 2;
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * SCALE_MS);
}

/* ------------------------------------------------------------------ */
/* ROM                                                                  */
/* ------------------------------------------------------------------ */
static void dsp_rom_start(HDADsp *s)
{
    if (s->state != 0) {
        return;
    }
    if (s->gen == DSPG_SKL) {
        dsp_wr(s, s->status, 1);                   /* ROM init done, waits for the CLDMA */
        s->state = 1;
        return;
    }
    if (!s->rom_pending) {
        return;
    }
    s->rom_pending = false;
    dsp_wr(s, s->req, dsp_rd(s, s->req) & ~(1u << 31));
    dsp_wr(s, s->ack, dsp_rd(s, s->ack) | s->ackbit);
    dsp_wr(s, s->status, 1);
    s->state = 1;
    dsp_log(s, "ROM: ROM_CONTROL %08x taken, init done", dsp_rd(s, s->req));
}

static void dsp_adspcs(HDADsp *s, uint32_t v)
{
    uint32_t spa = (v >> 16) & 0xFF;
    v = (v & 0x00FFFFFF) | (spa << 24);
    dsp_wr(s, 0x04, v);
    if ((v & (1u << 24)) && !(v & 1) && !(v & (1u << 8))) {
        dsp_rom_start(s);
    }
    if (!(v & (1u << 24))) {
        s->state = 0;                              /* core 0 off: start over */
        dsp_wr(s, s->status, 0);
    }
}

/* ------------------------------------------------------------------ */
/* IPC4                                                                 */
/* ------------------------------------------------------------------ */
static uint32_t fmt_ch(const uint32_t *f) { return f[5] & 0xFF; }

static void dsp_ipc4(HDADsp *s, uint32_t p, uint32_t ext)
{
    uint32_t reply = (p & ((0x1Fu << 24) | (1u << 30))) | (1u << 29), rext = 0, status = 0;
    int type = (p >> 24) & 0x1F;
    s->msgs++;
    if (!(p & (1u << 30))) {
        int id = (p >> 16) & 0xFF;
        switch (type) {
        case 17:
            if (id < 16) { s->ppl_used[id] = true; s->ppl_state[id] = 1; }
            dsp_log(s, "create pipeline %d (%u pages)", id, p & 0x7FF);
            break;
        case 18:
            if (id < 16) {
                int i;
                s->ppl_used[id] = false;
                for (i = 0; i < 32; i++) {
                    if (s->cpr[i].used && s->cpr[i].ppl == id) {
                        s->cpr[i].used = false;
                    }
                }
            }
            break;
        case 19:
            if (id < 16 && s->ppl_used[id]) {
                s->ppl_state[id] = p & 0xFFFF;
                dsp_log(s, "pipeline %d state %u", id, p & 0xFFFF);
            } else {
                status = 1;
            }
            break;
        default:
            status = 15;
        }
    } else {
        int inst = (p >> 16) & 0xFF;
        switch (type) {
        case 0: {                                 /* INIT_INSTANCE */
            uint32_t w[24], nw = MIN(ext & 0xFFFF, 200);
            int i;
            for (i = 0; i < 24; i++) {
                w[i] = dsp_rd(s, s->down + i * 4);
            }
            if (inst < 32) {
                HDADspCpr *c = &s->cpr[inst];
                c->used = true;
                c->ncfg = nw;
                for (i = 0; i < (int)nw; i++) {
                    c->cfg[i] = dsp_rd(s, s->down + i * 4);
                }
                c->ppl = (ext >> 16) & 0xFF;
                c->in_bits = w[5]; c->in_ch = fmt_ch(&w[4]);
                c->out_bits = w[11]; c->out_ch = fmt_ch(&w[10]);
                c->node = w[17];
                dsp_log(s, "copier %d (module %u) in pipeline %d: node %08x in %u ch %u bit, out %u ch %u bit, "
                        "dma buffer %u, config %u dwords", inst, p & 0xFFFF, c->ppl, c->node, c->in_ch, c->in_bits,
                        c->out_ch, c->out_bits, w[18], w[19]);
                if (!s->ppl_used[c->ppl & 15]) {
                    status = 1;
                }
            }
            break;
        }
        case 3: {                                 /* LARGE_CONFIG_GET */
            int param = (ext >> 20) & 0xFF, n = 0;
            uint32_t b[16];
            if (param == 7) {
                b[n++] = 0; b[n++] = 8; b[n++] = 2 | (14u << 16); b[n++] = 0;      /* FW version 2.14.0.0 */
                b[n++] = 6; b[n++] = 4; b[n++] = 4096;
                b[n++] = 7; b[n++] = 4; b[n++] = 4096;
                b[n++] = 9; b[n++] = 4; b[n++] = 16;
            } else if (param == 8) {
                b[n++] = 1; b[n++] = 4; b[n++] = 4;
            }
            for (int i = 0; i < n; i++) {
                dsp_wr(s, s->up + i * 4, b[i]);
            }
            rext = (n * 4) | (1u << 28) | (1u << 29) | (param << 20);
            break;
        }
        case 4: case 5: case 6: case 7: case 8: case 11:
            break;
        default:
            status = 15;
        }
    }
    dsp_wr(s, s->ack, dsp_rd(s, s->ack) | s->ackbit);
    dsp_send(s, reply | status, rext);
}

/* ------------------------------------------------------------------ */
/* IPC3                                                                 */
/* ------------------------------------------------------------------ */
static void dsp_ipc3(HDADsp *s)
{
    uint32_t size = dsp_rd(s, s->down), cmd = dsp_rd(s, s->down + 4), id = dsp_rd(s, s->down + 8);
    int32_t err = 0;
    uint32_t rsize = 12, i;
    s->msgs++;
    switch (cmd) {
    case 0x30010000: {                            /* COMP_NEW */
        uint32_t type = dsp_rd(s, s->down + 12), dir = dsp_rd(s, s->down + 64);
        if (type == 1 && dir < 2) { s->c3_host_comp[dir] = id; }
        if (type == 2 && dir < 2) {
            s->c3_dai_type[dir] = dsp_rd(s, s->down + 72);
            s->c3_dai_index[dir] = dsp_rd(s, s->down + 68);
        }
        dsp_log(s, "IPC3 comp %u type %u dir %u", id, type, dir);
        break;
    }
    case 0x80010000: {                            /* DAI_CONFIG: ALH stream of an ALH DAI */
        uint32_t type = dsp_rd(s, s->down + 8), index = dsp_rd(s, s->down + 12);
        for (i = 0; i < 2; i++) {
            if (type == 4 && s->c3_dai_type[i] == 4 && s->c3_dai_index[i] == index) {
                s->c3_alh[i] = dsp_rd(s, s->down + 52 + 4);
                dsp_log(s, "IPC3 ALH DAI %u (dir %u): stream %u, %u ch", index, i, s->c3_alh[i],
                        dsp_rd(s, s->down + 52 + 12));
            }
        }
        break;
    }
    case 0x60010000: {                            /* PCM_PARAMS */
        uint32_t dir = dsp_rd(s, s->down + 24 + 32) & 1;
        uint32_t tag = dsp_rd(s, s->down + 24 + 48) & 0xFFFF;
        s->c3_tag[dir] = tag;
        stl_le_p(s->mem + s->down + 12, id);
        stl_le_p(s->mem + s->down + 16, 0);
        rsize = 20;
        dsp_log(s, "IPC3 pcm params comp %u dir %u tag %u", id, dir, tag);
        break;
    }
    case 0x60040000: case 0x60050000:             /* TRIG_START / STOP */
        for (i = 0; i < 2; i++) {
            if (s->c3_host_comp[i] == id) {
                s->c3_run[i] = cmd == 0x60040000;
            }
        }
        dsp_log(s, "IPC3 trigger %s comp %u", cmd == 0x60040000 ? "start" : "stop", id);
        break;
    default:
        break;
    }
    (void)size;
    dsp_wr(s, s->down, rsize);
    dsp_wr(s, s->down + 4, cmd);
    dsp_wr(s, s->down + 8, (uint32_t)err);
    dsp_wr(s, s->req, dsp_rd(s, s->req) & ~(1u << 31));
    dsp_wr(s, s->ack, dsp_rd(s, s->ack) | s->ackbit);
}

static void dsp_req(HDADsp *s, uint32_t v)
{
    dsp_wr(s, s->req, v);
    if (!(v & (1u << 31))) {
        return;
    }
    if (s->state < 3) {
        s->rom_pending = true;
        if (s->ace ? (dsp_rd(s, 0x178D04) & (1u << 8)) : ((dsp_rd(s, 0x04) & 0x01000101) == 0x01000000)) {
            dsp_rom_start(s);
        }
        return;
    }
    if (s->ipc3) {
        dsp_ipc3(s);
        return;
    }
    dsp_wr(s, s->req, v & ~(1u << 31));
    dsp_ipc4(s, v & 0x7FFFFFFF, dsp_rd(s, s->reqx));
}

/* ------------------------------------------------------------------ */
/* SoundWire: Intel shim + Cadence masters + register-file peripherals */
/* ------------------------------------------------------------------ */
static int per_find(HDASdwPer *p, uint32_t a)
{
    uint32_t i;
    for (i = 0; i < p->n; i++) {
        if (p->addr[i] == a) {
            return i;
        }
    }
    return -1;
}

static void per_set(HDASdwPer *p, uint32_t a, uint8_t v)
{
    int i = per_find(p, a);
    if (i < 0) {
        if (p->n >= SDW_NREGS) {
            return;
        }
        i = p->n++;
        p->addr[i] = a;
    }
    p->val[i] = v;
}

static uint32_t per_full(HDASdwPer *p, uint32_t a16)
{
    return (a16 & 0x8000) ? ((uint32_t)p->page1 << 23) | ((uint32_t)p->page2 << 15) | (a16 & 0x7FFF) : a16;
}

static uint8_t per_read(HDASdwPer *p, uint32_t a16)
{
    uint32_t a = per_full(p, a16);
    int i;
    if (a16 >= 0x50 && a16 <= 0x55) {
        return p->devid[a16 - 0x50];
    }
    switch (a16) {
    case 0x44: return p->bank << 6;                  /* SCP_STAT: current bank */
    case 0x46: return p->devnum;
    case 0x48: return p->page1;
    case 0x49: return p->page2;
    }
    if (a >= 0x100 && a < 0x1000 && (a & 0xFF) == 0x04) {
        return 0;                                    /* DPn_PrepareStatus: prepared */
    }
    i = per_find(p, a);
    if (i >= 0) {
        return p->val[i];
    }
    if ((a & 0x40000000) && (((a >> 15) & 0x30) | ((a >> 3) & 0xF)) == 0x10) {
        /* SDCA control 0x10 never written: PDE Actual_PS follows Requested_PS (control 1) */
        i = per_find(p, (a & ~((3u << 19) | (0xFu << 3))) | (1u << 3));
        return i >= 0 ? p->val[i] : 0;
    }
    return 0;
}

static void per_write(HDADsp *s, HDASdwPer *p, uint32_t a16, uint8_t v)
{
    uint32_t a;
    switch (a16) {
    case 0x46: p->devnum = v & 0xF; return;
    case 0x48: p->page1 = v; return;
    case 0x49: p->page2 = v; return;
    }
    a = per_full(p, a16);
    if (p->part == 0x711 && a == ((0x58u << 20) | 0x2000)) {
        v &= 0x7F;                                   /* RT711 DAC DC calibration done at once */
    }
    if ((a & 0xC0002000) == 0x40000000 && s->d->debug > 1) {
        dsp_log(s, "sdw: %04x SDCA fn %u ent %02x ctl %02x ch %u = %02x", p->part, (a >> 22) & 7,
                ((a >> 15) & 0x40) | ((a >> 7) & 0x3F), ((a >> 15) & 0x30) | ((a >> 3) & 0xF),
                ((a >> 12) & 0x38) | (a & 7), v);
    }
    per_set(p, a, v);
}

static bool sdw_powered(HDADsp *s, int l)
{
    if (s->sdw_ext) {
        return (s->ml_lctl[3] >> (16 + l)) & 1;
    }
    return (dsp_rd(s, s->sdw_shim + 4) >> l) & 1;
}

static uint32_t *cdns_reg(HDADsp *s, int l, uint32_t c)
{
    return (uint32_t *)(s->sdwl[l].mem + s->sdw_cbase + (c & 0x7FFC));
}

static uint32_t sdw_cmd(HDADsp *s, int l, uint32_t v)
{
    uint32_t cmd = (v >> 28) & 7, dev = (v >> 24) & 0xF, a16 = (v >> 8) & 0xFFFF, resp = 0;
    bool ssp = v >> 31;
    int i;
    if (!sdw_powered(s, l) || !s->sdwl[l].reset_done) {
        return 0;
    }
    s->sdwl[l].cmds++;
    if (cmd == 0) {
        return 1;
    }
    for (i = 0; i < s->nper; i++) {
        HDASdwPer *p = s->per[i];
        if (p->link != l || !p->attached || (dev != 15 && p->devnum != dev)) {
            continue;
        }
        if (cmd == 2) {
            if (!(resp & 1)) {
                resp = 1 | ((uint32_t)per_read(p, a16) << 8);
            }
        } else if (cmd == 3) {
            per_write(s, p, a16, v & 0xFF);
            resp = 1;
        }
    }
    if (cmd == 3 && ssp && (a16 == 0x60 || a16 == 0x70)) {
        /* bank switch at the next SSP: master and peripherals */
        s->sdwl[l].bank = a16 == 0x70;
        for (i = 0; i < s->nper; i++) {
            if (s->per[i]->link == l) {
                s->per[i]->bank = a16 == 0x70;
            }
        }
        dsp_log(s, "sdw: link %d bank switch to %d", l, s->sdwl[l].bank);
    }
    return resp;
}

static uint32_t cdns_read(HDADsp *s, int l, uint32_t c)
{
    HDASdwLink *L = &s->sdwl[l];
    uint32_t ip = s->sdw_ip, st = 0;
    int i;
    if (c >= ip + 0x80 && c < ip + 0x100) {
        return L->resp[(c - ip - 0x80) / 4];
    }
    switch (c) {
    case 0x18: return 0;                              /* CONFIG_UPDATE: done */
    case 0x40: return L->bank ? (1u << 20) : 0;       /* MCP_STAT.ACTIVE_BANK */
    case 0x50:                                        /* SLAVE_STAT: 1 = attached per device number */
        for (i = 0; i < s->nper; i++) {
            if (s->per[i]->link == l && s->per[i]->attached && L->reset_done) {
                st |= 1u << (2 * s->per[i]->devnum);
            }
        }
        return st;
    case 0x7C: return L->nresp;                       /* FIFOSTAT: RX available */
    }
    return ldl_le_p(cdns_reg(s, l, c));
}

static void cdns_write(HDADsp *s, int l, uint32_t c, uint32_t v)
{
    HDASdwLink *L = &s->sdwl[l];
    uint32_t ip = s->sdw_ip, *r = cdns_reg(s, l, c);
    int i;
    if (c >= ip + 0x80 && c < ip + 0x100) {
        i = (c - ip - 0x80) / 4;
        if (i == 0) {
            L->nresp = 0;
        }
        L->resp[i] = sdw_cmd(s, l, v);
        if (i + 1 > L->nresp) {
            L->nresp = i + 1;
        }
        stl_le_p(cdns_reg(s, l, 0x44), ldl_le_p(cdns_reg(s, l, 0x44)) | 4);   /* RX_WL */
        return;
    }
    switch (c) {
    case 0x44:                                        /* INTSTAT: write 1 to clear */
        stl_le_p(r, ldl_le_p(r) & ~v);
        return;
    case 0x04:                                        /* MCP_CONTROL: CMD_RST / SOFT_RST self-clear */
        if (v & (1u << 7)) {
            L->nresp = 0;
        }
        stl_le_p(r, v & ~((1u << 7) | (1u << 6)));
        return;
    case 0x18:                                        /* CONFIG_UPDATE: HW_RST -> bus reset, peripherals sync */
        if (ldl_le_p(cdns_reg(s, l, 4)) & (1u << 4)) {
            stl_le_p(cdns_reg(s, l, 4), ldl_le_p(cdns_reg(s, l, 4)) & ~(1u << 4));
            L->reset_done = sdw_powered(s, l);
            L->bank = 0;
            for (i = 0; i < s->nper; i++) {
                HDASdwPer *p = s->per[i];
                if (p->link == l) {
                    p->devnum = 0;
                    p->attached = L->reset_done;
                    p->bank = 0;
                    p->page1 = p->page2 = 0;
                }
            }
            dsp_log(s, "sdw: link %d bus reset (%s)", l, L->reset_done ? "running" : "not powered");
        }
        stl_le_p(r, 0);
        return;
    }
    stl_le_p(r, v);
}

/* the shim of cAVS / ACE 1.x in BAR4: LCTL CPA follows SPA, SYNCCPU clears once a link runs */
static void sdw_shim_write(HDADsp *s, uint32_t o)
{
    uint32_t v = dsp_rd(s, s->sdw_shim + (o & ~3u));
    int l;
    if ((o & ~3u) == 0x4) {
        v = (v & ~0xF00u) | ((v & 0xF) << 8);
        dsp_wr(s, s->sdw_shim + 4, v);
        for (l = 0; l < SDW_NLINKS; l++) {
            if (!((v >> l) & 1)) {
                s->sdwl[l].reset_done = false;
            }
        }
    }
    if ((dsp_rd(s, s->sdw_shim + 4) & 0xF) && (o & ~3u) <= 0xC) {
        dsp_wr(s, s->sdw_shim + 0xC, dsp_rd(s, s->sdw_shim + 0xC) & ~(1u << 15));
    }
}

/* ACE 2.x: the per-link window in the HD Audio BAR (0x30000 + 0x8000 * link) */
static uint64_t sdw_win_read(void *opaque, hwaddr addr, unsigned size)
{
    HDADsp *s = opaque;
    int l = addr / 0x8000;
    uint32_t o = addr % 0x8000;
    uint64_t v = 0;
    if (l >= SDW_NLINKS) {
        return 0;
    }
    if (o >= 0x100 && o < 0x6000 && size == 4) {
        return cdns_read(s, l, o - 0x100);
    }
    memcpy(&v, s->sdwl[l].mem + o, MIN(size, 0x8000 - o));
    return v;
}

static void sdw_win_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    HDADsp *s = opaque;
    int l = addr / 0x8000;
    uint32_t o = addr % 0x8000;
    if (l >= SDW_NLINKS) {
        return;
    }
    if (o >= 0x100 && o < 0x6000 && size == 4) {
        cdns_write(s, l, o - 0x100, val);
        return;
    }
    memcpy(s->sdwl[l].mem + o, &val, MIN(size, 0x8000 - o));
}

static const MemoryRegionOps sdw_win_ops = {
    .read = sdw_win_read,
    .write = sdw_win_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* is the PDI wired to an attached peripheral port: master port in the active bank and a peripheral data
   port enabled at the same block offset; why gets the first missing piece */
static bool sdw_pdi_ok(HDADsp *s, int l, int pdi, bool cap, const char **why)
{
    HDASdwLink *L = &s->sdwl[l];
    uint32_t b = L->bank, pc, ctl, off, n;
    int i;
    if (l >= SDW_NLINKS || pdi < 0 || pdi > 14) {
        *why = "bad link / PDI";
        return false;
    }
    if (!sdw_powered(s, l) || !L->reset_done) {
        *why = "link not running";
        return false;
    }
    pc = ldl_le_p(cdns_reg(s, l, 0x1100 + 16 * pdi));
    if ((pc & 0x1F) != (uint32_t)pdi || !((pc >> 8) & 0xFF)) {
        *why = "PDI_CONFIG not set";
        return false;
    }
    ctl = ldl_le_p(cdns_reg(s, l, 0x130 + 0x80 * pdi));
    if (!!(ctl & (1u << 7)) != cap) {
        *why = "master port direction";
        return false;
    }
    if (!ldl_le_p(cdns_reg(s, l, 0x104 + 0x80 * pdi + 0x18 * b))) {
        *why = "master port not enabled in the active bank";
        return false;
    }
    off = ldl_le_p(cdns_reg(s, l, 0x10C + 0x80 * pdi + 0x18 * b)) & 0xFFFF;
    off = (off & 0xFF) | ((off >> 8) << 8);
    for (i = 0; i < s->nper; i++) {
        HDASdwPer *p = s->per[i];
        if (p->link != l || !p->attached || !p->devnum) {
            continue;
        }
        for (n = 1; n < 15; n++) {
            uint32_t base = 0x100 * n, en = per_read(p, base + 0x20 + 0x10 * b);
            uint32_t poff = per_read(p, base + 0x24 + 0x10 * b) | (per_read(p, base + 0x25 + 0x10 * b) << 8);
            if (en && poff == off) {
                return true;
            }
        }
    }
    *why = "no peripheral port enabled at the master's offset";
    return false;
}

/* an IPC4 ALH gateway copier: every ALH stream of its blob wired */
static bool sdw_gw_ok(HDADsp *s, HDADspCpr *c, bool cap, const char **why)
{
    uint32_t len = c->ncfg > 19 ? c->cfg[19] : 0, cnt, i, t, o;
    const uint32_t *blob = &c->cfg[20];
    if (len < 2 || c->ncfg < 22) {
        *why = "no ALH blob";
        return false;
    }
    cnt = blob[1];
    if (!cnt || cnt > 4 || 20 + 2 + 2 * cnt > c->ncfg) {
        *why = "bad ALH blob";
        return false;
    }
    for (i = 0; i < cnt; i++) {
        uint32_t dev = blob[2 + 2 * i];
        int l, pdi;
        if (s->sdw_ext) {
            uint32_t tag = 0, chan = 0, cm;
            bool found = false;
            l = dev >> 8;
            pdi = dev & 0xFF;
            for (o = 20 + 2 + 2 * cnt; o + 8 <= c->ncfg && o < 20 + len; o += 2 + c->cfg[o + 1] / 4) {
                if (c->cfg[o] == 0x1000 && c->cfg[o + 5] && c->cfg[o + 6] == dev) {
                    chan = c->cfg[o + 3];
                    tag = c->cfg[o + 4];
                    found = true;
                    break;
                }
                if (!c->cfg[o + 1]) {
                    break;
                }
            }
            if (!found) {
                *why = "no DMA configuration TLV for the stream";
                return false;
            }
            if (l >= SDW_NLINKS) {
                *why = "bad link";
                return false;
            }
            cm = lduw_le_p(s->sdwl[l].mem + 0x16 + 4 * pdi);
            if (((cm >> 8) & 0x3F) != tag || !!(cm & 0x8000) != cap) {
                *why = "PCMSyCM does not map the PDI to the stream tag";
                return false;
            }
            t = cap ? chan : 4 + chan;
            if (t >= 8 || !(s->pplc_ctl[t] & 2) || ((s->pplc_ctl[t] >> 20) & 0xF) != tag) {
                *why = "link DMA not running with the tag";
                return false;
            }
            if (!cap && !(s->ml_losidv[3] & (1u << tag))) {
                *why = "LOSIDV (output stream id) not valid";
                return false;
            }
        } else {
            uint32_t alh = dev & 0xFF, hm;
            if ((dev >> 8) != (cap ? 17u : 16u) || alh < 5) {
                *why = "ALH device node type";
                return false;
            }
            l = (alh - 5) / 16;
            pdi = (alh - 5) % 16;
            if (l >= SDW_NLINKS) {
                *why = "bad ALH stream";
                return false;
            }
            hm = lduw_le_p(s->mem + s->sdw_shim + 0x22 + 0x60 * l + 2 * pdi);
            if (((hm >> 8) & 0x3F) != alh || !!(hm & 0x8000) != cap) {
                *why = "PCMSyCHM does not map the PDI to the ALH stream";
                return false;
            }
            if ((dsp_rd(s, s->sdw_alh + 4 * alh) & 0xFF) != 3) {
                *why = "ALH STRMzCFG";
                return false;
            }
        }
        if (!sdw_pdi_ok(s, l, pdi, cap, why)) {
            return false;
        }
    }
    return true;
}

/* SOF IPC3 ALH DAI: the ALH stream id of its DAI_CONFIG */
static bool sdw_alh3_ok(HDADsp *s, uint32_t alh, bool cap, const char **why)
{
    uint32_t hm;
    int l, pdi;
    if (alh < 5 || (alh - 5) / 16 >= SDW_NLINKS) {
        *why = "bad ALH stream";
        return false;
    }
    l = (alh - 5) / 16;
    pdi = (alh - 5) % 16;
    hm = lduw_le_p(s->mem + s->sdw_shim + 0x22 + 0x60 * l + 2 * pdi);
    if (((hm >> 8) & 0x3F) != alh || !!(hm & 0x8000) != cap) {
        *why = "PCMSyCHM does not map the PDI to the ALH stream";
        return false;
    }
    return sdw_pdi_ok(s, l, pdi, cap, why);
}

static void sdw_realize(HDADsp *s, const char *spec)
{
    int l, y;
    s->sdw_ext = s->gen >= DSPG_LNL;
    s->sdw_shim = s->gen == DSPG_MTL ? 0x38000 : 0x2C000;
    s->sdw_alh = s->gen == DSPG_MTL ? 0x24000 : 0x2C800;
    s->sdw_link = 0x30000;
    s->sdw_lsize = 0x10000;
    s->sdw_cbase = s->sdw_ext ? 0x100 : 0;
    s->sdw_ip = s->sdw_ext ? 0x4000 : 0;
    if (s->gen >= DSPG_CNL && !s->sdw_ext) {
        dsp_wr(s, s->sdw_shim, SDW_NLINKS | (s->gen == DSPG_MTL ? 0x100 : 0));     /* LCAP: links, MLCS */
        for (l = 0; l < SDW_NLINKS; l++) {
            stw_le_p(s->mem + s->sdw_shim + 0x20 + 0x60 * l, 0x0800);                /* PCMSCAP: 8 bidirectional */
            for (y = 0; y < 8; y++) {
                stw_le_p(s->mem + s->sdw_shim + 0x42 + 0x60 * l + 2 * y, 7);         /* PCMSyCHC: 8 channels */
            }
        }
    }
    for (l = 0; l < SDW_NLINKS; l++) {
        stw_le_p(s->sdwl[l].mem + 0x10, 0x0800);                                     /* ACE 2.x shim2 PCMSCAP */
        for (y = 0; y < 8; y++) {
            stw_le_p(s->sdwl[l].mem + 0x14 + 4 * y, 7);
        }
    }
    while (spec && *spec && s->nper < SDW_NPER) {
        unsigned link, mfg, part, cls, uid;
        HDASdwPer *p;
        if (sscanf(spec, "%u:%x:%x:%x:%x", &link, &mfg, &part, &cls, &uid) != 5 || link >= SDW_NLINKS) {
            break;
        }
        p = g_new0(HDASdwPer, 1);
        p->link = link;
        p->part = part;
        p->devid[0] = (3 << 4) | (uid & 0xF);         /* SoundWire 1.2, unique id */
        p->devid[1] = mfg >> 8; p->devid[2] = mfg;
        p->devid[3] = part >> 8; p->devid[4] = part;
        p->devid[5] = cls;
        if (part == 0x711) {
            per_set(p, 0x320c, 0x40);                 /* RC calibration done */
            per_set(p, 0x40000000 | (1u << 22) | (1u << 21) | (0x09 << 7) | (2 << 3), 5);   /* GE49 detected: headset */
        }
        s->per[s->nper++] = p;
        dsp_log(s, "sdw: peripheral %04x:%04x on link %u, uid %u", mfg, part, link, uid);
        spec = strchr(spec, ',');
        if (spec) {
            spec++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* streaming                                                            */
/* ------------------------------------------------------------------ */
/* host stream (0..7) the DSP feeds or drains: IPC4 host gateways name the DMA
 * (index within the direction), SOF IPC3 the stream tag */
static int dsp_tag_stream(HDADsp *s, uint32_t tag, bool out)
{
    int i, base = out ? 4 : 0;
    for (i = base; i < base + 4; i++) {
        if (((s->d->st[i].ctl >> 20) & 0xF) == tag && (s->d->st[i].ctl & 2)) {
            return i;
        }
    }
    return -1;
}

/* a running SoundWire gateway that is not wired end to end: the stream carries silence */
static bool dsp_gateway_ok(HDADsp *s, bool cap)
{
    const char *why = "";
    bool ok = true, alh = false;
    int i, *logged = cap ? &s->cap_ok_logged : &s->play_ok_logged;
    if (s->ipc3) {
        if (s->c3_dai_type[cap] == 4) {
            alh = true;
            ok = sdw_alh3_ok(s, s->c3_alh[cap], cap, &why);
        }
    } else {
        for (i = 0; i < 32; i++) {
            HDADspCpr *c = &s->cpr[i];
            if (c->used && ((c->node >> 8) & 0x1F) == (cap ? 17u : 16u) && s->ppl_state[c->ppl & 15] == 4) {
                alh = true;
                ok = sdw_gw_ok(s, c, cap, &why);
                break;
            }
        }
    }
    if (alh && *logged != (ok ? 1 : 2)) {
        *logged = ok ? 1 : 2;
        dsp_log(s, "sdw: ALH %s gateway %s%s%s", cap ? "capture" : "playback", ok ? "wired" : "NOT wired: ",
                ok ? "" : why, ok ? "" : " (silence)");
    }
    return ok;
}

static int dsp_capture_stream(HDADsp *s, uint32_t *ch)
{
    int i;
    if (s->ipc3) {
        *ch = 2;
        return s->c3_run[1] ? dsp_tag_stream(s, s->c3_tag[1], false) : -1;
    }
    for (i = 0; i < 32; i++) {
        HDADspCpr *c = &s->cpr[i];
        if (c->used && ((c->node >> 8) & 0x1F) == 1 && s->ppl_state[c->ppl & 15] == 4) {
            *ch = c->out_ch ? c->out_ch : 2;
            return (c->node & 0xFF) < 4 ? (int)(c->node & 0xFF) : -1;
        }
    }
    return -1;
}

static int dsp_playback_stream(HDADsp *s)
{
    int i;
    if (s->ipc3) {
        return s->c3_run[0] ? dsp_tag_stream(s, s->c3_tag[0], true) : -1;
    }
    for (i = 0; i < 32; i++) {
        HDADspCpr *c = &s->cpr[i];
        if (c->used && ((c->node >> 8) & 0x1F) == 0 && s->ppl_state[c->ppl & 15] == 4) {
            return (c->node & 0xFF) < 4 ? 4 + (int)(c->node & 0xFF) : -1;
        }
    }
    return -1;
}

/* playback: the tick takes 1 ms from the host stream into a FIFO (the guest
 * sees the position advance at the sample rate), the audio callback drains it */
#define DSP_FIFO 16384
static void dsp_out_cb(void *opaque, int avail)
{
    HDADsp *s = opaque;
    uint8_t buf[4096];
    int n;
    while (avail > 0 && s->fifo_len) {
        n = MIN(MIN(avail, (int)sizeof(buf)), (int)s->fifo_len);
        for (int i = 0; i < n; i++) {
            buf[i] = s->fifo[(s->fifo_rd + i) % DSP_FIFO];
        }
        n = AUD_write(s->out, buf, n);
        if (!n) {
            break;
        }
        s->fifo_rd = (s->fifo_rd + n) % DSP_FIFO;
        s->fifo_len -= n;
        avail -= n;
    }
}

static void dsp_play_tick(HDADsp *s)
{
    int sd = dsp_playback_stream(s);
    uint8_t buf[192];
    uint32_t i;
    if (sd < 0 || sd >= 8 || !dsp_decoupled(s, sd)) {
        return;
    }
    memset(buf, 0, sizeof(buf));
    dsp_xfer(s, sd, buf, sizeof(buf), false);
    if (!dsp_gateway_ok(s, false)) {
        memset(buf, 0, sizeof(buf));
    }
    if (s->fifo_len + sizeof(buf) > DSP_FIFO) {
        return;                                    /* backend stalled: drop */
    }
    for (i = 0; i < sizeof(buf); i++) {
        s->fifo[(s->fifo_rd + s->fifo_len + i) % DSP_FIFO] = buf[i];
    }
    s->fifo_len += sizeof(buf);
}

static void dsp_tick(void *opaque)
{
    HDADsp *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t ch;
    int idx;
    if (s->state == 2) {
        dsp_fw_ready(s);
    }
    dsp_play_tick(s);
    /* CLDMA: take what SPIB made valid, half a ring at a time */
    if (s->cl_run) {
        uint32_t cbl = dsp_rd(s, 0x88), half = cbl / 2;
        if (half && s->cl_avail) {
            uint32_t n = MIN(s->cl_avail, half - s->cl_pos % half);
            s->cl_pos = (s->cl_pos + n) % cbl;
            s->cl_avail -= n;
            s->img_bytes += n;
            s->cl_idle = 0;
            dsp_wr(s, 0x84, s->cl_pos);
            if (!(s->cl_pos % half)) {
                s->mem[0x83] |= 4;                 /* buffer completion */
            }
        } else if (!s->cl_avail && ++s->cl_idle > 30 && s->img_bytes > 65536 && s->state == 1) {
            dsp_log(s, "CLDMA: %u bytes, base firmware entered", s->img_bytes);
            dsp_wr(s, s->status, 0xF);
            s->state = 2;
        }
    }
    /* capture: a tone into the host input stream, 48 frames a millisecond */
    idx = dsp_capture_stream(s, &ch);
    if (idx >= 0 && dsp_decoupled(s, idx) && ch && ch <= 8) {
        int16_t buf[48 * 8];
        int f, c;
        bool wired = dsp_gateway_ok(s, true);
        for (f = 0; f < 48; f++) {
            for (c = 0; c < (int)ch; c++) {
                double fr = c == 1 ? 1500.0 : 1000.0;
                buf[f * ch + c] = wired ? (int16_t)(sin(s->ph[c]) * 12000) : 0;
                s->ph[c] += 2 * M_PI * fr / 48000.0;
                if (s->ph[c] > 2 * M_PI) {
                    s->ph[c] -= 2 * M_PI;
                }
            }
        }
        dsp_xfer(s, idx, (uint8_t *)buf, 48 * ch * 2, true);
    }
    timer_mod(s->tick, now + SCALE_MS);
}

/* ------------------------------------------------------------------ */
/* BAR4                                                                 */
/* ------------------------------------------------------------------ */
static int dsp_sdw_link_at(HDADsp *s, hwaddr addr, uint32_t *c)
{
    int l;
    if (s->sdw_ext || s->gen < DSPG_CNL || addr < s->sdw_link) {
        return -1;
    }
    l = (addr - s->sdw_link) / s->sdw_lsize;
    *c = (addr - s->sdw_link) % s->sdw_lsize;
    return l < SDW_NLINKS && *c < 0x8000 ? l : -1;
}

static uint64_t dsp_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    HDADsp *s = opaque;
    uint64_t v = 0;
    uint32_t c;
    int l;
    if (addr + size > s->barsize) {
        return 0;
    }
    if (size == 4 && (l = dsp_sdw_link_at(s, addr, &c)) >= 0) {
        return cdns_read(s, l, c);
    }
    memcpy(&v, s->mem + addr, size);
    return v;
}

static void dsp_bar_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    HDADsp *s = opaque;
    uint32_t a = addr & ~3u, v, old;
    if (addr + size > s->barsize) {
        return;
    }
    if (size == 4 && dsp_sdw_link_at(s, addr, &v) >= 0) {
        cdns_write(s, dsp_sdw_link_at(s, addr, &v), v, val);
        return;
    }
    old = dsp_rd(s, a);
    memcpy(s->mem + addr, &val, size);
    v = dsp_rd(s, a);
    if (!s->sdw_ext && s->gen >= DSPG_CNL && a >= s->sdw_shim && a < s->sdw_shim + 0x10) {
        sdw_shim_write(s, a - s->sdw_shim);
    } else if (!s->ace && a == 0x04) {
        dsp_adspcs(s, v);
    } else if (a == s->req) {
        dsp_req(s, v);
    } else if (a == s->ack) {
        /* DONE is write 1 to clear; on SKL / APL the rest is the extension (HIPCIE) */
        dsp_wr(s, a, (v & ~s->ackbit) | (old & s->ackbit & ~v));
    } else if (a == s->rsp) {
        dsp_wr(s, a, old & ~(v & (1u << 31)));     /* write 1 to clear busy */
    } else if (s->rspack && a == s->rspack) {
        dsp_wr(s, a, s->ace ? v : (v & ~(1u << 31)));
    } else if (s->ace && a == 0x1000) {
        dsp_wr(s, a, (v & ~(1u << 24)) | ((v & (1u << 16)) << 8));
    } else if (s->ace && (a == 0x1D18 || a == 0x1D20)) {
        dsp_wr(s, a + 4, v & 0xFFFF);
    } else if (s->ace && a == 0x178D04) {
        v = (v & ~(1u << 8)) | ((v & 1) << 8);
        dsp_wr(s, a, v);
        if (v & 1) {
            dsp_rom_start(s);
        } else {
            s->state = 0;
        }
    } else if (s->gen == DSPG_SKL && a == 0x28) {
        uint32_t cbl = dsp_rd(s, 0x88);
        int32_t delta = (int32_t)v - (int32_t)s->cl_spib;
        if (delta <= 0) {
            delta += cbl;
        }
        s->cl_spib = v;
        s->cl_avail = MIN(s->cl_avail + delta, cbl);
    } else if (s->gen == DSPG_SKL && a == 0x80) {
        uint8_t ctl = s->mem[0x80];
        if (addr + size > 0x83) {                  /* status byte: write 1 to clear */
            s->mem[0x83] = (uint8_t)(old >> 24) & ~(uint8_t)(val >> ((0x83 - addr) * 8));
        }
        if (addr == 0x80) {
            if (ctl & 1) {
                s->cl_pos = 0;
                s->cl_avail = 0;
                s->cl_spib = 0;
                s->mem[0x83] = 0;
            }
            if ((ctl & 2) && !s->cl_run) {
                s->cl_run = true;
                s->img_bytes = 0;
                s->cl_idle = 0;
                dsp_log(s, "CLDMA run, ring %u bytes", dsp_rd(s, 0x88));
            } else if (!(ctl & 2)) {
                s->cl_run = false;
            }
        }
    }
}

static const MemoryRegionOps dsp_bar_ops = {
    .read = dsp_bar_read,
    .write = dsp_bar_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* HD Audio registers the base model does not have                     */
/* ------------------------------------------------------------------ */
#define DSP_SPB 0x700
#define DSP_PP  0x800
#define DSP_ML  0xC00

static int dsp_links(HDADsp *s) { return s->ace ? 4 : 1; }

static bool dsp_hda_read(IntelHDAState *d, hwaddr addr, unsigned size, uint64_t *val)
{
    HDADsp *s = d->dsp;
    uint32_t a = addr & ~3u, v = 0, sh = (addr & 3) * 8;
    if (!s) {
        return false;
    }
    if (a == 0x14) {
        v = DSP_SPB;                              /* LLCH: first capability */
    } else if (a >= DSP_SPB && a < DSP_PP) {
        uint32_t o = a - DSP_SPB;
        if (o == 0) v = (4u << 16) | DSP_PP;
        else if (o == 4) v = s->spbctl;
        else if (o >= 8 && (o - 8) / 8 < 8 && !((o - 8) & 4)) v = s->spib[(o - 8) / 8];
    } else if (a >= DSP_PP && a < DSP_ML) {
        uint32_t o = a - DSP_PP;
        if (o == 0) v = (3u << 16) | DSP_ML;
        else if (o == 4) v = s->ppctl;
        else if (o == 8) v = 0;
        else if (o >= 0x10 + 0x10 * 8 && o < 0x10 + 0x10 * 16) {
            uint32_t k = (o - 0x90) / 0x10, r = (o - 0x90) % 0x10;
            v = r == 0 ? s->pplc_ctl[k] : r == 4 ? s->pplc_fmt[k] : 0;
        }
    } else if (a >= DSP_ML && a < 0x1000) {
        uint32_t o = a - DSP_ML;
        if (o == 0) v = 2u << 16;
        else if (o == 4) v = dsp_links(s);
        else if (o >= 0x40 && (o - 0x40) / 0x40 < (uint32_t)dsp_links(s)) {
            int l = (o - 0x40) / 0x40, r = (o - 0x40) % 0x40;
            if (r == 0) v = l == 0 ? 0x1E : (1u << 28) | (l == 2 ? (2u << 20) : l == 3 ? ((SDW_NLINKS - 1) << 20) : 0);
            else if (r == 4) v = s->ml_lctl[l];
            else if (r == 8) v = s->ml_losidv[l];
            else if (r >= 0x0C && r < 0x14 && l == 3) v = s->ml_lsdiid[(r - 0x0C) / 2] | (s->ml_lsdiid[(r - 0x0C) / 2 + 1] << 16);
            else if (r == 0x1C && l == 3) v = s->ml_lsync;
            else if (r == 0x20) v = l == 1 ? (0xC1u << 24) | 0x10000 : l == 2 ? (0xC0u << 24) | 0x20000 :
                                    l == 3 ? (0x00u << 24) | 0x30000 : 0;
        }
    } else if (a == 0x1030) {
        v = s->em2;
    } else if (a == 0x1048) {
        v = s->ltrp;
    } else if (a >= 0x1084 && a < 0x1084 + 0x20 * 8 && !((a - 0x1084) % 0x20)) {
        v = d->st[(a - 0x1084) / 0x20].lpib;     /* SDxDPIB */
    } else {
        return false;
    }
    *val = (v >> sh) & MAKE_64BIT_MASK(0, size * 8);
    return true;
}

static bool dsp_hda_write(IntelHDAState *d, hwaddr addr, uint64_t val, unsigned size)
{
    HDADsp *s = d->dsp;
    uint32_t a = addr & ~3u, sh = (addr & 3) * 8, mask = (uint32_t)MAKE_64BIT_MASK(sh, size * 8), v;
    uint32_t *r = NULL;
    if (!s) {
        return false;
    }
    if (a >= DSP_SPB && a < DSP_PP) {
        uint32_t o = a - DSP_SPB;
        if (o == 4) r = &s->spbctl;
        else if (o >= 8 && (o - 8) / 8 < 8 && !((o - 8) & 4)) r = &s->spib[(o - 8) / 8];
    } else if (a >= DSP_PP && a < DSP_ML) {
        uint32_t o = a - DSP_PP;
        if (o == 4) r = &s->ppctl;
        else if (o >= 0x90 && o < 0x110) {
            uint32_t k = (o - 0x90) / 0x10, rr = (o - 0x90) % 0x10;
            if (rr == 0) r = &s->pplc_ctl[k];
            else if (rr == 4) r = &s->pplc_fmt[k];
        }
    } else if (a >= DSP_ML && a < 0x1000) {
        uint32_t o = a - DSP_ML;
        if (o >= 0x40 && (o - 0x40) / 0x40 < (uint32_t)dsp_links(s)) {
            int l = (o - 0x40) / 0x40, rr = (o - 0x40) % 0x40;
            if (rr == 4) r = &s->ml_lctl[l];
            else if (rr == 8) r = &s->ml_losidv[l];
            else if (rr == 0x1C && l == 3) r = &s->ml_lsync;
        }
    } else if (a == 0x1030) {
        r = &s->em2;
    } else if (a == 0x1048) {
        r = &s->ltrp;
    } else if (a == 0x14 || (a >= 0x1000 && a < 0x1200)) {
        return true;
    } else {
        return false;
    }
    if (r) {
        v = (*r & ~mask) | (((uint32_t)val << sh) & mask);
        if (r >= s->ml_lctl && r < s->ml_lctl + 4) {
            v = (v & ~(0xFFu << 23)) | (((v >> 16) & 0x7F) << 23);     /* CPA follows SPA */
            if (r == &s->ml_lctl[3]) {
                int l;
                for (l = 0; l < SDW_NLINKS; l++) {
                    if (!((v >> (16 + l)) & 1)) {
                        s->sdwl[l].reset_done = false;
                    }
                }
            }
        }
        if (r == &s->ml_lsync) {
            /* SYNCPU clears once a sublink runs; SYNCGO clears itself and the CMDSYNC bits */
            if (s->ml_lctl[3] & (0xFu << 16)) {
                v &= ~(1u << 20);
            }
            if (v & (1u << 23)) {
                v &= ~((1u << 23) | (0xFu << 24));
            }
        }
        if (r >= s->pplc_ctl && r < s->pplc_ctl + 8) {
            v &= ~1u;                                                  /* reset completes at once */
        }
        *r = v;
    }
    return true;
}

/* run / stop of a stream the processing pipe owns */
static bool dsp_stream_ctl(IntelHDAState *d, int idx, bool run)
{
    HDADsp *s = d->dsp;
    if (!s || !dsp_decoupled(s, idx)) {
        return false;
    }
    if (run) {
        intel_hda_parse_bdl(d, d->st + idx);
        if (idx >= 4 && s->state == 1) {
            dsp_code_load(s, idx);
        }
        dsp_log(s, "decoupled stream %d %s", idx, run ? "run" : "stop");
    }
    return true;
}

static void dsp_realize(IntelHDAState *d, Error **errp)
{
    static const struct { const char *n; int gen; uint16_t dev; bool ipc3; } tab[] = {
        { "kbl", DSPG_SKL, 0x9D71, false }, { "apl", DSPG_APL, 0x5A98, false }, { "cnl", DSPG_CNL, 0x02C8, false },
        { "icl", DSPG_ICL, 0x34C8, true }, { "tgl", DSPG_TGL, 0xA0C8, false }, { "mtl", DSPG_MTL, 0x7E28, false },
        { "lnl", DSPG_LNL, 0xA828, false }, { "ptl", DSPG_PTL, 0xE428, false },
    };
    HDADsp *s;
    unsigned i;
    struct audsettings as = { .freq = 48000, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .endianness = 0 };
    for (i = 0; i < ARRAY_SIZE(tab); i++) {
        if (!strcmp(d->dspgen, tab[i].n)) {
            break;
        }
    }
    if (i == ARRAY_SIZE(tab)) {
        error_setg(errp, "intel-hda: unknown dsp '%s'", d->dspgen);
        return;
    }
    s = d->dsp = g_new0(HDADsp, 1);
    s->d = d;
    s->gen = tab[i].gen;
    s->ipc3 = tab[i].ipc3;
    s->ace = s->gen >= DSPG_MTL;
    s->barsize = s->ace ? 0x200000 : 0x100000;
    s->mem = g_malloc0(s->barsize);
    pci_set_word(d->pci.config + PCI_DEVICE_ID, tab[i].dev);
    if (s->gen <= DSPG_APL) {
        s->rsp = 0x40; s->rspx = 0x44; s->req = 0x48; s->reqx = 0x4C; s->ack = 0x4C; s->ackbit = 1u << 30;
    } else if (!s->ace) {
        s->rsp = 0xC0; s->rspack = 0xC4; s->rspx = 0xC8; s->req = 0xD0; s->ack = 0xD4; s->reqx = 0xD8;
        s->ackbit = 1u << 31;
    } else {
        s->rsp = 0x73200; s->rspack = 0x73204; s->req = 0x73210; s->ack = 0x73214;
        s->rspx = 0x73300; s->reqx = 0x73380; s->ackbit = 1u << 31;
    }
    s->sram = s->gen == DSPG_SKL ? 0x8000 : s->ace ? 0x180000 : 0x80000;
    s->win = s->gen == DSPG_SKL ? 0x2000 : s->ace ? 0x8000 : 0x20000;
    s->status = s->gen == DSPG_MTL ? 0x163200 : s->gen >= DSPG_LNL ? 0x160200 : s->sram;
    s->up = s->sram + 0x1000;
    s->down = s->sram + s->win;
    s->ml_lctl[0] = (1u << 16) | (1u << 23) | 2;
    sdw_realize(s, d->sdwspec);
    if (s->sdw_ext) {
        memory_region_init_io(&s->sdwwin, OBJECT(d), &sdw_win_ops, s, "intel-hda-sdw", 0x8000 * SDW_NLINKS);
        memory_region_add_subregion(&d->container, 0x30000, &s->sdwwin);
    }
    memory_region_init_io(&s->bar, OBJECT(d), &dsp_bar_ops, s, "intel-hda-dsp", s->barsize);
    pci_register_bar(&d->pci, 4, d->bar64 ? PCI_BASE_ADDRESS_MEM_TYPE_64 : 0, &s->bar);
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_tick, s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
    if (AUD_register_card("intel-hda-dsp", &d->card, errp)) {
        s->out = AUD_open_out(&d->card, NULL, "intel-hda-dsp.out", s, dsp_out_cb, &as);
        if (s->out) {
            AUD_set_active_out(s->out, true);
        }
    }
}
