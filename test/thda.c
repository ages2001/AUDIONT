/*
 * thda - host test of the HDA codec parser against real codec dumps.
 *
 * The codec is simulated with state (power, pin control, EAPD, amps, connection
 * select, converter stream, vendor coefficients, GPIO).  After the driver's codec
 * set-up the output pins are checked for an audible path from the stream:
 *   DAC on the stream tag with a format, every node on the way powered (when it
 *   has power control) and unmuted, selectors pointing the right way, pin output
 *   enabled, EAPD on, and the Realtek rules: analog PLL gating off, EAPD under verb
 *   control, external amplifier GPIO high.
 *
 *   thda table [verbose]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define HDA_HOSTTEST
#include "../drv/hda.c"

static ULONG V_vendor, V_rev, V_sub, D_in, D_out;
typedef struct {
    ULONG nid, wc, pc, cfg, ai, ao, nc;
    ULONG c[64];
    /* state */
    ULONG pwr, pinctl, eapd, sel, tag, fmt;
    UCHAR inmute[16][2], outmute[2];
} MN;
static MN g_N[256]; static int g_nN; static int g_verbose;
static ULONG g_AfgPwr = 3, g_GpioMask, g_GpioDir, g_GpioData;
static ULONG g_CoefIdx[256];
typedef struct { ULONG nid, idx, val; } COEF;
static COEF g_Coef[512]; static int g_nCoef;
static ULONG g_SubDev;                   /* PCI subsystem device of the "board" */

static MN *FindN(ULONG nid) { int i; for (i = 0; i < g_nN; i++) if (g_N[i].nid == nid) return &g_N[i]; return NULL; }

static ULONG *CoefP(ULONG nid, ULONG idx)
{
    int i;
    for (i = 0; i < g_nCoef; i++) if (g_Coef[i].nid == nid && g_Coef[i].idx == idx) return &g_Coef[i].val;
    g_Coef[g_nCoef].nid = nid; g_Coef[g_nCoef].idx = idx; g_Coef[g_nCoef].val = 0;
    return &g_Coef[g_nCoef++].val;
}

/* ---- Realtek rules: bits that must end up in a given state ---- */
typedef struct { ULONG nid, idx, mask, want; } RULE;
static RULE g_Rule[16]; static int g_nRule;
static void Rule(ULONG nid, ULONG idx, ULONG mask, ULONG want)
{
    g_Rule[g_nRule].nid = nid; g_Rule[g_nRule].idx = idx; g_Rule[g_nRule].mask = mask; g_Rule[g_nRule].want = want;
    *CoefP(nid, idx) = (*CoefP(nid, idx) & ~mask) | (~want & mask);      /* power-on default: the wrong way */
    g_nRule++;
}

static void SetupRules(void)
{
    ULONG id = V_vendor & 0xFFFF;
    MN *v = FindN(0x20);
    if ((V_vendor >> 16) != 0x10EC || !v || ((v->wc >> 20) & 0xF) != 0xF) return;
    *CoefP(0x20, 0) = (V_rev & 0xFF) | 0x8000;    /* coef 0: chip revision (the variant bits stay 0) */
    switch (id) {                                 /* analog PLL gating */
    case 0x0272: case 0x0273: case 0x0662: case 0x0663: case 0x0665: case 0x0667: case 0x0668: case 0x0670:
    case 0x0671: case 0x0672: case 0x0867: case 0x0892: case 0x0897: Rule(0x20, 0x04, 1 << 15, 0); break;
    case 0x0883: case 0x0887: case 0x0888: case 0x0889: case 0x0899: case 0x1168: Rule(0x20, 0x0A, 1 << 10, 0); break;
    }
    switch (id) {                                 /* EAPD by verb */
    case 0x0892: case 0x0897: Rule(0x20, 0x7, 1 << 5, 0); break;
    case 0x0899: case 0x0900: case 0x0b00: case 0x1168: case 0x1220: Rule(0x20, 0x7, 1 << 1, 0); break;
    case 0x0230: case 0x0233: case 0x0235: case 0x0236: case 0x0245: case 0x0255: case 0x0256: case 0x0257:
    case 0x0282: case 0x0283: case 0x0286: case 0x0288: case 0x0298: case 0x0300: Rule(0x20, 0x10, 1 << 9, 0); break;
    case 0x0280: case 0x0284: case 0x0290: case 0x0292: Rule(0x20, 0x4, 1 << 15, 0); break;
    case 0x0867: Rule(0x20, 0x4, 1 << 10, 0); break;
    case 0x0262: Rule(0x20, 0x7, 1 << 5, 1 << 5); break;
    case 0x0272: case 0x0663: case 0x0665: case 0x0670: case 0x0671: Rule(0x20, 0xD, 1 << 14, 1 << 14); break;
    }
}

