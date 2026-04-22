#pragma once
/**
 * AgentOS — Core Agent Abstraction
 *
 * Inspired by Microsoft's Project Solara and agentic AI systems research.
 * Target: StarFive JH7110 (Star64 board), RISC-V RV64GC.
 *
 * The fundamental insight: modern processes are dumb executors. They have
 * code, a stack, and a priority number. The OS knows nothing about what
 * they are trying to accomplish.
 *
 * An Agent is different:
 *   - It declares a Goal (what it's trying to accomplish)
 *   - It reports Progress (how close it is to completion)
 *   - It sets a Deadline (when the goal must be done)
 *   - It holds Capabilities (what resources it needs — nothing more)
 *   - It participates in a Hierarchy (parent/child goal delegation)
 *
 * The scheduler uses Goal + Urgency + Progress to allocate CPU time.
 * A stuck agent gets preempted faster. A near-deadline agent gets boosted.
 * A completed agent's children are automatically cleaned up.
 *
 * This makes the OS a participant in goal achievement, not just a referee.
 */

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