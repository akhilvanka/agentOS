

#include "agent.h"
#include "riscv.h"
#include "mm.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

extern void kprintf(const char *fmt, ...);
extern void sched_print_goal_tree(void);
extern void logger_dump(void);       /* agents/logger.c */

/* ------------------------------------------------------------------ */
/* Minimal string helpers (no libc)                                    */
/* ------------------------------------------------------------------ */

static int sh_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int sh_atoi(const char *s) {
    int n = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

/* Advance past leading spaces; return pointer to first non-space char */
static const char *sh_skip(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Copy word into buf (up to n-1 chars), return pointer to char after word */
static const char *sh_word(const char *s, char *buf, size_t n) {
    s = sh_skip(s);
    size_t i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < n - 1)
        buf[i++] = *s++;
    buf[i] = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* Command implementations                                              */
/* ------------------------------------------------------------------ */

static void cmd_help(void) {
    kprintf("\n  AgentOS Shell — available commands:\n\n");
    kprintf("    ps                    list all agents\n");
    kprintf("    tree                  print live goal tree\n");
    kprintf("    blueprints            list available agent blueprints\n");
    kprintf("    spawn <blueprint>     dispatch an agent by blueprint name\n");
    kprintf("    kill <id>             forcibly terminate an agent\n");
    kprintf("    observe <id>          watch an agent live (any key to stop)\n");
    kprintf("    stats                 memory and scheduler statistics\n");
    kprintf("    log                   dump recent system log\n");
    kprintf("    help                  this message\n\n");
}

static void cmd_ps(void) {
    kprintf("\n  %-4s %-14s %-10s %-8s %-4s %-6s %-8s  %s\n",
            "ID", "Name", "Blueprint", "Status", "Urg", "Pct%", "Ticks", "Goal");
    kprintf("  %s\n",
            "-------------------------------------------------------------------------");
    for (agent_id_t i = 1; i < MAX_AGENTS; ++i) {
        Agent *a = &g_agents[i];
        if (a->status == AGENT_EMPTY) continue;

        const char *st =
            a->status == AGENT_READY    ? "READY   " :
            a->status == AGENT_RUNNING  ? "RUNNING " :
            a->status == AGENT_WAITING  ? "WAITING " :
            a->status == AGENT_SLEEPING ? "SLEEP   " :
            a->status == AGENT_DONE     ? "DONE    " :
            a->status == AGENT_FAILED   ? "FAILED  " : "?       ";

        kprintf("  [%2u] %-14s %-10s %s %3u  %3u%%  %6llu   %s\n",
                a->id, a->name,
                a->blueprint[0] ? a->blueprint : "-",
                st, a->urgency, a->progress,
                (unsigned long long)a->ticks_run,
                a->goal);
    }
    kprintf("\n");
}

static void cmd_stats(void) {
    size_t used, free, total;
    mm_stats(&used, &free, &total);

    int alive = 0, done = 0, failed = 0;
    uint64_t total_ticks = 0, total_msgs = 0;
    for (agent_id_t i = 1; i < MAX_AGENTS; ++i) {
        Agent *a = &g_agents[i];
        if (a->status == AGENT_EMPTY) continue;
        if (a->status == AGENT_DONE || a->status == AGENT_FAILED) {
            if (a->status == AGENT_FAILED) failed++;
            else done++;
        } else {
            alive++;
        }
        total_ticks += a->ticks_run;
        total_msgs  += a->messages_sent;
    }

    kprintf("\n  System statistics (tick=%llu):\n",
            (unsigned long long)g_ticks);
    kprintf("    Agents:   %d alive  %d done  %d failed\n", alive, done, failed);
    kprintf("    CPU:      %llu total ticks dispatched\n",
            (unsigned long long)total_ticks);
    kprintf("    IPC:      %llu messages sent\n",
            (unsigned long long)total_msgs);
    kprintf("    Memory:   %u KB used  %u KB free  %u KB total\n",
            (unsigned)(used >> 10),
            (unsigned)(free >> 10),
            (unsigned)(total >> 10));
    kprintf("\n");
}

static void cmd_observe(agent_id_t id) {
    Agent *a = agent_get(id);
    if (!a || a->status == AGENT_EMPTY) {
        kprintf("  No agent with id=%u\n", id);
        return;
    }