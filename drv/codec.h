/*
 * AUDIONT - I2S codecs and amplifiers behind the Intel Smart Sound DSP.
 *
 *   i2c.c        DesignWare I2C masters of the Intel LPSS (PCI functions of
 *                the PCH, or ACPI devices on Bay Trail / Cherry Trail)
 *   gpio.c       Intel PCH / SoC GPIO pads (ACPI GpioIo lines of codecs and
 *                amplifiers: enable, reset, shutdown)
 *   sstcodec.c   ACPI discovery of the codecs (_HID, I2cSerialBus, GpioIo),
 *                SSP port assignment, clocks from the NHLT blob, stream and
 *                volume hooks for sst.c
 *   cod_*.c      the chips
 *
 * A codec driver brings its chip up as an I2S clock consumer (the SSP drives
 * BCLK and the frame clock; MCLK only where the SSP drives one), routes the
 * DAC to the headphone / line / speaker outputs it has and the headset (or
 * internal analog) microphone to the ADC.  There is no jack detection: every
 * output stays on.
 */
#ifndef AUDIONT_CODEC_H
#define AUDIONT_CODEC_H

#include "sst.h"

/* ------------------------------------------------------------------ */
/* I2C (i2c.c)                                                          */
/* ------------------------------------------------------------------ */
typedef struct _I2C_BUS I2C_BUS;

I2C_BUS *I2cOpen(const char *ctrl);            /* ACPI device name of the controller ("I2C1"); cached */
BOOLEAN  I2cXfer(I2C_BUS *b, USHORT addr, const UCHAR *w, ULONG wn, UCHAR *r, ULONG rn);
const char *I2cName(I2C_BUS *b);
void     I2cCloseAll(void);

/* ------------------------------------------------------------------ */
/* GPIO (gpio.c)                                                        */
/* ------------------------------------------------------------------ */
typedef struct _GPIO_PIN {
    PUCHAR  Cfg;                /* PADCFG0 of the pad (NULL: not resolved) */
    ULONG   Pad;
    char    Ctrl[5];
    USHORT  Pin;                /* ACPI pin number */
} GPIO_PIN;

BOOLEAN GpioResolve(const AML_GPIO *g, GPIO_PIN *p);
void    GpioSet(GPIO_PIN *p, BOOLEAN high);    /* makes the pad a GPIO output (host owned) */
int     GpioGet(GPIO_PIN *p);                  /* input level, -1 unknown */
void    GpioCloseAll(void);

/* ------------------------------------------------------------------ */
/* SPI (spi.c): Intel LPSS SPI (PXA2xx-type SSP) masters, polled        */
/* ------------------------------------------------------------------ */
typedef struct _SPI_DEV SPI_DEV;

/* controller by ACPI name ("SPI1"), native chip select (ACPI DeviceSelection), clock, AML_SPI_xxx mode;
   csGpio: chip select by a GPIO line instead of the controller's (NULL: native) */
SPI_DEV *SpiOpen(const char *ctrl, ULONG cs, ULONG hz, ULONG mode, const GPIO_PIN *csGpio, BOOLEAN csGpioActiveHigh);
/* one chip-select assertion: write w (wn bytes), then read rn bytes (half duplex, MSB first) */
BOOLEAN  SpiXfer(SPI_DEV *d, const UCHAR *w, ULONG wn, UCHAR *r, ULONG rn);
const char *SpiName(SPI_DEV *d);        /* "SPI1.0" */
void     SpiCloseAll(void);

/* ------------------------------------------------------------------ */
/* codecs                                                               */
/* ------------------------------------------------------------------ */
#define CODEC_HEADSET 1         /* DAC + ADC: headphone / line / speaker outputs, microphone inputs */
#define CODEC_AMP     2         /* speaker amplifier (render only) */

/* capabilities a driver reports in Caps after Init */
#define CC_PLAY   1
#define CC_REC    2

#define CODEC_MAX_GPIO 4

/* board data of speaker amplifiers behind an HD Audio codec (sideamp.c: the Linux cs35l41_hda_property.c
   table or the ACPI _DSD "cirrus,..." properties).  Valid FALSE (SSP amplifiers of sstcodec.c): the
   chip driver's built-in defaults. */
#define AMP_BST_INTERNAL  1     /* boost converter: IndNh / IpkMa / CapUf (cs35l41_boost_config) */
#define AMP_BST_EXTERNAL  2     /* VBST from an external supply, VSPK switch on the amp's GPIO1 */
#define AMP_BST_EXT_NOSW  3     /* external supply without VSPK switch (Lenovo CLSA0100) */
#define AMP_BST_SAFE      4     /* board unknown: boost and bypass FET off (as external), GPIO1 untouched */
#define AMP_POS_LEFT      0
#define AMP_POS_RIGHT     1
#define AMP_POS_CENTER    2
#define AMP_GPIO_UNUSED   0     /* cirrus,gpioN-func */
#define AMP_GPIO_VSPK     1
#define AMP_GPIO_IRQ      2
#define AMP_GPIO_SYNC     3
#define AMP_GPIO_NONE     0xFF  /* not given: pad left at its reset default */
typedef struct _AMP_CFG {
    BOOLEAN  Valid;
    UCHAR    Boost;             /* AMP_BST_xxx */
    UCHAR    Pos;               /* AMP_POS_xxx (ASP RX slot: left 0, right 1, centre 0) */
    UCHAR    Gpio1, Gpio2;      /* AMP_GPIO_xxx */
    USHORT   IndNh, IpkMa, CapUf;   /* internal boost: inductor nH, peak current mA, capacitor uF */
} AMP_CFG;

