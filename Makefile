CC      = i686-w64-mingw32-gcc
PY      = python3
OUT     = build
KCFLAGS = -O2 -march=i386 -mtune=pentium -ffreestanding -fno-builtin-printf \
          -fno-stack-protector -fno-asynchronous-unwind-tables -fno-exceptions \
          -mno-stack-arg-probe -mgeneral-regs-only -fno-pic -fno-common -fno-strict-aliasing \
          -Wall -Wno-unused-function -Iinc -Idrv $(DEFS)
KLDFLAGS = -nostdlib -shared -Wl,--subsystem,native -Wl,-e,_DriverEntry@8 \
          -Wl,--image-base,0x10000 -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 \
          -Wl,--exclude-all-symbols -Wl,--enable-reloc-section -s -Llib
DRV_SRC = $(filter-out drv/pcireloc.c drv/wdm.c,$(wildcard drv/*.c))
DRV_OBJ = $(patsubst drv/%.c,$(OUT)/obj/%.o,$(DRV_SRC))

all: $(OUT)/audiont.sys

$(OUT)/obj/%.o: drv/%.c $(wildcard drv/*.h) drv/pcireloc.c inc/ntk.h
	@mkdir -p $(OUT)/obj
	$(CC) $(KCFLAGS) -c $< -o $@

$(OUT)/audiont.sys: $(DRV_OBJ) lib/libntoskrnl.a lib/libhal.a
	$(CC) $(KLDFLAGS) -o $@ $(DRV_OBJ) -lntoskrnl -lhal -lgcc
	$(PY) tools/pefix.py $@ driver

clean:
	rm -rf $(OUT)

UCFLAGS = -O2 -march=i386 -mtune=pentium -fno-stack-protector -fno-asynchronous-unwind-tables \
          -fno-builtin -Wall -Wno-unused-function -Iuser -Iinc $(UDEFS)
ULDFLAGS = -nostdlib -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 \
          -Wl,--image-base,0x400000 -Wl,--enable-reloc-section -s
ULIBS   = -luser32 -lkernel32 -ladvapi32 -lgdi32 -lwinmm -lcomdlg32 -lshell32 -lgcc

RC      = i686-w64-mingw32-windres

$(OUT)/uobj/%.res.o: tools/res/%.rc tools/res/%.ico
	@mkdir -p $(OUT)/uobj
	cd tools/res && $(RC) -O coff $*.rc -o ../../$@

$(OUT)/uobj/%.o: user/%.c user/ucrt.h
	@mkdir -p $(OUT)/uobj
	$(CC) $(UCFLAGS) -c $< -o $@

$(OUT)/nttest.exe: $(OUT)/uobj/nttest.o $(OUT)/uobj/ucrt.o
	$(CC) $(ULDFLAGS) -mconsole -Wl,-e,_WinMainCRTStartup@0 -o $@ $^ $(ULIBS)
	$(PY) tools/pefix.py $@ user

tools: $(OUT)/nttest.exe

$(OUT)/attest.exe: $(OUT)/uobj/attest.o $(OUT)/uobj/ucrt.o $(OUT)/uobj/audtest.res.o
	$(CC) $(ULDFLAGS) -mconsole -Wl,-e,_WinMainCRTStartup@0 -o $@ $^ $(ULIBS)
	$(PY) tools/pefix.py $@ user

tools: $(OUT)/attest.exe

$(OUT)/audiont.dll: $(OUT)/uobj/audrv.o $(OUT)/uobj/ucrt.o user/audrv.def
	$(CC) $(ULDFLAGS) -shared -Wl,-e,_DllMain@12 -Wl,--image-base,0x6F000000 -o $@ $(OUT)/uobj/audrv.o $(OUT)/uobj/ucrt.o user/audrv.def $(ULIBS)
	$(PY) tools/pefix.py $@ user

tools: $(OUT)/audiont.dll

$(OUT)/setup.exe: $(OUT)/uobj/setup.o $(OUT)/uobj/pnp5.o $(OUT)/uobj/ucrt.o $(OUT)/uobj/setup.res.o
	$(CC) $(ULDFLAGS) -mwindows -Wl,-e,_WinMainCRTStartup@0 -o $@ $^ $(ULIBS)
	$(PY) tools/pefix.py $@ user

$(OUT)/audmix.exe: $(OUT)/uobj/audmix.o $(OUT)/uobj/ucrt.o $(OUT)/uobj/audmix.res.o
	$(CC) $(ULDFLAGS) -mwindows -Wl,-e,_WinMainCRTStartup@0 -o $@ $^ $(ULIBS)
	$(PY) tools/pefix.py $@ user

tools: $(OUT)/setup.exe $(OUT)/audmix.exe

$(OUT)/uobj/setup.res.o: tools/res/setup2.ico
