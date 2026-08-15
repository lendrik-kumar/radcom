/**
 * poller.h — master polling state machine (init.txt §5, §6).
 *
 * Drives the round-robin polling loop.
 * Calls radio, routing, and queue directly.
 */
#pragma once
#include <stdint.h>

/* Maximum known nodes. Adjust before deployment. */
#define POLLER_MAX_NODES 20

/* How many consecutive timeouts before a node is moved to keepalive rotation */
#define POLLER_ABSENT_THRESHOLD 5

/* How often (in full cycles) to re-poll an absent node (keepalive) */
#define POLLER_KEEPALIVE_CYCLES 20

/* Per-slot TX→ACK wait in ms. At SF7/BW125, 138-byte round-trip is ~300ms.
 * 500ms gives 200ms margin. */
#define POLLER_SLOT_TIMEOUT_MS 500

void poller_init(uint8_t *node_ids, int count);
void poller_run(void);   /* blocking — runs until keep_running=false */
