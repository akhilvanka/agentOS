#include "agent.h"
#include "riscv.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern void kprintf(const char *fmt, ...);
extern void logger_write(const char *tag, const char *msg);

#define MAX_DELEGATIONS 16

typedef enum {
    DLG_EMPTY = 0,
    DLG_RUNNING,
    DLG_DONE,
    DLG_FAILED,
} dlg_status_t;

typedef struct {
    dlg_status_t status;
    agent_id_t   requester;
    agent_id_t   child;
    char         goal[52];
    char         blueprint[16];
    uint64_t     submitted;
    uint64_t     finished;
} delegation_t;

static delegation_t g_dlg[MAX_DELEGATIONS];
static int          g_n_dlg = 0;   /* total submitted; table is a ring */

agent_id_t g_taskmgr_id = AGENT_NONE;

static void copy_str(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && i < n - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Does word w (length wl) appear as a whole word in s? */
static bool word_in(const char *s, const char *w, int wl) {
    for (int i = 0; s[i]; ++i) {
        if (i == 0 || s[i - 1] == ' ') {
            int j = 0;
            while (j < wl && s[i + j] && lower(s[i + j]) == lower(w[j])) j++;
            if (j == wl && (s[i + j] == '\0' || s[i + j] == ' '))
                return true;
        }
    }
    return false;
}

/* Count how many words of the goal sentence the blueprint claims as skills.
 * Skill hits weigh double; a name hit counts once. Words under 3 chars
 * ("a", "to", "of") are ignored. */
static int match_score(const char *goal, const agent_blueprint_t *bp) {
    if (!bp->skills[0]) return 0;
    int score = 0;
    int i = 0;
    while (goal[i]) {
        while (goal[i] == ' ') i++;
        int start = i;
        while (goal[i] && goal[i] != ' ') i++;
        int len = i - start;
        if (len < 3) continue;
        if (word_in(bp->skills, &goal[start], len)) score += 2;
        if (word_in(bp->name,   &goal[start], len)) score += 1;
    }
    return score;
}

static const agent_blueprint_t *best_blueprint(const char *goal, int *out_score) {
    const agent_blueprint_t *best = NULL;
    int best_s = 0;
    for (int i = 0; i < agent_blueprint_count(); ++i) {
        const agent_blueprint_t *bp = agent_blueprint_at(i);
        int s = match_score(goal, bp);
        if (s > best_s) { best_s = s; best = bp; }
    }
    if (out_score) *out_score = best_s;
    return best;
}

static void reject(agent_id_t requester, const char *reason) {
    if (requester == AGENT_NONE || requester == g_taskmgr_id) return;
    message_t r = {0};
    r.type = MSG_GOAL_FAILED;
    r.to   = requester;
    r.done.success = 0;
    copy_str(r.done.reason, reason, sizeof(r.done.reason));
    agent_send(requester, &r);
}

static void handle_request(message_t *req) {
    char goal[52];
    copy_str(goal, req->goal_req.goal, sizeof(goal));

    int score = 0;
    const agent_blueprint_t *bp = best_blueprint(goal, &score);
    if (!bp) {
        kprintf("[TASK] no blueprint matches '%s' — rejecting\n", goal);
        logger_write("TASKMGR", "request rejected: no match");
        reject(req->from, "no blueprint matches goal");
        return;
    }

    agent_id_t id = agent_dispatch(g_taskmgr_id, bp->name);
    if (id == AGENT_NONE) {
        reject(req->from, "agent table full");
        return;
    }

    /* The dispatched agent works toward the requested goal, not its default */
    Agent *child = agent_get(id);
    copy_str(child->goal, goal, sizeof(child->goal));
    if (req->goal_req.urgency) child->urgency = req->goal_req.urgency;

    delegation_t *d = &g_dlg[g_n_dlg % MAX_DELEGATIONS];
    g_n_dlg++;
    d->status    = DLG_RUNNING;
    d->requester = req->from;
    d->child     = id;
    d->submitted = g_ticks;
    d->finished  = 0;
    copy_str(d->goal, goal, sizeof(d->goal));
    copy_str(d->blueprint, bp->name, sizeof(d->blueprint));

    kprintf("[TASK] '%s' -> blueprint '%s' (score %d), agent id=%u\n",
            goal, bp->name, score, id);
    logger_write("TASKMGR", goal);
}

static void handle_completion(message_t *msg, bool success) {
    for (int i = 0; i < MAX_DELEGATIONS; ++i) {
        delegation_t *d = &g_dlg[i];
        if (d->status != DLG_RUNNING || d->child != msg->from) continue;

        d->status   = success ? DLG_DONE : DLG_FAILED;
        d->finished = g_ticks;

        kprintf("[TASK] delegation '%s' %s: %s\n",
                d->goal, success ? "done" : "failed", msg->done.reason);

        /* Forward the outcome to whoever asked for the goal */
        if (d->requester != AGENT_NONE && d->requester != g_taskmgr_id) {
            message_t fwd = *msg;
            fwd.to = d->requester;
            agent_send(d->requester, &fwd);
        }
        return;
    }
}

void taskmgr_print_delegations(void) {
    int shown = g_n_dlg < MAX_DELEGATIONS ? g_n_dlg : MAX_DELEGATIONS;
    if (shown == 0) {
        kprintf("  (no delegations yet)\n");
        return;
    }

    kprintf("\n  Delegations (%d total):\n", g_n_dlg);
    kprintf("  %-8s %-6s %-12s %-8s  %s\n",
            "Status", "Agent", "Blueprint", "Ticks", "Goal");
    kprintf("  %s\n",
            "---------------------------------------------------------------");
    for (int i = 0; i < shown; ++i) {
        delegation_t *d = &g_dlg[i];
        if (d->status == DLG_EMPTY) continue;
        const char *st = d->status == DLG_RUNNING ? "RUNNING" :
                         d->status == DLG_DONE    ? "DONE   " : "FAILED ";
        uint64_t took = d->finished ? d->finished - d->submitted
                                    : g_ticks - d->submitted;
        kprintf("  %s  %-6u %-12s %-8llu  %s\n",
                st, d->child, d->blueprint,
                (unsigned long long)took, d->goal);
    }
    kprintf("\n");
}

bool agent_request_goal(const char *goal, urgency_t urgency) {
    if (g_taskmgr_id == AGENT_NONE) return false;
    message_t m = {0};
    m.type = MSG_GOAL_REQUEST;
    m.to   = g_taskmgr_id;
    copy_str(m.goal_req.goal, goal, sizeof(m.goal_req.goal));
    m.goal_req.urgency = (uint8_t)urgency;
    return agent_send(g_taskmgr_id, &m);
}

void taskmgr_agent_main(void) {
    g_taskmgr_id = (agent_id_t)g_current_agent;
    agent_set_goal("Route goal requests to specialized agents");
    agent_set_contract(
        "Blueprint registry populated",
        "Every goal request answered",
        "Requests routed by skill match only"
    );

    message_t msg;
    for (;;) {
        if (!agent_recv(&msg, 0)) continue;

        switch (msg.type) {
        case MSG_GOAL_REQUEST:
            handle_request(&msg);
            break;
        case MSG_GOAL_DONE:
            handle_completion(&msg, true);
            break;
        case MSG_GOAL_FAILED:
            handle_completion(&msg, false);
            break;
        case MSG_SHUTDOWN:
            agent_exit(true, "task manager shut down");
            break;
        default:
            break;
        }
    }
}
