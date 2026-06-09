
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
extern void triage_agent_main(void);

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
            }
            if (msg.from == att_id) { att_done = 1; }
        }
        if (att_done && prop_done) phase = PH_ANALYZE;
    }

    /* Phase 2: anomaly detection */
    agent_set_subgoal("Phase 2: anomaly detection");
    agent_set_progress(50);

    agent_id_t anom_id = agent_spawn(g_current_agent, "anom_detect",
        "Detect anomalies in telemetry",
        URGENCY_HIGH, 150, demo_anom_main,
        CAP_SEND | CAP_RECV | CAP_GOAL_SET);

    /* Forward representative telemetry to detector */
    {
        uint16_t prs[4] = {2100, 2098, min_p < 2000 ? 900 : 2099, 2099};
        uint8_t  flt[4] = {   0,    0, min_p < 2000 ? 1   :     0,    0};
        for (int i = 0; i < 4; ++i) {
            message_t f = {0};
            f.type = MSG_DATA; f.raw.data[0]=TELEM_IMU;
            f.raw.data[1]=0xE7; f.raw.data[2]=0x03; /* 999 */
            f.raw.data[5]=(uint8_t)i; f.raw.data[6]=FAULT_OK; f.raw.len=7;
            agent_send(anom_id, &f);
        }
        for (int i = 0; i < 4; ++i) {
            message_t f = {0};
            f.type = MSG_DATA; f.raw.data[0]=TELEM_PROP;
            f.raw.data[1]=(uint8_t)(prs[i]&0xFF); f.raw.data[2]=(uint8_t)(prs[i]>>8);
            f.raw.data[5]=(uint8_t)i; f.raw.data[6]=flt[i]; f.raw.len=7;
            agent_send(anom_id, &f);
        }
    }

    while (phase == PH_ANALYZE) {
        if (!agent_recv(&msg, 0)) continue;
        if (msg.type == MSG_DATA && msg.from == anom_id) {
            faults  = msg.raw.data[0];
            if (faults > 0) {
                uint16_t p = (uint16_t)(msg.raw.data[1]|(msg.raw.data[2]<<8));
                if (p < min_p) min_p = p;
            }
        } else if (msg.type == MSG_GOAL_DONE  && msg.from == anom_id) { phase = PH_REPORT; }
          else if (msg.type == MSG_GOAL_FAILED && msg.from == anom_id) { phase = PH_REPORT; }
    }

    /* Phase 3: report */
    agent_set_subgoal("Phase 3: composing health report");
    agent_set_progress(75);

    agent_id_t rep_id = agent_spawn(g_current_agent, "reporter",
        "Compose pre-maneuver health report",
        URGENCY_CRITICAL, 50, demo_reporter_main,
        CAP_SEND | CAP_RECV | CAP_GOAL_SET);

    {
        message_t f = {0};
        f.type = MSG_DATA;
        f.raw.data[0] = (uint8_t)faults;
        f.raw.data[1] = (uint8_t)(min_p & 0xFF);
        f.raw.data[2] = (uint8_t)(min_p >> 8);
        f.raw.len = 3;
        agent_send(rep_id, &f);
    }

    while (phase == PH_REPORT) {
        if (!agent_recv(&msg, 0)) continue;
        if ((msg.type == MSG_GOAL_DONE || msg.type == MSG_GOAL_FAILED)
            && msg.from == rep_id) phase = PH_DONE;
    }

    agent_set_progress(100);

    if (faults > 0) agent_exit(false, "Demo complete — anomaly detected, HOLD");
    else            agent_exit(true,  "Demo complete — spacecraft GO for burn");
}

