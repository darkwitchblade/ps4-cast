DPI_PAYLOAD_DIR ?= /private/tmp/dpi-src/DirectPackageInstaller-DN6/Payload
CC := /opt/homebrew/opt/llvm/bin/clang
LD := /opt/homebrew/opt/lld/bin/ld.lld
OBJCOPY := /opt/homebrew/opt/llvm/bin/llvm-objcopy
YASM := yasm
CFLAGS := --target=x86_64-pc-freebsd12-elf -fuse-ld=$(LD) -g -mcmodel=small -isystem $(DPI_PAYLOAD_DIR)/freebsd-headers -nostdinc -nostdlib -fno-stack-protector -static -fPIE -ffreestanding
CALLBACK_IP ?= 0x8B01A8C0U
CFLAGS += -DCALLBACK_IP_ADDR=$(CALLBACK_IP)
LDFLAGS := -Wl,-T,payload.ld -Wl,-gc-sections
LIB := build/lib.a
JBC := $(wildcard $(DPI_PAYLOAD_DIR)/ps4-libjbc/*.c)

.PHONY: all clean deps

all: build/ps4cast-launch.bin build/ps4cast-kill.bin build/ps4cast-uninstall.bin

.PHONY: unsafe-proc-tools

unsafe-proc-tools: build/ps4cast-probe.bin build/ps4cast-patch.bin
	@echo "WARNING: proc probe/patch payloads caused CE-36329-3 on this console."

.PHONY: handoff-test

handoff-test: build/ps4cast-handoff-test.bin

.PHONY: media-player-tools

media-player-tools: build/ps4cast-install-media.bin build/ps4cast-launch-media.bin build/ps4cast-app-status.bin

deps:
	$(MAKE) -C $(DPI_PAYLOAD_DIR)/lib

build:
	mkdir -p build

build/crt.o: $(DPI_PAYLOAD_DIR)/lib/crt.asm | build
	$(YASM) -f elf64 $< -o $@

build/syscalls.asm: $(DPI_PAYLOAD_DIR)/lib/syscalls.py | build
	python3 $< > $@

build/syscalls.o: build/syscalls.asm | build
	$(YASM) -f elf64 $< -o $@

build/dl.o: $(DPI_PAYLOAD_DIR)/lib/dl.c | build
	$(CC) --target=x86_64-pc-freebsd12-elf -c -isystem $(DPI_PAYLOAD_DIR)/freebsd-headers -nostdinc -fno-stack-protector $< -o $@ -fPIE -ffreestanding

$(LIB): build/crt.o build/dl.o build/syscalls.o | build
	$(LD) -r $^ -o $@

build/ps4cast-launch.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_LAUNCH $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-kill.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_KILL $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-uninstall.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_UNINSTALL $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-probe.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_PROBE $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-patch.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_PATCH $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-handoff-test.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_HANDOFF_TEST $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-launch-media.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_LAUNCH -DCONTROL_LAUNCH_MEDIA $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-launch-lapy20002.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_LAUNCH -DCONTROL_LAUNCH_LAPY20002 $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-launch-npxx51363.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_LAUNCH -DCONTROL_LAUNCH_NPXX51363 $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-install-media.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_INSTALL_MEDIA_PLAYER $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-install-pkg.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_INSTALL_PKG $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-app-status.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_APP_STATUS $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

build/ps4cast-reboot.elf: main.c $(LIB) | build
	$(CC) $(CFLAGS) -DCONTROL_REBOOT $(LIB) main.c $(JBC) $(LDFLAGS) -o $@

%.bin: %.elf
	$(OBJCOPY) $< --only-section .text --only-section .data --only-section .bss --only-section .rodata -O binary $@

clean:
	rm -rf build
