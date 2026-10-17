#!/bin/sh
# Build the release package: dist/AUDIONT/ (with the DSP firmware in FW), dist/AUDIONT-<ver>.zip
set -e
cd "$(dirname "$0")/.."
VER=1.0
[ -f fw/avs-skl.bin ] || sh tools/getfw.sh
rm -rf build dist
make all tools >/dev/null
bash tools/wdmbuild.sh >/dev/null
mkdir -p dist/AUDIONT
cp build/setup.exe   dist/AUDIONT/SETUP.EXE
cp build/audiont.sys dist/AUDIONT/AUDIONT.SYS
cp build/audiont.dll dist/AUDIONT/AUDIONT.DLL
cp build/audmix.exe  dist/AUDIONT/AUDMIX.EXE
cp build/attest.exe  dist/AUDIONT/AUDTEST.EXE
sed 's/$/\r/' tools/README.TXT > dist/AUDIONT/README.TXT
sed 's/$/\r/' LICENSE > dist/AUDIONT/LICENSE.TXT
# Intel audio DSP firmware (setup / the INF copy it to system32\drivers\audiont\fw)
mkdir -p dist/AUDIONT/FW
for f in fw/*; do cp "$f" "dist/AUDIONT/FW/$(basename "$f" | tr a-z A-Z)"; done
# Windows 2000 / XP
mkdir -p dist/AUDIONT/2KXP/I386 dist/AUDIONT/2KXP/AMD64
python3 - <<'PY'
import os
fw = sorted(os.listdir('fw'))
s = open('inf/audiont.inf').read()
s = s.replace(';FWDISK (tools/mkdist.sh lists the audio DSP firmware here)\n', ''.join('%s = 2\n' % f for f in fw))
s = s.replace(';FWLIST\n', ''.join('%s\n' % f for f in fw))
open('dist/AUDIONT/2KXP/AUDIONT.INF', 'w', newline='\r\n').write(s)
PY
cp build/2kxp/i386/audiont.sys dist/AUDIONT/2KXP/I386/AUDIONT.SYS
cp build/2kxp/amd64/audiont.sys dist/AUDIONT/2KXP/AMD64/AUDIONT.SYS
cp build/2kxp/amd64/audiont.dll dist/AUDIONT/2KXP/AMD64/AUDIONT.DLL
cp build/2kxp/amd64/setup.exe dist/AUDIONT/2KXP/AMD64/SETUP.EXE
cp build/2kxp/amd64/audmix.exe dist/AUDIONT/2KXP/AMD64/AUDMIX.EXE
cp build/2kxp/amd64/audtest.exe dist/AUDIONT/2KXP/AMD64/AUDTEST.EXE
# one package: the firmware (about 14 MB) does not fit a floppy, so no disk image any more
(cd dist && zip -q -r AUDIONT-$VER.zip AUDIONT)
git archive --format=zip --prefix=audiont-src-$VER/ -o dist/audiont-src-$VER.zip HEAD
ls -laR dist/AUDIONT; ls -la dist
