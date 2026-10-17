/*
 * AUDIONT private control interface (\\.\AudioNT<n>), shared between the
 * kernel driver, audiont.dll (mixer driver) and audmix.exe.
 */
#ifndef AUDIONT_IO_H
#define AUDIONT_IO_H

#define AT_CTL(f) (((0x1d) << 16) | ((0) << 14) | ((0x800 + (f)) << 2) | 0)

#define IOCTL_AUDIONT_GET_INFO     AT_CTL(0)
#define IOCTL_AUDIONT_GET_LINES    AT_CTL(1)
#define IOCTL_AUDIONT_SET_LINE     AT_CTL(2)
#define IOCTL_AUDIONT_SET_RECSRC   AT_CTL(3)
#define IOCTL_AUDIONT_GET_LOG      AT_CTL(4)
#define IOCTL_AUDIONT_GET_STATS    AT_CTL(5)
#define IOCTL_AUDIONT_CODEC_DUMP   AT_CTL(6)
#define IOCTL_AUDIONT_SAVE         AT_CTL(7)
#define IOCTL_AUDIONT_SET_DEBUG    AT_CTL(8)
#define IOCTL_AUDIONT_USERLOG      AT_CTL(9)   /* in: text, logged by the kernel driver */
#define IOCTL_AUDIONT_GET_LINENAMES AT_CTL(10) /* out: AUDIONT_LINENAMES (1.0 drivers of 2026-10 and later) */

/* line ids (match LINE_xxx in audiont.h) */
#define AT_LINE_MASTER   0
#define AT_LINE_WAVE     1
#define AT_LINE_SYNTH    2
#define AT_LINE_CD       3
#define AT_LINE_LINEIN   4
#define AT_LINE_MIC      5
#define AT_LINE_AUX      6
#define AT_LINE_PCSPK    7
#define AT_LINE_RECORD   8      /* record gain */
#define AT_LINE_REC_WAVEIN 9    /* reserved */
#define AT_LINE_COUNT    10

typedef struct {
    unsigned long  Version;          /* 0x0100 = 1.00 */
    unsigned long  CardIndex;
    unsigned long  NtVersion;
    unsigned long  NtBuild;
    unsigned long  HwType;
    unsigned short VendorId, DeviceId, SubVendor, SubDevice;
    unsigned long  Bus, Dev, Fn, Irq;
    unsigned long  InterruptMode;    /* 0 = timer polling, 1 = interrupt + timer */
    unsigned long  CanRecord;
    unsigned long  WaveOutDev, WaveInDev, MidiOutDev, AuxDev;  /* device numbers */
    char           Controller[64];
    char           Codec[64];
    unsigned long  Connected;        /* 0 = USB device unplugged (the card runs on a virtual clock) */
    char           Name[32];         /* card name: "Realtek ALC1220", "Logitech USB Headset" */
    char           DevName[5][32];   /* WinMM product names: wave out, wave in, MIDI out, aux, mixer */
} AUDIONT_INFO;

typedef struct {
    unsigned char  Present;
    unsigned char  HwVolume;
    unsigned char  HasMute;
    unsigned char  Mute;
    unsigned short Left, Right;
} AUDIONT_LINE;

typedef struct {
    unsigned long  Count;
    unsigned long  RecSrcMask;       /* bit n = line n selectable as record source */
    unsigned long  RecSrc;           /* current record source line mask */
    unsigned long  ChangeCount;
    AUDIONT_LINE   Line[AT_LINE_COUNT];
} AUDIONT_LINES;

/* names of lines that are not what the line id says (empty: the usual name) -
   e.g. the digital microphones of a Smart Sound DSP on the Aux line */
typedef struct {
    char           Short[AT_LINE_COUNT][16];   /* mixer short name, at most 15 characters */
    char           Long[AT_LINE_COUNT][32];
} AUDIONT_LINENAMES;

typedef struct {
    unsigned long  Line;
    unsigned short Left, Right;
    unsigned long  Mute;
} AUDIONT_SET_LINE;

typedef struct {
    unsigned long  Interrupts;
    unsigned long  Ticks;
    unsigned long  Underruns;
    unsigned long  PlayedFramesLo, PlayedFramesHi;
    unsigned long  RecFramesLo, RecFramesHi;
    unsigned long  ActivePlayStreams;
    unsigned long  ActiveRecStreams;
    unsigned long  SynthVoices;
    unsigned long  Playing, Recording;
} AUDIONT_STATS;

#endif
