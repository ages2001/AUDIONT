/*
 * AUDIONT - firmware ACPI tables (RSDP -> RSDT/XSDT -> FADT/DSDT/SSDTs/NHLT),
 * the Intel NHLT endpoint table and static AML scanning.
 *
 * The AML is not interpreted: devices are found as Device () opcodes, their
 * identification as Name (_HID / _CID / _ADR) objects and their resources as
 * resource descriptors inside the device (I2cSerialBus, SpiSerialBus,
 * GpioInt / GpioIo, Memory32Fixed, Interrupt).  This is what the sound
 * hardware of current laptops is described with (codecs and amplifiers on
 * I2C, SoundWire peripherals, Smart Sound DSP endpoints in the NHLT).
 */
#include "audiont.h"
#include "acpi.h"

#define ACPI_MAX_TABLES 64

static ACPI_TABLE g_Tables[ACPI_MAX_TABLES];
static int g_NTables;
static BOOLEAN g_AcpiDone;

#define G16(p) ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8))
#define G32(p) ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8) | ((ULONG)(p)[2] << 16) | ((ULONG)(p)[3] << 24))

static BOOLEAN SumOk(const UCHAR *p, ULONG n)
{
    UCHAR s = 0;
    ULONG i;
    for (i = 0; i < n; i++) s = (UCHAR)(s + p[i]);
    return s == 0;
}

/* maps one table; NULL when the header or the checksum is wrong (the mapping stays) */
static const UCHAR *MapTable(ULONG phys, ULONG *len)
{
    UCHAR *h, *t;
    ULONG l;
    if (!phys || phys >= 0xFFFFF000UL) return NULL;
    h = (UCHAR *)OsMapMmio(phys, 36);
    if (!h) return NULL;
    l = G32(h + 4);
    OsUnmapMmio(h, 36);
    if (l < 36 || l > 4UL * 1024 * 1024) return NULL;
    t = (UCHAR *)OsMapMmio(phys, l);
    if (!t) return NULL;
    if (!SumOk(t, l)) {
        LOGV("acpi: table %c%c%c%c at %08x has a bad checksum", t[0], t[1], t[2], t[3], phys);
        OsUnmapMmio(t, l);
        return NULL;
    }
    *len = l;
    return t;
}

static void AddTable(ULONG phys)
{
    const UCHAR *t;
    ULONG len;
    int i;
    if (g_NTables >= ACPI_MAX_TABLES) return;
    for (i = 0; i < g_NTables; i++) if (g_Tables[i].Phys == phys) return;
    t = MapTable(phys, &len);
    if (!t) return;
    g_Tables[g_NTables].Sig = G32(t);
    g_Tables[g_NTables].Va = t;
    g_Tables[g_NTables].Len = len;
    g_Tables[g_NTables].Phys = phys;
    g_NTables++;
    if (G32(t) == ACPI_SIG('F', 'A', 'C', 'P')) {       /* the FADT points at the DSDT */
        ULONG dsdt = len >= 44 ? G32(t + 40) : 0;
        if (len >= 148 && G32(t + 144) == 0 && G32(t + 140) != 0) dsdt = G32(t + 140);
        AddTable(dsdt);
    }
}

static ULONG FindRsdp(ULONG base, ULONG len)
{
    UCHAR *p = (UCHAR *)OsMapMmio(base, len);
    ULONG o, found = 0;
    if (!p) return 0;
    for (o = 0; o + 20 <= len; o += 16)
        if (!memcmp(p + o, "RSD PTR ", 8) && SumOk(p + o, 20)) { found = base + o; break; }
    OsUnmapMmio(p, len);
    return found;
}

int AcpiInit(void)
{
    ULONG rsdp = 0, root = 0, len, i;
    UCHAR *r, *bda;
    int entry = 4;
    const UCHAR *t;
    if (g_AcpiDone) return g_NTables;
    g_AcpiDone = TRUE;
    rsdp = OsRegGetDword(L"AcpiRsdp", 0);          /* for UEFI machines whose RSDP is not in the BIOS area */
    if (!rsdp && (bda = (UCHAR *)OsMapMmio(0x400, 0x100)) != NULL) {
        ULONG ebda = G16(bda + 0x0E) << 4;
        OsUnmapMmio(bda, 0x100);
        if (ebda >= 0x80000 && ebda < 0xA0000) rsdp = FindRsdp(ebda, 1024);
    }
    if (!rsdp) rsdp = FindRsdp(0xE0000, 0x20000);
    if (!rsdp) { LOGI("acpi: no RSDP found"); return 0; }
    r = (UCHAR *)OsMapMmio(rsdp, 36);
    if (!r) return 0;
    if (r[15] >= 2 && G32(r + 28) == 0 && G32(r + 24) != 0) { root = G32(r + 24); entry = 8; }
    t = root ? MapTable(root, &len) : NULL;
    if (!t) { root = G32(r + 16); entry = 4; t = MapTable(root, &len); }
    OsUnmapMmio(r, 36);
    if (!t) return 0;
    for (i = 36; i + entry <= len; i += entry) {
        if (entry == 8 && G32(t + i + 4)) continue;    /* above 4 GB */
        AddTable(G32(t + i));
    }
    LOGI("acpi: %d tables (%s at %08x)", g_NTables, entry == 8 ? "XSDT" : "RSDT", root);
    return g_NTables;
}

