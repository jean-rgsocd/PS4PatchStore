# JK_CHECK_VAR v4.16-LIC - VAR so em penalti/expulsao + LICENCA ASSINADA (Ed25519).
# Build SEPARADO: gera jk_check_var_v416.prx. A v4.13 (plugins/jk_check_var) continua intacta.
# Antes de distribuir: troque source/lic_pub.h pelo gerado em plataforma/central/gerar_chaves.mjs.
TOOLCHAIN := $(OO_PS4_TOOLCHAIN)
SDK       := ../goldhen_sdk
TARGET    := jk_check_var_v416.prx
CFLAGS    := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O1 -c -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -I$(SDK)/include -Isource
LDFLAGS   := -m elf_x86_64 -pie --export-dynamic --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr -L$(TOOLCHAIN)/lib -L$(SDK)
LIBS      := -lGoldHEN_Hook -lkernel -lSceLibcInternal -lSceSysmodule
OBJS      := build/main.o build/jk_lic.o build/tweetnacl.o

all: $(TARGET)

build/%.o: source/%.c
	@mkdir -p build
	clang $(CFLAGS) -o $@ $<

$(TARGET): $(OBJS)
	ld.lld $(OBJS) $(SDK)/build/crtprx.o -o build/jk_check_var_v416.elf $(LDFLAGS) $(LIBS)
	$(TOOLCHAIN)/bin/linux/create-fself -in=build/jk_check_var_v416.elf -out=build/jk_check_var_v416.oelf --lib=$(TARGET) --paid 0x3800000000000011

clean:
	rm -rf build $(TARGET)
