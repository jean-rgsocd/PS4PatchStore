.RECIPEPREFIX := >
TC := $(OO_PS4_TOOLCHAIN)
LIBS ?= -lc -lkernel -lSceSystemService
EXTRA ?=
CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c -isysroot $(TC) -isystem $(TC)/include $(EXTRA)
LDFLAGS := -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib $(LIBS) $(TC)/lib/crt1.o

all: image0/eboot.bin

build/main.o: src/main.c
> mkdir -p build
> clang $(CFLAGS) -o $@ $<

build/eboot.elf: build/main.o
> ld.lld build/main.o -o $@ $(LDFLAGS)

image0/eboot.bin: build/eboot.elf
> $(TC)/bin/linux/create-fself -in=build/eboot.elf --out=build/eboot.oelf --eboot image0/eboot.bin --paid 0x3800000000000011

# ---------------------------------------------------------------- app nativo (interface propria)
# Gera native0/eboot.bin. -O2 e essencial: a UI e desenhada por software na CPU do PS4.
NATIVE_SRC  := ps4_main ui gfx catalog install
NATIVE_OBJS := $(addprefix build/native/,$(addsuffix .o,$(NATIVE_SRC)))
NATIVE_LIBS := -lc -lkernel -lSceVideoOut -lScePad -lSceUserService -lSceSystemService
NATIVE_CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c -O2 -fno-math-errno -Isrc \
  -isysroot $(TC) -isystem $(TC)/include

native: native0/eboot.bin

build/native/%.o: src/%.c
> mkdir -p build/native
> clang $(NATIVE_CFLAGS) -o $@ $<

build/native/eboot.elf: $(NATIVE_OBJS)
> ld.lld $(NATIVE_OBJS) -o $@ -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib $(NATIVE_LIBS) $(TC)/lib/crt1.o

native0/eboot.bin: build/native/eboot.elf
> mkdir -p native0
> $(TC)/bin/linux/create-fself -in=build/native/eboot.elf --out=build/native/eboot.oelf --eboot native0/eboot.bin --paid 0x3800000000000011

clean:
> rm -rf build image0/eboot.bin native0 out