static void demo_init_main(void) {
    agent_set_goal("Run spacecraft pre-maneuver health check");
    agent_set_contract(NULL, "Maneuver verdict delivered to console", NULL);

    kprintf("\n[DEMO] Spacecraft health check starting...\n");
    kprintf("[DEMO] Spawning orchestrator\n\n");

    agent_id_t orch = agent_spawn(g_current_agent, "demo_orch",
        "Assess spacecraft health before maneuver burn",
        URGENCY_HIGH, 400, demo_orchestrator_main,
        CAP_SPAWN | CAP_SEND | CAP_RECV | CAP_GOAL_SET);

    agent_set_progress(25);
    message_t msg;
    while (1) {
        if (!agent_recv(&msg, 0)) continue;
        if (msg.type == MSG_GOAL_DONE && msg.from == orch) {
            kprintf("[DEMO] Orchestrator done: nominal\n"); break;
        }
        if (msg.type == MSG_GOAL_FAILED && msg.from == orch) {
            kprintf("[DEMO] Orchestrator done: fault detected\n"); break;
        }
        if (msg.type == MSG_DATA) {
            kprintf("[DEMO] Verdict: %s (faults=%u)\n",
                    msg.raw.data[0] ? "GO FOR BURN" : "HOLD",
                    msg.raw.data[1]);
        }
    }
    agent_set_progress(100);
    agent_exit(true, "Demo complete");
}

static void guardian_agent_main(void) {
    agent_set_goal("Observe system goal tree");
    agent_set_contract(NULL, "Goal tree printed on schedule", "Never modifies any agent");
    agent_subscribe(EVT_SHUTDOWN);

    for (;;) {
        agent_sleep(200);
        sched_print_goal_tree();
    }
}

static void idle_agent_main(void) {
    for (;;) {
        asm volatile("wfi");
        agent_yield();
    }
}

static void register_blueprints(void) {
    static const agent_blueprint_t bps[] = {
        {
            .name            = "shell",
            .description     = "Interactive UART agent dispatcher shell",
            .default_goal    = "Dispatch and observe agents via UART shell",
            .default_urgency = URGENCY_NORMAL,
            .default_deadline= 0,
            .default_caps    = CAP_SPAWN | CAP_SEND | CAP_RECV | CAP_GOAL_SET | CAP_OBSERVE,
            .entry           = shell_agent_main,
        },
        {
            .name            = "logger",
            .description     = "Ring-buffer system event log",
            .skills          = "log record events history",
            .default_goal    = "Record system events to ring-buffer log",
            .default_urgency = URGENCY_NORMAL,
            .default_deadline= 0,
            .default_caps    = CAP_RECV | CAP_GOAL_SET,
            .entry           = logger_agent_main,
        },
        {
            .name            = "monitor",
            .description     = "Periodic system health reporter",
            .skills          = "monitor report system memory status watch",
            .default_goal    = "Report system health periodically",
            .default_urgency = URGENCY_BACKGROUND,
            .default_deadline= 0,
            .default_caps    = CAP_OBSERVE | CAP_GOAL_SET,
            .entry           = monitor_agent_main,
        },
        {
            .name            = "guardian",
            .description     = "Background goal-tree observer",
            .skills          = "observe goal tree agents",
            .default_goal    = "Observe system goal tree",
            .default_urgency = URGENCY_BACKGROUND,
            .default_deadline= 0,
            .default_caps    = CAP_OBSERVE | CAP_GOAL_SET,
            .entry           = guardian_agent_main,
        },
        {
            .name            = "demo",
            .description     = "Spacecraft pre-maneuver health assessment",
            .skills          = "health check spacecraft telemetry assess maneuver burn imu",
            .default_goal    = "Run spacecraft pre-maneuver health check",
            .default_urgency = URGENCY_NORMAL,
            .default_deadline= 0,
            .default_caps    = CAP_SPAWN | CAP_SEND | CAP_RECV | CAP_GOAL_SET,
            .entry           = demo_init_main,
        },
        {
            .name            = "taskmgr",
            .description     = "Routes goal requests to matching blueprints",
            .default_goal    = "Route goal requests to specialized agents",
            .default_urgency = URGENCY_HIGH,
            .default_deadline= 0,
            .default_caps    = CAP_SPAWN | CAP_SEND | CAP_RECV | CAP_GOAL_SET | CAP_OBSERVE,
            .entry           = taskmgr_agent_main,
        },
        {
            .name            = "dispatcher",
            .description     = "Activates agents when trigger rules fire",
            .default_goal    = "Activate agents when trigger conditions arise",
            .default_urgency = URGENCY_HIGH,
            .default_deadline= 0,
            .default_caps    = CAP_SPAWN | CAP_SEND | CAP_OBSERVE | CAP_GOAL_SET,
            .entry           = dispatcher_agent_main,
        },
        {
            .name            = "triage",
            .description     = "Post-mortem analysis of a failed agent",
            .skills          = "diagnose failure triage debug postmortem inspect",
            .default_goal    = "Diagnose most recent agent failure",
            .default_urgency = URGENCY_HIGH,
            .default_deadline= 300,
            .default_caps    = CAP_RECV | CAP_OBSERVE | CAP_GOAL_SET,
            .entry           = triage_agent_main,
        },
    };

    for (int i = 0; i < (int)(sizeof(bps)/sizeof(bps[0])); ++i)
        agent_register_blueprint(&bps[i]);
}

