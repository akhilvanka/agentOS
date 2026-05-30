#pragma once
#include <stddef.h>
#include <stdint.h>

void  mm_init(uintptr_t heap_start, uintptr_t heap_end);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void  mm_stats(size_t *used_out, size_t *free_out, size_t *total_out);

/* Boot information record filled in by boot2_main, read by kernel_main. */
typedef struct {
    uint64_t ram_base;      /* physical base of RAM (from DTB /memory) */
    uint64_t ram_size;      /* total RAM size in bytes */
    uint64_t heap_start;    /* first allocatable byte (above kernel image) */
    uint64_t heap_end;      /* last allocatable byte */
    uint64_t dtb_addr;      /* physical address of FDT blob */
    uint32_t hart_id;       /* boot hart ID */
} boot_info_t;

extern boot_info_t g_boot_info;