const ACPI_TABLE *AcpiTable(int i)
{
    return (i < 0 || i >= g_NTables) ? NULL : &g_Tables[i];
}

const ACPI_TABLE *AcpiFind(ULONG sig)
{
    int i;
    AcpiInit();
    for (i = 0; i < g_NTables; i++) if (g_Tables[i].Sig == sig) return &g_Tables[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* NHLT                                                                 */
/* ------------------------------------------------------------------ */
static NHLT_EP g_Ep[NHLT_MAX_EP];
static int g_NEp = -1;

/* format configuration: WAVEFORMATEXTENSIBLE (40 bytes) + specific config (size + blob) */
static const UCHAR *FmtAt(const NHLT_EP *e, ULONG idx)
{
    const UCHAR *p = e->Fmts;
    ULONG i;
    for (i = 0; i < idx; i++) {
        if (p + 44 > e->End) return NULL;
        p += 44 + G32(p + 40);
    }
    return (p + 44 <= e->End && p + 44 + G32(p + 40) <= e->End) ? p : NULL;
}

BOOLEAN NhltFmt(const NHLT_EP *e, ULONG i, NHLT_FMT *f)
{
    const UCHAR *p;
    if (i >= e->NFmt || !(p = FmtAt(e, i))) return FALSE;
    f->Channels = (USHORT)G16(p + 2);
    f->Rate = G32(p + 4);
    f->Bits = (USHORT)G16(p + 14);
    f->ValidBits = (USHORT)G16(p + 18);
    f->ChannelMask = G32(p + 20);
    f->BlobLen = G32(p + 40);
    f->Blob = p + 44;
    if (!f->ValidBits) f->ValidBits = f->Bits;
    return TRUE;
}

int NhltInit(void)
{
    const ACPI_TABLE *t;
    const UCHAR *p, *end;
    ULONG n, i;
    if (g_NEp >= 0) return g_NEp;
    g_NEp = 0;
    t = AcpiFind(ACPI_SIG('N', 'H', 'L', 'T'));
    if (!t || t->Len < 37) { LOGI("nhlt: no NHLT table"); return 0; }
    p = t->Va + 37;
    end = t->Va + t->Len;
    n = t->Va[36];
    for (i = 0; i < n && p + 31 <= end && g_NEp < NHLT_MAX_EP; i++) {
        ULONG elen = G32(p), clen;
        NHLT_EP *e = &g_Ep[g_NEp];
        const UCHAR *cfg, *fmt;
        if (elen < 31 || p + elen > end) break;
        memset(e, 0, sizeof(*e));
        e->LinkType = p[4];
        e->Instance = p[5];
        e->Vendor = (USHORT)G16(p + 6);
        e->Device = (USHORT)G16(p + 8);
        e->DevType = p[16];
        e->Dir = p[17];
        e->Bus = p[18];
        cfg = p + 19;
        clen = G32(cfg);
        if (cfg + 4 + clen + 1 > p + elen) { p += elen; continue; }
        if (e->LinkType == NHLT_LINK_DMIC && clen >= 3) {
            /* device specific config: virtual slot, config type, array type (vendor: + mic count) */
            UCHAR type = cfg[4 + 1], arr = cfg[4 + 2] & 0x0F;
            if (type == 1 || clen == 3) {
                e->MicArray = arr;
                e->Mics = (arr == 0x0A || arr == 0x0B) ? 2 : (arr >= 0x0C && arr <= 0x0E) ? 4 :
                          (arr == 0x0F && clen >= 4) ? cfg[4 + 3] : 2;
            }
        } else if (e->LinkType == NHLT_LINK_DMIC) e->Mics = 2;
        fmt = cfg + 4 + clen;
        e->NFmt = fmt[0];
        e->Fmts = fmt + 1;
        e->End = p + elen;
        LOGI("nhlt: endpoint %d: link %u dev %u dir %u bus %u vid %04x did %04x, %u formats%s",
             g_NEp, e->LinkType, e->DevType, e->Dir, e->Bus, e->Vendor, e->Device, e->NFmt,
             e->LinkType == NHLT_LINK_DMIC ? (e->Mics == 4 ? ", 4 mics" : ", 2 mics") : "");
        {
            NHLT_FMT f;
            ULONG k;
            for (k = 0; k < e->NFmt; k++)
                if (NhltFmt(e, k, &f))
                    LOGV("nhlt:   %u ch %u/%u bit %u Hz mask %x, blob %u bytes", f.Channels, f.ValidBits, f.Bits,
                         f.Rate, f.ChannelMask, f.BlobLen);
        }
        g_NEp++;
        p += elen;
    }
    return g_NEp;
}

const NHLT_EP *NhltEp(int i)
{
    NhltInit();
    return (i < 0 || i >= g_NEp) ? NULL : &g_Ep[i];
}

/* exact match first, then the closest: same rate, then more channels / bits */
BOOLEAN NhltFind(int linkType, int bus, int dir, ULONG rate, ULONG bits, ULONG ch, NHLT_FMT *out, const NHLT_EP **epOut)
{
    int i;
    LONG best = -1;
    NhltInit();
    for (i = 0; i < g_NEp; i++) {
        const NHLT_EP *e = &g_Ep[i];
        ULONG k;
        if (e->LinkType != linkType) continue;
        if (linkType != NHLT_LINK_DMIC && bus >= 0 && e->Bus != bus) continue;
        if (dir >= 0 && e->Dir != dir && !(linkType == NHLT_LINK_DMIC)) continue;
        for (k = 0; k < e->NFmt; k++) {
            NHLT_FMT f;
            LONG score = 0;
            if (!NhltFmt(e, k, &f) || (f.BlobLen & 3)) continue;
            if (!rate || f.Rate == rate) score += 1000; else if (f.Rate == 48000) score += 400;
            if (!bits || f.Bits == bits) score += 100; else if (f.Bits > bits) score += 50;
            if (!ch || f.Channels == ch) score += 10; else if (f.Channels > ch) score += 5;
            if (score > best) {
                best = score;
                *out = f;
                if (epOut) *epOut = e;
            }
        }
    }
    return best >= 0;
}

ULONG NhltSspMask(int devType)
{
    int i;
    ULONG m = 0;
    NhltInit();
    for (i = 0; i < g_NEp; i++)
        if (g_Ep[i].LinkType == NHLT_LINK_SSP && (devType < 0 || g_Ep[i].DevType == devType) && g_Ep[i].Bus < 32)
            m |= 1UL << g_Ep[i].Bus;
    return m;
}

ULONG NhltDmicCount(void)
{
    int i;
    ULONG n = 0;
    NhltInit();
    for (i = 0; i < g_NEp; i++)
        if (g_Ep[i].LinkType == NHLT_LINK_DMIC && g_Ep[i].Mics > n) n = g_Ep[i].Mics;
    return n;
}

/* ------------------------------------------------------------------ */
/* AML                                                                  */
/* ------------------------------------------------------------------ */
static BOOLEAN NameChar(UCHAR ch, BOOLEAN first)
{
    if ((ch >= 'A' && ch <= 'Z') || ch == '_') return TRUE;
    return !first && ch >= '0' && ch <= '9';
}

static BOOLEAN SegOk(const UCHAR *p)
{
    return NameChar(p[0], TRUE) && NameChar(p[1], FALSE) && NameChar(p[2], FALSE) && NameChar(p[3], FALSE);
}

static ULONG PkgLen(const UCHAR *p, ULONG avail, ULONG *hdr)
{
    ULONG n = p[0] >> 6, len, i;
    if (avail < n + 1) return 0;
    if (!n) { *hdr = 1; return p[0] & 0x3F; }
    len = p[0] & 0x0F;
    for (i = 0; i < n; i++) len |= (ULONG)p[1 + i] << (4 + 8 * i);
    *hdr = n + 1;
    return len;
}

/* a NameString: its length; 'last' gets the last segment, 'full' the dotted path */
static ULONG NameStr(const UCHAR *p, ULONG avail, char *last, char *full, ULONG fullSize)
{
    ULONG o = 0, segs = 1, f = 0;
    while (o < avail && (p[o] == '\\' || p[o] == '^')) o++;
    if (o >= avail) return 0;
    if (p[o] == 0x2E) { segs = 2; o++; }
    else if (p[o] == 0x2F) { if (o + 1 >= avail) return 0; segs = p[o + 1]; o += 2; }
    else if (p[o] == 0x00) return 0;
    if (!segs || o + segs * 4 > avail) return 0;
    while (segs--) {
        ULONG k;
        if (!SegOk(p + o)) return 0;
        if (full && f + 6 < fullSize) {
            if (f) full[f++] = '.';
            for (k = 0; k < 4; k++) if (p[o + k] != '_' || k == 0) full[f++] = (char)p[o + k];
            full[f] = 0;
        }
        if (!segs && last) { memcpy(last, p + o, 4); last[4] = 0; }
        o += 4;
    }
    return o;
}

/* innermost Device () or Scope () that strictly contains pos; FALSE when none */
static BOOLEAN Enclosing(const UCHAR *T, ULONG len, ULONG pos, AML_DEV *d, BOOLEAN devOnly)
{
    ULONG i;
    BOOLEAN found = FALSE;
    for (i = 36; i + 8 < len && i < pos; i++) {
        ULONG hdr, plen, nl, op;
        char name[5];
        if (T[i] == 0x5B && T[i + 1] == 0x82) op = 2;
        else if (!devOnly && T[i] == 0x10) op = 1;
        else continue;
        plen = PkgLen(T + i + op, len - i - op, &hdr);
        if (plen < hdr + 1 || i + op + plen > len || i + op + plen <= pos) continue;
        nl = NameStr(T + i + op + hdr, plen - hdr, name, NULL, 0);
        if (!nl || i + op + hdr + nl > pos) continue;
        if (!found || i > d->Start) {
            d->T = T; d->Len = len; d->Start = i;
            d->Body = i + op + hdr + nl;
            d->End = i + op + plen;
            memcpy(d->Name, name, 5);
            found = TRUE;
        }
    }
    return found;
}

BOOLEAN AmlDeviceAt(const UCHAR *T, ULONG len, ULONG pos, AML_DEV *d)
{
    return Enclosing(T, len, pos, d, TRUE);
}

BOOLEAN AmlParent(const AML_DEV *d, AML_DEV *p)
{
    return Enclosing(d->T, d->Len, d->Start, p, FALSE);
}

static void MakePath(AML_DEV *d)
{
    AML_DEV chain[8], cur = *d;
    int n = 0, i;
    ULONG o = 0;
    while (n < 8 && Enclosing(cur.T, cur.Len, cur.Start, &chain[n], FALSE)) { cur = chain[n]; n++; }
    d->Path[0] = 0;
    for (i = n - 1; i >= 0; i--) {
        ULONG k;
        /* a Scope () name may be a full path: take what the opcode says */
        const UCHAR *q = chain[i].T + chain[i].Start;
        ULONG op = q[0] == 0x10 ? 1 : 2, hdr;
        char full[40];
        PkgLen(q + op, 4, &hdr);
        full[0] = 0;
        NameStr(q + op + hdr, chain[i].Body - chain[i].Start - op - hdr, NULL, full, sizeof(full));
        if (q[op + hdr] == '\\') o = 0, d->Path[0] = 0;     /* absolute: restart */
        for (k = 0; full[k] && o + 2 < sizeof(d->Path); k++) {
            if (k == 0 && o) d->Path[o++] = '.';
            d->Path[o++] = full[k];
        }
        d->Path[o] = 0;
    }
    if (o + 6 < sizeof(d->Path)) {
        ULONG k;
        if (o) d->Path[o++] = '.';
        for (k = 0; k < 4 && d->Name[k]; k++) if (d->Name[k] != '_' || k == 0) d->Path[o++] = d->Name[k];
        d->Path[o] = 0;
    }
}

void AmlForEachDevice(AML_DEV_FN fn, void *ctx)
{
    int ti;
    const ACPI_TABLE *t;
    AcpiInit();
    for (ti = 0; (t = AcpiTable(ti)) != NULL; ti++) {
        const UCHAR *T = t->Va;
        ULONG len = t->Len, i;
        if (t->Sig != ACPI_SIG('D', 'S', 'D', 'T') && t->Sig != ACPI_SIG('S', 'S', 'D', 'T')) continue;
        for (i = 36; i + 8 < len; i++) {
            ULONG hdr, plen, nl;
            AML_DEV d;
            if (T[i] != 0x5B || T[i + 1] != 0x82) continue;
            plen = PkgLen(T + i + 2, len - i - 2, &hdr);
            if (plen < hdr + 4 || i + 2 + plen > len) continue;
            nl = NameStr(T + i + 2 + hdr, plen - hdr, d.Name, NULL, 0);
            if (!nl) continue;
            d.T = T; d.Len = len; d.Start = i;
            d.Body = i + 2 + hdr + nl;
            d.End = i + 2 + plen;
            MakePath(&d);
            if (!fn(&d, ctx)) return;
        }
    }
}

static BOOLEAN Find(const UCHAR *T, ULONG from, ULONG to, const UCHAR *pat, ULONG n, ULONG *at)
{
    ULONG i;
    for (i = from; i + n <= to; i++)
        if (T[i] == pat[0] && !memcmp(T + i, pat, n)) { if (at) *at = i; return TRUE; }
    return FALSE;
}

/* the end of the first nested Device () inside [from, to): objects past it belong to children */
static ULONG OwnEnd(const AML_DEV *d)
{
    ULONG i;
    for (i = d->Body; i + 2 < d->End; i++) {
        ULONG hdr, plen;
        if (d->T[i] != 0x5B || d->T[i + 1] != 0x82) continue;
        plen = PkgLen(d->T + i + 2, d->End - i - 2, &hdr);
        if (plen > hdr && i + 2 + plen <= d->End) return i;
    }
    return d->End;
}

static void EisaStr(const UCHAR *b, char *out)
{
    ULONG v = ((ULONG)b[0] << 8) | b[1];
    static const char hex[] = "0123456789ABCDEF";
    out[0] = (char)('@' + ((v >> 10) & 0x1F));
    out[1] = (char)('@' + ((v >> 5) & 0x1F));
    out[2] = (char)('@' + (v & 0x1F));
    out[3] = hex[b[2] >> 4]; out[4] = hex[b[2] & 15];
    out[5] = hex[b[3] >> 4]; out[6] = hex[b[3] & 15];
    out[7] = 0;
}

static void IdAt(const UCHAR *T, ULONG at, ULONG end, char *out, ULONG size)
{
    ULONG i;
    out[0] = 0;
    if (at >= end) return;
    if (T[at] == 0x0D) {
        for (i = 0; i + 1 < size && at + 1 + i < end && T[at + 1 + i] >= 0x20 && T[at + 1 + i] < 0x7F; i++)
            out[i] = (char)T[at + 1 + i];
        out[i] = 0;
    } else if (T[at] == 0x0C && at + 5 <= end && size >= 8) EisaStr(T + at + 1, out);
}

void AmlHid(const AML_DEV *d, char *out, ULONG size)
{
    static const UCHAR pat[] = { 0x08, '_', 'H', 'I', 'D' };
    ULONG at, end = OwnEnd(d);
    out[0] = 0;
    if (Find(d->T, d->Body, end, pat, 5, &at)) IdAt(d->T, at + 5, end, out, size);
}

static BOOLEAN IdEq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return FALSE;
    }
    return *a == 0 && *b == 0;
}

