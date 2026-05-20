/**
 * AgentOS — Kernel Entry Point
 *
 * General-purpose agentic OS. No demo is hardcoded here — everything
 * is driven through the interactive shell or by dispatching blueprints.
 *
 * Boot sequence:
 *   boot.S → boot2_main (DTB + MM init) → kernel_main
 *     → register blueprints
 *     → spawn idle (slot 0), logger, guardian, shell
 *     → enable traps + timer
 *     → ctx_restore to shell
 *
 * Blueprints registered here (available via `spawn <name>` in the shell):
 *   shell    — interactive UART dispatcher (already running)
 *   logger   — ring-buffer event log
 *   monitor  — periodic system health reporter
 *   guardian — live goal-tree observer
 *   demo     — spacecraft pre-maneuver health assessment
 */

#include "agent.h"
#include "riscv.h"
#include "mm.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

extern void kprintf(const char *fmt, ...);
extern void sched_print_goal_tree(void);
extern void ctx_restore(void *ctx);
extern void trap_entry(void);

/* Agent entry points */
extern void shell_agent_main(void);
extern void logger_agent_main(void);
extern void monitor_agent_main(void);

/* ================================================================
 * Built-in demo: spacecraft pre-maneuver health assessment
 * (the full pipeline from the original demo, available as a blueprint)
 * ================================================================ */

#define TELEM_IMU       0
#define TELEM_PROP      1
#define FAULT_OK        0
#define FAULT_DETECTED  1

/* Forward declarations for demo agents */
static void demo_orchestrator_main(void);
static void demo_attitude_main(void);
static void demo_prop_main(void);
static void demo_anom_main(void);
static void demo_reporter_main(void);
static void demo_init_main(void);

static void demo_attitude_main(void) {
    agent_set_goal("Sample IMU — 4 attitude readings");
    agent_set_contract("Parent is orchestrator", "4 MSG_DATA samples sent", NULL);

    static const uint16_t imu_w[] = {999, 998, 997, 996};
    static const uint16_t imu_x[] = {  0,   5,  10,  15};

    for (int i = 0; i < 4; ++i) {
        agent_sleep(4);
        agent_set_subgoal("Reading IMU sample");
        agent_set_progress((uint8_t)((i + 1) * 25));

        message_t msg;
        msg.type = MSG_DATA;
        msg.raw.data[0] = TELEM_IMU;
        msg.raw.data[1] = (uint8_t)(imu_w[i] & 0xFF);
        msg.raw.data[2] = (uint8_t)(imu_w[i] >> 8);
        msg.raw.data[3] = (uint8_t)(imu_x[i] & 0xFF);
        msg.raw.data[4] = (uint8_t)(imu_x[i] >> 8);
        msg.raw.data[5] = (uint8_t)i;
        msg.raw.data[6] = FAULT_OK;
        msg.raw.len     = 7;
        agent_send(g_agents[g_current_agent].parent, &msg);
        kprintf("[ATT ] IMU sample %d: w=%u x=%u\n", i+1, imu_w[i], imu_x[i]);
    }
    agent_exit(true, "4/4 IMU samples acquired");
}

static void demo_prop_main(void) {
    agent_set_goal("Sample propulsion — chamber pressure");
    agent_set_contract("Parent is orchestrator", "4 MSG_DATA samples sent (fault at sample 3)", NULL);

    static const uint16_t pressure[] = {2100, 2098,  900, 2099};
    static const uint8_t  fault[]    = {   0,    0,    1,     0};

    for (int i = 0; i < 4; ++i) {
        agent_sleep(3);
        agent_set_subgoal("Reading chamber pressure");
        agent_set_progress((uint8_t)((i + 1) * 25));

        message_t msg;
        msg.type = MSG_DATA;
        msg.raw.data[0] = TELEM_PROP;
        msg.raw.data[1] = (uint8_t)(pressure[i] & 0xFF);
        msg.raw.data[2] = (uint8_t)(pressure[i] >> 8);
        msg.raw.data[3] = 0;
        msg.raw.data[4] = 0;
        msg.raw.data[5] = (uint8_t)i;
        msg.raw.data[6] = fault[i];
        msg.raw.len     = 7;
        agent_send(g_agents[g_current_agent].parent, &msg);

        if (fault[i]) {
            kprintf("[PROP] Sample %d: pressure=%u kPa — FAULT\n", i+1, pressure[i]);
            agent_exit(false, "Chamber pressure anomaly at sample 3");
        }
        kprintf("[PROP] Sample %d: pressure=%u kPa — OK\n", i+1, pressure[i]);
    }
    agent_exit(true, "4/4 propulsion samples acquired");
}

static void demo_anom_main(void) {
    agent_set_goal("Detect anomalies in collected telemetry");
    agent_set_contract("8 MSG_DATA samples queued by orchestrator", "Analysis result sent to parent", NULL);

    int imu_n = 0, prop_n = 0, faults = 0;
    uint16_t min_p = 0xFFFF;

    message_t msg;
    for (int total = 0; total < 8; ++total) {
        if (!agent_recv(&msg, 300)) break;
        if (msg.type != MSG_DATA) { total--; continue; }

        uint8_t  tt    = msg.raw.data[0];
        uint16_t val   = (uint16_t)(msg.raw.data[1] | (msg.raw.data[2] << 8));
        uint8_t  fault = msg.raw.data[6];

        if (tt == TELEM_IMU) { imu_n++; }
        else { prop_n++; if (val < min_p) min_p = val; if (fault) faults++; }
        agent_set_progress((uint8_t)((imu_n + prop_n) * 12));
    }

    kprintf("[ANOM] %d IMU + %d prop samples, %d faults, min_P=%u kPa\n",
            imu_n, prop_n, faults, min_p);

    message_t res;
    res.type = MSG_DATA;
    res.raw.data[0] = (uint8_t)faults;
    res.raw.data[1] = (uint8_t)(min_p & 0xFF);
    res.raw.data[2] = (uint8_t)(min_p >> 8);
    res.raw.len     = 3;
    agent_send(g_agents[g_current_agent].parent, &res);
    agent_set_progress(100);

    if (faults > 0) agent_exit(false, "Anomaly: propulsion pressure excursion");
    else            agent_exit(true,  "All telemetry nominal");
}

