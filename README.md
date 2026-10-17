# AUDIONT - Sound Driver for Windows NT 3.1 / 3.50 / 3.51 / 4.0, 2000 and XP (x86, x64)

A generic AC'97, Intel High Definition Audio, Intel Smart Sound (audio DSP)
and **USB Audio (UAC 1.0 / 2.0)** sound driver for Windows NT 3.1 - 4.0,
which have no driver for any of today's sound hardware, and for **Windows
2000 / XP (x86, x64)**. USB audio devices are driven through the
[USBNT](https://github.com/ages2001/USBNT) USB stack.

## Contents

- [Overview](#overview) - [Highlights](#highlights) - [Supported systems](#supported-systems) - [Installation](#installation)
- [Windows 2000 / XP](#windows-2000--xp-x86-x64) - [Components](#components) - [Building from source](#building-from-source)
- [Testing](#testing) - [Hardware compatibility](#hardware-compatibility-old-and-new-systems) - [Device names](#device-names)
- [Intel Smart Sound (SST)](#intel-smart-sound-sst) - [USB Audio](#usb-audio-uac-10--20) - [Registry](#registry)
- [Changelog](#changelog) - [License](#license)

## Overview

A single `audiont.sys` runs on all four NT versions: the driver statically
imports only functions that the NT 3.1 kernel exports (`tools/pefix.py`
checks this on every build) and resolves the ones that came later
(`IofCompleteRequest`, `NtBuildNumber`, ...) at run time with its own PE
export parser.

Around it come a user mode multimedia driver (`audiont.dll`, the WinMM
`wave` / `midi` / `aux` / `mixer` driver), a mixer that runs on every NT
version (`audmix.exe`), a diagnostics tool (`audtest.exe`) and an automatic
installer (`setup.exe`).

### Why?

NT 3.x and NT 4.0 shipped drivers only for the ISA sound cards of their
time; there is nothing for AC'97, HD Audio or USB audio, so a newer PC (or a
virtual machine) is silent. Windows 2000 / XP have no driver for the audio
DSPs of current notebooks either. AUDIONT gives them sound.

## Highlights

- **AC'97**: Intel ICH / ICH0 - ICH7 / 440MX / ESB, nVidia nForce - MCP51,
  AMD 768 / 8111, SiS 7012, VIA 82C686 / 8233 / 8235 / 8237 and other ICH
  compatible controllers, with any AC'97 codec
- **HD Audio**: every PCI class 04/03 controller (Intel ICH6 through PCH,
  Skylake to Arrow / Lunar / Panther Lake, also Intel controllers in DSP
  mode; AMD / ATI, nVidia, VIA, SiS, ULi, Creative, C-Media, Zhaoxin,
  Hygon, ...) with a generic codec parser: every line-out, speaker and
  headphone output, every input, headphone auto-mute
- **Intel Smart Sound DSP** (Skylake to Panther Lake, Bay Trail / Cherry
  Trail / Broadwell): digital microphones, I2S codecs and amplifiers,
  SoundWire codecs and amplifiers, HD Audio side amplifiers
- **USB Audio Class 1.0 / 2.0** and **USB MIDI** through USBNT, with hot plug
- **Software mixer**: 8 applications at the same time (plus MIDI), any PCM
  format (8 / 16 / 24 / 32-bit, mono / stereo, 1 - 200 kHz) through a
  polyphase Kaiser-sinc sample rate converter
- **Recording** with source selection (microphone, line in, CD, aux, stereo
  mix)
- **Built-in General MIDI synthesizer**: 32 voices, 2-operator FM, 128
  instruments and a drum kit
- **Mixer** for every NT version (`audmix.exe`; NT 3.5 and later also the
  standard mixer API, sndvol32)
- Interrupt or timer mode, chosen automatically
- Devices named after the real hardware ("Realtek ALC1220", "Logitech USB
  Headset", ...)

## Supported systems

| System | Driver |
|---|---|
| Windows NT 3.1, 3.5, 3.51, 4.0 (x86) | `AUDIONT.SYS` (one binary) and `AUDIONT.DLL` |
| Windows 2000, XP (x86) | `2KXP\I386\AUDIONT.SYS` (Plug and Play), `AUDIONT.DLL` |
| Windows XP x64 Edition | `2KXP\AMD64\AUDIONT.SYS` (Plug and Play), 64 bit and 32 bit `AUDIONT.DLL` |

USB audio needs USBNT 1.1 or later.

## Installation

1. Copy the `AUDIONT` folder of a release to the machine.
2. Run `SETUP.EXE` as an administrator. Everything is automatic: the files
   are copied, the kernel service and the multimedia driver entries are
   created, the MIDI Mapper is routed to the built-in synthesizer, the DSP
   firmware goes to `%SystemRoot%\System32\drivers\audiont\fw` and an
   *AUDIONT* program group is created.
3. NT 3.x / 4.0: answer *Yes* to restart. Windows 2000 / XP need no restart.

Command line options:

| Option | Meaning |
|---|---|
| `SETUP /u` | Uninstall (Windows 2000 / XP: the controllers go back to Windows' drivers) |
| `SETUP /s` | Silent install (no messages, no restart) |
| `SETUP /s /r` | Silent install and restart |
| `SETUP /mmdrv` | Use Windows' `mmdrv.dll` instead of `audiont.dll` |
| `SETUP /nomapper` | Leave the MIDI Mapper setting alone |

Files:

| File | Purpose |
|---|---|
| `AUDIONT.SYS` | Kernel driver (`%SystemRoot%\system32\drivers`) |
| `AUDIONT.DLL` | User mode multimedia driver (wave / midi / aux / mixer) |
| `AUDMIX.EXE` | Mixer (every NT version, NT 3.1 included) |
| `AUDTEST.EXE` | Console diagnostics and test tool: `audtest info`, `audtest seq`, ... |
| `SETUP.EXE` | Install / uninstall (copied to system32 as `AUDSETUP.EXE`) |
| `FW\` | Intel audio DSP firmware with its licenses |
| `2KXP\` | Windows 2000 / XP drivers and the x64 programs |

**Troubleshooting.** `AUDMIX.EXE` -> *Report* writes a codec dump and the
driver log to `%SystemRoot%\AUDIONT.TXT`; attach this file to bug reports.

## Windows 2000 / XP (x86, x64)

The same sources also build a **Plug and Play** driver
(`2kxp\i386\audiont.sys`, `2kxp\amd64\audiont.sys`):

- `2kxp\audiont.inf` installs it for the sound controllers (PCI class 04/03
  HD Audio, 04/01 AC'97, VIA 82C686 / 8233 ...) and for the `USBNT\Audio`
  device USBNT reports while a USB audio device is plugged in.
  START_DEVICE opens the card with the interrupt Plug and Play gives it (or
  with a timer); STOP / REMOVE and sleep close it, the return to S0 opens it
  again. The devices are created with the same names as on NT 4.0
  (`\Device\AudntWaveOut0`, ...).
- `audiont.dll` is the WinMM driver (Drivers32 `wave` / `midi` / `aux` /
  `mixer` entries); on XP x64 there is a 64 bit copy in System32 for 64 bit
  programs and a 32 bit copy in SysWOW64 for 32 bit programs. DirectSound
  programs play through WinMM.
- `setup.exe` installs the INF and loads audiont.sys on the controllers it
  finds (instead of Windows' own driver); no restart is needed. On 64 bit
  Windows the devices are installed by `2kxp\amd64\setup.exe` (a 32 bit
  program cannot install devices on 64 bit Windows). `setup /u` gives the
  controllers back to Windows' drivers.
- USB audio devices work with USBNT 1.1 (`USBNT USB Audio`).
- The drivers are unsigned: Windows XP asks "Continue Anyway" once per
  device.

Build: `tools/wdmbuild.sh` (DDK headers through `inc/ntkwdm.h`,
`-DAUDIONT_WDM`; imports are checked against the Windows 2000 and XP x64
export lists). Test: `tools/w5audio.py w2k|xp64 hda|ich9|ac97|usb` (the
Windows 2000 / XP x64 guests of USBNT's `test/vm5.py`; the tones are checked
in the wav with an FFT, on XP x64 from a 32 bit and a 64 bit program).

## Components

| File | Role |
|---|---|
| `drv/main.c` | DriverEntry, PCI scan (CF8/CFC), device objects, IRP dispatch |
| `drv/engine.c` | 48 kHz / 16-bit / stereo software mixer, DMA ring, playback / recording streams, IRP queues and cancellation |
| `drv/convert.c` | PCM conversion and a polyphase Kaiser-sinc sample rate converter (rational phase, no drift) |
| `drv/synth.c`, `drv/gmpatch.h` | 32-voice 2-operator FM General MIDI synthesizer, drum kit |
| `drv/ac97.c`, `drv/ac97codec.c` | Intel ICH compatible AC'97 (Intel / nVidia / AMD / SiS) and the common codec layer |
| `drv/via.c` | VIA 82C686 / 8233 / 8235 / 8237 SGD engine |
| `drv/hda.c` | HD Audio controller (CORB/RIRB and immediate commands), streams / BDL, generic codec parser |
| `drv/usbaud.c` | USB Audio 1.0 / 2.0 back end (USBNT's `\Device\UsbNtAudio` interface), USB MIDI output |
| `drv/ctrl.c` | Mixer lines, aux device, private control device (`\\.\AudioNTn`) |
| `drv/os.c` | Version detection, dynamic imports, log (ring, port E9, COM1), memory / DMA, registry, PCI |
| `drv/wdm.c` | Windows 2000 / XP: AddDevice, PnP, power, interrupt resources |
| `drv/sst.c`, `sstipc3.c`, `sstipc4.c` | Intel Smart Sound DSP: firmware load, SOF IPC3 / IPC4 (AVS); NHLT in `drv/acpi.c` |
| `drv/lpe.c` | Bay Trail / Cherry Trail / Broadwell LPE DSP |
| `drv/sstcodec.c`, `drv/cod_*.c`, `drv/i2c.c`, `spi.c`, `gpio.c` | I2S codec / amplifier drivers and LPSS I2C, SPI, GPIO |
| `drv/sideamp.c` | CS35L41 / CS35L56 amplifiers next to an HDA codec |
| `drv/sdw.c`, `drv/sdw_*.c` | SoundWire masters and codecs / amplifiers |
| `user/audrv.c` | `audiont.dll`: WinMM user mode driver (wod / wid / mod / aux / mxd), multi-client |
| `user/audmix.c` | `audmix.exe`: mixer for every NT version |
| `user/setup.c` | `setup.exe`: automatic install / uninstall |
| `user/pnp5.c` | Windows 2000 / XP installation (SetupAPI, INF, UpdateDriverForPlugAndPlayDevices) |
| `user/attest.c` | `audtest.exe`: diagnostics and test tool |
| `inc/ntkwdm.h` | DDK headers and helpers for the Windows 2000 / XP build |
| `inf/audiont.inf` | Windows 2000 / XP INF file |
| `fw/` | Audio DSP firmware (with their licenses) |

## Building from source

With mingw-w64 (i686) on Linux:

```
make            # audiont.sys      (make DEFS=-DAUDIONT_DEBUG -> detailed log)
make tools      # audiont.dll, setup.exe, audmix.exe, attest.exe, nttest.exe
tools/mkdist.sh # dist/AUDIONT/, dist/AUDIONT-<ver>.zip, dist/audiont-src-<ver>.zip
```

`lib/*.def` produce the NT 3.1 `ntoskrnl.exe` / `hal.dll` import libraries.
`tools/*.exports` are the export lists of the NT 3.1 DLLs; `pefix.py`
verifies that every import of the kernel and user mode binaries exists in
NT 3.1 and sets the PE version fields and the checksum. `tools/getfw.sh`
fetches the DSP firmware into `fw/`.

## Testing

`tools/vm.py` runs the NT 3.1 image in QEMU (no KVM needed). The guest
logs on automatically and the `nttest.exe` agent runs
`C:\AUDIONT\TEST\AUTORUN.BAT`, then shuts the system down. The driver log
goes to port E9 (`vmout/debugcon.log`), the agent's output to COM1
(`vmout/serial.log`), and the sound is captured with `-audiodev wav` into
`vmout/out.wav`.

```
python3 tools/vm.py prep                         # base image (once)
python3 tools/vm.py run --audio hda --bat x.bat  # ac97 | hda | ich9 | hda-output | hda-micro
python3 tools/suite.py                           # tone sequence on every device + FFT check
python3 tools/wavan.py vmout/out.wav 440 1000    # tone / SNR / glitch analysis
python3 tools/freshtest.py ac97                  # install from scratch
```

`test/thda.c` runs the HDA parser on the host against real codec dumps
(Linux `/proc/asound/card*/codec#*`); `test/codec2tab.py dump out` makes
the table. `test/tconv.c`, `test/trec.c` and `test/tsynth.c` are unit tests
of the converter and the synthesizer.

## Hardware compatibility (old and new systems)

Controller specific settings follow Linux' `snd-hda-intel`,
`snd-intel8x0`, `snd-via82xx` and `ac97_codec` drivers:

- **Above 4 GB:** 64-bit BARs that UEFI firmware put above 4 GB are moved
  into PCI space below 4 GB for **every** device in the system when the
  driver loads (largest first; bridges get a 32-bit window, emptied 64-bit
  windows are closed, BARs that do not fit are shrunk with Resizable BAR).
  Default on NT 3.x / 4.0 and on 32 bit 2000 / XP without PAE; the
  `RelocateBars` registry value changes it (the same code as USBNT).
- **PCI in general:** a device in D3 is brought to D0 (BARs and the IRQ
  line are written back), PCIe "Enable No Snoop" is turned off (DMA always
  snoops the CPU caches), and a failed start stops DMA and restores the PCI
  command register.
- **HDA controller:** Intel ICH / PCH / SCH (DEVC NOSNOOP), Skylake and
  later (CGCTL MISCBDCGE off during reset, multi-link LCTL 6 MHz fix-up,
  taking ownership back from the DSP, class 0401 / 0480 "DSP mode"
  accepted), Apollo Lake EM4L, Lunar Lake and later immediate commands;
  ATI / AMD (TCSEL left alone, 0x42 snoop, LPIB / FIFO correction), nVidia
  (0x4C - 0x4E coherency bits), VIA (uncached DMA when snooping is off),
  Creative Core3D / C-Media (uncached DMA and traffic priority), Creative
  X-Fi (RINTCNT 0xC0), ULi (GCAP = 0, CORBSIZE written explicitly), Hygon
  (dword access to SDnCTL).
- **HDA command path:** 50 ms timeout, stale responses dropped, codec
  address checked; codec addresses that do not answer ("ghost" codecs) are
  left out after a controller reset, as Linux does; when CORB / RIRB does
  not work at all the driver switches to immediate commands.
- **DMA position:** Linux' default choice (position buffer on Intel and
  others, LPIB on ATI / AMD / VIA / Creative); when the chosen counter
  stalls the other one is used; recording reads LPIB one FIFO behind.
- **AC'97:** ICH4 and later IOSE, extra DMA engines stopped, 20-bit mode
  off, primary / secondary / tertiary codec ready bits, stale status bits
  cleared; on unknown 04/01 devices a read-only ICH check before any write;
  full AC-link reset on VIA. On the codec side the initialization of Linux'
  `ac97_patch.c` (AD1885 / 1886 / 1888 / 1980 / 1985 output routing,
  ALC650 / 655 / 658 / 850 / 203, CMI9739 / 9761, STAC97xx analog
  settings, TR28028, IT2646) and a subsystem table for boards that use EAPD
  inverted.

## Device names

The WinMM devices of every card carry its real name: the codec name on HDA
and AC'97 cards (`drv/hdanames.h`, generated from Linux'
`HDA_CODEC_ENTRY` table, including Realtek's names that depend on the OEM
/ coefficient value: "Realtek ALC1220", "Realtek ALC897", "Conexant
CX20632" ...), the product name on USB devices ("Logitech USB Headset").
Wave out gets the card's name, wave in "... In", MIDI "... Synth" ("...
MIDI" for USB MIDI), aux "... Volume", the mixer "... Mixer" (at most 31
characters, cut at a word boundary); a second card with the same name gets
"#2". The targets of the mixer lines match the capabilities of the wave
devices exactly, so `mixerGetID(waveOut / waveIn)` finds the right mixer;
every card that records (USB microphones without a selector unit included)
has a recording target. `audmix.exe` lists the cards as "N: Name -
Controller", "N: Name (USB)" or "N: Name (not plugged in)" and shows "AUDIONT
Mixer - Name" in its title. NT 3.1 has no mixer API; `audtest info` prints
the names and the mapping.

## Intel Smart Sound (SST)

AUDIONT also drives Intel's controllers with an audio DSP (Smart Sound
Technology). The audio path is one of two kinds:

1. **Machines with an HDA codec** (e.g. Kaby Lake-LP 9D71 with Conexant /
   Realtek): playback and the headset microphone go through the HD Audio
   codec, the internal digital microphones (DMIC) through the DSP. When the
   controller is in DSP mode (class 0401 / 0480) ownership is taken back for
   HDA; when the NHLT lists a DMIC the DSP firmware is loaded and the mixer
   shows an "Internal Microphone" recording line.
2. **Machines without a codec on the HDA link:** all audio goes through the
   DSP - I2S (SSP) codecs and amplifiers, SoundWire codecs / amplifiers,
   DMIC.

| Generation | Devices | Firmware |
|---|---|---|
| Skylake / Kaby Lake / Comet Lake-S | 9D70, A170, 9D71, A171, A2F0, A3F0 | `avs-skl.bin` (AVS, IPC4, loaded with CLDMA) |
| Apollo Lake / Gemini Lake | 5A98, 3198 | `avs-apl.bin` |
| Cannon / Comet Lake, Rocket Lake-S | 9DC8, A348, 02C8, 06C8, F1C8 | `avs-cnl.bin` |
| Ice Lake, Jasper Lake, Elkhart Lake | 34C8, 3DC8, 38C8, 4DC8, 4B55, 4B58 | `sof-icl.ri`, `sof-jsl.ri`, `sof-ehl.ri` (SOF IPC3) |
| Tiger / Alder / Raptor Lake | A0C8, F0C8, 43C8, 51Cx, 54C8, 7AD0, 7A50 | `i4-tgl.ri`, `i4-tglh.ri`, `i4-adl.ri`, `i4-adln.ri`, `i4-adls.ri` (SOF IPC4) |
| Meteor / Arrow Lake | 7E28, 7728, 7F50 | `i4-mtl.ri`, `i4-arls.ri` |
| Lunar / Panther / Wildcat Lake | A828, E428, E328, 4D28 | `i4-lnl.ri`, `i4-ptl.ri`, `i4-wcl.ri` |
| Bay Trail, Cherry Trail, Broadwell (LPE) | 0F28, 22A8, 9CB6 (PCI or ACPI mode) | `sof-byt.ri`, `sof-cht.ri`, `sof-bdw.ri` |

- **Firmware:** ships in the release's `FW\` folder (Intel's
  redistribution license in `FW\LIC-AVS.TXT`, SOF's in `FW\LIC-SOF.TXT`).
  `setup.exe` (and on 2000 / XP the INF) copy the files to
  `%SystemRoot%\System32\drivers\audiont\fw`. The file names are 8.3 for NT
  3.1's FAT. Without the firmware the driver writes to its log where to put
  it and goes on without the DSP (HDA only).
- **Endpoints** come from the ACPI NHLT table (DMIC / SSP, the I2S format
  from the blob: SSCR0, SSPSP, MDIVC) and the DSDT. The I2C (LPSS
  DesignWare), SPI (LPSS PXA2xx) and GPIO (Intel pinctrl, SPT to MTL, APL /
  GLK) drivers are part of AUDIONT.
- **I2S codecs and amplifiers** (found by their ACPI ids): Realtek RT274 /
  286 / 288 / 298, RT5639 / 5640 / 5642, RT5645 / 5650, RT5651, RT5660,
  RT5663, RT5670 / 5671 / 5672, RT5682 / RT5682S, RT1011 / 1015 / 1015P /
  1019P / 1308 amplifiers; Dialog DA7212 / 7213, DA7219; Everest ES8316 /
  8336, ES8326; Nuvoton NAU8824, NAU8825, NAU8315 / 8318; Conexant CX20721 /
  20723; Cirrus CS42L42, CS35L41 (I2C / SPI), CS35L54 / 56 / 57; Maxim
  MAX98357A / 98360A (GPIO), MAX98373, MAX98390, MAX98927; TI TAS2563; ADI
  SSM4567.
- **HDA side amplifiers:** CS35L41 / CS35L56 amplifiers connected over I2C /
  SPI next to an HDA codec (Linux' quirk table) are switched on when
  playback starts.
- **SoundWire** (Cannon Lake and later, Lunar / Panther Lake included):
  Cadence masters, enumeration, static streams, ALH gateways. Codecs /
  amplifiers: RT700, RT711, RT712, RT715, RT722, RT5682, RT1308, RT1316,
  RT1318, RT1320, MAX98363 / 98373, CS35L56, CS42L43 and generic SDCA
  devices. The CS42L43 firmware (`fw\cs42l43.bin`) is not in the release;
  when it is needed the driver log says where to put it.
- **Mixer:** the DSP recording lines show with their names ("Internal
  Microphone", "Headset Microphone", "I2S Microphone"); `audmix` and the
  WinMM mixer API (2000 / XP) use these names, `audtest lines` prints them.

Registry (`Services\audiont\Parameters`, DWORD):

| Value | Default | Meaning |
|---|---|---|
| `Sst` | 1 | 0 = never use the DSP (HDA only); 2 = start the DSP even when the NHLT lists no endpoint |
| `SstCodecs` | 1 | 0 = do not use the I2S codec / amplifier drivers |
| `SstCodecPort`, `SstAmpPort`, `SstPlayPort` | 0 | Force the SSP port (port + 1) |
| `SstCodecMic` | 0 | Codec microphone input (driver specific; 0 = headset microphone) |
| `SstDmicBits` | 0 | NHLT blob for the DMIC format (16 / 32; 0 = prefer 32) |
| `SideAmps` | 1 | 0 = do not use the HDA side amplifiers |
| `SideAmpBoost`, `SideAmpBstInd`, `SideAmpBstIpk`, `SideAmpBstCap` | 0 | Force the CS35L41 boost settings (nH / mA / uF) |
| `Sdw` | 1 | 0 = do not use SoundWire |
| `SdwLinkMask` | 0 | SoundWire links to use (bit mask; 0 = those in ACPI) |
| `SdwDmic` | 0 | 1 = use the internal microphones of the SoundWire codec |
| `SdwRt715Mux` | - | RT715 recording input |
| `Cs42l43Jack`, `Cs42l43Mic`, `Cs42l43Spk` | - | CS42L43 headset type, microphone, speaker |

Test: the DSP model added to USBNT's QEMU (`test/qemu`; `intel-hda,dsp=kbl|
apl|cnl|icl|tgl|mtl|lnl|ptl`), the I2C / LPE / SoundWire models and ACPI
tables made with `tools/mkssdt.py` and `tools/mknhlt.py`
(`test/qemu/README.TXT`). DMIC recording (1 kHz) and 440 Hz playback over
I2S / SoundWire were verified on every generation; not tried on real
hardware yet.

## USB Audio (UAC 1.0 / 2.0)

When USBNT is installed, AUDIONT finds its `\Device\UsbNtAudio` interface
at startup and registers with USBNT as a client (`inc/usbntaud.h`). USBNT
handles the isochronous transfers, the alternate setting / sample rate /
clock selection, the feature unit volume and the feedback of asynchronous
devices; AUDIONT is called every few milliseconds and hands over / takes
the data in the device's own format.

- Every USB audio device becomes a "card" like the PCI cards: `WaveOutN`,
  `WaveInN`, `MidiOutN`, `MMAuxN`, `AudioNTN`; the device names carry the
  product name ("QEMU UAC Test Device", for example).
- Between the engine's 48 kHz / 16-bit stereo ring and the device the
  format (8 / 16 / 24 / 32-bit PCM, 32-bit float, mono / stereo /
  multichannel) and the sample rate (devices with only 44.1 / 96 / 192 kHz
  included, with the same Kaiser-sinc converter) are converted. The samples
  the device consumes are the card's clock: asynchronous devices are
  followed exactly at their own rate.
- Hot plug: a new device gets new device objects (applications started
  afterwards see it). A device that is pulled out keeps its card, which runs
  on a virtual clock, so the buffers of open applications keep completing;
  when the same device (VID / PID) comes back it gets its card and names
  back.
- Master and recording volume go to the device's feature unit (dB);
  selector units show in the mixer as recording sources (microphone, line,
  ...).
- On devices with a USB MIDI output the card's MIDI device is the USB MIDI
  port (external synthesizer, keyboard); on devices without MIDI the FM
  synthesizer plays through the USB device.
- The `UsbAudio` = 0 registry value turns USB support off.

Test: `tools/usbtest.py` (with the patched QEMU of USBNT's `test/qemu/`,
`QEMU=...`):

```
python3 tools/usbtest.py prep                     # install USBNT on the NT 3.1 base image
python3 tools/usbtest.py run uhci 1               # UAC1 full speed, UHCI
python3 tools/usbtest.py run ehci 2 hs=on         # UAC2 high speed, EHCI iTD
python3 tools/usbtest.py run ehci-tt 1            # full speed device behind a high speed hub (siTD)
python3 tools/usbtest.py run xhci 2 hs=on --midi  # xHCI + USB MIDI
python3 tools/usbtest.py run uhci 1 --hotplug     # unplug / replug while playing
python3 tools/nt4prep.py nt4.raw usbbase.raw      # prepare an NT 4.0 image offline
```

Verified configurations (QEMU, NT 3.1 and NT 4.0 SP6): UAC1 / UAC2 full
speed on UHCI and OHCI, UAC2 high speed on EHCI (iTD, 1 - 8 packets / ms)
and a full speed device behind a high speed hub (siTD / TT), UAC1 and UAC2
on xHCI; an 8-tone sequence (8 - 96 kHz, 8 / 16 / 24-bit), 1000 Hz
recording, a 44.1 kHz-only device (sample rate converter), USB MIDI,
unplug and replug while playing. On NT 4.0 also AC'97 and HDA playback.
`USBTEST_PRESLEEP=12000` keeps NT 4.0's logon sound out of the test;
`tools/nt4prep.py` installs USBNT and AUDIONT on the image with the real
installers (`usbinst`, `SETUP /s`).

## Registry

`HKLM\SYSTEM\CurrentControlSet\Services\audiont\Parameters` (DWORD values):

| Value | Meaning |
|---|---|
| `InterruptMode` | 1 = interrupt and timer (default), 0 = timer only |
| `LatencyMs` | 20 - 150, default 60 |
| `Synth` | 0 = MIDI synthesizer off |
| `HdaCodec` | Force an HDA codec address (0 - 14) |
| `HdaProbeMask` | Bit mask of the codec addresses to probe (0 = STATESTS) |
| `HdaPosBuf` | 0 = LPIB, 1 = DMA position buffer, 2 = automatic (default) |
| `HdaImmediate` | 0 = CORB / RIRB, 1 = immediate commands, 2 = automatic (default) |
| `UncachedDma` | 0 = cached, 1 = uncached DMA memory, 2 = automatic (default; for controllers that do not snoop) |
| `Ac97InvEapd` | 1 = the external amplifier is switched on with EAPD = 1 (some notebooks), 2 = automatic |
| `HdaGpioMask`, `HdaGpioDir`, `HdaGpioData` | Notebook amplifier GPIO setting |
| `GenericAc97` | 0 = do not try unknown 04/01 devices |
| `DebugLevel`, `DebugPorts` | 0 - 3; 1 = port E9, 2 = COM1 |
| `StandardNames` | 1 = device names compatible with mmdrv.dll |
| `UsbAudio` | 0 = do not use USB audio devices (default 1) |
| `RelocateBars` | PCI memory above 4 GB: 0 = leave it, 1 = sound controllers only, 2 = every device below 4 GB when the kernel does not map above 4 GB (NT 3.x / 4.0, 32 bit 2000 / XP without PAE; default), 3 = always |

The Intel Smart Sound values are listed under [Intel Smart Sound
(SST)](#intel-smart-sound-sst). `tools/README.TXT` (`README.TXT` in a
release) has the same list.

## Changelog

The changes of every release are in [CHANGELOG.md](CHANGELOG.md).

## License

GPL-3.0, see [LICENSE](LICENSE). The DSP firmware in `fw/` comes with its
own licenses (`fw/LIC-AVS.TXT`, `fw/LIC-SOF.TXT`).

**USE AT YOUR OWN RISK.** This driver programs sound hardware directly on
operating systems that never supported it.