/* SKU external amp GPIO the board needs (same decoding as Linux alc_subsystem_id) */
static ULONG NeededGpio(void)
{
    ULONG ass = V_sub & 0xFFFF, n = 0, i;
    MN *p;
    if ((V_vendor >> 16) != 0x10EC) return 0;
    if (!(ass != g_SubDev && (ass & 1))) {
        p = FindN((V_vendor & 0xFFFF) == 0x0260 ? 0x17 : 0x1D);
        if (!p) return 0;
        ass = p->cfg;
        if (!(ass & 1) || (ass >> 30) != 1) return 0;
        for (i = 1; i < 16; i++) if ((ass >> i) & 1) n++;
        if (((ass >> 16) & 0xF) != n) return 0;
    }
    switch ((ass & 0x38) >> 3) { case 1: return 1; case 3: return 2; case 7: return 4; }
    return 0;
}

ULONG HostCmd(HDA_STATE *h, USHORT nid, ULONG verb, ULONG pl)
{
    MN *n = FindN(nid);
    (void)h;
    if (verb == V_GET_PARAM) {
        if (nid == 0) {
            if (pl == P_VENDOR) return V_vendor;
            if (pl == P_REV) return V_rev;
            if (pl == P_NODES) return (1 << 16) | 1;
            return 0;
        }
        if (nid == 1) {
            ULONG mn = 255, mx = 0; int i;
            if (pl == P_FGTYPE) return 1;
            if (pl == P_INAMP) return D_in;
            if (pl == P_OUTAMP) return D_out;
            if (pl == P_PCM) return 0x000e07f0;
            if (pl == P_GPIO) return 0x40000003;
            if (pl == P_NODES) { for (i = 0; i < g_nN; i++) { if (g_N[i].nid < mn) mn = g_N[i].nid; if (g_N[i].nid > mx) mx = g_N[i].nid; } return (mn << 16) | (mx - mn + 1); }
            return 0;
        }
        if (!n) return 0;
        switch (pl) {
        case P_WCAPS: return n->wc;
        case P_PINCAPS: return n->pc;
        case P_INAMP: return n->ai;
        case P_OUTAMP: return n->ao;
        case P_PCM: return 0x000e07f0;
        case P_CONNLEN: { int lf = 0, i; for (i = 0; i < (int)n->nc; i++) if (n->c[i] > 0x7f) lf = 1; return n->nc | (lf << 7); }
        }
        return 0;
    }
    if (g_verbose && verb != V_GET_PARAM && (verb < 0xF00 || verb == V_GET_PROC_COEF))
        printf("    nid %02x verb %03x payload %04x\n", nid, verb, pl);
    if (nid == 1) {
        switch (verb) {
        case 0xF20: return V_sub;
        case V_SET_POWER: g_AfgPwr = pl & 0xF; return 0;
        case V_GET_POWER: return g_AfgPwr | (g_AfgPwr << 4);
        case V_SET_GPIO_MASK: g_GpioMask = pl & 0xFF; return 0;
        case V_SET_GPIO_DIR: g_GpioDir = pl & 0xFF; return 0;
        case V_SET_GPIO_DATA: g_GpioData = pl & 0xFF; return 0;
        }
        return 0;
    }
    if (!n) return 0;
    switch (verb) {
    case V_GET_CONFIG: return n->cfg;
    case V_GET_CONN_LIST: {
        int lf = 0, i; ULONG r = 0;
        for (i = 0; i < (int)n->nc; i++) if (n->c[i] > 0x7f) lf = 1;
        if (lf) { for (i = 0; i < 2 && pl + i < n->nc; i++) r |= n->c[pl + i] << (16 * i); }
        else    { for (i = 0; i < 4 && pl + i < n->nc; i++) r |= n->c[pl + i] << (8 * i); }
        return r;
    }
    case V_SET_POWER: n->pwr = pl & 0xF; return 0;
    case V_GET_POWER: return n->pwr | (n->pwr << 4);
    case V_SET_PIN_CTL: n->pinctl = pl & 0xFF; return 0;
    case V_GET_PIN_CTL: return n->pinctl;
    case V_SET_EAPD: n->eapd = pl & 0xFF; return 0;
    case V_GET_EAPD: return n->eapd;
    case V_SET_CONN_SEL: n->sel = pl & 0xFF; return 0;
    case V_GET_CONN_SEL: return n->sel;
    case V_SET_CONV: n->tag = (pl >> 4) & 0xF; return 0;
    case V_GET_SENSE: return 0;
    case V_SET_SENSE: return 0;
    case V_SET_FMT: n->fmt = pl; return 0;
    case V_SET_AMP: {
        ULONG idx = (pl >> 8) & 0xF, m = (pl >> 7) & 1, ch;
        for (ch = 0; ch < 2; ch++) {
            if (!(pl & (ch ? 0x1000 : 0x2000))) continue;
            if (pl & 0x8000) n->outmute[ch] = (UCHAR)m;
            if (pl & 0x4000) n->inmute[idx][ch] = (UCHAR)m;
        }
        return 0;
    }
    case V_SET_COEF_IDX: g_CoefIdx[nid] = pl; return 0;
    case V_SET_PROC_COEF: *CoefP(nid, g_CoefIdx[nid]) = pl & 0xFFFF; return 0;
    case V_GET_PROC_COEF: return *CoefP(nid, g_CoefIdx[nid]);
    }
    return 0;
}