void kernel_main(void) {
    kprintf("[KERNEL] Initialising agent table  (MAX_AGENTS=%d  STACK=%d  INBOX=%d)\n",
            MAX_AGENTS, STACK_SIZE, INBOX_SIZE);

    memset(g_agents, 0, sizeof(g_agents));
    g_current_agent = AGENT_NONE;
    g_ticks         = 0;

    /* Register all known agent blueprints */
    register_blueprints();

    /* Slot 0: idle agent (always exists) */
    g_agents[0].id          = 0;
    g_agents[0].status      = AGENT_READY;
    g_agents[0].urgency     = URGENCY_BACKGROUND;
    memcpy(g_agents[0].name, "idle", 5);
    memcpy(g_agents[0].goal, "Wait for runnable agents", 25);
    g_agents[0].ctx.pc      = (uint64_t)(uintptr_t)idle_agent_main;
    g_agents[0].ctx.sp      = (uint64_t)(uintptr_t)g_agents[0].stack + STACK_SIZE;
    g_agents[0].ctx.tp      = (uint64_t)(uintptr_t)&g_agents[0].ctx;
    g_agents[0].ctx.sstatus = SSTATUS_SPIE | SSTATUS_SIE | SSTATUS_SPP;

    /* Spawn logger (id=1, receives log_write calls) */
    agent_id_t logger_id = agent_dispatch(AGENT_NONE, "logger");
    if (logger_id == AGENT_NONE) {
        kprintf("[KERNEL] FATAL: logger spawn failed\n");
        for (;;) asm volatile("wfi");
    }

    /* Spawn shell (id=2, the primary user interface) */
    agent_id_t shell_id = agent_dispatch(AGENT_NONE, "shell");
    if (shell_id == AGENT_NONE) {
        kprintf("[KERNEL] FATAL: shell spawn failed\n");
        for (;;) asm volatile("wfi");
    }

    /* Spawn guardian and monitor as background observers */
    agent_dispatch(AGENT_NONE, "guardian");
    agent_dispatch(AGENT_NONE, "monitor");

    /* Task manager: routes 'delegate' goal requests to blueprints */
    agent_dispatch(AGENT_NONE, "taskmgr");

    /* Dispatcher: auto-activates agents when trigger rules fire.
     * Default rule: any unhandled agent failure spawns a triage agent. */
    dispatcher_add_rule(TRIG_AGENT_FAILED, 0, "triage", 50);
    agent_dispatch(AGENT_NONE, "dispatcher");

    int n_ready = 0;
    for (agent_id_t i = 0; i < MAX_AGENTS; ++i)
        if (g_agents[i].status != AGENT_EMPTY) n_ready++;
    kprintf("[KERNEL] %d agents ready. Entering shell (id=%u)...\n\n",
            n_ready, shell_id);

    /* Install trap vector and arm timer */
    w_stvec((uint64_t)(uintptr_t)trap_entry);
    asm volatile("mv tp, %0" : : "r"(&g_kernel_ctx));
    w_sie(r_sie() | SIE_STIE);
    sbi_set_timer(r_time() + 10000000UL / 100UL);

    /* Jump into the shell agent */
    g_current_agent = shell_id;
    g_agents[shell_id].status = AGENT_RUNNING;
    asm volatile("mv tp, %0" : : "r"(&g_agents[shell_id].ctx));
    ctx_restore(&g_agents[shell_id].ctx);

    for (;;) asm volatile("wfi");
}
