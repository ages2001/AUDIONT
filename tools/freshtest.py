#!/usr/bin/env python3
"""End-to-end test of the release package on a pristine NT 3.1 image, driven
like a user: log on, run A:\\SETUP.EXE from Program Manager, accept the
restart, log on again, play the test sequence and open the mixer."""
import os, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vm

audio = sys.argv[1] if len(sys.argv) > 1 else 'ac97'
img = os.path.join(vm.IMG_DIR, 'fresh.raw')
subprocess.run(['cp', '--sparse=always', vm.PRISTINE, img], check=True)
flp = os.path.join(vm.ROOT, 'dist', 'AUDIONT.IMG')
v = vm.VM(img, audio, ['-drive', 'file=%s,format=raw,if=floppy,readonly=on' % flp], noreboot=False)

def logon(tag):
    time.sleep(85)
    v.shot(tag + '-welcome')
    v.key('ctrl-alt-delete', 4)
    v.key('ret', 20)
    v.shot(tag + '-desktop')

def run(cmd, wait):
    v.key('alt-f', 1.5); v.key('r', 2.5)
    v.type(cmd + '\n')
    time.sleep(wait)

logon('a')
run('A:\\SETUP.EXE', 25)
v.shot('b-setupdone')
v.key('ret', 5)                     # "Yes" - restart
logon('c')
v.shot('c-group')
run('A:\\AUDTEST.EXE seq', 25)
run('C:\\WINNT31\\SYSTEM32\\AUDMIX.EXE', 12)
v.shot('d-mixer')
v.quit()
print('done; screenshots in vmout/')
