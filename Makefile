# Requer OpenOrbis PS4 Toolchain: export OO_PS4_TOOLCHAIN=/caminho/OpenOrbis/PS4Toolchain
TC   := $(OO_PS4_TOOLCHAIN)
BIN  := $(TC)/bin/linux
CFLAGS  := -cc1 -triple x86_64-pc-freebsd-elf -munwind-tables -I$(TC)/include -emit-obj
LDFLAGS := -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib -lc -lkernel -lSceSystemService $(TC)/lib/crt1.o

all: image0/eboot.bin

build/main.o: src/main.c
	@mkdir -p build
	clang $(CFLAGS) -o $@ $<

build/eboot.elf: build/main.o
	ld.lld $(LDFLAGS) -o $@ $<

image0/eboot.bin: build/eboot.elf
	$(BIN)/create-fself -in=build/eboot.elf -out=build/eboot.oelf --eboot image0/eboot.bin --paid 0x3800000000000011

clean:
	rm -rf build image0/eboot.bin out