BOOLEAN AmlHasId(const AML_DEV *d, const char *id)
{
    static const UCHAR hid[] = { 0x08, '_', 'H', 'I', 'D' }, cid[] = { 0x08, '_', 'C', 'I', 'D' };
    ULONG at, end = OwnEnd(d), from;
    char s[16];
    if (Find(d->T, d->Body, end, hid, 5, &at)) { IdAt(d->T, at + 5, end, s, sizeof(s)); if (IdEq(s, id)) return TRUE; }
    for (from = d->Body; Find(d->T, from, end, cid, 5, &at); from = at + 5) {
        ULONG p = at + 5;
        if (d->T[p] == 0x12) {                     /* Package of ids */
            ULONG hdr, plen = PkgLen(d->T + p + 1, end - p - 1, &hdr), q, k;
            if (!plen) continue;
            q = p + 1 + hdr + 1;
            for (k = 0; k < 8 && q < p + 1 + plen; k++) {
                IdAt(d->T, q, end, s, sizeof(s));
                if (s[0] && IdEq(s, id)) return TRUE;
                q += d->T[q] == 0x0C ? 5 : d->T[q] == 0x0D ? 2 + (ULONG)strlen_s(s) : 1;
            }
        } else {
            IdAt(d->T, p, end, s, sizeof(s));
            if (IdEq(s, id)) return TRUE;
        }
    }
    return FALSE;
}