typedef struct _SST_CODEC {
    const struct _CODEC_DRV *Drv;
    SST     *Sst;               /* NULL for amplifiers behind an HD Audio codec (sideamp.c) */
    I2C_BUS *Bus;               /* NULL for GPIO-only amplifiers and SPI chips */
    SPI_DEV *Spi;               /* control over SPI instead of I2C (CRd / CWr / CRaw) */
    UCHAR    SpiPad;            /* SPI: dummy bytes between register address and value (regmap pad_bits / 8) */
    UCHAR    SpiRead;           /* SPI: read flag OR'ed into the first address byte (regmap read_flag_mask) */
    USHORT   Addr;              /* 7-bit I2C address */
    UCHAR    RegBytes, ValBytes;/* register address / value width on the wire (big endian) */
    UCHAR    Port;              /* SSP port */
    UCHAR    Index;             /* amplifiers on one port: 0 left / first slot, 1 right, ... */
    UCHAR    Count;             /* amplifiers on this port */
    UCHAR    Slots;             /* slots per frame (2: I2S) */
    UCHAR    Slot;              /* slot width in bits (16, 24, 25, 32) */
    UCHAR    Valid;             /* valid bits per sample (16, 24) */
    UCHAR    Caps;              /* CC_xxx */
    ULONG    Rate;              /* frame rate (48000) */
    ULONG    Bclk;              /* bit clock (Hz) = Rate * Slots * Slot */
    ULONG    Mclk;              /* MCLK driven by the SSP (Hz); 0: none, use BCLK */
    ULONG    Mic;               /* registry SstCodecMic: 0 driver default, 1.. alternatives */
    GPIO_PIN Gpio[CODEC_MAX_GPIO];      /* the ACPI GpioIo lines in _CRS order */
    ULONG    NGpio;
    char     Hid[12];
    char     Path[40];
    BOOLEAN  Ready, PlayOn, RecOn;
    ULONG    Errors;
    AMP_CFG  Amp;               /* amplifier board data (sideamp.c) */
    ULONG    Priv[16];          /* driver private */
} SST_CODEC;

typedef struct _CODEC_DRV {
    const char *Name;           /* "RT5682" */
    const char *Hids;           /* "10EC5682\0RTL5682\0\0" */
    UCHAR    Kind;              /* CODEC_HEADSET / CODEC_AMP */
    UCHAR    RegBytes, ValBytes;
    BOOLEAN (*Probe)(SST_CODEC *c);                 /* the chip answers with its id */
    BOOLEAN (*Init)(SST_CODEC *c);                  /* reset and static setup, sets Caps */
    void    (*Stream)(SST_CODEC *c, BOOLEAN rec, BOOLEAN on);   /* on: before the SSP runs; off: after it stopped */
    void    (*Volume)(SST_CODEC *c, ULONG left, ULONG right, BOOLEAN mute);   /* 0..65535, NULL: software */
    void    (*Shutdown)(SST_CODEC *c);              /* power down */
} CODEC_DRV;

/* register access with the driver's widths; FALSE / 0xFFFFFFFF on an I2C / SPI error
   (SPI: address, SpiPad zero bytes, value; reads with SpiRead in the first address byte) */
BOOLEAN CRd(SST_CODEC *c, ULONG reg, ULONG *val);
ULONG   CGet(SST_CODEC *c, ULONG reg);
BOOLEAN CWr(SST_CODEC *c, ULONG reg, ULONG val);
BOOLEAN CUpd(SST_CODEC *c, ULONG reg, ULONG mask, ULONG val);
BOOLEAN CSeq(SST_CODEC *c, const ULONG *regval, ULONG pairs);   /* { reg, val, reg, val, ... } */
BOOLEAN CRaw(SST_CODEC *c, const UCHAR *w, ULONG wn, UCHAR *r, ULONG rn);
void    CDelay(ULONG ms);                           /* sleeps at passive level, stalls otherwise */
void    CGpio(SST_CODEC *c, ULONG i, BOOLEAN high);/* ACPI GpioIo line i, if there is one */
/* PLL helpers: N.K / M style dividers for a reference -> target ratio (see the callers) */
ULONG   CGcd(ULONG a, ULONG b);

/* level 0..65535 -> 0..steps (a register with steps+1 equal dB steps: 65535 = top step) */
ULONG   CVolSteps(ULONG level, ULONG steps);

extern const CODEC_DRV * const g_CodecDrvs[];

#endif
