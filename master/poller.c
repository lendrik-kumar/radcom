#include "poller.h"
#include "radio.h"
#include "routing.h"
#include "queue.h"
#include "packet.h"
#include <string.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

extern volatile int keep_running;

typedef struct {
    uint8_t node_id;
    int     absent_streak;
    int     keepalive_counter;
    int     active;
} node_state_t;

static node_state_t s_nodes[POLLER_MAX_NODES];
static int          s_node_count;
static uint8_t      s_msg_id; /* rolling 0..255 */

/* Track a piggybacked delivery — need row_id as int64 (NOT stuffed in packet field) */
static struct {
    int     active;
    uint8_t poll_msg_id;   /* msg_id of the POLL that carried the delivery */
    int64_t queue_row_id;  /* SQLite row to delete on confirmed ACK */
} s_pending;

void poller_init(uint8_t *node_ids, int count) {
    s_node_count = count > POLLER_MAX_NODES ? POLLER_MAX_NODES : count;
    for (int i = 0; i < s_node_count; i++) {
        s_nodes[i].node_id           = node_ids[i];
        s_nodes[i].absent_streak     = 0;
        s_nodes[i].keepalive_counter = 0;
        s_nodes[i].active            = 1;
    }
    s_msg_id = 1;
}

static void poll_node(node_state_t *ns) {
    radio_pkt_t poll_pkt;
    memset(&poll_pkt, 0, sizeof(poll_pkt));
    poll_pkt.type    = PKT_POLL;
    poll_pkt.node_id = ns->node_id;
    poll_pkt.msg_id  = s_msg_id++;
    poll_pkt.node_id = NODE_MASTER; /* src is master */

    /* Wait — POLL's node_id means "addressed to this node" in our wire format.
     * Looking at ESP32 lora_task: it checks pkt.node_id != s_node_id.
     * So node_id in POLL = destination node id. Fix: */
    poll_pkt.node_id = ns->node_id; /* destination node */

    s_pending.active = 0;

    /* Check if we have a queued message for a user at this node */
    radio_pkt_t queued;
    int64_t row_id = 0;
    if (queue_pop_for_node(ns->node_id, &queued, &row_id)) {
        /* Piggyback: embed the DATA fields into the POLL */
        poll_pkt.ttl         = queued.ttl;
        strncpy(poll_pkt.src_uid, queued.src_uid, UID_MAX_LEN);
        strncpy(poll_pkt.dst_uid, queued.dst_uid, UID_MAX_LEN);
        poll_pkt.payload_len = queued.payload_len;
        memcpy(poll_pkt.payload, queued.payload, queued.payload_len);
        s_pending.active       = 1;
        s_pending.poll_msg_id  = poll_pkt.msg_id;
        s_pending.queue_row_id = row_id;
        syslog(LOG_INFO, "poll: node %d POLL+delivery %s→%s (%d bytes)",
               ns->node_id, queued.src_uid, queued.dst_uid, queued.payload_len);
    }

    /* radio_send blocks until TxDone, then re-enters RX mode internally */
    if (radio_send(&poll_pkt) < 0) {
        syslog(LOG_ERR, "poll: radio_send failed for node %d", ns->node_id);
        return;
    }

    /* Now wait for the node's reply (ACK, DATA, or PRESENCE) */
    radio_pkt_t resp;
    int ret = radio_wait_rx(&resp, POLLER_SLOT_TIMEOUT_MS);

    if (ret == 0) {
        /* Timeout — node didn't respond */
        ns->absent_streak++;
        syslog(LOG_DEBUG, "poll: node %d timeout (streak=%d)", ns->node_id, ns->absent_streak);
        if (ns->absent_streak >= POLLER_ABSENT_THRESHOLD && ns->active) {
            ns->active = 0;
            ns->keepalive_counter = POLLER_KEEPALIVE_CYCLES;
            syslog(LOG_WARNING, "poll: node %d absent after %d timeouts", ns->node_id, ns->absent_streak);
        }
        return;
    }

    if (ret < 0) {
        syslog(LOG_ERR, "poll: radio_wait_rx error for node %d", ns->node_id);
        return;
    }

    /* Got a response */
    ns->absent_streak = 0;
    if (!ns->active) {
        ns->active = 1;
        syslog(LOG_NOTICE, "poll: node %d back online", ns->node_id);
    }

    /* Check it came from the node we polled (node sends its own node_id) */
    if (resp.node_id != ns->node_id) {
        syslog(LOG_WARNING, "poll: response node_id=%d != expected %d — ignoring",
               resp.node_id, ns->node_id);
        return;
    }

    switch (resp.type) {

    case PKT_ACK:
        /* Node ACK — may confirm delivery of piggybacked message */
        if (s_pending.active && resp.ack_id == s_pending.poll_msg_id) {
            queue_delete(s_pending.queue_row_id);
            syslog(LOG_INFO, "poll: node %d confirmed delivery (row=%lld deleted)",
                   ns->node_id, (long long)s_pending.queue_row_id);
            s_pending.active = 0;
        } else {
            syslog(LOG_DEBUG, "poll: node %d ACK (empty slot, nothing queued)", ns->node_id);
        }
        break;

    case PKT_DATA: {
        /* Node is sending us an outgoing message from a phone */
        syslog(LOG_INFO, "poll: DATA from node %d: %s→%s (%d bytes)",
               ns->node_id, resp.src_uid, resp.dst_uid, resp.payload_len);

        /* 3-hop model: master acks the uplink so the node can tell the phone "sent" */
        radio_pkt_t ack;
        pkt_make_ack(&ack, NODE_MASTER, resp.msg_id);
        /* We send this ACK, which blocks until TxDone, then auto-re-enters RX */
        radio_send(&ack);

        /* Route message to destination */
        uint8_t dest_node = routing_lookup(resp.dst_uid);
        if (dest_node != NODE_MASTER) {
            syslog(LOG_INFO, "poll: routing %s→%s via node %d",
                   resp.src_uid, resp.dst_uid, dest_node);
        } else {
            syslog(LOG_INFO, "poll: %s offline, storing in queue", resp.dst_uid);
        }
        /* queue_push stores dst_uid; poller will pick it up when dest_node gets polled */
        queue_push(resp.dst_uid, &resp);
        break;
    }

    case PKT_PRESENCE: {
        if (resp.payload_len < 1) { syslog(LOG_WARNING, "poll: empty PRESENCE"); break; }
        char event = (char)resp.payload[0];
        if (!uid_valid(resp.src_uid)) {
            syslog(LOG_WARNING, "poll: PRESENCE with invalid uid '%s'", resp.src_uid);
            break;
        }
        if (event == PRESENCE_ATTACH) {
            routing_attach(resp.src_uid, ns->node_id);
            syslog(LOG_NOTICE, "presence: ATTACH %s @ node %d (total online=%d)",
                   resp.src_uid, ns->node_id, routing_count());
            queue_expire();
        } else if (event == PRESENCE_DETACH) {
            routing_detach(resp.src_uid);
            syslog(LOG_NOTICE, "presence: DETACH %s (total online=%d)",
                   resp.src_uid, routing_count());
        } else {
            syslog(LOG_WARNING, "poll: unknown PRESENCE event '%c'", event);
        }
        break;
    }

    default:
        syslog(LOG_DEBUG, "poll: unexpected pkt type 0x%02x from node %d",
               resp.type, ns->node_id);
        break;
    }
}

void poller_run(void) {
    syslog(LOG_NOTICE, "poller: starting with %d node(s)", s_node_count);
    int cycle = 0;
    while (keep_running) {
        for (int i = 0; i < s_node_count && keep_running; i++) {
            node_state_t *ns = &s_nodes[i];
            if (!ns->active) {
                if (--ns->keepalive_counter > 0) continue;
                ns->keepalive_counter = POLLER_KEEPALIVE_CYCLES;
                syslog(LOG_DEBUG, "poller: keepalive poll node %d", ns->node_id);
            }
            radio_start_rx(); /* ensure RX before every slot */
            poll_node(ns);
        }
        /* Expire old queue rows periodically */
        if (++cycle % 100 == 0) queue_expire();
    }
    syslog(LOG_NOTICE, "poller: stopped");
}
