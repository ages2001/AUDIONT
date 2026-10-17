#!/bin/bash
# Builds AUDIONT for Windows 2000 / XP with MinGW-w64: build/2kxp/i386/audiont.sys and
# build/2kxp/amd64/audiont.sys (Plug and Play), the WinMM driver audiont.dll and the tools
# for both architectures.
set -e
cd "$(dirname "$0")/.."
SRC="ac97 ac97codec convert ctrl engine hda main os synth usbaud via wdm acpi sst sstipc4 sstipc3 sstcodec sdw sdw_sdca sdw_rt711 sdw_rt1316 sdw_rtcom sdw_rt712 sdw_rt722 sdw_rt715 sdw_rt700 sdw_rt5682 sdw_rt1318 sdw_rt1320 sdw_rt1308 sdw_max98373 sdw_cs35l56 sdw_cs42l43 i2c gpio spi sideamp lpe $(cd drv && ls cod_*.c | sed 's/\.c$//' | tr '\n' ' ')"
COMMON="-c -O2 -std=gnu99 -Wall -Wno-unused-function -Wno-unused-parameter -fno-builtin -ffreestanding \
 -fno-stack-protector -mno-stack-arg-probe -mgeneral-regs-only -fno-strict-aliasing -fno-common -DAUDIONT_WDM=1 \
 -I/usr/share/mingw-w64/include/ddk -Iinc -Idrv -Werror=implicit-function-declaration -Werror=int-to-pointer-cast \
 -Werror=pointer-to-int-cast"
for arch in i386 amd64; do
  O=build/2kxp/$arch; mkdir -p $O/obj
  if [ $arch = i386 ]; then
    CC=i686-w64-mingw32-gcc; CF="$COMMON -march=i586 -mtune=generic -fno-asynchronous-unwind-tables -D_X86_=1"
    ENTRY=_DriverEntry@8; OSV="5,--minor-subsystem-version,0"
  else
    CC=x86_64-w64-mingw32-gcc; CF="$COMMON -mno-red-zone -mcmodel=small -D_AMD64_=1 -D_WIN64=1"
    ENTRY=DriverEntry; OSV="5,--minor-subsystem-version,2"
  fi
  OBJS=""
  for f in $SRC; do $CC $CF drv/$f.c -o $O/obj/$f.o; OBJS="$OBJS $O/obj/$f.o"; done
  $CC -nostdlib -o $O/audiont.sys $OBJS -Wl,--subsystem,native -Wl,--image-base,0x10000 -Wl,--entry,$ENTRY \
    -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -Wl,--enable-reloc-section \
    -Wl,--major-subsystem-version,$OSV -Wl,--no-insert-timestamp -Wl,-s -lntoskrnl -lhal -lgcc
  python3 tools/fixpe5.py $O/audiont.sys $arch
done
# the 64 bit WinMM driver and tools (2kxp/amd64); the 32 bit ones (build/*.dll, *.exe) serve
# Windows 2000 / XP x86 and the 32 bit programs on x64
O=build/2kxp/amd64; CC=x86_64-w64-mingw32-gcc
UCF="-O2 -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin -Wall -Wno-unused-function -Iuser -Iinc"
ULF="-nostdlib -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -Wl,--enable-reloc-section -s \
 -Wl,--major-subsystem-version,5,--minor-subsystem-version,2 -Wl,--major-os-version,5,--minor-os-version,2"
ULIBS="-luser32 -lkernel32 -ladvapi32 -lgdi32 -lwinmm -lcomdlg32 -lshell32 -lgcc"
mkdir -p $O/uobj
for f in audrv setup pnp5 audmix attest ucrt; do $CC $UCF -c user/$f.c -o $O/uobj/$f.o; done
for r in setup audmix audtest; do (cd tools/res && x86_64-w64-mingw32-windres -O coff $r.rc -o ../../$O/uobj/$r.res.o); done
$CC $ULF -shared -Wl,-e,DllMain -o $O/audiont.dll $O/uobj/audrv.o $O/uobj/ucrt.o user/audrv64.def $ULIBS
$CC $ULF -mwindows -Wl,-e,WinMainCRTStartup -o $O/setup.exe $O/uobj/setup.o $O/uobj/pnp5.o $O/uobj/ucrt.o $O/uobj/setup.res.o $ULIBS
$CC $ULF -mwindows -Wl,-e,WinMainCRTStartup -o $O/audmix.exe $O/uobj/audmix.o $O/uobj/ucrt.o $O/uobj/audmix.res.o $ULIBS
$CC $ULF -mconsole -Wl,-e,WinMainCRTStartup -o $O/audtest.exe $O/uobj/attest.o $O/uobj/ucrt.o $O/uobj/audtest.res.o $ULIBS
ls -la build/2kxp/*/*.sys $O/*.dll $O/*.exe
