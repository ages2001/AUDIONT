#!/usr/bin/env python3
"""AUDIONT QEMU test harness.

  vm.py prep                    build base image (auto logon + agent + service)
  vm.py run [--audio ac97|hda|ich9|hda-dsp|hda-output|hda-micro|none] [--timeout s]
            [--bat file] [--keep] [--files src=dst ...]
                                boot a fresh copy of the base image, run the
                                agent batch file, collect logs and audio.
Results land in vmout/: debugcon.log (driver log), serial.log (agent),
out.wav (captured audio), OUT.TXT, screen*.png.
"""
import argparse, os, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG_DIR = os.environ.get('AUDIONT_IMG', '/home/claude/nt31')
PRISTINE = os.path.join(IMG_DIR, 'nt31.raw')
BASE = os.path.join(IMG_DIR, 'base.raw')
WORK = os.path.join(IMG_DIR, 'work.raw')
OUT = os.environ.get('AUDIONT_OUT') or os.path.join(ROOT, 'vmout')
OFF = '@@32256'
WIN = 'WINNT31'

def sh(*a, **kw):
    return subprocess.run(a, check=kw.pop('check', True), **kw)

def mcopy(img, src, dst):
    sh('mcopy', '-o', '-i', img + OFF, src, '::' + dst)

def mmd(img, d):
    sh('mmd', '-i', img + OFF, '::' + d, check=False, stderr=subprocess.DEVNULL)

def mget(img, src, dst):
    return subprocess.run(['mcopy', '-o', '-i', img + OFF, '::' + src, dst],
                          stderr=subprocess.DEVNULL).returncode == 0

AUDIO = {
    'ac97': ['-device', 'AC97,audiodev=snd0'],
    'hda': ['-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0'],
    'hda-output': ['-device', 'intel-hda', '-device', 'hda-output,audiodev=snd0'],
    'hda-micro': ['-device', 'intel-hda', '-device', 'hda-micro,audiodev=snd0'],
    'ich9': ['-device', 'ich9-intel-hda', '-device', 'hda-duplex,audiodev=snd0'],
    # HD Audio with the DSP enabled (class 04 03 80, Intel Comet Lake .. with digital microphones)
    'hda-dsp': ['-device', 'ich9-intel-hda,progif=0x80', '-device', 'hda-duplex,audiodev=snd0'],
    'none': [],
    # HD Audio BAR placed above 4 GB by the firmware (64-bit BAR + a 4 GB BAR that does not
    # fit the 32-bit hole); needs AUDIONT_CPU with long mode for SeaBIOS to use the 64-bit window
    'hda64': ['-device', 'intel-hda,bar64=on', '-device', 'hda-duplex,audiodev=snd0',
              '-object', 'memory-backend-ram,id=big,size=4G', '-device', 'ivshmem-plain,memdev=big'],
}

class VM:
    def __init__(self, img, audio, extra=None, backend='wav', noreboot=True):
        os.makedirs(OUT, exist_ok=True)
        self.mon = os.path.join(OUT, 'mon.sock')
        for f in ('debugcon.log', 'serial.log', 'out.wav', 'mon.sock'):
            p = os.path.join(OUT, f)
            if os.path.exists(p): os.remove(p)
        qemu = os.environ.get('QEMU', 'qemu-system-i386')
        bios = ['-L', os.environ['QEMU_BIOS']] if os.environ.get('QEMU_BIOS') else []
        args = [qemu] + bios + ['-M', 'pc', '-cpu', os.environ.get('AUDIONT_CPU', 'pentium'), '-m', os.environ.get('AUDIONT_MEM', '64'),
                '-drive', 'file=%s,format=raw,if=ide' % img,
                '-audiodev', ('wav,id=snd0,path=%s,out.frequency=48000' % os.path.join(OUT, 'out.wav'))
                             if backend == 'wav' else 'none,id=snd0',
                '-display', 'none'] + os.environ.get('AUDIONT_VGA', '-vga std').split() + (['-no-reboot'] if noreboot else []) + [
                '-monitor', 'unix:%s,server,nowait' % self.mon,
                '-debugcon', 'file:%s' % os.path.join(OUT, 'debugcon.log'),
                '-serial', 'file:%s' % os.path.join(OUT, 'serial.log'),
                '-rtc', 'base=localtime'] + AUDIO[audio] + (extra or [])
        self.p = subprocess.Popen(args, stdout=open(os.path.join(OUT, 'qemu.log'), 'w'),
                                  stderr=subprocess.STDOUT)
        self.sock = None
        for _ in range(50):
            try:
                self.sock = socket.socket(socket.AF_UNIX); self.sock.connect(self.mon); break
            except OSError:
                time.sleep(0.1)
        time.sleep(0.3)
        self._drain()
        self.shots = 0

    def _drain(self):
        self.sock.setblocking(False)
        try:
            while self.sock.recv(65536): pass
        except BlockingIOError:
            pass
        self.sock.setblocking(True)

    def cmd(self, c):
        self.sock.sendall((c + '\n').encode()); time.sleep(0.15); self._drain()

    def key(self, k, delay=0.15):
        self.cmd('sendkey ' + k); time.sleep(delay)

    def type(self, text):
        m = {' ': 'spc', '\\': 'backslash', '.': 'dot', ':': 'shift-semicolon', '/': 'slash',
             '-': 'minus', '_': 'shift-minus', '\n': 'ret', '=': 'equal', ',': 'comma'}
        for ch in text:
            if ch in m: k = m[ch]
            elif ch.isupper(): k = 'shift-' + ch.lower()
            else: k = ch
            self.key(k, 0.08)

    def shot(self, name=None):
        self.shots += 1
        name = name or 'screen%02d' % self.shots
        ppm = os.path.join(OUT, name + '.ppm')
        self.cmd('screendump ' + ppm); time.sleep(0.5)
        try:
            from PIL import Image
            Image.open(ppm).save(os.path.join(OUT, name + '.png')); os.remove(ppm)
        except Exception as e:
            print('screenshot failed', e)
        return os.path.join(OUT, name + '.png')

    def serial(self):
        try: return open(os.path.join(OUT, 'serial.log'), 'rb').read().decode('latin1')
        except OSError: return ''

    def wait_serial(self, marker, timeout, shots=0):
        t0 = time.time(); last = t0
        while time.time() - t0 < timeout:
            if marker in self.serial(): return True
            if self.p.poll() is not None: return False
            if shots and time.time() - last >= shots:
                last = time.time(); self.shot('t%03d' % int(last - t0))
            time.sleep(1)
        return False

    def quit(self):
        try: self.cmd('quit')
        except Exception: pass
        try: self.p.wait(10)
        except subprocess.TimeoutExpired: self.p.kill()

