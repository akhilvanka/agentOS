
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
    kprintf("  Observing agent %u '%s' — press any key to stop\n\n", id, a->name);

    uint8_t last_pct = 0xFF;
    while (1) {
        /* Check for keypress */
        if (sbi_getchar() != -1) break;

        a = agent_get(id);
        if (!a || a->status == AGENT_EMPTY || a->status == AGENT_DONE
               || a->status == AGENT_FAILED) {
            kprintf("  Agent terminated (status=%u)\n", a ? a->status : 0);
            break;
        }

        if (a->progress != last_pct) {
            last_pct = a->progress;
            const char *st =
                a->status == AGENT_READY    ? "READY  " :
                a->status == AGENT_RUNNING  ? "RUNNING" :
                a->status == AGENT_WAITING  ? "WAITING" :
                a->status == AGENT_SLEEPING ? "SLEEP  " : "?      ";
            kprintf("  [%llu] id=%-2u %s  urgency=%-3u  progress=%3u%%  deadline=%u\n"
                    "         goal: %s\n"
                    "         sub:  %s\n\n",
                    (unsigned long long)g_ticks,
                    id, st, a->urgency, a->progress, a->deadline_ticks,
                    a->goal[0]    ? a->goal    : "(none)",
                    a->subgoal[0] ? a->subgoal : "(none)");
        }
        agent_yield();
    }
}

/* ------------------------------------------------------------------ */
/* Command dispatcher                                                   */
/* ------------------------------------------------------------------ */

static void shell_dispatch(const char *line) {
    char cmd[32];
    const char *rest = sh_word(line, cmd, sizeof(cmd));

    if (!cmd[0]) return;

    if (sh_strcmp(cmd, "help") == 0) {
        cmd_help();

    } else if (sh_strcmp(cmd, "ps") == 0) {
        cmd_ps();

    } else if (sh_strcmp(cmd, "tree") == 0) {
        sched_print_goal_tree();

    } else if (sh_strcmp(cmd, "blueprints") == 0) {
        kprintf("\n");
        agent_list_blueprints();
        kprintf("\n");

    } else if (sh_strcmp(cmd, "spawn") == 0) {
        char name[32];
        sh_word(rest, name, sizeof(name));
        if (!name[0]) {
            kprintf("  usage: spawn <blueprint_name>\n");
            return;
        }
        agent_id_t id = agent_dispatch(AGENT_NONE, name);
        if (id == AGENT_NONE) {
            kprintf("  Failed to spawn '%s' — unknown blueprint or table full\n", name);
        } else {
            kprintf("  Spawned agent id=%u from blueprint '%s'\n", id, name);
        }

    } else if (sh_strcmp(cmd, "kill") == 0) {
        char num[16];
        sh_word(rest, num, sizeof(num));
        if (!num[0]) { kprintf("  usage: kill <id>\n"); return; }
        agent_id_t id = (agent_id_t)sh_atoi(num);
        agent_kill(id);

    } else if (sh_strcmp(cmd, "observe") == 0) {
        char num[16];
        sh_word(rest, num, sizeof(num));
        if (!num[0]) { kprintf("  usage: observe <id>\n"); return; }
        cmd_observe((agent_id_t)sh_atoi(num));

    } else if (sh_strcmp(cmd, "stats") == 0) {
        cmd_stats();

    } else if (sh_strcmp(cmd, "log") == 0) {
        logger_dump();

    } else {
        kprintf("  Unknown command '%s'. Type 'help' for options.\n", cmd);
    }
}

/* ------------------------------------------------------------------ */
/* Shell agent main loop                                                */
/* ------------------------------------------------------------------ */

void shell_agent_main(void) {
    agent_set_goal("Dispatch and observe agents via UART shell");
    agent_set_contract(
        "UART available via SBI",
        "User commands are dispatched until shutdown",
        "Shell never holds the CPU without yielding"
    );
    agent_subscribe(EVT_SHUTDOWN);

    static char line[128];
    int pos = 0;

    kprintf("\n  AgentOS shell ready. Type 'help' for commands.\n\n");
    kprintf("agentOS> ");

    for (;;) {
        int c = sbi_getchar();
        if (c == -1) {
                        agent_sleep(3);
            continue;
        }

        if (c == '\r' || c == '\n') {
            sbi_putchar('\n');
            line[pos] = '\0';
            if (pos > 0) shell_dispatch(line);
            pos = 0;
            kprintf("agentOS> ");

        } else if ((c == 8 || c == 127) && pos > 0) {
            /* backspace */
            pos--;
            sbi_putchar('\b');
            sbi_putchar(' ');
            sbi_putchar('\b');

        } else if (c >= 0x20 && c < 0x7f && pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)c;
            sbi_putchar((char)c);
        }
    }
}
