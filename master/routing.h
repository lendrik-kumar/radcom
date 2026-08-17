/**
 * routing.h — UID-to-node routing table for radcom master.
 * Maps E.164 UIDs to the node_id they are attached to.
 * In-memory. Populated from PKT_PRESENCE events.
 * Open-addressing hash map, linear probing.
 */
#pragma once
#include <stdint.h>
#include "packet.h"

#define ROUTING_MAX_ENTRIES 10000

void    routing_init(void);
void    routing_attach(const char *uid, uint8_t node_id);
void    routing_detach(const char *uid);
void    routing_evict_node(uint8_t node_id);
uint8_t routing_lookup(const char *uid); /* returns NODE_MASTER(0) = not found */
int     routing_count(void);
void    routing_dump(void);
/* Iterate all UIDs attached to node_id, calling cb for each. */
void routing_each_at_node(uint8_t node_id,
                           void (*cb)(const char *uid, void *ctx),
                           void *ctx);