/* integer data object at p: TRUE and its value */
static BOOLEAN IntAt(const UCHAR *T, ULONG p, ULONG end, ULONGLONG *v, ULONG *size)
{
    ULONG s = 1;
    if (p >= end) return FALSE;
    switch (T[p]) {
    case 0x00: *v = 0; break;
    case 0x01: *v = 1; break;
    case 0xFF: *v = ~(ULONGLONG)0; break;
    case 0x0A: if (p + 2 > end) return FALSE; *v = T[p + 1]; s = 2; break;
    case 0x0B: if (p + 3 > end) return FALSE; *v = G16(T + p + 1); s = 3; break;
    case 0x0C: if (p + 5 > end) return FALSE; *v = G32(T + p + 1); s = 5; break;
    case 0x0E: if (p + 9 > end) return FALSE; *v = G32(T + p + 1) | ((ULONGLONG)G32(T + p + 5) << 32); s = 9; break;
    default: return FALSE;
    }
    if (size) *size = s;
    return TRUE;
}

BOOLEAN AmlNamedInt(const AML_DEV *d, const char *name4, ULONGLONG *val)
{
    UCHAR pat[5];
    ULONG at, end = OwnEnd(d);
    pat[0] = 0x08; memcpy(pat + 1, name4, 4);
    return Find(d->T, d->Body, end, pat, 5, &at) && IntAt(d->T, at + 5, end, val, NULL);
}

