

#include "agent.h"
#include "mm.h"
#include <stdint.h>

extern void kprintf(const char *fmt, ...);
extern void logger_write(const char *tag, const char *msg);

void monitor_agent_main(void) {
    agent_set_goal("Report system health periodically");
    agent_set_subgoal("Waiting for next reporting interval");
    agent_set_contract(
        "Agent table and MM initialized",
        "Periodic health reports printed to console",
        "Does not modify any agent or system state"
    );

    int round = 0;
    for (;;) {
        agent_sleep(200);
        round++;

        agent_set_subgoal("Collecting metrics");
        agent_set_progress((uint8_t)((round % 4) * 25));

        /* Memory */
        size_t used, free, total;
        mm_stats(&used, &free, &total);

        /* Agent counts */
        int alive = 0, waiting = 0, sleeping = 0;
        for (agent_id_t i = 1; i < MAX_AGENTS; ++i) {
            Agent *a = &g_agents[i];
            if (a->status == AGENT_EMPTY || a->status == AGENT_DONE
                                         || a->status == AGENT_FAILED) continue;
            alive++;
            if (a->status == AGENT_WAITING)  waiting++;
            if (a->status == AGENT_SLEEPING) sleeping++;
        }

        kprintf("[MON] tick=%-6llu  agents=%d (wait=%d sleep=%d)"
                "  mem=%uKB/%uKB used\n",
                (unsigned long long)g_ticks,
                alive, waiting, sleeping,
                (unsigned)(used >> 10),
                (unsigned)(total >> 10));

        logger_write("MONITOR", "health tick");

        agent_set_subgoal("Waiting for next reporting interval");
    }
}
