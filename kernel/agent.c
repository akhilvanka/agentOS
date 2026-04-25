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