static void demo_reporter_main(void) {
    agent_set_goal("Compose pre-maneuver health report");
    agent_set_contract("Analysis result in inbox", "Report printed and verdict sent to grandparent", NULL);

    message_t msg;
    uint8_t  faults = 0;
    uint16_t min_p  = 0;

    if (agent_recv(&msg, 100) && msg.type == MSG_DATA) {
        faults = msg.raw.data[0];
        min_p  = (uint16_t)(msg.raw.data[1] | (msg.raw.data[2] << 8));
        agent_set_progress(50);
    }

    agent_sleep(2);
    agent_set_progress(100);

    kprintf("\n");
    kprintf("  ┌────────────────────────────────────────────┐\n");
    kprintf("  │  PRE-MANEUVER HEALTH REPORT                │\n");
    kprintf("  ├────────────────────────────────────────────┤\n");
    kprintf("  │  IMU:         NOMINAL                      │\n");
    if (faults > 0) {
        kprintf("  │  Propulsion:  ANOMALY — %u fault(s)        │\n", faults);
        kprintf("  │  Min P:       %u kPa (BELOW THRESHOLD)    │\n", min_p);
        kprintf("  │  VERDICT:     HOLD — FAULT DETECTED        │\n");
    } else {
        kprintf("  │  Propulsion:  NOMINAL                      │\n");
        kprintf("  │  Min P:       %u kPa                       │\n", min_p);
        kprintf("  │  VERDICT:     GO FOR BURN                  │\n");
    }
    kprintf("  └────────────────────────────────────────────┘\n\n");

    /* Send verdict to orchestrator's parent (demo_init) */
    agent_id_t orch_parent = g_agents[g_agents[g_current_agent].parent].parent;
    if (orch_parent != AGENT_NONE) {
        message_t verdict;
        verdict.type        = MSG_DATA;
        verdict.raw.data[0] = faults > 0 ? 0 : 1;
        verdict.raw.data[1] = faults;
        verdict.raw.len     = 2;
        agent_send(orch_parent, &verdict);
    }

    agent_exit(true, "Health report transmitted");
}

typedef enum { PH_COLLECT=0, PH_ANALYZE, PH_REPORT, PH_DONE } orch_phase_t;

static void demo_orchestrator_main(void) {
    agent_set_goal("Assess spacecraft health before maneuver burn");
    agent_set_subgoal("Phase 1: spawning parallel collectors");
    agent_set_contract("Sensors available", "Health verdict delivered to init", NULL);

    orch_phase_t phase = PH_COLLECT;

    agent_id_t att_id = agent_spawn(g_current_agent, "att_collect",
        "Sample IMU — 4 attitude readings",
        URGENCY_NORMAL, 100, demo_attitude_main,
        CAP_SEND | CAP_RECV | CAP_GOAL_SET);

    agent_id_t prop_id = agent_spawn(g_current_agent, "prop_collect",
        "Sample propulsion — chamber pressure",
        URGENCY_HIGH, 80, demo_prop_main,
        CAP_SEND | CAP_RECV | CAP_GOAL_SET);

    agent_set_progress(10);

    int att_done = 0, prop_done = 0, prop_retried = 0;
    int faults = 0;
    uint16_t min_p = 0xFFFF;
    int tcount = 0;

    message_t msg;
    while (phase == PH_COLLECT) {
        if (!agent_recv(&msg, 0)) continue;
        if (msg.type == MSG_DATA) {
            tcount++;
            uint8_t  tt    = msg.raw.data[0];
            uint16_t val   = (uint16_t)(msg.raw.data[1] | (msg.raw.data[2] << 8));
            uint8_t  fault = msg.raw.data[6];
            if (tt == TELEM_PROP && val < min_p) min_p = val;
            if (fault) faults++;
            agent_set_progress((uint8_t)(10 + tcount * 5));
        } else if (msg.type == MSG_GOAL_DONE) {
            if (msg.from == att_id)  { att_done  = 1; kprintf("[ORCH] att complete\n"); }
            if (msg.from == prop_id) { prop_done = 1; kprintf("[ORCH] prop complete\n"); }
        } else if (msg.type == MSG_GOAL_FAILED) {
            if (msg.from == prop_id && !prop_retried) {
                prop_retried = 1;
                kprintf("[ORCH] prop FAILED — spawning recovery (CRITICAL)\n");
                if (min_p == 0xFFFF) min_p = 900;
                prop_id = agent_spawn(g_current_agent, "prop_recover",
                    "Re-sample propulsion with fault isolation",
                    URGENCY_CRITICAL, 60, demo_prop_main,
                    CAP_SEND | CAP_RECV | CAP_GOAL_SET);
            } else {
                /* Second failure: continue with fault flag */
                prop_done = 1;
                faults++;
                kprintf("[ORCH] Recovery also failed — proceeding with fault\n");