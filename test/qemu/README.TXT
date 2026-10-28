QEMU 8.2 intel-hda with an Intel Smart Sound DSP model, for testing the SST
code of AUDIONT without the hardware.  Copy both files over hw/audio/ of a
QEMU 8.2 tree and build.  intel-hda.c also has the bar64, progif and subclass
properties used by the other tests.

  -device intel-hda,progif=0x80,dsp=kbl|apl|cnl|icl|tgl|mtl|lnl|ptl,audiodev=snd0
  -acpitable file=nhlt.bin            (tools/mknhlt.py)

kbl: Intel cAVS firmware through the CLDMA; apl, cnl: Intel cAVS firmware
through the host DMA code loader; icl: SOF IPC3; tgl, mtl, lnl, ptl: SOF IPC4
(mtl and later with the ACE registers).  A host input stream behind a DMIC or
I2S gateway gets 1 kHz (left) / 1.5 kHz (right); a host output stream is played
to the audio device.  debug=1 logs the DSP side to stderr.

I2S codecs: dw_i2c_pci.c (DesignWare I2C as an LPSS PCI function) and
i2c_regfile.c (a register file standing in for a codec) go to hw/i2c/ (add
them to hw/i2c/meson.build); tools/mkssdt.py describes the chips in an SSDT:

  -acpitable file=ssdt.aml -device dw-i2c-pci,id=i2c0,addr=05.0
  -device i2c-regfile,bus=i2c0.0,address=0x1a,reg-bytes=2,val-bytes=2,regs=0xff:0x6530,trace=on

Bay Trail / Cherry Trail / Broadwell audio DSP: intel-lpe.c (hw/audio, add it
to the CONFIG_HDA line of hw/audio/meson.build):

  -device intel-lpe,gen=byt|cht|bdw,audiodev=snd0     PCI mode
  -device intel-lpe,gen=cht,acpi-base=0xd0000000      ACPI mode, with
      tools/mkssdt.py --lpe 808622A8@0xd0200010 (the _CRS base comes from an NVS field)

SoundWire (sdw.c): the DSP model has the Intel SoundWire controller - the
shim (cnl / icl / tgl in BAR4 at 0x2C000, ALH 0x2C800; mtl at 0x38000 /
0x24000) or, for lnl / ptl, the multi-link SoundWire extended link (ML link
3, LEPTR id 0x00) with the per-link shims in the HD Audio BAR at 0x30000
(intel-hda.c makes BAR0 512 KB for those) - and four Cadence masters
(command / response FIFO, PING slave status, bus reset, bank switch by
SCP_FrameCtrl; data port, PDI and ALH registers are stored).  Peripherals
are register files with a DevId (RT711-SDCA: RC / DC calibration complete,
headset detected):

  -device intel-hda,addr=0x0e,progif=0x80,dsp=tgl,audiodev=snd0,sdw=0:25d:711:1:0,,1:25d:1316:1:1
          (link:mfg:part:class:uid, peripherals separated by ",,")
  -acpitable file=sdw.aml   tools/mkssdt.py sdw.aml --sdw-hda 0x000E0000 \
                              --sdw 0:25d:711:1:0:14:8 --sdw 1:25d:1316:1:1:4:2
                            (--sdw-func N:TYPE adds a DisCo SDCA function for the generic driver)

An ALH capture gateway gets the 1 kHz tone only if its stream is wired end to
end (ALH stream / PCMSyCHM, or PCMSyCM + link DMA + DMA TLV on ACE 2.x, PDI,
master port enabled in the active bank, a peripheral port enabled at the
same block offset); otherwise silence.  debug=1 logs bus resets, bank
switches and whether the gateways are wired, debug=2 also the SDCA control
writes.  Runner: /tmp/claude-0/t/run-sdw.sh <dsp> <fw> (SDW=, SSDT=, DBG=).

ac97-mixer-delay.patch (hw/audio/ac97.c, patch -p1 in a QEMU 8.2 tree): an AC'97
codec whose mixer comes up late, as the CS4299 of an IBM ThinkPad T23 does:

  -global AC97.mixer-delay=400

after a reset the mixer volume registers (02h - 1Eh) ignore writes for 400 ms.
AUDIONT 1.0 / early 1.1 then left PCM out muted (no sound); the driver now waits
until a mixer write sticks.  Run: AUDIONT_VGA='-vga std -global AC97.mixer-delay=400'
QEMU=<patched qemu-system-i386> python3 tools/suite.py ac97
