#!/usr/bin/env python3
"""USB audio tests: AUDIONT on top of USBNT in an NT guest.

Needs the QEMU build with the usb-uac test device (QEMU=..., QEMU_BIOS=...).

  usbtest.py prep                        install USBNT into a copy of the base image
  usbtest.py run HC UAC [opts] [--hotplug] [--midi]
        HC : uhci ohci ehci ehci-tt xhci
        UAC: 1 or 2; opts are usb-uac properties (hs=on, fbppm=300, ...)

The guest plays the standard tone sequence through WaveOut0 (the USB
device), records from it, and the host checks the audio the device
received (vmout/out.wav), the captured tone, the device statistics and
the driver logs (vmout/debugcon.log = AUDIONT, vmout/com2.log = USBNT).
"""
import os, re, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vm, wavan

USBNT = os.environ.get('USBNT', '/home/claude/USBNT')
IMG = os.environ.get('USBTEST_IMG', os.path.join(vm.IMG_DIR, 'usbbase.raw'))
WORK = os.path.join(os.path.dirname(IMG), 'usbwork.raw')
WIN = os.environ.get('USBTEST_WIN', vm.WIN)
TONES = [440, 1000, 1500, 2000, 2500, 3000, 3500, 750]

HC = {
    'uhci': ['-device', 'piix3-usb-uhci,id=hc'],
    'ohci': ['-device', 'pci-ohci,id=hc'],
    'ehci': ['-device', 'usb-ehci,id=hc'],
    'ehci-tt': ['-device', 'usb-ehci,id=hc', '-device', 'usb-hub,bus=hc.0,port=1,hs=on'],
    'xhci': ['-device', 'qemu-xhci,id=hc'],
    # PCI sound cards (no USB device): regression of the AC'97 / HD Audio paths
    'ac97': ['-device', 'AC97,audiodev=snd0'],
    'hda': ['-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0'],
}
PCI = ('ac97', 'hda')


def usbnt_files(img):
    vm.mmd(img, '/USBNT')
    for f in ('usbinst.exe', 'usbmon.exe', 'usbtree.exe', 'usbeject.exe'):
        vm.mcopy(img, os.path.join(USBNT, 'bin', f), '/USBNT/' + f.upper())
    vm.mcopy(img, os.path.join(USBNT, 'build/out/usbnt.sys'), '/USBNT/USBNT.SYS')


def prep(a):
    shutil.copyfile(a.base or vm.BASE, IMG)
    usbnt_files(IMG)
    vm.mcopy(IMG, os.path.join(vm.ROOT, 'build/attest.exe'), '/AUDIONT/TEST/ATTEST.EXE')
    bat = '/tmp/usbprep.bat'
    open(bat, 'w').write('@echo off\r\nC:\\USBNT\\USBINST.EXE /install /set DebugPort=0x2f8 '
                         '/set DebugLevel=2\r\nC:\\USBNT\\USBINST.EXE /status\r\n')
    vm.mcopy(IMG, bat, '/AUDIONT/TEST/AUTORUN.BAT')
    v = vm.VM(IMG, 'none')
    ok = v.wait_serial('##AGENT-SHUTDOWN##', 300)
    time.sleep(10)
    v.quit()
    print(v.serial()[-1500:])
    print('PREP', 'OK' if ok else 'FAILED')


def guest_bat(a):
    t = 'C:\\AUDIONT\\TEST\\ATTEST.EXE'
    lines = ['@echo off', '%s info' % t]
    if os.environ.get('USBTEST_PRESLEEP'):
        # NT 4.0 plays its logon sound on the new device: let it finish first
        lines.append('%s sleep %s' % (t, os.environ['USBTEST_PRESLEEP']))
    if a.quick:
        lines += ['%s dump' % t, '%s tone 1000 3000 48000 16 2' % t, '%s dump' % t]
        return '\r\n'.join(lines) + '\r\n'
    if a.hotplug:
        # long tone: the host pulls the device out in the middle and plugs it back
        lines += ['%s tone 1000 9000 48000 16 2' % t, '%s sleep 3000' % t, '%s info' % t,
                  '%s tone 1500 2000 44100 16 2' % t]
    else:
        lines += ['%s seq' % t, '%s rec 3000 44100 16 2' % t, '%s rec 2000 48000 16 1' % t]
        lines += ['%s midi %s' % (t, '0')]
    lines += ['%s stats' % t, '%s dump' % t]
    if not a.hotplug:
        lines += ['%s lines' % t]
    return '\r\n'.join(lines) + '\r\n'


