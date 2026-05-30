#include "mm.h"
#include <stdint.h>
#include <string.h>

extern void kprintf(const char *fmt, ...);

#define ALLOC_MAGIC   0xA110C8EDu
#define FREE_MAGIC    0xFEEDFACEu
#define ALIGN         16u

typedef struct alloc_hdr {
    uint32_t magic;
    uint32_t size;          /* usable bytes after this header */
    struct alloc_hdr *next_free;
} alloc_hdr_t;

static alloc_hdr_t *g_free_list = NULL;
static uintptr_t    g_heap_start, g_heap_end;
static size_t       g_used;

boot_info_t g_boot_info;

static size_t align_up(size_t n) {
    return (n + ALIGN - 1) & ~(ALIGN - 1);
}

void mm_init(uintptr_t start, uintptr_t end) {
    g_heap_start = start;
    g_heap_end   = end;
    g_used       = 0;

    /* Align start to ALIGN boundary */
    start = (start + ALIGN - 1) & ~(ALIGN - 1u);
    if (start + sizeof(alloc_hdr_t) >= end) return;

    alloc_hdr_t *block = (alloc_hdr_t *)start;
    block->magic     = FREE_MAGIC;
    block->size      = (uint32_t)(end - start - sizeof(alloc_hdr_t));
    block->next_free = NULL;
    g_free_list      = block;
}

void *kmalloc(size_t size) {
    if (!size) return NULL;
    size = align_up(size);

    alloc_hdr_t *prev = NULL, *cur = g_free_list;
    while (cur) {
        if (cur->size >= size) {
            /* Split block if large enough to leave a useful remainder */
            size_t leftover = cur->size - size;
            if (leftover > sizeof(alloc_hdr_t) + ALIGN) {
                alloc_hdr_t *split = (alloc_hdr_t *)((uint8_t *)cur
                                     + sizeof(alloc_hdr_t) + size);
                split->magic     = FREE_MAGIC;
                split->size      = (uint32_t)(leftover - sizeof(alloc_hdr_t));
                split->next_free = cur->next_free;
                cur->size        = (uint32_t)size;
                cur->next_free   = split;
            }
            /* Remove from free list */
            if (prev) prev->next_free = cur->next_free;
            else      g_free_list     = cur->next_free;

            cur->magic     = ALLOC_MAGIC;
            cur->next_free = NULL;
            g_used        += cur->size;

            memset((uint8_t *)cur + sizeof(alloc_hdr_t), 0, cur->size);
            return (uint8_t *)cur + sizeof(alloc_hdr_t);
        }
        prev = cur;
        cur  = cur->next_free;
    }
    return NULL; /* OOM */
}

void kfree(void *ptr) {
    if (!ptr) return;
    alloc_hdr_t *hdr = (alloc_hdr_t *)((uint8_t *)ptr - sizeof(alloc_hdr_t));
    if (hdr->magic != ALLOC_MAGIC) {
        kprintf("[MM] kfree: bad magic at %p\n", ptr);
        return;
    }
    g_used       -= hdr->size;
    hdr->magic    = FREE_MAGIC;
    hdr->next_free = g_free_list;
    g_free_list   = hdr;
}

void mm_stats(size_t *used_out, size_t *free_out, size_t *total_out) {
    size_t total = g_heap_end - g_heap_start;
    size_t free  = 0;
    for (alloc_hdr_t *b = g_free_list; b; b = b->next_free)
        free += b->size;
    if (used_out)  *used_out  = g_used;
    if (free_out)  *free_out  = free;
    if (total_out) *total_out = total;
}