BOOLEAN AmlAdr(const AML_DEV *d, ULONGLONG *adr)
{
    return AmlNamedInt(d, "_ADR", adr);
}

/* Package () { "name", value } anywhere in the device (its _DSD and data packages) */
BOOLEAN AmlDsdInt(const AML_DEV *d, const char *prop, ULONGLONG *val)
{
    UCHAR pat[64];
    ULONG n = 0, at, from = d->Body, end = d->End;
    pat[n++] = 0x0D;
    while (*prop && n < 62) pat[n++] = (UCHAR)*prop++;
    pat[n++] = 0;
    while (Find(d->T, from, end, pat, n, &at)) {
        if (IntAt(d->T, at + n, end, val, NULL)) return TRUE;
        from = at + 1;
    }
    return FALSE;
}

/* one package element at p: its size in bytes, 0 when it cannot be parsed */
static ULONG ElemAt(const UCHAR *T, ULONG p, ULONG end, AML_ELEM *e)
{
    ULONG size, n, hdr, plen;
    memset(e, 0, sizeof(*e));
    if (p >= end) return 0;
    if (IntAt(T, p, end, &e->Int, &size)) { e->Kind = AML_EL_INT; return size; }
    if (T[p] == 0x0D) {
        for (n = 0; p + 1 + n < end && T[p + 1 + n]; n++) if (n < sizeof(e->Name) - 1) e->Name[n] = (char)T[p + 1 + n];
        e->Kind = AML_EL_STR;
        return p + 1 + n < end ? n + 2 : 0;
    }
    if (T[p] == 0x12 || T[p] == 0x13 || T[p] == 0x11) {          /* nested package / buffer: skipped */
        plen = PkgLen(T + p + 1, end - p - 1, &hdr);
        e->Kind = AML_EL_OTHER;
        return plen && p + 1 + plen <= end ? plen + 1 : 0;
    }
    n = NameStr(T + p, end - p, e->Name, NULL, 0);
    if (n) e->Kind = AML_EL_REF;
    return n;
}

