AUDIONT 1.1 - AC'97 / Intel HD Audio / USB Audio driver for Windows NT 3.1-4.0,
              Windows 2000 and XP (x86, x64)
==============================================================================

INSTALLATION
------------
1. Copy this folder to the NT machine.
2. Run SETUP.EXE as Administrator. Everything is automatic: the files are
   copied, the kernel service and the multimedia driver entries are created,
   the MIDI Mapper is routed to the built-in synthesizer, the DSP firmware is
   copied and an "AUDIONT" group is created in Program Manager.
3. Answer "Yes" to "Restart now?". After the restart sound works.

Windows 2000 / XP: run SETUP.EXE as Administrator. The driver is installed
with Plug and Play on the sound controllers (instead of Windows' own driver)
and on USBNT's "USBNT USB Audio" device; no restart is needed. On XP x64 the
64 bit part is installed automatically by 2KXP\AMD64\SETUP.EXE (64 bit
AUDIONT.DLL for 64 bit programs). The driver is unsigned: XP asks "Continue
Anyway" once per device. SETUP /u gives the controllers back to Windows'
drivers.

Command line options:
   SETUP /u         uninstall
   SETUP /s         silent install (no messages, no restart)
   SETUP /s /r      silent install and restart
   SETUP /mmdrv     use Windows' mmdrv.dll instead of audiont.dll
   SETUP /nomapper  leave the MIDI Mapper setting alone

Files
   AUDIONT.SYS   kernel driver  (%SystemRoot%\system32\drivers)
   AUDIONT.DLL   user mode multimedia driver (wave/midi/aux/mixer)
   AUDMIX.EXE    mixer (works on every version, NT 3.1 included)
   AUDTEST.EXE   diagnostics/test tool (console): audtest info, audtest seq, ...
   SETUP.EXE     install/uninstall (copied to system32 as AUDSETUP.EXE)
   FW\           Intel audio DSP firmware and its licenses
   2KXP\         Windows 2000 / XP drivers and the x64 programs

Features
 - AC'97: Intel ICH/ICH0..ICH7/440MX/ESB, nVidia nForce..MCP51, AMD 768/8111,
   SiS 7012, VIA 82C686 / 8233 / 8235 / 8237 and other ICH compatible
   controllers; every AC'97 codec (codec independent recording model).
 - HD Audio: every controller of PCI class 04/03 (Intel ICH6 -> PCH ->
   Skylake..Arrow/Lunar/Panther Lake, Intel controllers in DSP mode included;
   AMD/ATI, nVidia, VIA, SiS, ULi, Creative, C-Media, Zhaoxin, Hygon, ...).
   Chipset specific snoop/clock/position settings are applied the same way
   as the Linux driver does. Generic codec parser: the codec's widget graph
   is walked, every line-out / speaker / headphone output is enabled, the
   speaker is muted when headphones are plugged in.
 - Intel Smart Sound DSP (Skylake..Panther Lake, Bay Trail / Cherry Trail /
   Broadwell): digital microphones, I2S codecs and amplifiers, SoundWire
   codecs and amplifiers, HD Audio side amplifiers.
 - Software mixer: 8 applications can play at the same time (+ MIDI).
 - Every PCM format: 8/16/24/32-bit, mono/stereo, 1-200 kHz; 32/64-bit float
   and WAVE_FORMAT_EXTENSIBLE with up to 8 channels (mixed down to stereo)
   (polyphase sinc sample rate converter, no drift).
 - Recording: Microphone / Line In / CD / Aux / Stereo Mix selection.
 - Built-in General MIDI synthesizer: 32 voices, 128 instruments + drum kit.
 - Mixer: AUDMIX.EXE for NT 3.1; the standard mixer API for NT 3.5 and
   later (sndvol32 etc.).
 - Interrupt (IRQ) or timer mode, chosen automatically.
 - USB Audio 1.0 and 2.0 (when the USBNT USB driver is installed): old USB
   1.1 sound cards / headsets and new USB 2.0 (UAC2) interfaces and DACs;
   on UHCI, OHCI, EHCI and xHCI. Playback + recording, 8..32-bit / float,
   44.1..192 kHz (converted automatically), hardware volume and recording
   source, USB MIDI output, hot plug (the applications of a device that is
   pulled out do not hang; the same device gets the same device numbers
   when it comes back).
   Needs USBNT 1.1 or later: https://github.com/ages2001/USBNT

Troubleshooting
 - AUDMIX.EXE -> "Report" writes a codec dump and the driver log to
   %SystemRoot%\AUDIONT.TXT. Please attach this file to bug reports.
 - Registry settings:
   HKLM\SYSTEM\CurrentControlSet\Services\audiont\Parameters
     InterruptMode  DWORD 1 = interrupt + timer (default), 0 = timer only
     LatencyMs      DWORD 20..150, default 60
     LatencyMaxMs   DWORD 20..150: the latency grows up to this after underruns
                    (default 0 = fixed; for systems whose DPCs come late)
     Synth          DWORD 0 = MIDI synthesizer off
     HdaCodec       DWORD force an HDA codec address (0..14)
     HdaProbeMask   DWORD bit mask of the codec addresses to try (0 = STATESTS)
     HdaPosBuf      DWORD 0 = LPIB, 1 = DMA position buffer, 2 = automatic (default)
     HdaImmediate   DWORD 0 = CORB/RIRB, 1 = immediate commands, 2 = automatic (default)
     UncachedDma    DWORD 0 = cached, 1 = uncached DMA memory, 2 = automatic
                    (for controllers that do not snoop; default 2)
     Ac97InvEapd    DWORD 1 = external amplifier on with EAPD=1 (some notebooks), 2 = automatic
     HdaGpioMask / HdaGpioDir / HdaGpioData  notebook amplifier GPIO setting
     GenericAc97    DWORD 0 = do not try unknown 04/01 devices
     DebugLevel     DWORD 0..3, DebugPorts DWORD (1 = port E9, 2 = COM1)
     StandardNames  DWORD 1 = device names compatible with mmdrv.dll
     RegisterWinmm  DWORD Windows 2000/XP: 0 = audiont.sys does not write its
                    Drivers32 entries (default 1)
     DeviceNames    DWORD Windows 2000/XP: 0 = Device Manager keeps the INF's name,
                    1 = the card's own name, e.g. "Realtek ALC3234 High
                    Definition Audio (AUDIONT)" (default)
     UsbAudio       DWORD 0 = do not use USB audio devices (default 1)
     RelocateBars   DWORD PCI memory above 4 GB: 0 = leave it,
                    1 = sound controllers only, 2 = every device in the system
                    below 4 GB when the kernel does not map above 4 GB (NT 3.x/4.0,
                    32 bit 2000/XP without PAE; default), 3 = always
     Sst            DWORD Intel Smart Sound DSP: 0 = do not use (HDA only),
                    1 = when the NHLT lists microphones / I2S / SoundWire (default), 2 = always
     SstCodecs      DWORD 0 = do not use the I2S codec / amplifier drivers
     SideAmps       DWORD 0 = do not use the CS35L41 / CS35L56 amplifiers next to HDA
     Sdw            DWORD 0 = do not use SoundWire
     (more DSP settings: README.md, "Intel Smart Sound")
   DSP firmware: %SystemRoot%\System32\drivers\audiont\fw (SETUP copies it).


Version history
---------------
1.1   - AMD Ryzen (AM4 / AM5) HD Audio: the codec probe is retried for a few
        seconds (codecs that answer late were not found); more HD Audio
        controller IDs (AMD / ATI Radeon HDMI, VIA, Zhaoxin, Teradici)
      - no sound on AC'97 codecs whose mixer comes up late (CS4299 in the
        IBM ThinkPad T23): the driver waits until a mixer write sticks
      - Cirrus Logic CS4297A / CS4299 / CS4201 / CS4205 (IBM ThinkPad T2x /
        X2x / A2x): AC mode register set, the DACs go through the mixer
      - AC'97: codec registers, power state and AC-link checked when playback
        starts and set again when the BIOS or a suspend reset them
      - no more repeating sound (loop) or stutter when the system is busy:
        silence after the mixed data; AC'97 / VIA DMA positions read until
        they are stable; LatencyMaxMs lets the latency grow after underruns
      - headphone / mono output no louder than the master at low volume
1.0   Initial release:
      - AC'97 (Intel ICH compatible, nVidia, AMD, SiS, VIA) and Intel HD Audio
        with a generic codec parser and controller specific settings for old
        and new systems
      - Intel Smart Sound DSP: digital microphones, I2S and SoundWire codecs
        and amplifiers, HD Audio side amplifiers
      - USB Audio 1.0 / 2.0 through USBNT: hot plug, every rate and format,
        hardware volume, recording source, USB MIDI output
      - 8-stream software mixer, sinc sample rate converter, General MIDI FM
        synthesizer, AUDMIX mixer, automatic installation
      - Windows 2000 / XP (x86, x64): Plug and Play driver, 32 and 64 bit
        audiont.dll, automatic installation; USB audio devices through
        USBNT 1.1
