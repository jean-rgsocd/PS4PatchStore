# JK_CHECK_VAR v4.14-EXP - build experimental SEPARADO (nao substitui o jk_check_var.prx v4.13).
# Gera jk_check_var_exp.prx (nome proprio): troque no plugins.ini so durante o teste.
# A v4.13 (plugins/jk_check_var) continua intacta.
TOOLCHAIN := $(OO_PS4_TOOLCHAIN)
SDK       := ../goldhen_sdk
TARGET    := jk_check_var_exp.prx
CFLAGS    := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O1 -c -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -I$(SDK)/include
LDFLAGS   := -m elf_x86_64 -pie --export-dynamic --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr -L$(TOOLCHAIN)/lib -L$(SDK)
LIBS      := -lGoldHEN_Hook -lkernel -lSceLibcInternal -lSceSysmodule

all: $(TARGET)

build/main.o: source/main.c
	@mkdir -p build
	clang $(CFLAGS) -o $@ $<

$(TARGET): build/main.o
	ld.lld build/main.o $(SDK)/build/crtprx.o -o build/jk_check_var_exp.elf $(LDFLAGS) $(LIBS)
	$(TOOLCHAIN)/bin/linux/create-fself -in=build/jk_check_var_exp.elf -out=build/jk_check_var_exp.oelf --lib=$(TARGET) --paid 0x3800000000000011

clean:
	rm -rf build $(TARGET)
