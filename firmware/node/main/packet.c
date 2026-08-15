/**
 * packet.c — wire protocol serialise / deserialise
 *
 * Pure C, no RTOS dependency. Tested on host via tests/test_main.c.
 *
 * Key layout note: PRESENCE wire format skips dst_uid (not needed for a
 * presence report). The struct has dst_uid between src_uid and payload_len,
 * so pkt_serialize handles PRESENCE explicitly to avoid that mismatch.
 *
 * PRESENCE wire: [0]type [1]msg_id [2]ack_id [3]node_id [4]ttl
 *                [5..20]src_uid(16) [21]payload_len [22]event  = 23 bytes
 *
 * DATA wire:     full struct layout (packed), up to payload_len + payload.
 * ACK/POLL wire: first 4 bytes only (when no piggybacked data).
 */

#include "packet.h"
#include <string.h>
#include <ctype.h>
#include <stddef.h>

bool uid_valid(const char *uid) {
    if (!uid || uid[0] == '\0') return false;
    size_t len = 0;
    for (const char *p = uid; *p; ++p, ++len) {
        if (!isdigit((unsigned char)*p)) return false;
        if (len >= UID_MAX_LEN) return false;
    }
    return len > 0;
}

void pkt_make_ack(radio_pkt_t *out, uint8_t node_id, uint8_t ack_id) {
    memset(out, 0, sizeof(*out));
    out->type    = PKT_ACK;
    out->node_id = node_id;
    out->ack_id  = ack_id;
}

void pkt_make_presence(radio_pkt_t *out, uint8_t node_id,
                        const char *phone_num, char event) {
    memset(out, 0, sizeof(*out));
    out->type        = PKT_PRESENCE;
    out->node_id     = node_id;
    out->ttl         = 0;
    strncpy(out->src_uid, phone_num, UID_MAX_LEN);
    out->src_uid[UID_MAX_LEN] = '\0';
    out->payload_len = 1;
    out->payload[0]  = (uint8_t)event;
}

size_t pkt_serialize(const radio_pkt_t *pkt, uint8_t *buf, size_t buf_size) {
    if (!pkt || !buf) return 0;
    if (pkt->payload_len > PAYLOAD_MAX) return 0;

    size_t wire = pkt_wire_len(pkt);
    if (wire > buf_size) return 0;

    if (pkt->type == PKT_PRESENCE) {
        buf[0] = (uint8_t)pkt->type;
        buf[1] = pkt->msg_id;
        buf[2] = pkt->ack_id;
        buf[3] = pkt->node_id;
        buf[4] = pkt->ttl;
        memcpy(&buf[5], pkt->src_uid, UID_BUF_SIZE);
        buf[21] = pkt->payload_len;
        buf[22] = pkt->payload[0];
        return wire;
    }

    memcpy(buf, pkt, wire);
    return wire;
}

bool pkt_deserialize(const uint8_t *buf, size_t len, radio_pkt_t *out) {
    if (!buf || !out || len < 4) return false;

    memset(out, 0, sizeof(*out));

    out->type    = (pkt_type_t)buf[0];
    out->msg_id  = buf[1];
    out->ack_id  = buf[2];
    out->node_id = buf[3];

    switch (out->type) {
        case PKT_DATA:
        case PKT_ACK:
        case PKT_POLL:
        case PKT_PRESENCE:
            break;
        default:
            return false;
    }

    if (out->type == PKT_ACK) return true;
    if (out->type == PKT_POLL && len == 4) return true;

    if (out->type == PKT_PRESENCE) {
        if (len < 23) return false;
        out->ttl = buf[4];
        memcpy(out->src_uid, &buf[5], UID_BUF_SIZE);
        out->src_uid[UID_MAX_LEN] = '\0';
        out->payload_len = buf[21];
        if (out->payload_len != 1) return false;
        out->payload[0] = buf[22];
        char ev = (char)out->payload[0];
        if (ev != PRESENCE_ATTACH && ev != PRESENCE_DETACH) return false;
        if (!uid_valid(out->src_uid)) return false;
        return true;
    }

    if (len < PKT_HDR_SIZE) return false;

    out->ttl = buf[4];
    memcpy(out->src_uid, &buf[5],  UID_BUF_SIZE);
    memcpy(out->dst_uid, &buf[21], UID_BUF_SIZE);
    out->src_uid[UID_MAX_LEN] = '\0';
    out->dst_uid[UID_MAX_LEN] = '\0';

    out->payload_len = buf[37];
    if (out->payload_len > PAYLOAD_MAX) return false;
    if (len < PKT_HDR_SIZE + out->payload_len) return false;

    memcpy(out->payload, &buf[38], out->payload_len);

    if (out->type == PKT_DATA) {
        if (!uid_valid(out->src_uid)) return false;
        if (!uid_valid(out->dst_uid)) return false;
    }

    return true;
}
