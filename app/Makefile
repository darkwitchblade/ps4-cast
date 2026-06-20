# PS4 Cast — package metadata.
# STABLE title id: DirectPackageInstaller updates it in place (one icon). The
# display TITLE carries the version, and /status reports APP_VER so we always
# know which build is actually running.
VERSION     := 02.92
TITLE       := PS4 Cast $(VERSION)
TITLE_ID    := PCST00001
CONTENT_ID  := IV0000-PCST00001_00-PS4CAST000000001

# Stable receiver shell: conservative boot, dynamic playback/native handoff probes.
BOOT_MINIMAL := 0

# Playback backend: USE_FFMPEG=1 uses the ffmpeg software-decode player
# (player_ff.c + portlibs/ffmpeg); default uses the libSceAvPlayer player.c.
USE_FFMPEG  ?= 1
FFMPEG      := ../portlibs/ffmpeg
BEARSSL     := ../portlibs/bearssl

# Libraries linked into the ELF. BearSSL provides TLS for https; handshake
# entropy is derived in tls.c (sceRandom does not resolve on this setup).
LIBS        := -lc -lkernel -lc++ -lSceVideoOut -lSceGnmDriver -lSceNet -lSceNetCtl -lSceSysmodule \
               -lSceUserService -lSceSystemService -lSceAudioOut -lScePad \
               -L$(BEARSSL)/lib -lbearssl

ifeq ($(USE_FFMPEG),1)
LIBS        += -L$(FFMPEG)/lib --start-group \
               -lavformat -lavcodec -lswscale -lswresample -lavutil \
               --end-group -lm
endif

ASSETS      := $(shell find assets -type f 2>/dev/null)
LIBMODULES  := $(wildcard sce_module/*)

# Conservative profile that is known to boot on this console. Privileged
# Piglet authinfo currently trips PS4 system software errors before /status.
PAID        := 0x3800000000000011
AUTHINFO    :=
AUTHINFO_ARG := $(if $(strip $(AUTHINFO)),--authinfo $(AUTHINFO),)

# Toolchain root
TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
INTDIR      := build

# Sources
ifeq ($(BOOT_MINIMAL),1)
CFILES      := src/main.c src/gfx.c
else
ALLC        := $(wildcard src/*.c)
ifeq ($(USE_FFMPEG),1)
CFILES      := $(filter-out src/player.c,$(ALLC))
else
CFILES      := $(filter-out src/player_ff.c src/compat_ff.c,$(ALLC))
endif
endif
CPPFILES    := $(wildcard src/*.cpp)
OBJS        := $(patsubst src/%.c, $(INTDIR)/%.o, $(CFILES)) \
               $(patsubst src/%.cpp, $(INTDIR)/%.o, $(CPPFILES))

# Flags
CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) \
               -DAPP_VER='"$(VERSION)"' \
               $(if $(filter 1,$(BOOT_MINIMAL)),-DBOOT_MINIMAL,) \
               $(if $(filter 1,$(USE_FFMPEG)),-DUSE_FFMPEG -I$(FFMPEG)/include,) \
               -I$(BEARSSL)/include \
               -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
CXXFLAGS    := $(CFLAGS) -isystem $(TOOLCHAIN)/include/c++/v1
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr \
               -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

_unused     := $(shell mkdir -p $(INTDIR))

# Compilers — default to Apple-Silicon Homebrew paths; override on the CLI if needed.
UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CC      := clang
    CCX     := clang++
    LD      := ld.lld
    CDIR    := linux
endif
ifeq ($(UNAME_S),Darwin)
    CC      := /opt/homebrew/opt/llvm/bin/clang
    CCX     := /opt/homebrew/opt/llvm/bin/clang++
    LD      := /opt/homebrew/opt/lld/bin/ld.lld
    CDIR    := macos
endif

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(INTDIR)/ps4cast.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(INTDIR)/ps4cast.elf -out=$(INTDIR)/ps4cast.oelf --eboot "eboot.bin" --paid $(PAID) $(AUTHINFO_ARG)

$(INTDIR)/%.o: src/%.c
	$(CC) $(CFLAGS) -o $@ $<

$(INTDIR)/%.o: src/%.cpp
	$(CCX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(CONTENT_ID).pkg pkg.gp4 sce_sys/param.sfo eboot.bin $(INTDIR)/*.o $(INTDIR)/*.elf $(INTDIR)/*.oelf