ULONG AmlDsdPkg(const AML_DEV *d, const char *prop, AML_ELEM *e, ULONG max)
{
    UCHAR pat[64];
    ULONG n = 0, at, from = d->Body, end = d->End;
    pat[n++] = 0x0D;
    while (*prop && n < 62) pat[n++] = (UCHAR)*prop++;
    pat[n++] = 0;
    while (Find(d->T, from, end, pat, n, &at)) {
        const UCHAR *T = d->T;
        ULONG p = at + n, cnt = 0;
        from = at + 1;
        if (p < end && (T[p] == 0x12 || T[p] == 0x13)) {
            ULONG hdr, plen = PkgLen(T + p + 1, end - p - 1, &hdr), pend, q, num, sz;
            ULONGLONG vnum;
            if (!plen || p + 1 + plen > end) continue;
            pend = p + 1 + plen;
            q = p + 1 + hdr;
            if (T[p] == 0x12) { num = T[q]; q++; }
            else { if (!IntAt(T, q, pend, &vnum, &sz)) continue; num = (ULONG)vnum; q += sz; }
            while (q < pend && cnt < num && cnt < max) {
                sz = ElemAt(T, q, pend, &e[cnt]);
                if (!sz) break;
                q += sz;
                cnt++;
            }
        } else if (max && ElemAt(T, p, end, &e[0]) && e[0].Kind != AML_EL_OTHER) cnt = 1;
        if (cnt) return cnt;
    }
    return 0;
}

BOOLEAN AmlNamedStr(const AML_DEV *d, const char *name4, char *out, ULONG size)
{
    const UCHAR *T = d->T;
    UCHAR pat[5];
    ULONG at, end = OwnEnd(d), p = 0, n, i;
    out[0] = 0;
    pat[0] = 0x08; memcpy(pat + 1, name4, 4);
    if (Find(T, d->Body, end, pat, 5, &at) && T[at + 5] == 0x0D) p = at + 5;
    else {
        /* Method (XXXX, ..) { Return ("...") }: 14 pkglen name flags ... A4 0D */
        for (i = d->Body; i + 8 < end && !p; i++) {
            ULONG hdr, plen, k;
            if (T[i] != 0x14) continue;
            plen = PkgLen(T + i + 1, end - i - 1, &hdr);
            if (!plen || i + 1 + plen > end || memcmp(T + i + 1 + hdr, name4, 4)) continue;
            for (k = i + 1 + hdr + 5; k + 2 < i + 1 + plen; k++)
                if (T[k] == 0xA4 && T[k + 1] == 0x0D) { p = k + 1; break; }
        }
    }
    if (!p) return FALSE;
    for (n = 0; p + 1 + n < end && T[p + 1 + n] >= 0x20 && T[p + 1 + n] < 0x7F && n + 1 < size; n++)
        out[n] = (char)T[p + 1 + n];
    out[n] = 0;
    return n != 0;
}

BOOLEAN AmlChildByAdr(const AML_DEV *d, ULONGLONG adr, AML_DEV *child)
{
    ULONG i;
    for (i = d->Body; i + 8 < d->End; i++) {
        ULONG hdr, plen, nl;
        AML_DEV c;
        ULONGLONG a;
        if (d->T[i] != 0x5B || d->T[i + 1] != 0x82) continue;
        plen = PkgLen(d->T + i + 2, d->End - i - 2, &hdr);
        if (plen < hdr + 4 || i + 2 + plen > d->End) continue;
        nl = NameStr(d->T + i + 2 + hdr, plen - hdr, c.Name, NULL, 0);
        if (!nl) continue;
        c.T = d->T; c.Len = d->Len; c.Start = i; c.Body = i + 2 + hdr + nl; c.End = i + 2 + plen;
        if (AmlAdr(&c, &a) && a == adr) { MakePath(&c); *child = c; return TRUE; }
    }
    return FALSE;
}

/* last name segment of a resource source string ("\\_SB.PCI0.I2C1" -> I2C1) */
static void SrcName(const UCHAR *T, ULONG s, ULONG end, char *out)
{
    ULONG seg = s, k, n, m;
    out[0] = 0;
    for (k = s; k < end && T[k]; k++) if (T[k] == '.' || T[k] == '\\' || T[k] == '^') seg = k + 1;
    n = k - seg;
    if (k >= end || n < 1 || n > 4) return;
    for (m = 0; m < 4; m++) out[m] = (char)(m < n ? T[seg + m] : '_');
    out[4] = 0;
    if (!SegOk((const UCHAR *)out)) out[0] = 0;
}

