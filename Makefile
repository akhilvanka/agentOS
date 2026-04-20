# AgentOS Build System
#
# Requires: riscv64-unknown-elf-gcc (or riscv64-linux-gnu-gcc)
# QEMU:     qemu-system-riscv64
#
# Install on macOS:   brew install riscv-gnu-toolchain qemu
# Install on Ubuntu:  apt install gcc-riscv64-unknown-elf qemu-system-misc
#
# Build and run:
#   make          — build kernel ELF
#   make run      — run in QEMU virt (no display)
#   make debug    — run QEMU with GDB server on :1234
#   make star64   — build for Star64 (changes load address to 0x40200000)
#   make clean

CROSS   ?= riscv64-unknown-elf
CC       = $(CROSS)-gcc
AS       = $(CROSS)-gcc
LD       = $(CROSS)-ld
OBJCOPY  = $(CROSS)-objcopy
OBJDUMP  = $(CROSS)-objdump

TARGET   = agentOS

# Flags
CFLAGS   = -march=rv64gc -mabi=lp64d \
           -mcmodel=medany \
           -ffreestanding -fno-builtin -nostdlib -nostartfiles \
           -fno-stack-protector \
           -O2 -g \
           -Wall -Wextra -Wno-unused-parameter \
           -Iinclude

LDFLAGS  = -T linker.ld -nostdlib -Map=$(TARGET).map

SRCS_C   = kernel/kernel.c \
           kernel/agent.c \
           kernel/scheduler.c \
           kernel/string.c \
           kernel/mm.c \
           kernel/boot2.c \
           kernel/shell.c \
           agents/logger.c \
           agents/monitor.c \
           drivers/uart.c

SRCS_S   = kernel/boot.S

OBJS     = $(SRCS_C:.c=.o) $(SRCS_S:.S=.o)

# ---- Targets ----

all: $(TARGET).elf

$(TARGET).elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	@echo ""
	@echo "Built $(TARGET).elf"
	@$(CROSS)-size $@
	@echo ""

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -D__ASSEMBLER__ -c -o $@ $<

# ---- QEMU run ----

QEMU     = qemu-system-riscv64
QFLAGS   = -machine virt \
           -bios default \
           -nographic \
           -serial mon:stdio \
           -kernel $(TARGET).elf

run: all
	@echo "Running AgentOS in QEMU virt (Ctrl+A X to exit)..."
	$(QEMU) $(QFLAGS)

debug: all
	@echo "Starting QEMU with GDB server on :1234 (connect with: riscv64-unknown-elf-gdb agentOS.elf)"
	$(QEMU) $(QFLAGS) -S -gdb tcp::1234

# ---- Star64 target ----

star64: CFLAGS += -DSTAR64
star64: $(TARGET).elf
	@echo "Patching load address for Star64 (0x40200000)..."
	@sed -i.bak 's/0x80200000/0x40200000/' linker.ld
	$(MAKE) clean
	$(MAKE) $(TARGET).elf
	@sed -i.bak 's/0x40200000/0x80200000/' linker.ld
	@mv $(TARGET).elf $(TARGET)_star64.elf
	@echo "Star64 binary: $(TARGET)_star64.elf"
	@echo "Flash to SD: dd if=agentOS_star64.elf of=/dev/sdX bs=512 seek=2048"

# ---- Disassembly (useful for debugging bare-metal) ----

disasm: $(TARGET).elf
	$(OBJDUMP) -d -S $< | less

# ---- Clean ----

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).map $(TARGET)_star64.elf
	rm -f linker.ld.bak

.PHONY: all run debug star64 disasm clean
