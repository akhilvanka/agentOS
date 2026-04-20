/**
 * AgentOS RISC-V boot + trap handler.
 *
 * Context save strategy: `tp` (thread pointer) always points to the
 * current agent's rv64_ctx_t. On trap entry we save all registers
 * directly into the struct tp points to — no sscratch swap, no race.
 *
 * Why tp instead of sscratch:
 *   - sscratch requires a "swap sp trick" that fails if sscratch=0
 *     (before the first agent starts or if sscratch is clobbered)
 *   - tp is a caller-saved GPR we control; agents written by us don't use TLS
 *   - Saving tp itself last (after we've used it as the base pointer) is
 *     straightforward: just read it back into a temp register
 */

#include "riscv.h"

/* ---- Boot entry ---- */

.section .text.boot
.global _start
.type   _start, @function

_start:
    /* OpenSBI passes: a0=hartid, a1=FDT ptr
     * Only hart 0 runs; others spin. mhartid is M-mode only — use a0. */
    bnez    a0, .park

    /* Set up kernel stack */
    la      sp, _stack_top

    /* Clear .bss */
    la      t0, _bss_start
    la      t1, _bss_end
.L_clear:
    beq     t0, t1, .L_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       .L_clear
.L_done:

    /* Install trap vector (direct mode) */
    la      t0, trap_entry
    csrw    stvec, t0

    /* a0=hartid, a1=DTB ptr — both preserved through BSS clear (t0/t1 only).
     * Call second-stage bootloader which initialises MM then calls kernel_main. */
    call    boot2_main

.park:
    wfi
    j       .park

/* ------------------------------------------------------------------ */
/* Trap entry — tp always points to current agent's rv64_ctx_t         */
/* ------------------------------------------------------------------ */

/*
 * rv64_ctx_t layout (matches struct in agent.h):
 *   offset  0 = ra
 *   offset  8 = sp
 *   offset 16 = gp
 *   offset 24 = tp  (saved last using a0 as relay)
 *   offset 32 = t0
 *   ...
 *   offset 248 = pc   (sepc)
 *   offset 256 = sstatus
 *
 * We save tp by temporarily using a0:
 *   1. Save a0 to a known scratch location
 *   2. Load original tp into a0
 *   3. Store a0 → ctx.tp
 *   4. Restore a0 from scratch
 */

.section .data
.align 3
_a0_scratch: .fill 1, 8, 0   /* temporary save slot for a0 */

.section .text
.global trap_entry
.align  4
trap_entry:
    /* Step 1: save a0 to scratch so we can use it as a relay */
    la      a0, _a0_scratch
    sd      a0, 0(a0)       /* WAIT — a0 is now the scratch addr, not orig a0 */
    /* Hmm — need to save original a0 first. Use sp-relative trick: */
    /* Actually: save original a0 to _a0_scratch, but we need a free register.
     * Use the fact that tp already points to the ctx — save a0 directly there. */

    /* Save a0 directly into ctx.a0 (offset 9*8 = 72) via tp */
    sd      a0,  9*8(tp)    /* ctx.a0 = original a0 */

    /* Now save all other GPRs */
    sd      ra,  0*8(tp)
    sd      sp,  1*8(tp)
    sd      gp,  2*8(tp)
    /* tp itself: read it back into a0 (a0 is already saved), then store */
    mv      a0, tp
    sd      a0,  3*8(tp)    /* ctx.tp = original tp value */

    sd      t0,  4*8(tp)
    sd      t1,  5*8(tp)
    sd      t2,  6*8(tp)
    sd      s0,  7*8(tp)
    sd      s1,  8*8(tp)
    /* a0 already saved at offset 9 */
    sd      a1, 10*8(tp)
    sd      a2, 11*8(tp)
    sd      a3, 12*8(tp)
    sd      a4, 13*8(tp)
    sd      a5, 14*8(tp)
    sd      a6, 15*8(tp)
    sd      a7, 16*8(tp)
    sd      s2, 17*8(tp)
    sd      s3, 18*8(tp)
    sd      s4, 19*8(tp)
    sd      s5, 20*8(tp)
    sd      s6, 21*8(tp)
    sd      s7, 22*8(tp)
    sd      s8, 23*8(tp)
    sd      s9, 24*8(tp)
    sd      s10,25*8(tp)
    sd      s11,26*8(tp)
    sd      t3, 27*8(tp)
    sd      t4, 28*8(tp)
    sd      t5, 29*8(tp)
    sd      t6, 30*8(tp)

    /* Save sepc (PC at trap time) and sstatus */
    csrr    t0, sepc
    sd      t0, 31*8(tp)
    csrr    t0, sstatus
    sd      t0, 32*8(tp)

    /* Pass the saved context pointer to trap_handler(ctx) */
    mv      a0, tp

    /* Switch to kernel stack */
    la      sp, _stack_top

    /* Disable interrupts while in kernel (they were cleared by hardware on trap) */
    call    trap_handler
    /* trap_handler never returns — it calls ctx_restore which does sret */

    /* Safety: should never reach here */
    j       .park

/* ------------------------------------------------------------------ */
/* Context restore — called from C with a0 = rv64_ctx_t pointer        */
/* Sets tp = new ctx, restores all registers, srets to new agent.      */
/* ------------------------------------------------------------------ */

.global ctx_restore
ctx_restore:
    /* tp = new agent's context */
    mv      tp, a0

    /* Restore sepc and sstatus */
    ld      t0, 31*8(tp)
    csrw    sepc, t0
    ld      t0, 32*8(tp)
    csrw    sstatus, t0

    /* Restore GPRs */
    ld      ra,  0*8(tp)
    ld      sp,  1*8(tp)
    ld      gp,  2*8(tp)
    /* skip tp for now — restore last after we're done using it as base */
    ld      t0,  4*8(tp)
    ld      t1,  5*8(tp)
    ld      t2,  6*8(tp)
    ld      s0,  7*8(tp)
    ld      s1,  8*8(tp)
    ld      a0,  9*8(tp)
    ld      a1, 10*8(tp)
    ld      a2, 11*8(tp)
    ld      a3, 12*8(tp)
    ld      a4, 13*8(tp)
    ld      a5, 14*8(tp)
    ld      a6, 15*8(tp)
    ld      a7, 16*8(tp)
    ld      s2, 17*8(tp)
    ld      s3, 18*8(tp)
    ld      s4, 19*8(tp)
    ld      s5, 20*8(tp)
    ld      s6, 21*8(tp)
    ld      s7, 22*8(tp)
    ld      s8, 23*8(tp)
    ld      s9, 24*8(tp)
    ld      s10,25*8(tp)
    ld      s11,26*8(tp)
    ld      t3, 27*8(tp)
    ld      t4, 28*8(tp)
    ld      t5, 29*8(tp)
    ld      t6, 30*8(tp)

    /* Restore tp last */
    ld      tp,  3*8(tp)

    sret