void AmlResources(const AML_DEV *d, AML_RES *r)
{
    const UCHAR *T = d->T;
    ULONG i, end = OwnEnd(d);
    memset(r, 0, sizeof(*r));
    for (i = d->Body; i + 12 < end; i++) {
        ULONG dl;
        if (T[i] == 0x8E && (T[i + 3] == 1 || T[i + 3] == 2) && (dl = G16(T + i + 1)) >= 15 && i + 3 + dl <= end) {
            ULONG tl = G16(T + i + 10);
            if (T[i + 5] == 1 && tl >= 6 && r->NI2c < 4) {              /* I2C */
                AML_I2C *c = &r->I2c[r->NI2c++];
                c->Speed = G32(T + i + 12);
                c->Addr = (USHORT)(G16(T + i + 16) & 0x3FF);
                SrcName(T, i + 12 + tl, i + 3 + dl, c->Ctrl);
            } else if (T[i + 5] == 2 && tl >= 9 && r->NSpi < 4) {        /* SPI */
                r->SpiSpeed[r->NSpi] = G32(T + i + 12);
                r->SpiBits[r->NSpi] = T[i + 16];
                /* type specific flags: wire mode 0, device polarity 1; clock phase 17, polarity 18 */
                r->SpiMode[r->NSpi] = (UCHAR)((T[i + 17] ? AML_SPI_CPHA : 0) | (T[i + 18] ? AML_SPI_CPOL : 0) |
                                              ((T[i + 7] & 2) ? AML_SPI_CS_HIGH : 0) | ((T[i + 7] & 1) ? AML_SPI_3WIRE : 0));
                r->SpiCs[r->NSpi] = (USHORT)G16(T + i + 19);
                SrcName(T, i + 12 + tl, i + 3 + dl, r->SpiCtrl[r->NSpi]);
                r->NSpi++;
            }
            i += 2 + dl;
            continue;
        }
        if (T[i] == 0x8C && T[i + 3] <= 2 && T[i + 4] <= 1 && (dl = G16(T + i + 1)) >= 20 && i + 3 + dl <= end) {
            ULONG pt = G16(T + i + 14), rn = G16(T + i + 17);
            if (pt >= 23 && pt + 2 <= dl + 3 && r->NGpio < 8) {
                AML_GPIO *g = &r->Gpio[r->NGpio++];
                g->Pin = (USHORT)G16(T + i + pt);
                g->Int = T[i + 4] == 0;
                g->Pull = T[i + 9];
                if (rn && rn < dl + 3) SrcName(T, i + rn, i + 3 + dl, g->Ctrl);
            }
            i += 2 + dl;
            continue;
        }
        if (T[i] == 0x86 && T[i + 1] == 9 && T[i + 2] == 0 && r->NMem < 8 && G32(T + i + 8)) {
            r->MemBase[r->NMem] = G32(T + i + 4);
            r->MemLen[r->NMem] = G32(T + i + 8);
            r->NMem++;
            i += 11;
            continue;
        }
        if (T[i] == 0x89 && (dl = G16(T + i + 1)) >= 6 && i + 3 + dl <= end && T[i + 4] >= 1 && T[i + 4] <= 8) {
            if (r->NIrq < 8) r->Irq[r->NIrq++] = G32(T + i + 5);
            i += 2 + dl;
            continue;
        }
    }
}

