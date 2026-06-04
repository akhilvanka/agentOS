

#include "agent.h"
#include "riscv.h"
#include <stdint.h>
#include <string.h>

extern void kprintf(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Ring buffer                                                          */
/* ------------------------------------------------------------------ */

#define LOG_ENTRIES 128

typedef struct {
    uint64_t tick;
    agent_id_t agent_id;
    char tag[8];
    char msg[48];
} log_entry_t;

static log_entry_t g_log[LOG_ENTRIES];
static volatile uint32_t g_log_head = 0;  /* oldest entry */
static volatile uint32_t g_log_count = 0; /* total written (for wrap detection) */
static agent_id_t g_logger_id = AGENT_NONE;

static void strncpy_safe(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && i < n - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

/* Called from any agent (or from C code) — interrupt-safe */
void logger_write(const char *tag, const char *msg) {
    uint32_t idx = (g_log_head + g_log_count) % LOG_ENTRIES;
    log_entry_t *e = &g_log[idx];
    e->tick     = g_ticks;
    e->agent_id = g_current_agent;
    strncpy_safe(e->tag, tag ? tag : "?", sizeof(e->tag));
    strncpy_safe(e->msg, msg ? msg : "", sizeof(e->msg));

    if (g_log_count < LOG_ENTRIES) {
        g_log_count++;
    } else {
        /* Overwrite oldest */
        g_log_head = (g_log_head + 1) % LOG_ENTRIES;
    }

    /* Also forward to logger agent inbox so it can react */
    if (g_logger_id != AGENT_NONE) {
        message_t m;
        m.type = MSG_DATA;
        m.to   = g_logger_id;
        /* We can't call agent_send from interrupt context safely here,
         * so just update the ring buffer and let the agent poll/dump. */
        (void)m;
    }
}

void logger_dump(void) {
    uint32_t n = g_log_count < LOG_ENTRIES ? g_log_count : LOG_ENTRIES;
    if (n == 0) { kprintf("  (no log entries)\n"); return; }

    kprintf("\n  System log (%u entries):\n", n);
    kprintf("  %-10s %-6s %-8s  %s\n", "Tick", "Agent", "Tag", "Message");
    kprintf("  %s\n", "----------------------------------------------");

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t idx = (g_log_head + i) % LOG_ENTRIES;
        log_entry_t *e = &g_log[idx];
        kprintf("  %-10llu %-6u %-8s  %s\n",
                (unsigned long long)e->tick,
                (unsigned)e->agent_id,
                e->tag,
                e->msg);
    }
    kprintf("\n");
}

/* ------------------------------------------------------------------ */
/* Logger agent main                                                    */
/* ------------------------------------------------------------------ */

void logger_agent_main(void) {
    g_logger_id = g_current_agent;
    agent_set_goal("Record system events to ring-buffer log");
    agent_set_contract(
        "Ring buffer allocated",
        "All log_write calls are persisted",
        "Buffer never corrupts (head+count invariant)"
    );

    logger_write("LOGGER", "Logger agent started");

    /* Receive MSG_DATA log requests from other agents */
    message_t msg;
    for (;;) {
        if (!agent_recv(&msg, 0)) continue;

        if (msg.type == MSG_SHUTDOWN) {
            logger_write("LOGGER", "Shutdown requested");
            agent_exit(true, "Logger shut down");
            return;
        }

        if (msg.type == MSG_DATA && msg.raw.len > 0) {
            /* data[0..7] = tag (null-padded), data[8..] = message text */
            char tag[9]  = {0};
            char text[48] = {0};
            uint8_t tlen = msg.raw.len > 8 ? 8 : msg.raw.len;
            for (int i = 0; i < tlen && msg.raw.data[i]; ++i) tag[i] = msg.raw.data[i];
            uint8_t mlen = msg.raw.len > 8 ? msg.raw.len - 8 : 0;
            if (mlen > 47) mlen = 47;
            for (int i = 0; i < mlen; ++i) text[i] = msg.raw.data[8 + i];
            logger_write(tag, text);
        }
    }
}
