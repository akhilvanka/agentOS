#include "agent.h"
#include "riscv.h"
#include <stdint.h>

extern void kprintf(const char *fmt, ...);
extern void logger_write(const char *tag, const char *msg);

/* Auto-activated by the dispatcher when an agent fails. Receives the
 * failed agent's id, inspects its state read-only, and prints a
 * post-mortem so the failure is explained, not just logged. */
void triage_agent_main(void) {
    agent_set_goal("Diagnose most recent agent failure");
    agent_set_contract(
        "Failure context in inbox",
        "Post-mortem printed and logged",
        "Never modifies the failed agent"
    );

    message_t msg;
    if (!agent_recv(&msg, 100) || msg.type != MSG_DATA || msg.raw.len < 2)
        agent_exit(false, "no failure context received");

    agent_id_t fid = (agent_id_t)(msg.raw.data[0] | (msg.raw.data[1] << 8));
    Agent *f = agent_get(fid);
    if (!f || f->status == AGENT_EMPTY)
        agent_exit(false, "failed agent no longer exists");

    agent_set_subgoal("Inspecting failed agent state");
    agent_set_progress(50);

    kprintf("\n[TRIAGE] post-mortem for agent %u '%s'\n", fid, f->name);
    kprintf("  blueprint:      %s\n", f->blueprint[0] ? f->blueprint : "(ad hoc)");
    kprintf("  goal:           %s\n", f->goal);
    if (f->subgoal[0])
        kprintf("  last subgoal:   %s\n", f->subgoal);
    if (f->contract.postcondition[0])
        kprintf("  unmet contract: %s\n", f->contract.postcondition);
    kprintf("  progress:       %u%% after %llu ticks\n",
            f->progress, (unsigned long long)f->ticks_run);
    kprintf("  ipc:            %u sent / %u received\n",
            f->messages_sent, f->messages_recv);
    kprintf("  verdict:        %s\n\n",
            f->progress >= 75 ? "failed late — likely external fault" :
            f->progress >= 25 ? "failed mid-goal — check dependencies" :
                                "failed early — check preconditions");

    logger_write("TRIAGE", f->name);
    agent_set_progress(100);
    agent_exit(true, "post-mortem complete");
}