/* strlen for IdAt users (no C run time here) */
ULONG strlen_s(const char *s)
{
    ULONG n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp_s(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(UCHAR)*a - (int)(UCHAR)*b;
}

/* ------------------------------------------------------------------ */
/* run-time _CRS bases: Memory32Fixed (..., 0, len, _Yxx) patched by the _CRS method from a  */
/* firmware NVS field: CreateDWordField (RBUF, <_BAS offset>, B0BA); B0BA = LPE0 (Store),     */
/* LPE0 a Field unit of an OperationRegion (SystemMemory, <constant>, ...)                     */
/* ------------------------------------------------------------------ */
static BOOLEAN SegAt(const UCHAR *T, ULONG p, ULONG end, char *seg, ULONG *len)
{
    ULONG o = p;
    while (o < end && (T[o] == '\\' || T[o] == '^')) o++;
    if (o + 4 > end || !SegOk(T + o)) return FALSE;
    memcpy(seg, T + o, 4); seg[4] = 0;
    if (len) *len = o + 4 - p;
    return TRUE;
}

/* Name (SEG, integer) anywhere in the tables */
static BOOLEAN GlobalInt(const char *seg, ULONGLONG *v)
{
    int t;
    for (t = 0; t < g_NTables; t++) {
        const UCHAR *T = g_Tables[t].Va;
        ULONG i, len = g_Tables[t].Len;
        if (g_Tables[t].Sig != ACPI_SIG('D', 'S', 'D', 'T') && g_Tables[t].Sig != ACPI_SIG('S', 'S', 'D', 'T')) continue;
        for (i = 36; i + 6 < len; i++)
            if (T[i] == 0x08 && !memcmp(T + i + 1, seg, 4) && IntAt(T, i + 5, len, v, NULL)) return TRUE;
    }
    return FALSE;
}

/* a Field unit: its OperationRegion's SystemMemory address and width in bits */
static BOOLEAN FieldAddr(const char *unit, ULONG *pa, ULONG *bits)
{
    int t;
    for (t = 0; t < g_NTables; t++) {
        const UCHAR *T = g_Tables[t].Va;
        ULONG i, len = g_Tables[t].Len;
        if (g_Tables[t].Sig != ACPI_SIG('D', 'S', 'D', 'T') && g_Tables[t].Sig != ACPI_SIG('S', 'S', 'D', 'T')) continue;
        for (i = 36; i + 8 < len; i++) {
            ULONG hdr, plen, nl, p, end, bit = 0;
            char region[5];
            if (T[i] != 0x5B || T[i + 1] != 0x81) continue;
            plen = PkgLen(T + i + 2, len - i - 2, &hdr);
            if (!plen || i + 2 + plen > len) continue;
            end = i + 2 + plen;
            p = i + 2 + hdr;
            if (!SegAt(T, p, end, region, &nl)) continue;
            p += nl + 1;                                           /* field flags */
            while (p < end) {
                UCHAR c = T[p];
                if (c == 0x00) {                                   /* ReservedField */
                    ULONG h2 = 1, w = PkgLen(T + p + 1, end - p - 1, &h2);
                    bit += w; p += 1 + h2;
                } else if (c == 0x01) p += 3;                      /* AccessField */
                else if (c == 0x03) p += 4;                        /* ExtendedAccessField */
                else if (c == 0x02) break;                         /* ConnectField: give up on this list */
                else if (SegOk(T + p)) {
                    ULONG h2 = 1, w = PkgLen(T + p + 4, end - p - 4, &h2);
                    if (!memcmp(T + p, unit, 4)) {
                        /* the region */
                        ULONG k;
                        for (k = 36; k + 8 < len; k++) {
                            ULONGLONG base = 0;
                            ULONG sz = 0;
                            char rn[5], ref[5];
                            if (T[k] != 0x5B || T[k + 1] != 0x80 || !SegAt(T, k + 2, len, rn, &nl) || memcmp(rn, region, 4))
                                continue;
                            if (T[k + 2 + nl] != 0) return FALSE;    /* not SystemMemory */
                            if (!IntAt(T, k + 3 + nl, len, &base, &sz)) {
                                if (!SegAt(T, k + 3 + nl, len, ref, NULL) || !GlobalInt(ref, &base)) return FALSE;
                            }
                            if (!base || base >= 0xFFFFFFFFULL) return FALSE;
                            *pa = (ULONG)base + bit / 8;
                            *bits = w;
                            return (bit & 7) == 0;
                        }
                        return FALSE;
                    }
                    bit += w; p += 4 + h2;
                } else break;
            }
        }
    }
    return FALSE;
}

ULONG AmlCrsMemBase(const AML_DEV *d, ULONG index)
{
    const UCHAR *T = d->T;
    ULONG i, end = OwnEnd(d), n = 0, desc = 0, bufData = 0, k;
    char bufName[5] = { 0 }, fld[5] = { 0 }, src[5] = { 0 };
    /* the index-th Memory32Fixed and the Name () of its buffer */
    for (i = d->Body; i + 12 < end; i++)
        if (T[i] == 0x86 && T[i + 1] == 9 && T[i + 2] == 0 && n++ == index) { desc = i; break; }
    if (!desc) return 0;
    for (k = desc; k > d->Body + 6 && desc - k < 4096; k--) {
        ULONG hdr, plen, s2;
        ULONGLONG bsz;
        if (T[k] != 0x11) continue;
        plen = PkgLen(T + k + 1, end - k - 1, &hdr);
        if (!plen || k + 1 + plen <= desc || !IntAt(T, k + 1 + hdr, end, &bsz, &s2)) continue;
        bufData = k + 1 + hdr + s2;
        if (k >= 5 && T[k - 5] == 0x08 && SegOk(T + k - 4)) { memcpy(bufName, T + k - 4, 4); bufName[4] = 0; }
        break;
    }
    if (!bufData || !bufName[0]) return 0;
    /* CreateDWordField (bufName, desc - bufData + 4, fld) */
    for (i = d->Body; i + 10 < end && !fld[0]; i++) {
        ULONG nl, isz;
        ULONGLONG idx;
        char b[5];
        if (T[i] != 0x8A || !SegAt(T, i + 1, end, b, &nl) || memcmp(b, bufName, 4)) continue;
        if (!IntAt(T, i + 1 + nl, end, &idx, &isz) || idx != desc - bufData + 4) continue;
        SegAt(T, i + 1 + nl + isz, end, fld, NULL);
    }
    if (!fld[0]) return 0;
    /* Store (src, fld): 70 <src> <fld>; an integer source is a constant base */
    for (i = d->Body; i + 9 < end; i++) {
        ULONG nl;
        ULONGLONG v;
        ULONG isz;
        if (T[i] != 0x70) continue;
        if (IntAt(T, i + 1, end, &v, &isz)) {
            char f2[5];
            if (SegAt(T, i + 1 + isz, end, f2, NULL) && !memcmp(f2, fld, 4) && v) return (ULONG)v;
            continue;
        }
        if (SegAt(T, i + 1, end, src, &nl)) {
            char f2[5];
            if (SegAt(T, i + 1 + nl, end, f2, NULL) && !memcmp(f2, fld, 4)) break;
        }
        src[0] = 0;
    }
    if (!src[0]) return 0;
    {
        ULONG pa, bits, v = 0;
        ULONGLONG gv;
        PUCHAR va;
        if (GlobalInt(src, &gv)) return (ULONG)gv;
        if (!FieldAddr(src, &pa, &bits) || bits > 32 || bits < 8) return 0;
        va = (PUCHAR)OsMapMmio(pa & ~0xFFFUL, 0x2000);
        if (!va) return 0;
        memcpy(&v, va + (pa & 0xFFF), bits / 8);
        OsUnmapMmio(va, 0x2000);
        LOGV("acpi: %s %s: %s = %s at %08x -> %08x", d->Path, bufName, fld, src, pa, v);
        return v;
    }
}
