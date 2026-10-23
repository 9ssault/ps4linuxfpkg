# PS4 Linux Installer - fake package build
#
# Requires the OpenOrbis PS4 toolchain (see scripts/setup-toolchain.sh) and a
# clang/ld.lld new enough for the x86_64-pc-freebsd12-elf target.

TITLE       := PS4 Linux Installer
VERSION     := 1.00
TITLE_ID    := PLNX00001
CONTENT_ID  := IV0000-PLNX00001_00-PS4LINUXINSTALL0

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
BIN         := $(TOOLCHAIN)/bin/linux

LIBS        := -lc -lkernel -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad \
               -lSceSysmodule -lSceFreeType -lSceNet -lSceHttp -lSceSsl -lSDL2

BUILD       := build
OUT         := out
PACKAGE     := $(OUT)/ps4-linux-installer.fpkg

SRCS        := $(wildcard src/*.c)
HDRS        := $(wildcard src/*.h)
OBJS        := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

# The boot payload sits at the package root: create-gp4 only knows the stock
# asset subdirectories, so a nested payload/ dir breaks PkgTool's tree lookup.
PAYLOADS    := ps4-linux-loader.elf ps4-linux-loader.bin
ASSETS      := $(shell find assets -type f | sort)
MODULES     := $(wildcard sce_module/*)

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -c \
               -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
               -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

.PHONY: all clean package
all: $(PACKAGE)

package: $(PACKAGE)

$(BUILD)/%.o: src/%.c $(HDRS)
	@mkdir -p $(BUILD)
	clang $(CFLAGS) -o $@ $<

$(BUILD)/app.elf: $(OBJS)
	ld.lld $(OBJS) -o $@ $(LDFLAGS)

eboot.bin: $(BUILD)/app.elf
	$(BIN)/create-fself -in=$< -out=$(BUILD)/app.oelf --eboot eboot.bin --paid 0x3800000000000011

sce_sys/param.sfo: Makefile
	$(BIN)/PkgTool.Core sfo_new $@
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(BIN)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(BIN)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

# The .gp4 must live next to eboot.bin/sce_sys: PkgTool resolves its file list
# relative to the project file's own directory.
pkg.gp4: eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx $(MODULES) $(ASSETS) $(PAYLOADS)
	$(BIN)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

$(PACKAGE): pkg.gp4
	@mkdir -p $(OUT)
	$(BIN)/PkgTool.Core pkg_build $< $(OUT)
	@cp $(OUT)/$(CONTENT_ID).pkg $@
	@echo "built $@"

clean:
	rm -rf $(BUILD) $(OUT) eboot.bin pkg.gp4 sce_sys/param.sfo
