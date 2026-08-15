/**
 * queue.h — SQLite-backed store-and-forward queue.
 * Survives Pi reboots. TTL in minutes.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "packet.h"

int   queue_init(const char *db_path);
int   queue_push(const char *dst_uid, const radio_pkt_t *pkt);
/* Pop oldest non-expired message for any uid attached to node_id.
 * On success, sets *row_id to the DB primary key (pass to queue_delete after ACK).
 * Returns 1 if found, 0 if empty. */
int   queue_pop_for_node(uint8_t node_id, radio_pkt_t *out, int64_t *row_id);
void  queue_delete(int64_t row_id);
void  queue_expire(void);
int   queue_count(void);
void  queue_close(void);
void queue_expire_all(void); /* test use only */
