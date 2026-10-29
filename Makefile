.RECIPEPREFIX := >
TC := $(OO_PS4_TOOLCHAIN)
LIBS := -lc -lkernel -lSceSystemService
CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c -isysroot $(TC) -isystem $(TC)/include
LDFLAGS := -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib $(LIBS) $(TC)/lib/crt1.o

all: image0/eboot.bin

build/main.o: src/main.c
> mkdir -p build
> clang $(CFLAGS) -o $@ $<

build/eboot.elf: build/main.o
> ld.lld build/main.o -o $@ $(LDFLAGS)

image0/eboot.bin: build/eboot.elf
> $(TC)/bin/linux/create-fself -in=build/eboot.elf --out=build/eboot.oelf --eboot image0/eboot.bin --paid 0x3800000000000011

clean:
> rm -rf build image0/eboot.bin out
