# ==============================================================================
#  Quanta OS — Makefile
#  Phase 2: x2APIC, SMP, Scheduler, VirtIO, VFS, QAI Shell
# ==============================================================================

# ---------------------------------------------------------------------------
# Toolchain — prefer x86_64-elf-gcc, fall back to x86_64-linux-gnu
# ---------------------------------------------------------------------------
ifneq ($(shell which x86_64-elf-gcc 2>/dev/null),)
    CC      := x86_64-elf-gcc
    LD      := x86_64-elf-ld
    OBJCOPY := x86_64-elf-objcopy
else
    CC      := x86_64-linux-gnu-gcc
    LD      := x86_64-linux-gnu-ld
    OBJCOPY := x86_64-linux-gnu-objcopy
endif

QEMU    := qemu-system-x86_64
XORRISO := xorriso
PYTHON  := python3

# ---------------------------------------------------------------------------
# Directories
# ---------------------------------------------------------------------------
KERNEL_DIR := kernel
SRC_DIR    := $(KERNEL_DIR)/src
BUILD_DIR  := build
ISO_DIR    := $(BUILD_DIR)/iso_root

# ---------------------------------------------------------------------------
# Sources — auto-discover all .c and .S files under kernel/src/
# ---------------------------------------------------------------------------
C_SOURCES := $(shell find $(SRC_DIR) -name '*.c')
S_SOURCES := $(shell find $(SRC_DIR) -name '*.S' ! -name 'test_user.S')

C_OBJECTS := $(patsubst $(SRC_DIR)/%.c,  $(BUILD_DIR)/%.o,   $(C_SOURCES))
S_OBJECTS := $(patsubst $(SRC_DIR)/%.S,  $(BUILD_DIR)/%.S.o, $(S_SOURCES))
OBJECTS   := $(C_OBJECTS) $(S_OBJECTS)

KERNEL_ELF := $(BUILD_DIR)/quanta.elf
ISO_IMAGE  := $(BUILD_DIR)/quanta.iso

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
CFLAGS := \
    -std=c11                        \
    -O2                             \
    -Wall                           \
    -Wextra                         \
    -Wno-unused-parameter           \
    -ffreestanding                  \
    -fno-stack-protector            \
    -fno-stack-check                \
    -fno-pic                        \
    -fno-pie                        \
    -m64                            \
    -march=x86-64                   \
    -mno-red-zone                   \
    -mno-mmx                        \
    -mno-sse                        \
    -mno-sse2                       \
    -mcmodel=kernel                 \
    -I$(KERNEL_DIR)/include         \
    -I$(SRC_DIR)                    \
    -I$(BUILD_DIR)/generated        \
    -MMD                            \
    -MP                             \
    -Wa,--noexecstack 

# ---------------------------------------------------------------------------
# Linker flags
# ---------------------------------------------------------------------------
LDFLAGS := \
    -T $(KERNEL_DIR)/kernel.ld      \
    -nostdlib                       \
    -static                         \
    -z max-page-size=0x1000         \
    --no-dynamic-linker             \
    -z noexecstack

# ---------------------------------------------------------------------------
# Limine bootloader
# Pinned to v8.x-binary — prebuilt blobs, only need `make` for the
# limine bios-install utility.
# ---------------------------------------------------------------------------
LIMINE_DIR     := limine
LIMINE_VERSION := v8.x-binary
LIMINE_REPO    := https://github.com/limine-bootloader/limine.git

# ---------------------------------------------------------------------------
# Test userspace binary (Ring 3 hello world)
# ---------------------------------------------------------------------------
TEST_USER_ASM := $(SRC_DIR)/realm/test_user.S
TEST_USER_ELF := $(BUILD_DIR)/test_user.elf
TEST_USER_HDR := $(BUILD_DIR)/generated/realm/test_user_bin.h

$(TEST_USER_ELF): $(TEST_USER_ASM)
	@mkdir -p $(dir $@)
	$(CC) -m64 -nostdlib -static -Wl,-Ttext=0x400000 -Wl,--no-dynamic-linker \
	    -Wa,--noexecstack -Wl,-z,noexecstack $< -o $@
	@echo "[USER] $@"

$(TEST_USER_HDR): $(TEST_USER_ELF)
	@mkdir -p $(dir $@)
	xxd -i $< | sed 's/unsigned/static const unsigned/' > $@
	@echo "[XXD]  $@"

.PHONY: test_user
test_user: $(TEST_USER_HDR)

# ---------------------------------------------------------------------------
# Default target
# ---------------------------------------------------------------------------
.PHONY: all
all: generate_stubs test_user $(KERNEL_ELF)

# ---------------------------------------------------------------------------
# Compile C sources
# ---------------------------------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(TEST_USER_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "[CC]  $<"

