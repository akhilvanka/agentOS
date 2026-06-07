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

    /* Save current agent's context */
    if (g_current_agent != AGENT_NONE) {
        Agent *cur = &g_agents[g_current_agent];
        /* ctx is already saved by trap_entry assembly into cur->ctx */
        if (cur->status == AGENT_RUNNING) cur->status = AGENT_READY;
        cur->ticks_run += TICKS_PER_TIMER;
    }

    g_ticks++;

    /* --- Handle cause --- */

    if (scause == SCAUSE_TIMER_IRQ) {
        /* Reprogram timer for next tick */
        uint64_t next_t = r_time() + CLINT_FREQ / (1000000 / TIMER_INTERVAL_US);
        sbi_set_timer(next_t);

        /* Advance sleep timers and deadline counters only on real timer ticks */
        advance_timers();

    } else if (scause == SCAUSE_ECALL_U || scause == SCAUSE_ECALL_S) {
                ((rv64_ctx_t *)ctx_ptr)->pc += 4;

    } else if (scause == 3) {
                ((rv64_ctx_t *)ctx_ptr)->pc += 4;

    } else {
        /* Unexpected trap — log and continue */
        kprintf("[TRAP] scause=0x%llx sepc=0x%llx stval=0x%llx agent=%u\n",
                (unsigned long long)scause,
                (unsigned long long)sepc,
                (unsigned long long)stval,
                (unsigned)g_current_agent);
    }

    /* --- Goal-directed scheduling --- */

    agent_id_t next_id = pick_next();

    if (next_id == AGENT_NONE) {
        /* All agents done/failed — print summary and halt */
        kprintf("\n[SCHED] All agents complete. System summary:\n");
        for (agent_id_t i = 1; i < MAX_AGENTS; ++i) {
            Agent *a = &g_agents[i];
            if (a->status == AGENT_EMPTY) continue;
            kprintf("  [%2u] %-12s ticks=%-6llu progress=%3u%% done=%u fail=%u\n",
                    a->id, a->name,
                    (unsigned long long)a->ticks_run,
                    a->progress,
                    a->goal_completions, a->goal_failures);
        }
        kprintf("[SCHED] Halting.\n");
        for (;;) asm volatile("wfi");
    }

    Agent *next = &g_agents[next_id];
    next->status = AGENT_RUNNING;
    g_current_agent = next_id;

        asm volatile("mv tp, %0" : : "r"(&next->ctx));

    ctx_restore(&next->ctx);
    /* Never returns */
}

/* ---- Print goal tree (diagnostics) ---- */

void sched_print_goal_tree(void) {
    kprintf("\n=== AgentOS Goal Tree (tick=%llu) ===\n",
            (unsigned long long)g_ticks);
    kprintf("%-4s %-12s %-8s %-4s %-4s %-6s %s\n",
            "ID", "Name", "Status", "Urg", "Pct", "Ticks", "Goal");
    kprintf("%s\n", "-------------------------------------------------------------------");

    for (agent_id_t i = 1; i < MAX_AGENTS; ++i) {
        Agent *a = &g_agents[i];
        if (a->status == AGENT_EMPTY) continue;

        const char *st = a->status == AGENT_READY    ? "READY  " :
                         a->status == AGENT_RUNNING   ? "RUNNING" :
                         a->status == AGENT_WAITING   ? "WAITING" :
                         a->status == AGENT_SLEEPING  ? "SLEEP  " :
                         a->status == AGENT_DONE      ? "DONE   " :
                         a->status == AGENT_FAILED    ? "FAILED " : "?      ";

        kprintf("[%2u] %-12s %s %3u %3u%% %6llu  %s\n",
                a->id, a->name, st,
                a->urgency, a->progress,
                (unsigned long long)a->ticks_run,
                a->goal);
    }
    kprintf("\n");
}