def run(a):
    sh = vm.sh
    sh('cp', '--sparse=always', IMG, WORK)
    vm.mcopy(WORK, os.path.join(vm.ROOT, 'build/audiont.sys'), '/%s/SYSTEM32/DRIVERS/AUDIONT.SYS' % WIN)
    vm.mcopy(WORK, os.path.join(USBNT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % WIN)
    vm.mcopy(WORK, os.path.join(vm.ROOT, 'build/attest.exe'), '/AUDIONT/TEST/ATTEST.EXE')
    vm.mcopy(WORK, os.path.join(vm.ROOT, 'build/audiont.dll'), '/%s/SYSTEM32/AUDIONT.DLL' % WIN)
    bat = '/tmp/usbrun.bat'
    open(bat, 'w').write(guest_bat(a))
    vm.mcopy(WORK, bat, '/AUDIONT/TEST/AUTORUN.BAT')
    hs = 'hs=on' in a.opts
    opts = [o for o in a.opts if o != 'hs=on']
    port = 'port=1.1,' if a.hc == 'ehci-tt' else ''
    dev = 'usb-uac,id=ua,bus=hc.0,%suac=%s,hs=%s,debug=1,audiodev=snd0%s%s' % (
        port, a.uac, 'on' if hs else 'off', ''.join(',' + o for o in opts), ',midi=on' if a.midi else '')
    com2 = os.path.join(vm.OUT, 'com2.log')
    if os.path.exists(com2):
        os.remove(com2)
    extra = HC[a.hc] + ([] if a.hc in PCI else ['-device', dev]) + ['-serial', 'file:' + com2] + \
        (a.qextra.split() if a.qextra else [])
    v = vm.VM(WORK, 'none', extra)
    t0 = time.time()
    # NT 3.1 does not always log on by itself: answer the Welcome box
    if not v.wait_serial('##AGENT-START##', a.logon):
        print('host: logging on by hand at %.0fs' % (time.time() - t0))
        v.key('ctrl-alt-delete', 4)
        v.key('ret', 2)
    if a.hotplug:
        if v.wait_serial('##AGENT-START##', 240):
            time.sleep(8 + int(os.environ.get('USBTEST_PRESLEEP', '0')) / 1000.0)
            v.cmd('device_del ua')
            print('host: device removed at %.0fs' % (time.time() - t0))
            time.sleep(4)
            v.cmd('device_add ' + dev)
            print('host: device added at %.0fs' % (time.time() - t0))
    if a.sample:
        import threading, collections
        samples = collections.Counter()
        exact = collections.Counter()
        stop = [False]

        def sampler():
            while not stop[0]:
                try:
                    v.sock.sendall(b'info registers\n')
                    time.sleep(0.02)
                    v.sock.setblocking(False)
                    out = b''
                    try:
                        while True:
                            d = v.sock.recv(65536)
                            if not d:
                                break
                            out += d
                    except BlockingIOError:
                        pass
                    v.sock.setblocking(True)
                    m = re.search(rb'EIP=([0-9a-f]{8})', out)
                    if m:
                        samples[int(m.group(1), 16) & 0xFFFFF000] += 1
                        exact[int(m.group(1), 16)] += 1
                except Exception:
                    break
        th = threading.Thread(target=sampler, daemon=True)
        th.start()
    if a.sample:
        time.sleep(40)
        stop[0] = True
        time.sleep(0.5)
        v.cmd('memsave 0xfe4c0000 0x30000 %s' % os.path.join(vm.OUT, 'mem_fe4c0000.bin'))
        v.cmd('memsave 0x80010000 0x10000 %s' % os.path.join(vm.OUT, 'mem_80010000.bin'))
        time.sleep(1)
        stop[0] = False
        th = threading.Thread(target=sampler, daemon=True)
        th.start()
    done = v.wait_serial('##AGENT-SHUTDOWN##', a.timeout, a.shots)
    if a.sample:
        stop[0] = True
        time.sleep(0.2)
        hot = [pg for pg, n in samples.most_common(6) if pg >= 0x80000000]
        for pg in hot:
            # find the image the page belongs to: walk back to an MZ header
            for back in range(0, 0x40000, 0x1000):
                f = os.path.join(vm.OUT, 'mem_%08x.bin' % (pg - back))
                v.cmd('memsave 0x%x 4096 %s' % (pg - back, f))
                time.sleep(0.2)
                try:
                    d = open(f, 'rb').read()
                except OSError:
                    break
                if d[:2] == b'MZ':
                    import struct
                    pe = struct.unpack('<I', d[0x3c:0x40])[0]
                    names = re.findall(rb'[A-Za-z0-9_]{3,}\.(?:sys|SYS|dll|DLL|exe|EXE)', d)
                    print('page %08x: image at %08x (+%x) %s' % (pg, pg - back, back, names[:3]))
                    v.cmd('memsave 0x%x 0x40000 %s' % (pg - back, os.path.join(vm.OUT, 'img_%08x.bin' % (pg - back))))
                    break
        tot = sum(samples.values())
        print('--- EIP samples (%d) ---' % tot)
        for page, n in samples.most_common(25):
            print('%08x %5d %4.1f%%' % (page, n, 100.0 * n / tot))
        print('--- exact ---')
        for eip, n in exact.most_common(40):
            print('%08x %5d' % (eip, n))
    v.shot('final')
    time.sleep(6)
    v.quit()
    ser = v.serial()
    qlog = open(os.path.join(vm.OUT, 'qemu.log'), encoding='latin1').read()
    dbg = open(os.path.join(vm.OUT, 'debugcon.log'), 'rb').read().decode('latin1')
    try:
        ulog = open(com2, 'rb').read().decode('latin1')
    except OSError:
        ulog = ''
    fails = []

    def chk(c, what):
        print('%s %s' % ('ok  ' if c else 'FAIL', what))
        if not c:
            fails.append(what)

    chk(done, 'guest finished')
    chk('BugCheck' not in ser and 'STOP' not in dbg[-3000:], 'no crash')
    if a.hc not in PCI:
        chk('USB audio through USBNT' in dbg, 'AUDIONT found USBNT')
    if a.hc in PCI:
        chk(re.search(r'card0 ready', dbg) is not None, 'PCI card created')
    else:
        chk(re.search(r'card\d ready: "QEMU UAC', dbg) is not None, 'USB card created')
    stats = re.findall(r'uac: stats .*', qlog)
    if stats:
        print('    ' + stats[-1])
        bad = re.findall(r'bad=(\d+)', stats[-1])
        chk(bad and bad[0] == '0', 'no malformed packets')
    res = wavan.tones(os.path.join(vm.OUT, 'out.wav'))
    print('    tones: ' + ', '.join('%.1f Hz %.1fs snr %.0f gl %d' % (t['freq'], t['dur'], t['snr'], t['glitches'])
                                    for t in res) if res and 'dur' in res[0] else
          '    tones: ' + ', '.join('%.1f Hz snr %.0f gl %d' % (t['freq'], t['snr'], t['glitches']) for t in res))
    if a.hotplug:
        # QEMU's wav backend starts the file again for the new device instance, so the
        # audio before the unplug is only visible in the first instance's statistics
        first = []
        for l in qlog.splitlines():
            if l.startswith('uac: uac1') or l.startswith('uac: uac2'):
                if first: break
                first.append(l)
            elif l.startswith('uac: stats') and first:
                first.append(l)
        pk = max([int(x) for x in re.findall(r' pkts=(\d+)', '\n'.join(first))] or [0])
        chk(pk > 3000, 'playback reached the device before the unplug (%d packets)' % pk)
        chk(any(abs(t['freq'] - 1500) < 3 and t['snr'] > 40 for t in res), 'tone after replug')
        chk('unplugged, running on the virtual clock' in dbg, 'unplug noticed')
        chk('is back' in dbg, 'same card after replug')
        chk(ser.count('RESULT OK') >= 3, 'guest tools succeeded (%d)' % ser.count('RESULT OK'))
    else:
        for e in TONES:
            m = [t for t in res if abs(t['freq'] - e) < max(2, e * 0.002)]
            if not m:
                chk(False, 'tone %d' % e)
                continue
            chk(not m[0]['glitches'] and m[0]['snr'] >= 40, 'tone %d (snr %.0f, glitches %d)' %
                (e, m[0]['snr'], m[0]['glitches']))
        recs = re.findall(r'recorded (\d+) bytes in (\d+) ms \(expect ~(\d+)\), peak (\d+) avg (\d+) freq (\d+)', ser)
        chk(len(recs) == 2, 'two recordings')
        for r in recs:
            got, ms, exp, peak, avg, fr = map(int, r)
            # stereo: the left channel (1000 Hz); mono: the 1000 + 1500 Hz downmix
            if a.hc in PCI:
                # QEMU's wav audio backend has no input: PCI capture DMA never runs
                continue
            chk(got > exp * 0.8 and (abs(fr - 1000) < 15 or (exp == 192000 and abs(fr - 1500) < 25)) and peak > 10000,
                'capture %d bytes / %d expected, %d Hz, peak %d' % (got, exp, fr, peak))
        chk(a.hc in PCI or 'RESULT FAIL' not in ser, 'guest tools report no failure')
        if a.midi:
            notes = re.findall(r'uac: midi 09 9', qlog)
            chk(len(notes) >= 4, 'USB MIDI note-on events at the device (%d)' % len(notes))
    for l in dbg.splitlines():
        if 'usb' in l.lower() or 'card' in l:
            pass
    print('--- AUDIONT log (usb) ---')
    print('\n'.join(l for l in dbg.splitlines() if 'usb' in l.lower() or 'card' in l)[-2500:])
    print('--- USBNT log (audio) ---')
    print('\n'.join(l for l in ulog.splitlines() if 'audio' in l)[-2500:])
    print('--- guest ---')
    print(ser[-3500:])
    shutil.copy(os.path.join(vm.OUT, 'out.wav'), os.path.join(vm.OUT, 'out-%s-%s%s.wav' % (a.hc, a.uac, '-hotplug' if a.hotplug else '')))
    print('RESULT %s %s%s%s%s %s' % (a.hc, a.uac, ' ' + ' '.join(a.opts) if a.opts else '',
                                   ' hotplug' if a.hotplug else '', ' midi' if a.midi else '', 'PASS' if not fails else 'FAIL: ' + '; '.join(fails)))
    return not fails


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('mode')
    ap.add_argument('hc', nargs='?', default='uhci')
    ap.add_argument('uac', nargs='?', default='1')
    ap.add_argument('opts', nargs='*')
    ap.add_argument('--hotplug', action='store_true')
    ap.add_argument('--midi', action='store_true')
    ap.add_argument('--timeout', type=int, default=400)
    ap.add_argument('--base')
    ap.add_argument('--shots', type=int, default=0)
    ap.add_argument('--logon', type=int, default=110)
    ap.add_argument('--qextra')
    ap.add_argument('--sample', action='store_true')
    ap.add_argument('--quick', action='store_true')
    a = ap.parse_args()
    if a.mode == 'prep':
        prep(a)
    else:
        sys.exit(0 if run(a) else 1)


if __name__ == '__main__':
    main()
