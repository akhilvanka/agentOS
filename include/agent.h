#pragma once

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#endif

#define MAX_AGENTS       64
#define MAX_CAPS         16
#define MAX_CHILDREN     8
#define MAX_GOAL_LEN     64
#define INBOX_SIZE       16
#define STACK_SIZE       8192   /* 8 KB per agent */
#define MAX_BLUEPRINTS   32

/* ---- Agent identity ---- */

typedef uint16_t agent_id_t;
#define AGENT_NONE    ((agent_id_t)0xFFFF)
#define AGENT_KERNEL  ((agent_id_t)0)
#define AGENT_INIT    ((agent_id_t)1)

/* ---- Goal urgency ---- */

typedef enum {
    URGENCY_BACKGROUND = 0,   /* run when nothing else needs CPU */
    URGENCY_NORMAL     = 64,
    URGENCY_HIGH       = 128,
    URGENCY_CRITICAL   = 192,
    URGENCY_REALTIME   = 255, /* never preempt, always run to yield */
} urgency_t;

/* ---- Capabilities ---- */

typedef enum {
    CAP_NONE       = 0,
    CAP_MEM_READ   = (1u << 0),  /* read a memory region */
    CAP_MEM_WRITE  = (1u << 1),  /* write a memory region */
    CAP_SPAWN      = (1u << 2),  /* create child agents */
    CAP_SEND       = (1u << 3),  /* send IPC messages */
    CAP_RECV       = (1u << 4),  /* receive IPC messages */
    CAP_DEVICE_IO  = (1u << 5),  /* MMIO / device access */
    CAP_GOAL_SET   = (1u << 6),  /* update own goal/progress */
    CAP_OBSERVE    = (1u << 7),  /* read other agents' goal/progress */
} cap_flags_t;

typedef struct {
    cap_flags_t flags;
    uintptr_t   resource;  /* memory base for MEM caps, agent_id for SEND */
    size_t      size;      /* memory size for MEM caps */
} cap_t;

/* ---- Typed IPC message ---- */

typedef enum {
    MSG_NONE          = 0,
    MSG_GOAL_REQUEST  = 1,   /* parent delegates a sub-goal */
    MSG_PROGRESS      = 2,   /* agent reports progress to parent */
    MSG_GOAL_DONE     = 3,   /* agent reports goal completion */
    MSG_GOAL_FAILED   = 4,   /* agent reports goal failure */
    MSG_DATA          = 5,   /* raw data transfer */
    MSG_PING          = 6,   /* liveness check */
    MSG_PONG          = 7,
    MSG_SHUTDOWN      = 8,   /* ask agent to exit */
    MSG_OBSERVE_REQ   = 9,   /* request goal snapshot of another agent */
    MSG_OBSERVE_RESP  = 10,
} msg_type_t;

typedef struct {
    msg_type_t  type;
    agent_id_t  from;
    agent_id_t  to;
    uint32_t    seq;
    union {
        struct { char goal[52]; uint8_t urgency; uint8_t _pad[3]; } goal_req;
        struct { uint8_t pct; uint32_t ticks_remaining; }           progress;
        struct { uint8_t success; char reason[51]; }                 done;
        struct { uint8_t data[56]; uint8_t len; }                    raw;
        struct { char goal[52]; uint8_t urgency; uint8_t pct; }      observe;
    };
} __attribute__((packed)) message_t;

/* ---- Agent status ---- */

typedef enum {
    AGENT_EMPTY    = 0,  /* slot unused */
    AGENT_READY    = 1,  /* runnable */
    AGENT_RUNNING  = 2,  /* currently on CPU */
    AGENT_WAITING  = 3,  /* blocked on IPC recv */
    AGENT_DONE     = 4,  /* goal completed, awaiting reap */
    AGENT_FAILED   = 5,  /* goal failed */
    AGENT_SLEEPING = 6,  /* sleeping for N ticks */
} agent_status_t;

/* ---- RISC-V saved context (all 32 GPRs + PC) ---- */

typedef struct {
    uint64_t ra, sp, gp, tp;
    uint64_t t0, t1, t2;
    uint64_t s0, s1;
    uint64_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint64_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint64_t t3, t4, t5, t6;
    uint64_t pc;
    uint64_t sstatus;
} rv64_ctx_t;

/* ---- Agent contract (Solaris-style formal specification) ---- */

typedef struct {
    char precondition[48];   /* what must be true before the agent starts */
    char postcondition[48];  /* what will be true when the goal succeeds */
    char invariant[48];      /* must remain true throughout execution */
} agent_contract_t;

/* ---- Agent blueprint (dispatachable agent type) ---- */

