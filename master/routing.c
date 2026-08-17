#include <syslog.h>
#include "routing.h"
#include <string.h>
#include <stdio.h>

/* Open-addressing hash map: UID -> node_id.
 * Empty slot: uid[0]=='\0'. Tombstone: node_id==255. */
#define TABLE_SIZE (ROUTING_MAX_ENTRIES * 2)

typedef struct { char uid[UID_BUF_SIZE]; uint8_t node_id; } entry_t;
static entry_t s_table[TABLE_SIZE];
static int     s_count;

static uint32_t hash(const char *uid) {
    uint32_t h = 2166136261u;
    for (const char *p = uid; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h % TABLE_SIZE;
}

void routing_init(void) { memset(s_table, 0, sizeof(s_table)); s_count = 0; }

void routing_attach(const char *uid, uint8_t node_id) {
    uint32_t i = hash(uid);
    for (int p = 0; p < TABLE_SIZE; p++) {
        uint32_t idx = (i + p) % TABLE_SIZE;
        if (s_table[idx].uid[0] == '\0' || s_table[idx].node_id == 255
                || strcmp(s_table[idx].uid, uid) == 0) {
            int is_new = (s_table[idx].uid[0] == '\0' || s_table[idx].node_id == 255);
            strncpy(s_table[idx].uid, uid, UID_BUF_SIZE - 1);
            s_table[idx].uid[UID_BUF_SIZE-1] = '\0';
            s_table[idx].node_id = node_id;
            if (is_new) s_count++;
            return;
        }
    }
    fprintf(stderr, "routing: table full!\n");
}

void routing_detach(const char *uid) {
    uint32_t i = hash(uid);
    for (int p = 0; p < TABLE_SIZE; p++) {
        uint32_t idx = (i + p) % TABLE_SIZE;
        if (s_table[idx].uid[0] == '\0') break;
        if (s_table[idx].node_id != 255 && strcmp(s_table[idx].uid, uid) == 0) {
            s_table[idx].node_id = 255; s_count--; return;
        }
    }
}

void routing_evict_node(uint8_t node_id) {
    int evicted = 0;
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (s_table[i].uid[0] != '\0' && s_table[i].node_id == node_id) {
            s_table[i].node_id = 255;
            s_count--;
            evicted++;
        }
    }
    if (evicted > 0) {
        syslog(LOG_INFO, "routing: evicted %d users from offline node %d", evicted, node_id);
    }
}

uint8_t routing_lookup(const char *uid) {
    uint32_t i = hash(uid);
    for (int p = 0; p < TABLE_SIZE; p++) {
        uint32_t idx = (i + p) % TABLE_SIZE;
        if (s_table[idx].uid[0] == '\0') return NODE_MASTER;
        if (s_table[idx].node_id != 255 && strcmp(s_table[idx].uid, uid) == 0)
            return s_table[idx].node_id;
    }
    return NODE_MASTER;
}

int routing_count(void) { return s_count; }

void routing_dump(void) {
    syslog(LOG_INFO, "[routing] %d users online", s_count);
    for (int i = 0; i < TABLE_SIZE; i++)
        if (s_table[i].uid[0] != '\0' && s_table[i].node_id != 255)
            syslog(LOG_DEBUG, "  %s -> node %d", s_table[i].uid, s_table[i].node_id);
}
void routing_each_at_node(uint8_t node_id,
                           void (*cb)(const char *uid, void *ctx),
                           void *ctx) {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (s_table[i].uid[0] != '\0' && s_table[i].node_id == node_id)
            cb(s_table[i].uid, ctx);
    }
}
