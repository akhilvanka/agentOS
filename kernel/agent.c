#include "agent.h"
#include "riscv.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ---- Global state ---- */

Agent g_agents[MAX_AGENTS];
volatile agent_id_t g_current_agent = AGENT_NONE;
volatile uint64_t g_ticks = 0;

/* Safe save area for traps that fire while kernel code is running
 * (before the first ctx_restore to an agent). tp points here from
 * startup until the first real agent is scheduled. */
rv64_ctx_t g_kernel_ctx;

extern void kprintf(const char *fmt, ...);

/* ---- Internal helpers ---- */

static void strncpy_safe(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && i < n - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static agent_id_t alloc_agent_slot(void) {
    for (agent_id_t i = 1; i < MAX_AGENTS; ++i)
        if (g_agents[i].status == AGENT_EMPTY) return i;
    return AGENT_NONE;
}

static bool inbox_full(Agent *a) {
    return ((a->inbox_tail + 1) % INBOX_SIZE) == a->inbox_head;
}

static bool inbox_empty(Agent *a) {
    return a->inbox_head == a->inbox_tail;
}

/* ---- Goal-directed scheduling priority ---- */

/**
 * Compute a scheduling score for an agent.
 *
 * Score = urgency * (1 + deadline_urgency + progress_bonus)
 *
 * deadline_urgency: rises sharply as deadline approaches (exponential)
 * progress_bonus:   slightly favors agents making active progress
 *
 * This means:
 *   - High-urgency agents always beat low-urgency ones
 *   - Near-deadline agents get boosted within their urgency class
 *   - Agents making progress get a small bonus (positive feedback)
 *   - Stuck agents (no progress delta) get no bonus (natural preemption)
 */
uint32_t agent_schedule_score(const Agent *a) {
    if (a->status != AGENT_READY && a->status != AGENT_RUNNING)
        return 0;

    uint32_t score = (uint32_t)a->urgency;

    /* Deadline urgency: if within 10% of deadline, multiply score */
    if (a->deadline_ticks > 0 && a->deadline_ticks < 1000) {
        /* Boost inversely proportional to remaining time */
        uint32_t boost = 10000 / (a->deadline_ticks + 1);
        score += boost;
    }

    /* Progress velocity bonus: more progress last tick → small boost */
    if (a->progress > a->prev_progress) {
        score += (a->progress - a->prev_progress) / 4;
    }

    return score;
}

/* ---- Agent spawn ---- */

agent_id_t agent_spawn(agent_id_t parent,
                       const char *name,
                       const char *goal,
                       urgency_t urgency,
                       uint32_t deadline_ticks,
                       void (*entry)(void),
                       cap_flags_t caps) {
    intr_off();

    agent_id_t id = alloc_agent_slot();
    if (id == AGENT_NONE) {
        intr_on();
        return AGENT_NONE;
    }

    Agent *a = &g_agents[id];
    memset(a, 0, sizeof(*a));

    a->id     = id;
    a->status = AGENT_READY;
    a->parent = parent;
    strncpy_safe(a->name, name, sizeof(a->name));
    strncpy_safe(a->goal, goal, sizeof(a->goal));
    a->urgency        = (uint8_t)urgency;
    a->deadline_ticks = deadline_ticks;

    /* Default capability: send + recv + goal management */
    a->caps[0].flags = caps | CAP_SEND | CAP_RECV | CAP_GOAL_SET;

    /* Register with parent */
    if (parent != AGENT_NONE && parent < MAX_AGENTS) {
        Agent *p = &g_agents[parent];
        if (p->n_children < MAX_CHILDREN)
            p->children[p->n_children++] = id;
    }

    /* Set up initial context: sp → top of stack, pc → entry */
    uintptr_t stack_top = (uintptr_t)a->stack + STACK_SIZE;
    stack_top &= ~0xFUL; /* 16-byte align */

    a->ctx.pc = (uint64_t)(uintptr_t)entry;
    a->ctx.sp = stack_top;
    /* tp = pointer to this ctx (trap_entry uses tp as the save base).
     * SPP=1: sret returns to S-mode (agents run in S-mode, not U-mode).
     * SPIE=1: re-enable interrupts after sret. */
    a->ctx.tp = (uint64_t)(uintptr_t)&a->ctx;
    a->ctx.sstatus = SSTATUS_SPIE | SSTATUS_SIE | SSTATUS_SPP;

    kprintf("[AGENT] spawn id=%u name='%s' goal='%s' urgency=%u deadline=%u\n",
            id, name, goal, urgency, deadline_ticks);

    intr_on();
    return id;
}

/* ---- Agent exit ---- */

void agent_exit(bool success, const char *reason) {
    intr_off();
    Agent *a = agent_current();
    if (!a) { intr_on(); return; }

    a->status = success ? AGENT_DONE : AGENT_FAILED;

    if (success) {
        a->goal_completions++;
        kprintf("[AGENT] id=%u '%s' DONE: %s\n", a->id, a->name, reason);
    } else {
        a->goal_failures++;
        kprintf("[AGENT] id=%u '%s' FAILED: %s\n", a->id, a->name, reason);
    }

    /* Notify parent */
    if (a->parent != AGENT_NONE) {
        message_t msg;
        msg.type = success ? MSG_GOAL_DONE : MSG_GOAL_FAILED;
        msg.from = a->id;
        msg.to   = a->parent;
        msg.done.success = success ? 1 : 0;
        strncpy_safe(msg.done.reason, reason, sizeof(msg.done.reason));
        agent_send(a->parent, &msg);
    }

    intr_on();
    agent_yield();
    /* Never returns */
    for (;;) asm volatile("wfi");
}

/* ---- Progress reporting ---- */

void agent_set_progress(uint8_t pct) {
    Agent *a = agent_current();
    if (!a) return;
    a->prev_progress = a->progress;
    a->progress      = pct;

    /* Notify parent on significant milestones */
    if (a->parent != AGENT_NONE && (pct % 25 == 0) && pct != a->prev_progress) {
        message_t msg;
        msg.type               = MSG_PROGRESS;
        msg.from               = a->id;
        msg.to                 = a->parent;
        msg.progress.pct       = pct;
        msg.progress.ticks_remaining = a->deadline_ticks;
        agent_send(a->parent, &msg);
    }
}

void agent_set_goal(const char *goal) {
    Agent *a = agent_current();
    if (a) strncpy_safe(a->goal, goal, sizeof(a->goal));
}

void agent_set_subgoal(const char *subgoal) {
    Agent *a = agent_current();
    if (a) strncpy_safe(a->subgoal, subgoal, sizeof(a->subgoal));
}

void agent_set_contract(const char *pre, const char *post, const char *inv) {
    Agent *a = agent_current();
    if (!a) return;
    if (pre)  strncpy_safe(a->contract.precondition,  pre,  sizeof(a->contract.precondition));
    if (post) strncpy_safe(a->contract.postcondition, post, sizeof(a->contract.postcondition));
    if (inv)  strncpy_safe(a->contract.invariant,     inv,  sizeof(a->contract.invariant));
}

void agent_kill(agent_id_t id) {