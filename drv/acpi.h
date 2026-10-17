/*
 * AUDIONT - firmware ACPI tables, the Intel NHLT table and a small static
 * AML scanner (Windows NT 3.x / 4.0 have no ACPI of their own; on 2000 / XP
 * the same code reads the tables the firmware left in memory).
 */
#ifndef AUDIONT_ACPI_H
#define AUDIONT_ACPI_H

#define ACPI_SIG(a, b, c, d) ((ULONG)(a) | ((ULONG)(b) << 8) | ((ULONG)(c) << 16) | ((ULONG)(d) << 24))

typedef struct _ACPI_TABLE {
    ULONG        Sig;
    const UCHAR *Va;
    ULONG        Len;
    ULONG        Phys;
} ACPI_TABLE;

int               AcpiInit(void);                 /* number of tables found */
const ACPI_TABLE *AcpiTable(int index);
const ACPI_TABLE *AcpiFind(ULONG sig);

/* ------------------------------------------------------------------ */
/* NHLT (Non HD Audio Link Table): the DSP endpoints of Intel Smart Sound  */
/* ------------------------------------------------------------------ */
#define NHLT_LINK_HDA   0
#define NHLT_LINK_DSP   1
#define NHLT_LINK_DMIC  2
#define NHLT_LINK_SSP   3
#define NHLT_DEV_BT     0
#define NHLT_DEV_DMIC   1
#define NHLT_DEV_I2S    4
#define NHLT_DIR_RENDER  0
#define NHLT_DIR_CAPTURE 1

typedef struct _NHLT_FMT {
    USHORT Channels;
    USHORT Bits;              /* container */
    USHORT ValidBits;
    ULONG  Rate;
    ULONG  ChannelMask;
    const UCHAR *Blob;        /* specific configuration (gateway blob) */
    ULONG  BlobLen;
} NHLT_FMT;

typedef struct _NHLT_EP {
    UCHAR  LinkType;
    UCHAR  Instance;
    UCHAR  DevType;
    UCHAR  Dir;
    UCHAR  Bus;               /* virtual bus id: SSP port */
    UCHAR  MicArray;          /* DMIC array type (0x0A..0x0F) */
    UCHAR  Mics;              /* microphones (DMIC) */
    UCHAR  NFmt;
    USHORT Vendor, Device;
    const UCHAR *Fmts;        /* first format configuration */
    const UCHAR *End;
} NHLT_EP;

#define NHLT_MAX_EP 24
int   NhltInit(void);                              /* number of endpoints */
const NHLT_EP *NhltEp(int i);
BOOLEAN NhltFmt(const NHLT_EP *e, ULONG i, NHLT_FMT *f);
/* the best format configuration of an endpoint for rate / bits / channels (0 = any) */
BOOLEAN NhltFind(int linkType, int bus, int dir, ULONG rate, ULONG bits, ULONG ch, NHLT_FMT *f, const NHLT_EP **ep);
ULONG NhltSspMask(int devType);                    /* SSP ports with endpoints of a device type */
ULONG NhltDmicCount(void);                         /* digital microphones */

/* ------------------------------------------------------------------ */
/* AML scanning                                                         */
/* ------------------------------------------------------------------ */
typedef struct _AML_DEV {
    const UCHAR *T;           /* definition block */
    ULONG Len;
    ULONG Start;              /* the 5B 82 opcode */
    ULONG Body;               /* first byte after the name */
    ULONG End;
    char  Name[5];
    char  Path[40];           /* parent names, "PCI0.I2C1.CODC" style (best effort) */
} AML_DEV;

/* calls fn for every Device () in every DSDT / SSDT; stops when fn returns FALSE */
typedef BOOLEAN (*AML_DEV_FN)(const AML_DEV *d, void *ctx);
void    AmlForEachDevice(AML_DEV_FN fn, void *ctx);
void    AmlHid(const AML_DEV *d, char *out, ULONG size);          /* _HID text ("10EC5682", "PNP0C50") */
BOOLEAN AmlHasId(const AML_DEV *d, const char *id);               /* _HID or _CID string / EISA id */
BOOLEAN AmlAdr(const AML_DEV *d, ULONGLONG *adr);                 /* Name (_ADR, ...) */
BOOLEAN AmlDeviceAt(const UCHAR *T, ULONG len, ULONG pos, AML_DEV *d);
BOOLEAN AmlParent(const AML_DEV *d, AML_DEV *p);
BOOLEAN AmlDsdInt(const AML_DEV *d, const char *prop, ULONGLONG *val);   /* _DSD integer property */
BOOLEAN AmlNamedInt(const AML_DEV *d, const char *name4, ULONGLONG *val); /* Name (XXXX, integer) */
BOOLEAN AmlNamedStr(const AML_DEV *d, const char *name4, char *out, ULONG size); /* Name (XXXX, "..") or a
                                                                                   Method returning a string */
/* the elements of a _DSD property: Package () { "name", value } with value an integer, string,
   reference or Package () of those; returns the number of elements stored (0: not found) */
#define AML_EL_INT   1
#define AML_EL_STR   2
#define AML_EL_REF   3          /* a NameString: Name = its last segment */
#define AML_EL_OTHER 4
typedef struct _AML_ELEM {
    UCHAR     Kind;
    ULONGLONG Int;
    char      Name[16];         /* reference: last segment; string: the first 15 characters */
} AML_ELEM;
ULONG   AmlDsdPkg(const AML_DEV *d, const char *prop, AML_ELEM *e, ULONG max);
/* the first child device with a given _ADR */
BOOLEAN AmlChildByAdr(const AML_DEV *d, ULONGLONG adr, AML_DEV *child);

/* resources found in a device (its Name (_CRS, ...) buffers and methods) */
typedef struct _AML_I2C {
    USHORT Addr;
    ULONG  Speed;
    char   Ctrl[5];           /* last segment of the controller path ("I2C1") */
} AML_I2C;
typedef struct _AML_GPIO {
    USHORT Pin;
    UCHAR  Int;               /* GpioInt (else GpioIo) */
    UCHAR  Pull;
    char   Ctrl[5];
} AML_GPIO;
typedef struct _AML_RES {
    ULONG    NI2c;
    AML_I2C  I2c[4];
    ULONG    NGpio;
    AML_GPIO Gpio[8];           /* GpioIo and GpioInt in _CRS order (the _DSD / crs_entry_index numbering) */
    ULONG    NMem;
    ULONG    MemBase[8], MemLen[8];
    ULONG    NIrq;
    ULONG    Irq[8];
    ULONG    NSpi;
    USHORT   SpiCs[4];          /* DeviceSelection */
    ULONG    SpiSpeed[4];
    UCHAR    SpiMode[4];        /* AML_SPI_xxx */
    UCHAR    SpiBits[4];        /* DataBitLength */
    char     SpiCtrl[4][5];
} AML_RES;
#define AML_SPI_CPHA     1      /* ClockPhaseSecond */
#define AML_SPI_CPOL     2      /* ClockPolarityHigh */
#define AML_SPI_CS_HIGH  4      /* PolarityHigh device selection */
#define AML_SPI_3WIRE    8
void AmlResources(const AML_DEV *d, AML_RES *r);
/* base of the index-th Memory32Fixed when the _CRS method fills it in from a firmware variable */
ULONG AmlCrsMemBase(const AML_DEV *d, ULONG index);
ULONG strlen_s(const char *s);
int   strcmp_s(const char *a, const char *b);

#endif