typedef struct {
    char         name[16];            /* short identifier used in shell */
    char         description[64];     /* human-readable purpose */
    char         default_goal[MAX_GOAL_LEN];
    urgency_t    default_urgency;
    uint32_t     default_deadline;    /* 0 = no deadline */
    cap_flags_t  default_caps;
    void       (*entry)(void);        /* agent entry point */
} agent_blueprint_t;

/* ---- Event flags (bitfield, for agent_wait_event / agent_emit_event) ---- */

#define EVT_CHILD_DONE    (1u << 0)
#define EVT_CHILD_FAILED  (1u << 1)
#define EVT_MSG_ARRIVED   (1u << 2)
#define EVT_DEADLINE_NEAR (1u << 3)   /* fired when deadline_ticks < 50 */
#define EVT_SHUTDOWN      (1u << 4)   /* OS sends this on kill */
#define EVT_USER_0        (1u << 8)   /* user-defined events */
#define EVT_USER_1        (1u << 9)
#define EVT_USER_2        (1u << 10)
#define EVT_USER_3        (1u << 11)

/* ---- Agent Control Block ---- */

typedef struct Agent {
    rv64_ctx_t    ctx;                   /* must be first — trap handler assumes this */

    /* Identity */
    agent_id_t           id;
    volatile agent_status_t status;      /* modified by scheduler in trap context */
    char                 name[16];
    char                 blueprint[16];  /* blueprint name this instance came from */

    /* Goal model (Solaris: every agent declares WHAT, not HOW) */
    char          goal[MAX_GOAL_LEN];    /* primary goal */
    char          subgoal[MAX_GOAL_LEN]; /* current sub-task within the goal */
    uint8_t       urgency;
    uint8_t       progress;
    uint8_t       prev_progress;
    uint32_t      deadline_ticks;
    uint32_t      sleep_ticks;

    /* Contract: formal pre/post/invariant specification */
    agent_contract_t contract;

    /* Resource budget (0 = unlimited) */
    uint32_t      cpu_budget_ticks;      /* max CPU ticks per scheduling period */
    uint32_t      cpu_used_period;       /* ticks consumed in current period */

    /* Capability table */
    cap_t         caps[MAX_CAPS];

    /* Hierarchy */
    agent_id_t    parent;
    agent_id_t    children[MAX_CHILDREN];
    uint8_t       n_children;

    /* Event subscription */
    volatile uint32_t event_mask;        /* events this agent subscribes to */
    volatile uint32_t event_pending;     /* events that have fired, not yet consumed */

    /* IPC inbox (ring buffer) */
    message_t     inbox[INBOX_SIZE];
    volatile uint8_t inbox_head;
    volatile uint8_t inbox_tail;

    /* Statistics */
    uint64_t      ticks_run;
    uint64_t      ticks_waiting;
    uint32_t      goal_completions;
    uint32_t      goal_failures;
    uint32_t      messages_sent;
    uint32_t      messages_recv;

    /* Stack */
    uint8_t       stack[STACK_SIZE] __attribute__((aligned(16)));
} Agent;

/* The global agent table */
extern Agent g_agents[MAX_AGENTS];
/* volatile: trap handler modifies these; compiler must reload on every access */
extern volatile agent_id_t g_current_agent;
extern volatile uint64_t g_ticks;
extern rv64_ctx_t g_kernel_ctx; /* safe save area during kernel init (tp points here) */

/* ---- Core API ---- */

agent_id_t agent_spawn(agent_id_t parent,
                       const char *name,
                       const char *goal,
                       urgency_t urgency,
                       uint32_t deadline_ticks,
                       void (*entry)(void),
                       cap_flags_t caps);

void agent_exit(bool success, const char *reason);
void agent_set_progress(uint8_t pct);
void agent_set_goal(const char *goal);
void agent_set_subgoal(const char *subgoal);   /* fine-grained current task */
void agent_set_contract(const char *pre, const char *post, const char *inv);
void agent_sleep(uint32_t ticks);
void agent_yield(void);
void agent_kill(agent_id_t id);                /* forcibly terminate any agent */

bool agent_send(agent_id_t to, message_t *msg);
bool agent_recv(message_t *out, uint32_t timeout_ticks);

/* Event API */
void     agent_subscribe(uint32_t event_mask);
uint32_t agent_wait_event(uint32_t mask, uint32_t timeout_ticks);
void     agent_emit_event(agent_id_t target, uint32_t event_flags);

Agent* agent_current(void);
Agent* agent_get(agent_id_t id);

/* ---- Blueprint registry (Solaris: agent dispatcher) ---- */

void                       agent_register_blueprint(const agent_blueprint_t *bp);
const agent_blueprint_t   *agent_find_blueprint(const char *name);
int                        agent_list_blueprints(void);  /* returns count */
/* Spawn an agent by blueprint name; returns AGENT_NONE on failure */
agent_id_t                 agent_dispatch(agent_id_t parent, const char *bp_name);
