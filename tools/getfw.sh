#!/bin/sh
# Gathers the Intel audio DSP firmware AUDIONT loads into fw/ under 8.3 names
# (the drivers\audiont\fw directory of an installation; NT 3.1 on FAT has no
# long names).  Sources: a linux-firmware checkout and a sof-bin checkout.
#   tools/getfw.sh [linux-firmware] [sof-bin]
set -e
LF=${1:-/home/claude/ref/fw-endlessm}
SB=${2:-/home/claude/ref/sofbin}
OUT=$(dirname "$0")/../fw
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$LF/intel/avs/skl/dsp_basefw.bin" "$OUT/avs-skl.bin"
cp "$LF/intel/avs/apl/dsp_basefw.bin" "$OUT/avs-apl.bin"
cp "$LF/intel/avs/cnl/dsp_basefw.bin" "$OUT/avs-cnl.bin"
cp "$LF/LICENCE.adsp_sst" "$OUT/LIC-AVS.TXT"
sof() { (cd "$SB" && git show "HEAD:$1") > "$OUT/$2"; }
for p in icl jsl ehl; do sof "v2.2.x/sof-v2.2/intel-signed/sof-$p.ri" "sof-$p.ri"; done
for p in bdw byt cht; do sof "v2.2.x/sof-v2.2/sof-$p.ri" "sof-$p.ri"; done
for p in tgl tgl-h adl adl-s adl-n; do              # rpl, rpl-s: the adl, adl-s images
    sof "v2.12.x/sof-ipc4-v2.12/$p/intel-signed/sof-$p.ri" "i4-$(echo $p | tr -d -).ri"
done
for p in mtl arl-s lnl ptl wcl nvl nvl-s; do        # arl: the mtl image
    sof "v2.15.x/sof-ipc4-v2.15/$p/intel-signed/sof-$p.ri" "i4-$(echo $p | tr -d -).ri"
done
(cd "$SB" && git show HEAD:LICENCE.Intel) > "$OUT/LIC-SOF.TXT"
cat > "$OUT/README.TXT" <<'EOF'
Intel audio DSP firmware loaded by AUDIONT (Intel Smart Sound Technology).
Install it to %SystemRoot%\system32\drivers\audiont\fw (setup does so).

avs-skl.bin  Skylake, Kaby Lake, Amber Lake, Coffee Lake-S  (linux-firmware intel/avs/skl, 9.21.00.4899)
avs-apl.bin  Apollo Lake, Gemini Lake                       (intel/avs/apl, 9.22.01.4917)
avs-cnl.bin  Cannon Lake, Whiskey Lake, Comet Lake          (intel/avs/cnl, 10.23.00.8551)
             licence: LIC-AVS.TXT
sof-*.ri     Sound Open Firmware 2.2 (IPC3): icl jsl ehl (Intel signed); bdw (Broadwell),
             byt (Bay Trail), cht (Cherry Trail / Braswell)
i4-*.ri      Sound Open Firmware IPC4, Intel signed: tgl tglh adl (also rpl) adls (also rpl-s) adln (2.12),
             mtl (also arl) arls lnl ptl wcl nvl nvls (2.15)
             licence: LIC-SOF.TXT (BSD-3-Clause)
EOF
ls -la "$OUT" | awk '{s+=$5} END {print s " bytes"}'