# ---------------------------------------------------------------------------
# Assemble .S files
# ---------------------------------------------------------------------------
$(BUILD_DIR)/%.S.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@
	@echo "[AS]  $<"

# ---------------------------------------------------------------------------
# Link kernel ELF
# ---------------------------------------------------------------------------
$(KERNEL_ELF): $(OBJECTS) $(KERNEL_DIR)/kernel.ld
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $@
	@echo "[LD]  $@"

DEPFILES := $(C_OBJECTS:.o=.d)
-include $(DEPFILES)

# ---------------------------------------------------------------------------
# Regenerate ISR assembly stubs
# ---------------------------------------------------------------------------
STUB_GENERATOR := $(SRC_DIR)/cpu/gen_isr_stubs.py

.PHONY: generate_stubs
generate_stubs: $(SRC_DIR)/cpu/isr_stubs.S

$(SRC_DIR)/cpu/isr_stubs.S: $(STUB_GENERATOR)
	$(PYTHON) $(STUB_GENERATOR)
	@echo "[PY]  generated isr_stubs.S"

# ---------------------------------------------------------------------------
# Fetch Limine (clone once, build the bios-install tool)
# ---------------------------------------------------------------------------
$(LIMINE_DIR)/limine:
	@echo "[LIMINE] Cloning Limine $(LIMINE_VERSION)..."
	git clone --depth=1 --branch=$(LIMINE_VERSION) $(LIMINE_REPO) $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR)
	@echo "[LIMINE] Done."

.PHONY: limine
limine: $(LIMINE_DIR)/limine

# ---------------------------------------------------------------------------
# Blank disk image (64 MiB) for VirtIO block device
# ---------------------------------------------------------------------------
.PHONY: disk
disk: $(BUILD_DIR)/disk.img

$(BUILD_DIR)/disk.img:
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	@echo "[DISK] $@"

# ---------------------------------------------------------------------------
# Bootable ISO (Limine BIOS + UEFI hybrid)
# ---------------------------------------------------------------------------
.PHONY: iso
iso: $(KERNEL_ELF) $(LIMINE_DIR)/limine
	@echo "[ISO] Building $(ISO_IMAGE)..."
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/limine
	@mkdir -p $(ISO_DIR)/EFI/BOOT
	cp $(KERNEL_ELF)                             $(ISO_DIR)/boot/
	cp limine.conf                               $(ISO_DIR)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys             $(ISO_DIR)/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin          $(ISO_DIR)/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin          $(ISO_DIR)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI                $(ISO_DIR)/EFI/BOOT/
	$(XORRISO) -as mkisofs \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image \
	    --protective-msdos-label \
	    $(ISO_DIR) -o $(ISO_IMAGE) 2>/dev/null
	$(LIMINE_DIR)/limine bios-install $(ISO_IMAGE)
	@echo "[ISO] Done: $(ISO_IMAGE)"

# ---------------------------------------------------------------------------
# QEMU run targets
# ---------------------------------------------------------------------------
QEMU_BASE := \
    $(QEMU)            \
    -M q35             \
    -m 512M            \
    -smp 4,cores=4     \

.PHONY: run
run: iso disk
	$(QEMU_BASE) \
	    -cdrom $(ISO_IMAGE) \
	    -device virtio-blk-pci,drive=vblk0 \
	    -drive id=vblk0,file=$(BUILD_DIR)/disk.img,if=none,format=raw \
	    -serial stdio \
	    -display sdl

.PHONY: run-nographic
run-nographic: iso disk
	$(QEMU_BASE) \
	    -cdrom $(ISO_IMAGE) \
	    -device virtio-blk-pci,drive=vblk0 \
	    -drive id=vblk0,file=$(BUILD_DIR)/disk.img,if=none,format=raw \
	    -serial stdio \
	    -display none

.PHONY: run-kvm
run-kvm: iso disk
	$(QEMU_BASE) \
	    -enable-kvm -cpu host \
	    -cdrom $(ISO_IMAGE) \
	    -device virtio-blk-pci,drive=vblk0 \
	    -drive id=vblk0,file=$(BUILD_DIR)/disk.img,if=none,format=raw \
	    -serial stdio \
	    -display sdl

.PHONY: run-uefi
run-uefi: iso disk
	$(QEMU_BASE) \
	    -bios /usr/share/ovmf/OVMF.fd \
	    -cdrom $(ISO_IMAGE) \
	    -device virtio-blk-pci,drive=vblk0 \
	    -drive id=vblk0,file=$(BUILD_DIR)/disk.img,if=none,format=raw \
	    -serial stdio \
	    -display sdl

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(SRC_DIR)/cpu/isr_stubs.S
	@echo "[CLEAN] build/ removed"

.PHONY: distclean
distclean: clean
	rm -rf $(LIMINE_DIR)
	@echo "[CLEAN] limine/ removed"
