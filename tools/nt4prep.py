#!/usr/bin/env python3
"""Prepares an NT 4.0 test image offline (no keyboard driving needed):
auto logon, the test agent at logon, the AUDIONT and USBNT services.

  nt4prep.py SRC.raw DST.raw

SRC is a raw disk with one FAT partition at sector 63 holding \\WINNT4."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.environ.get('USBNT', '/home/claude/USBNT'), 'test'))
import ntsetup

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
USBNT = os.environ.get('USBNT', '/home/claude/USBNT')
WIN = os.environ.get('NT4_WIN', 'WINNT4')
OFF = '@@32256'


def mc(img, src, dst):
    subprocess.check_call(['mcopy', '-o', '-i', img + OFF, src, '::' + dst])


def md(img, d):
    subprocess.run(['mmd', '-i', img + OFF, '::' + d], stderr=subprocess.DEVNULL)


def main(src, dst):
    subprocess.check_call(['cp', '--sparse=always', src, dst])
    md(dst, '/AUDIONT'); md(dst, '/AUDIONT/TEST'); md(dst, '/USBNT')
    mc(dst, os.path.join(ROOT, 'build/nttest.exe'), '/AUDIONT/TEST/NTTEST.EXE')
    mc(dst, os.path.join(ROOT, 'build/attest.exe'), '/AUDIONT/TEST/ATTEST.EXE')
    mc(dst, os.path.join(ROOT, 'build/audiont.sys'), '/%s/SYSTEM32/DRIVERS/AUDIONT.SYS' % WIN)
    mc(dst, os.path.join(USBNT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % WIN)
    for f in ('usbtree.exe', 'usbmon.exe', 'usbeject.exe', 'usbinst.exe', 'usbnt.sys'):
        mc(dst, os.path.join(USBNT, 'build/out', f), '/USBNT/' + f.upper())
    open('/tmp/nt4auto.bat', 'w').write('@echo off\r\necho nt4 image ready\r\n')
    mc(dst, '/tmp/nt4auto.bat', '/AUDIONT/TEST/AUTORUN.BAT')

    tmp = tempfile.mkdtemp()
    sw = os.path.join(tmp, 'software')
    subprocess.check_call(['mcopy', '-o', '-i', dst + OFF, '::/%s/SYSTEM32/CONFIG/SOFTWARE' % WIN, sw])
    ntsetup.set_values(sw, '\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon', {
        'AutoAdminLogon': 'string:1', 'DefaultUserName': 'string:Administrator',
        'DefaultPassword': 'string:', 'Userinit': 'string:userinit,C:\\AUDIONT\\TEST\\NTTEST.EXE',
        'DontDisplayLastUserName': 'string:0'})
    # only values of existing keys are changed offline: keys that hivex creates use
    # "lh" subkey lists, which the NT 4.0 loader rejects as a corrupt SYSTEM hive
    subprocess.check_call(['mcopy', '-o', '-i', dst + OFF, sw, '::/%s/SYSTEM32/CONFIG/SOFTWARE' % WIN])

    # first boot: the real installers do the rest (usbinst, AUDIONT SETUP /s)
    md(dst, '/AUDINST')
    for src, name in (('setup.exe', 'SETUP.EXE'), ('audiont.sys', 'AUDIONT.SYS'), ('audiont.dll', 'AUDIONT.DLL'),
                      ('audmix.exe', 'AUDMIX.EXE'), ('attest.exe', 'AUDTEST.EXE')):
        mc(dst, os.path.join(ROOT, 'build', src), '/AUDINST/' + name)
    par = 'SYSTEM\\CurrentControlSet\\Services\\audiont\\Parameters'
    t = 'C:\\AUDIONT\\TEST\\ATTEST.EXE'
    open('/tmp/nt4auto.bat', 'w').write('\r\n'.join([
        '@echo off',
        'C:\\USBNT\\USBINST.EXE /install /set DebugPort=0x2f8 /set DebugLevel=2',
        'C:\\AUDINST\\SETUP.EXE /s',
        '%s regdw %s DebugPorts 1' % (t, par),
        '%s regdw %s DebugLevel 3' % (t, par),
        'C:\\USBNT\\USBINST.EXE /status', 'type C:\\WINNT4\\AUDSETUP.LOG', '']))
    mc(dst, '/tmp/nt4auto.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import time, vm
    v = vm.VM(dst, 'none')
    ok = v.wait_serial('##AGENT-SHUTDOWN##', 400)
    time.sleep(15)
    v.quit()
    print(v.serial()[-2500:])
    print('prepared', dst, 'OK' if ok else 'FAILED')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
