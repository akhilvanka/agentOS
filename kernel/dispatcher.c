#include "agent.h"
#include "riscv.h"
#include "mm.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern void kprintf(const char *fmt, ...);
extern void logger_write(const char *tag, const char *msg);

#define MAX_RULES           8
#define DISPATCH_POLL_TICKS 20

typedef struct {
    trigger_kind_t kind;
    uint32_t       threshold;
    char           blueprint[16];
    uint32_t       cooldown;
    uint64_t       last_fired;
    uint32_t       fires;
    bool           used;
} trigger_rule_t;

static trigger_rule_t g_rules[MAX_RULES];
static int            g_n_rules = 0;

static void copy_str(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && i < n - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

int dispatcher_add_rule(trigger_kind_t kind, uint32_t threshold,
                        const char *blueprint, uint32_t cooldown_ticks) {
    if (g_n_rules >= MAX_RULES) return -1;
    trigger_rule_t *r = &g_rules[g_n_rules];
    r->kind       = kind;
    r->threshold  = threshold;
    r->cooldown   = cooldown_ticks;
    r->last_fired = 0;
    r->fires      = 0;
    r->used       = true;
    copy_str(r->blueprint, blueprint, sizeof(r->blueprint));
    return g_n_rules++;
}

/* Activate the rule's blueprint. If the trigger has a subject agent
 * (e.g. the one that failed), pass its id along as the first message. */
static void fire(trigger_rule_t *r, agent_id_t subject) {
    agent_id_t id = agent_dispatch((agent_id_t)g_current_agent, r->blueprint);
    if (id == AGENT_NONE) return;

    r->last_fired = g_ticks;
    r->fires++;

    if (subject != AGENT_NONE) {
        message_t m = {0};
        m.type = MSG_DATA;
        m.to   = id;
        m.raw.data[0] = (uint8_t)(subject & 0xFF);
        m.raw.data[1] = (uint8_t)(subject >> 8);
        m.raw.len     = 2;
        agent_send(id, &m);
    }
}

static void evaluate_rules(void) {
    for (int i = 0; i < g_n_rules; ++i) {
        trigger_rule_t *r = &g_rules[i];
        if (!r->used) continue;
        if (r->last_fired && g_ticks - r->last_fired < r->cooldown) continue;

        if (r->kind == TRIG_AGENT_FAILED) {
            for (agent_id_t j = 1; j < MAX_AGENTS; ++j) {
                Agent *a = &g_agents[j];
                if (a->status != AGENT_FAILED || a->fault_handled) continue;
                a->fault_handled = 1;
                kprintf("[DISP] agent %u '%s' failed -> activating '%s'\n",
                        j, a->name, r->blueprint);
                logger_write("DISP", a->name);
                fire(r, j);
                break;   /* one activation per rule per pass */
            }

        } else if (r->kind == TRIG_MEM_USED_PCT) {
            size_t used, free_b, total;
            mm_stats(&used, &free_b, &total);
            if (total && (used * 100 / total) >= r->threshold) {
                kprintf("[DISP] heap at %u%% -> activating '%s'\n",
                        (unsigned)(used * 100 / total), r->blueprint);
                logger_write("DISP", "memory pressure");
                fire(r, AGENT_NONE);
            }

        } else if (r->kind == TRIG_AGENT_COUNT) {
            uint32_t alive = 0;
            for (agent_id_t j = 1; j < MAX_AGENTS; ++j) {
                agent_status_t s = g_agents[j].status;
                if (s != AGENT_EMPTY && s != AGENT_DONE && s != AGENT_FAILED)
                    alive++;
            }
            if (alive >= r->threshold) {
                kprintf("[DISP] %u live agents -> activating '%s'\n",
                        alive, r->blueprint);
                logger_write("DISP", "agent count high");
                fire(r, AGENT_NONE);
            }
        }
    }
}

void dispatcher_print_rules(void) {
    if (g_n_rules == 0) {
        kprintf("  (no trigger rules)\n");
        return;
    }

    kprintf("\n  Trigger rules (%d):\n", g_n_rules);
    kprintf("  %-3s %-14s %-10s %-12s %-9s %s\n",
            "#", "Condition", "Threshold", "Blueprint", "Cooldown", "Fires");
    kprintf("  %s\n",
            "--------------------------------------------------------------");
    for (int i = 0; i < g_n_rules; ++i) {
        trigger_rule_t *r = &g_rules[i];
        const char *kind = r->kind == TRIG_AGENT_FAILED ? "agent-failed" :
                           r->kind == TRIG_MEM_USED_PCT ? "mem-used-pct" :
                                                          "agent-count";
        kprintf("  %-3d %-14s %-10u %-12s %-9u %u\n",
                i, kind, r->threshold, r->blueprint, r->cooldown, r->fires);
    }
    kprintf("\n");
}

void dispatcher_agent_main(void) {
    agent_set_goal("Activate agents when trigger conditions arise");
    agent_set_contract(
        "Trigger rules registered",
        "Matching blueprint dispatched per firing rule",
        "At most one activation per rule per cooldown"
    );

    for (;;) {
        agent_sleep(DISPATCH_POLL_TICKS);
        evaluate_rules();
    }
}