static ULONG Type(MN *n) { return (n->wc >> 20) & 0xF; }
static int Powered(MN *n) { return !(n->wc & WC_POWERCTL) || n->pwr == 0; }
static int OutOpen(MN *n) { return !(n->wc & WC_OUTAMP) || !n->outmute[0]; }
static int InOpen(MN *n, ULONG i) { return !(n->wc & WC_INAMP) || !n->inmute[i < 16 ? i : 15][0]; }

static int Carries(MN *n, int depth)
{
    ULONG i;
    if (!n || depth > 12 || !Powered(n)) return 0;
    switch (Type(n)) {
    case W_OUT:
        return n->tag == OUT_TAG && n->fmt != 0 && OutOpen(n);
    case W_MIX:
        for (i = 0; i < n->nc; i++)
            if (InOpen(n, i) && Carries(FindN(n->c[i]), depth + 1)) return OutOpen(n);
        return 0;
    case W_SEL: case W_PIN: {
        ULONG k = n->nc > 1 ? n->sel : 0;
        if (k >= n->nc) return 0;
        if (Type(n) == W_SEL && (n->wc & WC_INAMP) && n->inmute[k < 16 ? k : 15][0] && n->inmute[0][0]) return 0;
        return Carries(FindN(n->c[k]), depth + 1) && OutOpen(n);
    }
    }
    return 0;
}

static int RulesOk(void)
{
    int i, ok = 1;
    for (i = 0; i < g_nRule; i++)
        if ((*CoefP(g_Rule[i].nid, g_Rule[i].idx) & g_Rule[i].mask) != g_Rule[i].want) {
            if (g_verbose) printf("  rule: coef %02x[%02x] mask %04x not %04x\n", g_Rule[i].nid, g_Rule[i].idx,
                                  g_Rule[i].mask, g_Rule[i].want);
            ok = 0;
        }
    return ok;
}

