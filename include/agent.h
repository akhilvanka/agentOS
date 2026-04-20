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