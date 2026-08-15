/**
 * packet.h — radcom wire protocol
 *
 * Binary packed format, little-endian. LoRa's built-in CRC covers integrity;
 * no app-layer checksum. See init.txt §7.
 *
 * Reliability mode: 3-hop (ack only master→dest hop). init.txt §6.
 *
 * UID scheme: E.164 phone number, e.g. "919876543210", stored as null-terminated
 * string. Max 15 digits. Globally unique, no coordination required. init.txt §3.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Packet types ─────────────────────────────────────────────────────────── */
typedef enum __attribute__((packed)) {
    PKT_DATA     = 0x01,  /* node→master: outgoing message from a phone        */
    PKT_ACK      = 0x02,  /* node→master: delivery confirmation / empty reply  */
    PKT_POLL     = 0x03,  /* master→node: it's your turn; may carry delivery   */
    PKT_PRESENCE = 0x04,  /* node→master: phone attached or detached           */
} pkt_type_t;

/* ── Presence event codes (in payload[0]) ────────────────────────────────── */
#define PRESENCE_ATTACH  'A'
#define PRESENCE_DETACH  'D'

/* ── Addressing ─────────────────────────────────────────────────────────── */
#define NODE_MASTER   0x00   /* master's node_id on the wire */
#define NODE_BCAST    0xFF   /* broadcast (reserved, not used in v1) */
#define UID_MAX_LEN   15     /* E.164 max digits (excl. null terminator) */
#define UID_BUF_SIZE  16     /* including null terminator */
#define PAYLOAD_MAX   100    /* bytes; caps airtime. ~100 UTF-8 chars */

/* ── Wire packet ─────────────────────────────────────────────────────────── */
/*
 * All packet types share this header. Optional fields are zeroed when unused.
 * Serialization only writes bytes required for the given type (see pkt_wire_len).
 *
 * Wire layout (packed, in order):
 *   [0]     type        1 B
 *   [1]     msg_id      1 B   wraps 0..255; used for dedup at receiver
 *   [2]     ack_id      1 B   msg_id being confirmed (0 = nothing to ack)
 *   [3]     node_id     1 B   sender's node id
 *   [4]     ttl         1 B   expiry in minutes (DATA/POLL with payload only)
 *   [5..20] src_uid    16 B   null-terminated E.164 phone number
 *   [21..36]dst_uid    16 B   null-terminated E.164 phone number
 *   [37]    payload_len 1 B   0..PAYLOAD_MAX
 *   [38..]  payload    ≤100 B message text (UTF-8) or presence event byte
 *
 * Total max: 138 bytes — well within SX1278's 255-byte FIFO limit at SF7.
 */
typedef struct __attribute__((packed)) {
    pkt_type_t type;
    uint8_t    msg_id;
    uint8_t    ack_id;       /* delivery ack: echoes the POLL's msg_id when delivering */
    uint8_t    node_id;
    uint8_t    ttl;          /* minutes; 0 = no expiry (for control packets) */
    char       src_uid[UID_BUF_SIZE];
    char       dst_uid[UID_BUF_SIZE];
    uint8_t    payload_len;
    uint8_t    payload[PAYLOAD_MAX];
} radio_pkt_t;

/* Fixed header size (fields always present on the wire) */
#define PKT_HDR_SIZE  offsetof(radio_pkt_t, payload)

/* ── Wire lengths per type ────────────────────────────────────────────────── */
/*
 * We only put bytes on air that the receiver needs. Reduces ToA.
 *
 *  POLL     = 4 bytes (type+msg_id+ack_id+node_id) when no piggybacked delivery
 *             full struct when piggybacking a DATA delivery
 *  ACK      = 4 bytes
 *  PRESENCE = 4 + 1 + 16 + 1 + 1 = 23 bytes (header + ttl + src_uid + plen + event)
 *  DATA     = full struct (4 + 1 + 16 + 16 + 1 + payload_len)
 *
 * pkt_wire_len() returns the exact number of bytes to transmit / expect.
 */
static inline size_t pkt_wire_len(const radio_pkt_t *p) {
    switch (p->type) {
        case PKT_ACK:
            return 4;  /* type, msg_id, ack_id, node_id — no payload needed */
        case PKT_POLL:
            if (p->payload_len == 0) return 4;
            /* fall through: piggybacking a delivery = full DATA fields */
            /* FALLTHROUGH */
        case PKT_DATA:
            return PKT_HDR_SIZE + p->payload_len;
        case PKT_PRESENCE:
            /* src_uid + 1 byte event in payload[0] */
            return 4 + 1 + UID_BUF_SIZE + 1 + 1; /* hdr4 + ttl + src + plen + event */
        default:
            return sizeof(radio_pkt_t); /* unknown: send full (safe fallback) */
    }
}

/* ── Serialise / deserialise ─────────────────────────────────────────────── */

/**
 * Serialise *pkt into buf. Returns bytes written, or 0 on error.
 * buf must be at least sizeof(radio_pkt_t) bytes.
 */
size_t pkt_serialize(const radio_pkt_t *pkt, uint8_t *buf, size_t buf_size);

/**
 * Deserialise len bytes from buf into *out. Returns false on error.
 * Validates type, payload_len bounds, and UID format.
 */
bool pkt_deserialize(const uint8_t *buf, size_t len, radio_pkt_t *out);

/**
 * Validate a UID: non-empty, digits only, max UID_MAX_LEN chars.
 * Empty string returns false.
 */
bool uid_valid(const char *uid);

/**
 * Build a minimal ACK packet in *out.
 * ack_id echoes the msg_id we're confirming.
 */
void pkt_make_ack(radio_pkt_t *out, uint8_t node_id, uint8_t ack_id);

/**
 * Build a PRESENCE packet in *out.
 * event: PRESENCE_ATTACH or PRESENCE_DETACH
 */
void pkt_make_presence(radio_pkt_t *out, uint8_t node_id,
                        const char *phone_num, char event);