def fresh_files(img, files):
    for spec in files:
        src, dst = spec.split('=', 1)
        d = os.path.dirname(dst)
        parts = [x for x in d.split('/') if x]
        for i in range(1, len(parts) + 1):
            mmd(img, '/' + '/'.join(parts[:i]))
        mcopy(img, src, dst)

def prep(args):
    shutil.copyfile(PRISTINE, BASE)
    mmd(BASE, '/AUDIONT'); mmd(BASE, '/AUDIONT/TEST')
    mcopy(BASE, os.path.join(ROOT, 'build/nttest.exe'), '/AUDIONT/TEST/NTTEST.EXE')
    mcopy(BASE, os.path.join(ROOT, 'build/audiont.sys'), '/%s/SYSTEM32/DRIVERS/AUDIONT.SYS' % WIN)
    with open('/tmp/autorun.bat', 'w') as f: f.write('@echo off\r\necho base image ready\r\n')
    mcopy(BASE, '/tmp/autorun.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    vm = VM(BASE, 'none')
    time.sleep(args.boot)
    vm.shot('prep1')
    vm.key('ctrl-alt-delete', 3)
    vm.shot('prep2')
    vm.key('ret', 15)
    vm.shot('prep3')
    vm.key('alt-f', 1); vm.key('r', 2)
    vm.shot('prep4')
    vm.type('C:\\AUDIONT\\TEST\\NTTEST.EXE /prep\n')
    ok = vm.wait_serial('##AGENT-SHUTDOWN##', 120)
    vm.shot('prep5')
    time.sleep(20)
    vm.shot('prep6')
    vm.quit()
    print(vm.serial())
    print('PREP', 'OK' if ok else 'FAILED')

def run(args):
    sh('cp', '--sparse=always', BASE, WORK)
    mcopy(WORK, args.sys or os.path.join(ROOT, 'build/audiont.sys'),
          '/%s/SYSTEM32/DRIVERS/AUDIONT.SYS' % WIN)
    mcopy(WORK, os.path.join(ROOT, 'build/nttest.exe'), '/AUDIONT/TEST/NTTEST.EXE')
    fresh_files(WORK, args.files or [])
    if args.prebat:
        mcopy(WORK, args.prebat, '/AUDIONT/TEST/AUTORUN.BAT')
        vm = VM(WORK, 'none')
        ok = vm.wait_serial('##AGENT-SHUTDOWN##', args.timeout)
        time.sleep(args.settle)
        vm.quit()
        print('prebat:', 'done' if ok else 'TIMEOUT')
        print(vm.serial()[-2000:])
    if args.bat:
        mcopy(WORK, args.bat, '/AUDIONT/TEST/AUTORUN.BAT')
    vm = VM(WORK, args.audio, args.qemu.split() if args.qemu else None, args.backend)
    t0 = time.time()
    done = vm.wait_serial('##AGENT-SHUTDOWN##', args.timeout, args.shots) or '##AGENT-NOSHUT##' in vm.serial()
    if done:
        time.sleep(args.settle)
    vm.shot('final')
    vm.quit()
    mget(WORK, '/AUDIONT/TEST/OUT.TXT', os.path.join(OUT, 'OUT.TXT'))
    for g in args.get or []:
        src, dst = g.split('=', 1)
        mget(WORK, src, os.path.join(OUT, dst))
    print('elapsed %.0fs %s' % (time.time() - t0, 'DONE' if done else 'TIMEOUT'))
    print('--- debugcon ---')
    try: sys.stdout.write(open(os.path.join(OUT, 'debugcon.log'), 'rb').read().decode('latin1')[-args.tail:])
    except OSError: pass
    print('--- serial ---')
    sys.stdout.write(vm.serial()[-args.tail:])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode')
    ap.add_argument('--audio', default='ac97')
    ap.add_argument('--timeout', type=int, default=240)
    ap.add_argument('--settle', type=int, default=8)
    ap.add_argument('--boot', type=int, default=80)
    ap.add_argument('--bat')
    ap.add_argument('--prebat')
    ap.add_argument('--shots', type=int, default=0)
    ap.add_argument('--backend', default='wav')
    ap.add_argument('--sys')
    ap.add_argument('--qemu')
    ap.add_argument('--tail', type=int, default=6000)
    ap.add_argument('--files', nargs='*')
    ap.add_argument('--get', nargs='*')
    a = ap.parse_args()
    {'prep': prep, 'run': run}[a.mode](a)

if __name__ == '__main__':
    main()
