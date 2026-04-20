/**
 * AgentOS linker script
 *
 * QEMU virt: load address 0x80200000 (OpenSBI puts us here)
 * Star64:    load address 0x40200000 (OpenSBI on JH7110)
 *
 * To switch targets: change ORIGIN to 0x40200000 for Star64.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x80200000, LENGTH = 120M
}

SECTIONS {
    . = ORIGIN(RAM);

    /* Boot code first — _start must be at the entry address */
    .text.boot : {
        KEEP(*(.text.boot))
    } > RAM

    .text : {
        *(.text .text.*)
    } > RAM

    .rodata : {
        *(.rodata .rodata.*)
    } > RAM

    .data : {
        *(.data .data.*)
    } > RAM

    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    } > RAM

    /* 64KB kernel stack */
    . = ALIGN(16);
    _stack_bottom = .;
    . += 0x10000;
    _stack_top = .;
}