/* stubs */
void OsLog(ULONG l, const char *f, ...) { if (g_verbose > 1) { va_list ap; va_start(ap, f); vprintf(f, ap); va_end(ap); printf("\n"); } }
int OsSprintf(char *b, int s, const char *f, ...) { va_list ap; int n; va_start(ap, f); n = vsnprintf(b, s, f, ap); va_end(ap); return n; }
ULONG OsRegGetDword(const WCHAR *n, ULONG d) { return d; }
ULONG VolToDb100(USHORT v) { return v ? 0 : 100000; }
void OsStallUs(ULONG us) {}
void OsSleepMs(ULONG ms) {}
void EngineLock(CARD *c) {} void EngineUnlock(CARD *c) {}
PVOID OsMapMmio(ULONG pa, ULONG sz) { return 0; } void OsUnmapMmio(PVOID v, ULONG s) {}
BOOLEAN OsAllocDma(DMA_BUFFER *b, ULONG s, BOOLEAN u) { return 0; }
void PciWrite32(ULONG b, ULONG d, ULONG f, ULONG o, ULONG v) {}
ULONG OsTimeMs(void) { return 0; }
PVOID OsAlloc(ULONG n) { return calloc(1, n); }
void OsFree(PVOID p) { free(p); }
ULONG PciRead32(ULONG b, ULONG d, ULONG f, ULONG o) { return 0; }
USHORT PciRead16(ULONG b, ULONG d, ULONG f, ULONG o) { return 0; }
UCHAR PciRead8(ULONG b, ULONG d, ULONG f, ULONG o) { return 0; }
void PciWrite8(ULONG b, ULONG d, ULONG f, ULONG o, UCHAR v) {}
void PciWrite16(ULONG b, ULONG d, ULONG f, ULONG o, USHORT v) {}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "r"); char line[4096]; CARD c; HDA_STATE h; int i, audible = 0, pins = 0, primOk = 0;
    ULONG gpio;
    if (!f) return 2;
    g_verbose = argc > 2 ? atoi(argv[2]) : 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'V') sscanf(line + 2, "%lx %lx %lx", &V_vendor, &V_rev, &V_sub);
        else if (line[0] == 'D') sscanf(line + 2, "%lu %lu", &D_in, &D_out);
        else if (line[0] == 'N') {
            MN *n = &g_N[g_nN++]; char *p = line + 2; int k, off;
            memset(n, 0, sizeof(*n));
            sscanf(p, "%lu %lu %lu %lu %lu %lu %lu%n", &n->nid, &n->wc, &n->pc, &n->cfg, &n->ai, &n->ao, &n->nc, &off);
            p += off;
            for (k = 0; k < (int)n->nc && k < 64; k++) { n->c[k] = strtoul(p, &p, 10); }
            n->pwr = (n->wc & WC_POWERCTL) ? 3 : 0;
            memset(n->inmute, 1, sizeof(n->inmute)); memset(n->outmute, 1, sizeof(n->outmute));
        }
    }
    g_SubDev = 0x1234;                           /* board SSID differs from the codec's */
    SetupRules();
    memset(&c, 0, sizeof c); memset(&h, 0, sizeof h);
    c.Hda = &h;
    c.SubDevice = (USHORT)g_SubDev;
    if (!ProbeCodec(&c, &h, 0)) { printf("RESULT %s: NO-OUTPUT\n", argv[1]); return 1; }
    SetupLoopback(&c, &h);
    SetupInputs(&c, &h);
    ConfigConverters(&h);
    gpio = NeededGpio();

    /* audibility of every output pin the driver uses */
    for (i = 0; i < (int)h.NOut; i++) {
        MN *p = FindN(h.OutPath[i].Nid[0]);
        int ok;
        if (h.OutDev[i] == DEV_SPEAKER && h.HpPresent) continue;
        ok = g_AfgPwr == 0 && Carries(p, 0) && (p->pinctl & 0x40) && (!(p->pc & PC_EAPD) || (p->eapd & 2)) &&
             RulesOk() && (g_GpioData & g_GpioMask & g_GpioDir & gpio) == gpio;
        pins++;
        if (ok) { audible++; if (i == 0) primOk = 1; }
        if (g_verbose) printf("  pin %02x (%s): %s\n", p->nid, DevName(h.OutDev[i]), ok ? "audible" : "SILENT");
    }
    printf("RESULT %s: %s | %s %d/%d | out", argv[1], c.Codec, primOk && audible == pins ? "AUDIBLE" :
           audible ? "PARTIAL" : "SILENT", audible, pins);
    for (i = 0; i < (int)h.NOut; i++) { int k; printf(" %s:", DevName(h.OutDev[i])); for (k = 0; k < h.OutPath[i].N; k++) printf("%02x%s", h.OutPath[i].Nid[k], k + 1 < h.OutPath[i].N ? ">" : ""); }
    printf(" | rec adc %02x src %lx gpio %lx\n", h.Adc, c.RecSrcMask, gpio);
    return 0;
}
