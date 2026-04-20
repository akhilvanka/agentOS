#pragma once

#define UART_BASE        0x10000000UL
#define UART_THR         0x00  /* transmit holding register */
#define UART_RBR         0x00  /* receive buffer register */
#define UART_LSR         0x05  /* line status register */
#define UART_LSR_TX_EMPTY 0x20
#define UART_LSR_RX_READY 0x01

#define CLINT_BASE       0x02000000UL
#define CLINT_MTIMECMP   0x00004000UL  /* per-hart, 8 bytes */
#define CLINT_MTIME      0x0000BFF8UL

/* sstatus bits */
#define SSTATUS_SIE      (1UL << 1)   /* S-mode interrupt enable */
#define SSTATUS_SPIE     (1UL << 5)   /* S-mode prior interrupt enable */
#define SSTATUS_SPP      (1UL << 8)   /* S-mode previous privilege */
#define SSTATUS_SUM      (1UL << 18)  /* supervisor user memory access */

/* sie bits */
#define SIE_SSIE         (1UL << 1)   /* S-mode software interrupt */
#define SIE_STIE         (1UL << 5)   /* S-mode timer interrupt */
#define SIE_SEIE         (1UL << 9)   /* S-mode external interrupt */

/* scause values */
#define SCAUSE_IRQ_BIT   (1UL << 63)
#define SCAUSE_TIMER_IRQ (SCAUSE_IRQ_BIT | 5)
#define SCAUSE_ECALL_U   8UL
#define SCAUSE_ECALL_S   9UL

/* SBI function IDs (OpenSBI) */
#define SBI_SET_TIMER    0
#define SBI_PUTCHAR      1
#define SBI_GETCHAR      2

#ifndef __ASSEMBLER__
#include <stdint.h>

static inline uint64_t r_sstatus(void) {
    uint64_t x; asm volatile("csrr %0, sstatus" : "=r"(x)); return x;
}
static inline void w_sstatus(uint64_t x) {
    asm volatile("csrw sstatus, %0" : : "r"(x));
}
static inline uint64_t r_sie(void) {
    uint64_t x; asm volatile("csrr %0, sie" : "=r"(x)); return x;
}
static inline void w_sie(uint64_t x) {
    asm volatile("csrw sie, %0" : : "r"(x));
}
static inline uint64_t r_sepc(void) {
    uint64_t x; asm volatile("csrr %0, sepc" : "=r"(x)); return x;
}
static inline void w_sepc(uint64_t x) {
    asm volatile("csrw sepc, %0" : : "r"(x));
}
static inline uint64_t r_scause(void) {
    uint64_t x; asm volatile("csrr %0, scause" : "=r"(x)); return x;
}
static inline uint64_t r_stval(void) {
    uint64_t x; asm volatile("csrr %0, stval" : "=r"(x)); return x;
}
static inline void w_stvec(uint64_t x) {
    asm volatile("csrw stvec, %0" : : "r"(x));
}
static inline uint64_t r_time(void) {
    uint64_t x; asm volatile("csrr %0, time" : "=r"(x)); return x;
}
static inline void intr_on(void) {
    w_sstatus(r_sstatus() | SSTATUS_SIE);
}
static inline void intr_off(void) {
    w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}
static inline int intr_get(void) {
    return (r_sstatus() & SSTATUS_SIE) ? 1 : 0;
}

/* SBI ecall — talk to OpenSBI firmware */
static inline long sbi_call(long which, long arg0, long arg1, long arg2) {
    register long a0 asm("a0") = arg0;
    register long a1 asm("a1") = arg1;
    register long a2 asm("a2") = arg2;
    register long a7 asm("a7") = which;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline void sbi_set_timer(uint64_t stime_value) {
    sbi_call(SBI_SET_TIMER, (long)stime_value, 0, 0);
}

static inline void sbi_putchar(char c) {
    sbi_call(SBI_PUTCHAR, c, 0, 0);
}

/* Returns the next character from the console, or -1 if none available. */
static inline int sbi_getchar(void) {
    return (int)sbi_call(SBI_GETCHAR, 0, 0, 0);
}

/* Memory-mapped I/O accessors */
static inline volatile uint8_t* mmio8(uintptr_t addr) {
    return (volatile uint8_t*)addr;
}

#endif /* !__ASSEMBLER__ */
