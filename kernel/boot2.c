#include "mm.h"
#include "riscv.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern void kprintf(const char *fmt, ...);
extern void kernel_main(void);

/* Symbols from linker script */
extern char _stack_top[];

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

static uint32_t fdt_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static uint64_t fdt_u64_safe(const uint8_t *p) {
    uint64_t hi = fdt_u32(p);
    uint64_t lo = fdt_u32(p + 4);
    return (hi << 32) | lo;
}

static int dtb_find_memory(uint64_t dtb_addr, uint64_t *base_out, uint64_t *size_out) {
    const uint8_t *blob = (const uint8_t *)(uintptr_t)dtb_addr;

    if (fdt_u32(blob) != FDT_MAGIC) return -1;

    uint32_t off_struct  = fdt_u32(blob + 8);
    uint32_t off_strings = fdt_u32(blob + 12);

    const uint8_t *s = blob + off_struct;
    const char    *strings = (const char *)(blob + off_strings);

    int depth = 0;
    int in_memory = 0;

    while (1) {
        /* Align to 4 bytes */
        uintptr_t addr = (uintptr_t)s;
        addr = (addr + 3) & ~3u;
        s = (const uint8_t *)addr;

        uint32_t token = fdt_u32(s);
        s += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)s;
            /* Advance past null-terminated name (padded to 4 bytes) */
            size_t nlen = strlen(name) + 1;
            s += (nlen + 3) & ~3u;
            depth++;
            /* Match memory nodes at depth 1: "memory" or "memory@..." */
            in_memory = (depth == 2 &&
                         (name[0]=='m' && name[1]=='e' && name[2]=='m' &&
                          name[3]=='o' && name[4]=='r' && name[5]=='y' &&
                          (name[6]=='\0' || name[6]=='@')));
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (in_memory && depth < 2) in_memory = 0;
            break;
        case FDT_PROP: {
            uint32_t len     = fdt_u32(s);     s += 4;
            uint32_t nameoff = fdt_u32(s);     s += 4;
            const char *pname = strings + nameoff;
            if (in_memory && len >= 16 &&
                pname[0]=='r' && pname[1]=='e' && pname[2]=='g' && pname[3]=='\0') {
                *base_out = fdt_u64_safe(s);
                *size_out = fdt_u64_safe(s + 8);
                return 0;
            }
            s += (len + 3) & ~3u;
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return -1;
        default:
            return -1;
        }
    }
}

void boot2_main(uint32_t hart_id, uint64_t dtb_addr) {
    /* Detect RAM via DTB */
    uint64_t ram_base = 0x80000000ULL; /* QEMU virt default */
    uint64_t ram_size = 0x08000000ULL; /* 128 MB default */

    if (dtb_addr && dtb_addr != 0xFFFFFFFF) {
        uint64_t b = 0, sz = 0;
        if (dtb_find_memory(dtb_addr, &b, &sz) == 0) {
            ram_base = b;
            ram_size = sz;
        }
    }

    /* Heap starts just above the kernel image (top of stack) */
    uintptr_t heap_start = (uintptr_t)_stack_top;
    uintptr_t heap_end   = (uintptr_t)(ram_base + ram_size);

    /* Guard: ensure heap doesn't exceed RAM */
    if (heap_start >= heap_end || heap_end - heap_start < 4096) {
        heap_end = heap_start + 0x400000; /* fallback: 4MB */
    }

    mm_init(heap_start, heap_end);

    /* Fill boot info record for kernel */
    g_boot_info.ram_base   = ram_base;
    g_boot_info.ram_size   = ram_size;
    g_boot_info.heap_start = heap_start;
    g_boot_info.heap_end   = heap_end;
    g_boot_info.dtb_addr   = dtb_addr;
    g_boot_info.hart_id    = hart_id;

    /* Boot banner */
    kprintf("\n");
    kprintf("  +--------------------------------------------------+\n");
    kprintf("  |  AgentOS  --  RISC-V RV64GC bare-metal kernel    |\n");
    kprintf("  |  Inspired by Microsoft Project Solara             |\n");
    kprintf("  +--------------------------------------------------+\n\n");
    kprintf("  Hart:   %u\n", hart_id);
    kprintf("  RAM:    0x%llx  (%llu MB)\n",
            (unsigned long long)ram_base,
            (unsigned long long)(ram_size >> 20));
    kprintf("  Heap:   0x%llx – 0x%llx  (%llu KB)\n",
            (unsigned long long)heap_start,
            (unsigned long long)heap_end,
            (unsigned long long)((heap_end - heap_start) >> 10));
    kprintf("  DTB:    0x%llx\n\n",
            (unsigned long long)dtb_addr);

    kernel_main();

    /* Should never reach here */
    for (;;) asm volatile("wfi");
}
