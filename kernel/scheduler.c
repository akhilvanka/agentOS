#include "agent.h"
#include "riscv.h"
#include <stdint.h>
#include <string.h>

extern void kprintf(const char *fmt, ...);
extern void ctx_restore(void *ctx);
extern uint32_t agent_schedule_score(const Agent *a);
extern bool     agent_send(agent_id_t to, message_t *msg);

/* Timer interval: 10ms at 10MHz CLINT clock (QEMU virt) */
#define TIMER_INTERVAL_US  10000UL
#define CLINT_FREQ         10000000UL
#define TICKS_PER_TIMER    1

/* ---- Goal-directed scheduler ---- */

static void advance_timers(void) {
    for (agent_id_t i = 1; i < MAX_AGENTS; ++i) {
        Agent *a = &g_agents[i];
        if (a->status == AGENT_EMPTY) continue;

        /* Wake sleeping agents */
        if (a->status == AGENT_SLEEPING) {
            if (a->sleep_ticks > 0) a->sleep_ticks--;
            if (a->sleep_ticks == 0) a->status = AGENT_READY;
        }

        /* Tick deadlines for active agents */
        if (a->deadline_ticks > 0 &&
            (a->status == AGENT_READY || a->status == AGENT_RUNNING ||
             a->status == AGENT_WAITING || a->status == AGENT_SLEEPING)) {
            a->deadline_ticks--;
            if (a->deadline_ticks == 0) {
                kprintf("[SCHED] DEADLINE MISSED: agent %u '%s' goal='%s'\n",
                        a->id, a->name, a->goal);
                a->status = AGENT_FAILED;
                a->goal_failures++;
                /* Notify parent */
                if (a->parent != AGENT_NONE) {
                    message_t msg;
                    msg.type = MSG_GOAL_FAILED;
                    msg.from = a->id;
                    msg.to   = a->parent;
                    msg.done.success = 0;
                    const char *r = "deadline exceeded";
                    for (int k = 0; r[k] && k < 50; ++k) msg.done.reason[k] = r[k];
                    agent_send(a->parent, &msg);
                }
            }
        }
    }
}

static agent_id_t pick_next(void) {
    agent_id_t best_id    = AGENT_NONE;
    uint32_t   best_score = 0;
    bool       any_alive  = false;
    bool       any_ready  = false;

    for (agent_id_t i = 1; i < MAX_AGENTS; ++i) {
        Agent *a = &g_agents[i];
        if (a->status == AGENT_EMPTY) continue;
        if (a->status == AGENT_DONE || a->status == AGENT_FAILED) continue;

        any_alive = true;

        uint32_t score = agent_schedule_score(a);
        if (score == 0 && a->status != AGENT_READY && a->status != AGENT_RUNNING) continue;

        /* Schedule this agent if it's the first candidate OR has higher score */
        if (best_id == AGENT_NONE || score > best_score) {
            best_score = score;
            best_id    = i;
            any_ready  = true;
        }
    }

    if (!any_alive) return AGENT_NONE;   /* all done/failed — print summary + halt */
    if (!any_ready) return 0;            /* all alive but blocked — run idle */
    return best_id;
}

/* ---- Trap handler (called from assembly trap_entry) ---- */

void trap_handler(void *ctx_ptr) {
    uint64_t scause  = r_scause();
    uint64_t sepc    = r_sepc();
    uint64_t stval   = r_stval();
