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