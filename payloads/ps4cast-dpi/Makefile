ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
DPI := $(ROOT)/third_party/DirectPackageInstaller/Payload
CC := /opt/homebrew/opt/llvm/bin/clang
LD := /opt/homebrew/opt/lld/bin/ld.lld
OBJCOPY := /opt/homebrew/opt/llvm/bin/llvm-objcopy
YASM := yasm
CFLAGS := --target=x86_64-pc-freebsd12-elf -fuse-ld=$(LD) -g -mcmodel=small -isystem $(DPI)/freebsd-headers -I$(DPI)/ps4-libjbc -nostdinc -nostdlib -fno-stack-protector -static -fPIE -ffreestanding
CFLAGS += -DDPI_BGFT_OPTIONS=0x10008 -DDPI_SINGLE_SHOT=1 -DDPI_UNINSTALL_FIRST=1
CFLAGS += -DDPI_RESIDENT_SERVER=1 -DDPI_AGENT_PORT=9192 -DDPI_PERSISTENT=1
LDFLAGS := -Wl,-T,$(ROOT)/payloads/ps4cast-control/payload.ld -Wl,-gc-sections
LIB := build/lib.a
JBC := $(wildcard $(DPI)/ps4-libjbc/*.c)

.PHONY: all clean

all: build/ps4cast-dpi.bin

build:
	mkdir -p build

build/crt.o: $(DPI)/lib/crt.asm | build
	$(YASM) -f elf64 $< -o $@

build/syscalls.o: $(DPI)/lib/syscalls.asm | build
	$(YASM) -f elf64 $< -o $@

build/dl.o: $(DPI)/lib/dl.c | build
	$(CC) --target=x86_64-pc-freebsd12-elf -c -isystem $(DPI)/freebsd-headers -nostdinc -fno-stack-protector $< -o $@ -fPIE -ffreestanding

$(LIB): build/crt.o build/dl.o build/syscalls.o | build
	$(LD) -r $^ -o $@

build/ps4cast-dpi.elf: $(DPI)/main.c $(DPI)/info.c $(LIB) | build
	$(CC) $(CFLAGS) $(LIB) $(DPI)/main.c $(DPI)/info.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-dpi.bin: build/ps4cast-dpi.elf
	$(OBJCOPY) $< --only-section .text --only-section .data --only-section .bss --only-section .rodata -O binary $@

clean:
	rm -rf build
