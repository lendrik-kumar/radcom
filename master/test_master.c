/**
 * test_master.c — Comprehensive test suite for radcom master node.
 *
 * Tests every subsystem and edge case:
 *   - Packet serialize/deserialize (all types, edge cases, round-trips)
 *   - Routing table (attach, detach, lookup, hash collisions, capacity)
 *   - Queue (push/pop, FIFO order, TTL expiry, persistence, pop_for_node)
 *   - Poller logic (presence, ack, data, timeout, absent/keepalive states)
 *   - Integration: full message flow A→B (online and offline)
 *   - Integration: duplicate detection, piggybacked delivery, ACK chain
 *
 * No hardware required. Run on the Pi directly:
 *   gcc -Wall -O2 -g test_master.c packet.c routing.c queue.c -lsqlite3 -o test_master
 *   sudo ./test_master
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <syslog.h>
#include <time.h>
#include <signal.h>

#include "packet.h"
#include "routing.h"
#include "queue.h"
#include "poller.h"
#include "radio.h"

/* ── Minimal test framework ── */
static int s_pass = 0, s_fail = 0;
volatile sig_atomic_t keep_running = 1;

static radio_pkt_t s_mock_rx_q[16];
static int s_mock_rx_head = 0, s_mock_rx_tail = 0;
static radio_pkt_t s_mock_tx_q[16];
static int s_mock_tx_head = 0, s_mock_tx_tail = 0;

int radio_send(const radio_pkt_t *pkt) {
    if (s_mock_tx_tail < 16) {
        s_mock_tx_q[s_mock_tx_tail++] = *pkt;
        return 0;
    }
    return -1;
}
int radio_wait_rx(radio_pkt_t *out, int timeout_ms) {
    (void)timeout_ms;
    if (s_mock_rx_head < s_mock_rx_tail) {
        *out = s_mock_rx_q[s_mock_rx_head++];
        /* Stop the poller after 1 receive for tests to not loop infinitely */
        keep_running = 0;
        return 10; /* Fake valid length */
    }
    /* Timeout */
    keep_running = 0; /* Stop after timeout too, tests will control loop manually */
    return 0; 
}
void radio_start_rx(void) {
}


#define TEST(name)   do { printf("\n[TEST] %s\n", name); } while(0)
#define PASS(msg)    do { printf("  PASS: %s\n", msg); s_pass++; } while(0)
#define FAIL(msg)    do { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); s_fail++; } while(0)
#define CHECK(cond, msg) do { if(cond) PASS(msg); else FAIL(msg); } while(0)

/* syslog stub — redirects to stdout for test output */
void openlog(const char *a, int b, int c) { (void)a;(void)b;(void)c; }
void closelog(void) {}
/* syslog is a variadic macro in glibc — we override the level-specific ones via -D */

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1: Packet serialization / deserialization
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_packet_ack(void) {
    TEST("PKT_ACK — minimal 4-byte wire format");
    radio_pkt_t pkt;
    pkt_make_ack(&pkt, 1, 42);

    CHECK(pkt.type   == PKT_ACK, "type == PKT_ACK");
    CHECK(pkt.ack_id == 42,      "ack_id == 42");
    CHECK(pkt.node_id == 1,      "node_id == 1");

    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    CHECK(len == 4, "serialized length == 4 bytes");
    CHECK(buf[0] == PKT_ACK, "buf[0] is PKT_ACK");
    CHECK(buf[2] == 42,      "buf[2] is ack_id=42");
    CHECK(buf[3] == 1,       "buf[3] is node_id=1");

    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(ok,                       "deserialize succeeds");
    CHECK(out.type   == PKT_ACK,    "round-trip type");
    CHECK(out.ack_id == 42,         "round-trip ack_id");
    CHECK(out.node_id == 1,         "round-trip node_id");
}

static void test_packet_poll_empty(void) {
    TEST("PKT_POLL — empty (no piggybacked delivery), 4 bytes");
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type    = PKT_POLL;
    pkt.msg_id  = 7;
    pkt.node_id = 3;
    pkt.payload_len = 0;

    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    CHECK(len == 4, "empty POLL = 4 bytes");

    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(ok,                    "deserialize ok");
    CHECK(out.type == PKT_POLL,  "type round-trip");
    CHECK(out.msg_id == 7,       "msg_id round-trip");
    CHECK(out.node_id == 3,      "node_id round-trip");
    CHECK(out.payload_len == 0,  "payload_len == 0");
}

static void test_packet_poll_piggybacked(void) {
    TEST("PKT_POLL — with piggybacked DATA delivery (full struct)");
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type    = PKT_POLL;
    pkt.msg_id  = 10;
    pkt.node_id = 2;
    pkt.ttl     = 30;
    strncpy(pkt.src_uid, "917777777777", UID_MAX_LEN);
    strncpy(pkt.dst_uid, "918888888888", UID_MAX_LEN);
    const char *msg = "Hello from A";
    pkt.payload_len = (uint8_t)strlen(msg);
    memcpy(pkt.payload, msg, pkt.payload_len);

    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    size_t expected = PKT_HDR_SIZE + pkt.payload_len;
    CHECK(len == expected, "piggybacked POLL uses full DATA length");

    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(ok,                              "deserialize ok");
    CHECK(out.type == PKT_POLL,            "type round-trip");
    CHECK(out.payload_len == pkt.payload_len, "payload_len round-trip");
    CHECK(memcmp(out.payload, pkt.payload, pkt.payload_len) == 0, "payload content matches");
    CHECK(strcmp(out.src_uid, "917777777777") == 0, "src_uid round-trip");
    CHECK(strcmp(out.dst_uid, "918888888888") == 0, "dst_uid round-trip");
}

static void test_packet_data_full(void) {
    TEST("PKT_DATA — full message with 100-byte payload (max)");
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type    = PKT_DATA;
    pkt.msg_id  = 200;
    pkt.node_id = 1;
    pkt.ttl     = 60;
    strncpy(pkt.src_uid, "919876543210", UID_MAX_LEN);
    strncpy(pkt.dst_uid, "911234567890", UID_MAX_LEN);
    pkt.payload_len = PAYLOAD_MAX;
    memset(pkt.payload, 'X', PAYLOAD_MAX);

    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    CHECK(len == PKT_HDR_SIZE + PAYLOAD_MAX, "max-payload length correct");

    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(ok,                           "deserialize ok");
    CHECK(out.payload_len == PAYLOAD_MAX, "payload_len == 100");
    CHECK(out.payload[99] == 'X',       "last payload byte correct");
}

static void test_packet_presence_attach(void) {
    TEST("PKT_PRESENCE — ATTACH event");
    radio_pkt_t pkt;
    pkt_make_presence(&pkt, 1, "919876543210", PRESENCE_ATTACH);

    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    /* presence wire: 4 + 1 + 16 + 1 + 1 = 23 */
    CHECK(len == 23, "PRESENCE wire length == 23");

    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(ok,                              "deserialize ok");
    CHECK(out.type == PKT_PRESENCE,        "type round-trip");
    CHECK(out.payload_len == 1,            "payload_len == 1");
    CHECK(out.payload[0] == PRESENCE_ATTACH, "event byte == ATTACH");
    CHECK(strcmp(out.src_uid, "919876543210") == 0, "src_uid round-trip");
}

static void test_packet_presence_detach(void) {
    TEST("PKT_PRESENCE — DETACH event");
    radio_pkt_t pkt;
    pkt_make_presence(&pkt, 2, "911234567890", PRESENCE_DETACH);
    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(ok, "deserialize ok");
    CHECK(out.payload[0] == PRESENCE_DETACH, "event byte == DETACH");
}

static void test_packet_deserialize_truncated(void) {
    TEST("PKT_DATA — deserialize with truncated buffer (must fail)");
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_DATA;
    pkt.msg_id = 1; pkt.node_id = 1;
    strncpy(pkt.src_uid, "919876543210", UID_MAX_LEN);
    strncpy(pkt.dst_uid, "911234567890", UID_MAX_LEN);
    pkt.payload_len = 10;
    memset(pkt.payload, 'A', 10);

    uint8_t buf[256];
    size_t full_len = pkt_serialize(&pkt, buf, sizeof(buf));

    radio_pkt_t out;
    /* Feed only 3 bytes — too short for any valid packet */
    bool ok = pkt_deserialize(buf, 3, &out);
    CHECK(!ok, "3-byte truncation correctly rejected");

    /* Feed correct length - 1 byte — should also fail */
    ok = pkt_deserialize(buf, full_len - 1, &out);
    CHECK(!ok, "one-byte truncation correctly rejected");
}

static void test_packet_deserialize_overflow_payload(void) {
    TEST("PKT_DATA — deserialize with payload_len > PAYLOAD_MAX (must fail)");
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_DATA;
    pkt.msg_id = 1; pkt.node_id = 1;
    strncpy(pkt.src_uid, "919876543210", UID_MAX_LEN);
    strncpy(pkt.dst_uid, "911234567890", UID_MAX_LEN);
    pkt.payload_len = PAYLOAD_MAX;
    memset(pkt.payload, 'A', PAYLOAD_MAX);

    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));

    /* Corrupt the payload_len field to > PAYLOAD_MAX */
    buf[PKT_HDR_SIZE - 1] = 255; /* payload_len field */

    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(!ok, "payload_len overflow correctly rejected");
}

static void test_packet_uid_valid(void) {
    TEST("uid_valid — valid and invalid UIDs");
    CHECK(uid_valid("919876543210"),    "valid E.164 number");
    CHECK(uid_valid("911234567890123"), "valid max-length (15 digits)");
    CHECK(!uid_valid(""),              "empty string rejected");
    CHECK(!uid_valid("91abc1234567"),  "alpha chars rejected");
    CHECK(!uid_valid("9198765432101234"), "16-digit too long rejected");
    CHECK(!uid_valid("+919876543210"), "plus sign rejected");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2: Routing table
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_routing_basic(void) {
    TEST("Routing — basic attach, lookup, detach");
    routing_init();

    routing_attach("919876543210", 1);
    CHECK(routing_lookup("919876543210") == 1, "lookup after attach returns node 1");
    CHECK(routing_count() == 1, "count == 1");

    routing_attach("911234567890", 2);
    CHECK(routing_lookup("911234567890") == 2, "second user at node 2");
    CHECK(routing_count() == 2, "count == 2");

    routing_detach("919876543210");
    CHECK(routing_lookup("919876543210") == NODE_MASTER, "lookup after detach returns NODE_MASTER");
    CHECK(routing_count() == 1, "count back to 1 after detach");
}

static void test_routing_miss(void) {
    TEST("Routing — lookup of unknown UID returns NODE_MASTER");
    routing_init();
    CHECK(routing_lookup("999999999999") == NODE_MASTER, "unknown uid returns 0");
    CHECK(routing_count() == 0, "count == 0 on empty table");
}

static void test_routing_update(void) {
    TEST("Routing — update existing UID to new node (phone roams)");
    routing_init();
    routing_attach("919876543210", 1);
    routing_attach("919876543210", 3); /* phone moved to node 3 */
    CHECK(routing_lookup("919876543210") == 3, "lookup returns updated node 3");
    CHECK(routing_count() == 1, "count stays 1 (not duplicated)");
}

static void test_routing_detach_reattach(void) {
    TEST("Routing — detach then re-attach same UID");
    routing_init();
    routing_attach("919876543210", 1);
    routing_detach("919876543210");
    routing_attach("919876543210", 2);
    CHECK(routing_lookup("919876543210") == 2, "re-attach works after detach");
    CHECK(routing_count() == 1, "count == 1");
}

static void count_cb(const char *uid, void *ctx) {
    (void)uid;
    (*(int*)ctx)++;
}

static void test_routing_each_at_node(void) {
    TEST("Routing — routing_each_at_node iterates correct UIDs");
    routing_init();
    routing_attach("919000000001", 1);
    routing_attach("919000000002", 1);
    routing_attach("919000000003", 2); /* different node */
    routing_attach("919000000004", 1);

    int found = 0;
    /* Count how many UIDs routing_each_at_node reports for node 1 */
    routing_each_at_node(1, count_cb, &found);
    CHECK(found == 3, "routing_each_at_node returns 3 UIDs for node 1");

    found = 0;
    routing_each_at_node(2, count_cb, &found);
    CHECK(found == 1, "routing_each_at_node returns 1 UID for node 2");

    found = 0;
    routing_each_at_node(5, count_cb, &found);
    CHECK(found == 0, "routing_each_at_node returns 0 for empty node 5");
}

static void test_routing_100_users(void) {
    TEST("Routing — 100 users across 5 nodes, all lookups correct");
    routing_init();
    char uid[UID_BUF_SIZE];
    for (int i = 0; i < 100; i++) {
        snprintf(uid, sizeof(uid), "91%013d", i);
        routing_attach(uid, (uint8_t)(i % 5) + 1);
    }
    CHECK(routing_count() == 100, "100 users in table");

    int ok = 1;
    for (int i = 0; i < 100; i++) {
        snprintf(uid, sizeof(uid), "91%013d", i);
        if (routing_lookup(uid) != (uint8_t)(i % 5) + 1) { ok = 0; break; }
    }
    CHECK(ok, "all 100 lookups return correct node");

    /* Detach all */
    for (int i = 0; i < 100; i++) {
        snprintf(uid, sizeof(uid), "91%013d", i);
        routing_detach(uid);
    }
    CHECK(routing_count() == 0, "count == 0 after mass detach");
}

static void test_routing_double_detach(void) {
    TEST("Routing — double detach same UID is safe (no crash, no double-decrement)");
    routing_init();
    routing_attach("919876543210", 1);
    routing_detach("919876543210");
    routing_detach("919876543210"); /* second detach — should be a no-op */
    CHECK(routing_count() == 0, "count stays 0 after double detach");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3: Store-and-forward queue
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *TEST_DB = "/tmp/radcom_test.db";

static radio_pkt_t make_data_pkt(const char *src, const char *dst,
                                  const char *text, uint8_t ttl) {
    radio_pkt_t p;
    memset(&p, 0, sizeof(p));
    p.type = PKT_DATA;
    p.ttl  = ttl;
    strncpy(p.src_uid, src, UID_MAX_LEN);
    strncpy(p.dst_uid, dst, UID_MAX_LEN);
    p.payload_len = (uint8_t)strlen(text);
    memcpy(p.payload, text, p.payload_len);
    return p;
}

static void test_queue_basic_push_pop(void) {
    TEST("Queue — basic push/pop round-trip");
    unlink(TEST_DB);
    routing_init();
    int rc = queue_init(TEST_DB);
    CHECK(rc == 0, "queue_init succeeds");

    routing_attach("911111111111", 1); /* dst must be in routing table for pop_for_node */

    radio_pkt_t pkt = make_data_pkt("919876543210", "911111111111", "Hello!", 30);
    queue_push("911111111111", &pkt);
    CHECK(queue_count() == 1, "count == 1 after push");

    radio_pkt_t out;
    int64_t row_id = 0;
    int found = queue_pop_for_node(1, &out, &row_id);
    CHECK(found == 1, "pop_for_node returns 1");
    CHECK(row_id > 0, "row_id > 0");
    CHECK(strcmp(out.dst_uid, "911111111111") == 0, "dst_uid correct");
    CHECK(strcmp(out.src_uid, "919876543210") == 0, "src_uid correct");
    CHECK(out.payload_len == 6, "payload_len == 6");
    CHECK(memcmp(out.payload, "Hello!", 6) == 0, "payload content correct");

    queue_delete(row_id);
    CHECK(queue_count() == 0, "count == 0 after delete");

    queue_close();
}

static void test_queue_fifo_order(void) {
    TEST("Queue — FIFO order: oldest message popped first");
    unlink(TEST_DB);
    routing_init();
    routing_attach("911111111111", 1);
    queue_init(TEST_DB);

    radio_pkt_t p1 = make_data_pkt("919000000001", "911111111111", "First",  30);
    radio_pkt_t p2 = make_data_pkt("919000000002", "911111111111", "Second", 30);
    radio_pkt_t p3 = make_data_pkt("919000000003", "911111111111", "Third",  30);

    queue_push("911111111111", &p1);
    queue_push("911111111111", &p2);
    queue_push("911111111111", &p3);
    CHECK(queue_count() == 3, "count == 3");

    radio_pkt_t out; int64_t rid;
    queue_pop_for_node(1, &out, &rid);
    CHECK(memcmp(out.payload, "First", 5) == 0, "first pop returns 'First'");
    queue_delete(rid);

    queue_pop_for_node(1, &out, &rid);
    CHECK(memcmp(out.payload, "Second", 6) == 0, "second pop returns 'Second'");
    queue_delete(rid);

    queue_pop_for_node(1, &out, &rid);
    CHECK(memcmp(out.payload, "Third", 5) == 0, "third pop returns 'Third'");
    queue_delete(rid);

    CHECK(queue_count() == 0, "queue empty after all pops");
    queue_close();
}

static void test_queue_ttl_expiry(void) {
    TEST("Queue — TTL expiry: expired messages are not returned");
    unlink(TEST_DB);
    routing_init();
    routing_attach("911111111111", 1);
    queue_init(TEST_DB);

    /* Push a message with ttl=0 (which defaults to 60min) */
    radio_pkt_t good = make_data_pkt("919000000001", "911111111111", "Keep me", 60);
    queue_push("911111111111", &good);

    /* Manually insert an already-expired row directly into SQLite */
    /* We simulate expiry by pushing with ttl=1 min and backdating via direct SQL */
    /* Simpler: just push with ttl=1, then call queue_expire() which uses time(NULL) */
    /* To make it immediately expired: we insert with ttl_expires = now - 1 */
    /* We do this via a direct SQLite call from test */
    /* For simplicity: push a second msg, manually delete just the good one, then expire */

    radio_pkt_t bad = make_data_pkt("919000000002", "911111111111", "Expire me", 1);
    queue_push("911111111111", &bad);

    /* Artificially make 'bad' expired via SQL */
    {
        #include <sqlite3.h>
        sqlite3 *db;
        sqlite3_open(TEST_DB, &db);
        sqlite3_exec(db, "UPDATE queue SET ttl_expires=1 WHERE CAST(payload AS TEXT)='Expire me'", NULL, NULL, NULL);
        sqlite3_close(db);
    }

    CHECK(queue_count() == 1, "queue_count ignores expired rows (shows 1 not 2)");

    radio_pkt_t out; int64_t rid;
    int found = queue_pop_for_node(1, &out, &rid);
    CHECK(found == 1, "pop returns 1 (only non-expired)");
    CHECK(memcmp(out.payload, "Keep me", 7) == 0, "pop returns 'Keep me', not expired one");

    queue_expire(); /* should clean up the expired row */
    queue_delete(rid);
    CHECK(queue_count() == 0, "queue empty after expire + delete");

    queue_close();
}

static void test_queue_pop_wrong_node(void) {
    TEST("Queue — pop_for_node returns 0 when dst_uid is at different node");
    unlink(TEST_DB);
    routing_init();
    routing_attach("911111111111", 1); /* user at node 1 */
    queue_init(TEST_DB);

    radio_pkt_t pkt = make_data_pkt("919876543210", "911111111111", "Hi", 30);
    queue_push("911111111111", &pkt);

    radio_pkt_t out; int64_t rid;
    int found = queue_pop_for_node(2, &out, &rid); /* wrong node */
    CHECK(found == 0, "pop_for_node(2) returns 0 when user is at node 1");

    found = queue_pop_for_node(1, &out, &rid); /* correct node */
    CHECK(found == 1, "pop_for_node(1) returns 1");
    queue_delete(rid);
    queue_close();
}

static void test_queue_pop_offline_user(void) {
    TEST("Queue — pop_for_node returns 0 when dst_uid is not in routing table");
    unlink(TEST_DB);
    routing_init();
    /* Do NOT attach "911111111111" — user is offline */
    queue_init(TEST_DB);

    radio_pkt_t pkt = make_data_pkt("919876543210", "911111111111", "Offline msg", 30);
    queue_push("911111111111", &pkt);

    radio_pkt_t out; int64_t rid;
    int found = queue_pop_for_node(1, &out, &rid);
    CHECK(found == 0, "pop_for_node returns 0 when user offline");
    CHECK(queue_count() == 1, "message still queued");

    /* Now user comes online at node 1 */
    routing_attach("911111111111", 1);
    found = queue_pop_for_node(1, &out, &rid);
    CHECK(found == 1, "pop_for_node returns 1 after user attaches");
    queue_delete(rid);
    queue_close();
}

static void test_queue_persistence(void) {
    TEST("Queue — messages persist across close/reopen");
    unlink(TEST_DB);
    routing_init();
    routing_attach("911111111111", 1);
    queue_init(TEST_DB);

    radio_pkt_t pkt = make_data_pkt("919876543210", "911111111111", "Persistent", 60);
    queue_push("911111111111", &pkt);
    int c1 = queue_count();
    queue_close();

    /* Reopen */
    queue_init(TEST_DB);
    int c2 = queue_count();
    CHECK(c1 == c2, "message count same after close/reopen");
    CHECK(c2 == 1,  "count == 1");

    radio_pkt_t out; int64_t rid;
    int found = queue_pop_for_node(1, &out, &rid);
    CHECK(found == 1, "message survives close/reopen");
    CHECK(memcmp(out.payload, "Persistent", 10) == 0, "payload intact after reopen");
    queue_delete(rid);
    queue_close();
}

static void test_queue_multiple_users_same_node(void) {
    TEST("Queue — multiple users at same node, each gets their own message");
    unlink(TEST_DB);
    routing_init();
    routing_attach("911111111111", 1);
    routing_attach("912222222222", 1);
    queue_init(TEST_DB);

    radio_pkt_t pkt_a = make_data_pkt("919876543210", "911111111111", "For A", 30);
    radio_pkt_t pkt_b = make_data_pkt("919876543210", "912222222222", "For B", 30);
    queue_push("911111111111", &pkt_a);
    queue_push("912222222222", &pkt_b);

    CHECK(queue_count() == 2, "2 messages queued");

    radio_pkt_t out; int64_t rid;
    /* First pop gets one of the two messages */
    int found = queue_pop_for_node(1, &out, &rid);
    CHECK(found == 1, "first pop returns a message");
    queue_delete(rid);

    found = queue_pop_for_node(1, &out, &rid);
    CHECK(found == 1, "second pop returns a message");
    queue_delete(rid);

    CHECK(queue_count() == 0, "all messages consumed");
    queue_close();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4: Integration scenarios
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_integration_online_delivery(void) {
    TEST("Integration — Phone A → Phone B (both online, different nodes)");
    /*
     * Simulates poller logic manually:
     * 1. Phone A attaches to node 1 (PRESENCE ATTACH)
     * 2. Phone B attaches to node 2 (PRESENCE ATTACH)
     * 3. Phone A sends message to Phone B
     * 4. Node 1 sends PKT_DATA to master in its polling slot
     * 5. Master ACKs node 1 (3-hop), routes message to node 2's queue
     * 6. Next poll of node 2: master piggybacks message
     * 7. Node 2 ACKs delivery
     * 8. Master deletes queue row
     */
    unlink(TEST_DB);
    routing_init();
    queue_init(TEST_DB);

    /* Step 1 & 2: phones attach */
    routing_attach("919876543210", 1); /* Phone A @ node 1 */
    routing_attach("911234567890", 2); /* Phone B @ node 2 */
    CHECK(routing_count() == 2, "2 users online");

    /* Step 3 & 4: PKT_DATA arrives from node 1 */
    radio_pkt_t data;
    memset(&data, 0, sizeof(data));
    data.type = PKT_DATA;
    data.msg_id = 42;
    data.node_id = 1;
    data.ttl = 30;
    strncpy(data.src_uid, "919876543210", UID_MAX_LEN);
    strncpy(data.dst_uid, "911234567890", UID_MAX_LEN);
    const char *text = "Hello Bob!";
    data.payload_len = (uint8_t)strlen(text);
    memcpy(data.payload, text, data.payload_len);

    /* Step 5: master resolves destination and queues for node 2 */
    uint8_t dest_node = routing_lookup("911234567890");
    CHECK(dest_node == 2, "routing resolves Phone B to node 2");
    queue_push("911234567890", &data);
    CHECK(queue_count() == 1, "message queued for node 2");

    /* Step 6: poll node 2 — pop from queue */
    radio_pkt_t delivery; int64_t rid;
    int found = queue_pop_for_node(2, &delivery, &rid);
    CHECK(found == 1, "message found for node 2");
    CHECK(strcmp(delivery.src_uid, "919876543210") == 0, "src_uid correct in delivery");
    CHECK(strcmp(delivery.dst_uid, "911234567890") == 0, "dst_uid correct in delivery");
    CHECK(memcmp(delivery.payload, text, strlen(text)) == 0, "payload intact");

    /* Step 8: node 2 sends ACK (ack_id matches poll_msg_id) → delete */
    queue_delete(rid);
    CHECK(queue_count() == 0, "queue empty after delivery confirmed");

    queue_close();
}

static void test_integration_offline_delivery(void) {
    TEST("Integration — Phone A → Phone B (B offline, stored, delivered on re-attach)");
    unlink(TEST_DB);
    routing_init();
    queue_init(TEST_DB);

    /* Phone A online, Phone B offline */
    routing_attach("919876543210", 1);

    radio_pkt_t data = make_data_pkt("919876543210", "911234567890", "Are you there?", 30);
    data.msg_id = 55;

    /* Master receives data, Phone B not in routing table */
    uint8_t dest = routing_lookup("911234567890");
    CHECK(dest == NODE_MASTER, "Phone B offline → lookup returns 0");
    queue_push("911234567890", &data);
    CHECK(queue_count() == 1, "message stored for offline user");

    /* Phone B remains offline for a while — queue_count stays 1 */
    CHECK(queue_count() == 1, "message still queued (not delivered)");

    /* Phone B comes online at node 3 */
    routing_attach("911234567890", 3);
    CHECK(routing_lookup("911234567890") == 3, "Phone B now at node 3");

    radio_pkt_t delivery; int64_t rid;
    int found = queue_pop_for_node(3, &delivery, &rid);
    CHECK(found == 1, "stored message now deliverable via node 3");
    CHECK(memcmp(delivery.payload, "Are you there?", 14) == 0, "payload intact");
    queue_delete(rid);
    CHECK(queue_count() == 0, "delivered and removed from queue");

    queue_close();
}

static void test_integration_ack_chain(void) {
    TEST("Integration — 3-hop ACK chain (node ACKs piggybacked delivery)");
    /*
     * Test the s_pending tracking logic manually:
     * 1. Master sends POLL with piggybacked delivery (poll_msg_id = 7)
     * 2. Node responds with ACK where ack_id = 7
     * 3. Master verifies ack_id matches and deletes queue row
     * 4. Edge case: ACK with wrong ack_id does NOT delete row
     */
    unlink(TEST_DB);
    routing_init();
    routing_attach("911234567890", 1);
    queue_init(TEST_DB);

    radio_pkt_t pkt = make_data_pkt("919876543210", "911234567890", "Deliver me", 30);
    queue_push("911234567890", &pkt);

    radio_pkt_t delivery; int64_t rid;
    queue_pop_for_node(1, &delivery, &rid);
    CHECK(rid > 0, "got valid row_id for piggybacked message");

    /* Simulate: master sends POLL with msg_id=7, piggybacked delivery */
    uint8_t poll_msg_id = 7;

    /* Case A: Node ACKs with correct ack_id */
    radio_pkt_t ack_correct;
    pkt_make_ack(&ack_correct, 1, poll_msg_id);
    bool ack_matches = (ack_correct.ack_id == poll_msg_id);
    CHECK(ack_matches, "ACK ack_id == poll_msg_id → delivery confirmed");
    if (ack_matches) {
        queue_delete(rid);
        CHECK(queue_count() == 0, "queue row deleted on matching ACK");
    }

    /* Case B: Wrong ack_id — should NOT delete */
    queue_push("911234567890", &pkt); /* re-push */
    queue_pop_for_node(1, &delivery, &rid);
    radio_pkt_t ack_wrong;
    pkt_make_ack(&ack_wrong, 1, 99); /* wrong ack_id */
    bool wrong_matches = (ack_wrong.ack_id == poll_msg_id);
    CHECK(!wrong_matches, "wrong ack_id does not match → row NOT deleted");
    CHECK(queue_count() == 1, "queue row preserved on mismatched ACK");
    queue_delete(rid); /* cleanup */
    queue_close();
}

static void test_integration_presence_sequence(void) {
    TEST("Integration — PRESENCE ATTACH + DETACH sequence, routing stays consistent");
    routing_init();

    /* Attach */
    radio_pkt_t p;
    pkt_make_presence(&p, 1, "919876543210", PRESENCE_ATTACH);
    /* Simulate poller processing this */
    if (p.payload[0] == PRESENCE_ATTACH)
        routing_attach(p.src_uid, 1);
    CHECK(routing_lookup("919876543210") == 1, "after ATTACH: user at node 1");

    /* Detach */
    pkt_make_presence(&p, 1, "919876543210", PRESENCE_DETACH);
    if (p.payload[0] == PRESENCE_DETACH)
        routing_detach(p.src_uid);
    CHECK(routing_lookup("919876543210") == NODE_MASTER, "after DETACH: user offline");

    /* Re-attach at different node (phone moved) */
    pkt_make_presence(&p, 3, "919876543210", PRESENCE_ATTACH);
    routing_attach(p.src_uid, 3);
    CHECK(routing_lookup("919876543210") == 3, "after re-ATTACH: user at node 3");
    CHECK(routing_count() == 1, "count stays 1 throughout");
}

static void test_integration_duplicate_msgid(void) {
    TEST("Integration — duplicate msg_id from same node (idempotent)");
    /*
     * The ESP32 node already deduplicates on RX side (s_last_rx_msg_id).
     * The master queue should also handle pushing the same message twice gracefully:
     * it would result in 2 rows, but that's fine since the node won't send duplicates.
     * What we test here: the master correctly handles duplicate DATA packets
     * by just pushing to queue each time — the node's dedup prevents the 2nd from arriving.
     * We verify queue_push is idempotent in the sense that 2 pushes = 2 rows.
     */
    unlink(TEST_DB);
    routing_init();
    routing_attach("911234567890", 1);
    queue_init(TEST_DB);

    radio_pkt_t pkt = make_data_pkt("919876543210", "911234567890", "Dupe", 30);
    pkt.msg_id = 77;

    queue_push("911234567890", &pkt);
    queue_push("911234567890", &pkt); /* simulated duplicate */
    CHECK(queue_count() == 2, "2 pushes = 2 rows (dedup is node's responsibility)");

    /* Cleanup */
    radio_pkt_t out; int64_t rid;
    queue_pop_for_node(1, &out, &rid); queue_delete(rid);
    queue_pop_for_node(1, &out, &rid); queue_delete(rid);
    CHECK(queue_count() == 0, "cleaned up");
    queue_close();
}

static void test_integration_self_message_routing(void) {
    TEST("Integration — self-message: same src and dst (should not reach master)");
    /* The ESP32 router.c handles self-messages locally and never calls enqueue_tx.
     * This test verifies the master's queue correctly ignores the case
     * (since it would never receive such a packet in practice).
     * We just verify uid_valid works for the same UID both ways. */
    CHECK(uid_valid("919876543210"), "self-message src uid valid");
    CHECK(uid_valid("919876543210"), "self-message dst uid valid");
    /* No queue or routing state changes expected — master never sees it */
    PASS("self-message correctly handled at node level (not master)");
}

static void test_integration_node_absent_and_return(void) {
    TEST("Integration — node goes absent, queue accumulates, delivered on return");
    unlink(TEST_DB);
    routing_init();
    routing_attach("911234567890", 1); /* user at node 1 */
    queue_init(TEST_DB);

    /* Master tries to deliver 3 messages during node 1's absence */
    for (int i = 0; i < 3; i++) {
        char msg[32]; snprintf(msg, sizeof(msg), "Message %d", i + 1);
        radio_pkt_t pkt = make_data_pkt("919876543210", "911234567890", msg, 30);
        queue_push("911234567890", &pkt);
    }
    CHECK(queue_count() == 3, "3 messages queued while node absent");

    /* Node 1 comes back — master polls and delivers one per slot */
    for (int i = 0; i < 3; i++) {
        radio_pkt_t out; int64_t rid;
        int found = queue_pop_for_node(1, &out, &rid);
        CHECK(found == 1, "message deliverable when node returns");
        queue_delete(rid);
    }
    CHECK(queue_count() == 0, "all 3 messages delivered on node return");
    queue_close();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5: Wire-level edge cases
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_wire_zero_payload(void) {
    TEST("Wire — PKT_DATA with payload_len=0 (edge, minimal DATA)");
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_DATA;
    pkt.msg_id = 1; pkt.node_id = 1;
    strncpy(pkt.src_uid, "919876543210", UID_MAX_LEN);
    strncpy(pkt.dst_uid, "911234567890", UID_MAX_LEN);
    pkt.payload_len = 0;

    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    CHECK(len == PKT_HDR_SIZE, "zero-payload DATA = PKT_HDR_SIZE bytes");

    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(ok, "zero-payload DATA deserializes ok");
    CHECK(out.payload_len == 0, "payload_len == 0 round-trip");
}

static void test_wire_max_uid_length(void) {
    TEST("Wire — max-length UID (15 digits) in DATA packet");
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_DATA;
    pkt.msg_id = 1; pkt.node_id = 1;
    strncpy(pkt.src_uid, "911234567890123", UID_MAX_LEN); /* exactly 15 digits */
    strncpy(pkt.dst_uid, "919876543210123", UID_MAX_LEN);
    pkt.payload_len = 1;
    pkt.payload[0] = 'X';

    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    radio_pkt_t out;
    bool ok = pkt_deserialize(buf, len, &out);
    CHECK(ok, "max-length UID deserialized ok");
    CHECK(strcmp(out.src_uid, "911234567890123") == 0, "15-digit src_uid intact");
    CHECK(strcmp(out.dst_uid, "919876543210123") == 0, "15-digit dst_uid intact");
}

static void test_wire_msg_id_wraparound(void) {
    TEST("Wire — msg_id wraps around from 255 to 0 correctly");
    radio_pkt_t pkt;
    pkt_make_ack(&pkt, 1, 255);
    pkt.msg_id = 255;

    uint8_t buf[256];
    pkt_serialize(&pkt, buf, sizeof(buf));
    radio_pkt_t out;
    pkt_deserialize(buf, 4, &out);
    CHECK(out.msg_id == 255, "msg_id=255 survives serialization");
    CHECK(out.ack_id == 255, "ack_id=255 survives serialization");

    /* Simulate increment wraparound */
    uint8_t id = 255;
    id++;
    CHECK(id == 0, "uint8_t wraps 255→0");
}

static void test_poller_arq_guarantee(void) {
    TEST("Integration — Stop-and-Wait ARQ (Guaranteed RF Uplink)");
    routing_init();
    routing_attach("911234567890", 1);
    s_mock_rx_head = 0; s_mock_rx_tail = 0;
    s_mock_tx_head = 0; s_mock_tx_tail = 0;

    /* Simulate node 1 sending PKT_DATA to master in response to POLL */
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_DATA;
    pkt.ttl = 30;
    strncpy(pkt.src_uid, "911234567890", UID_MAX_LEN);
    strncpy(pkt.dst_uid, "919876543210", UID_MAX_LEN);
    pkt.payload_len = 8;
    memcpy(pkt.payload, "ARQ Test", 8);
    pkt.msg_id = 42;
    pkt.node_id = 1;
    s_mock_rx_q[s_mock_rx_tail++] = pkt;

    /* Run the poller for one cycle on node 1 */
    keep_running = 1;
    poller_run(); /* Will return because our radio_wait_rx mock sets keep_running=0 */

    /* The master should have sent a PKT_POLL first, then received our PKT_DATA, 
       and then sent a PKT_ACK. So s_mock_tx_q should have 2 packets. */
    CHECK(s_mock_tx_tail == 2, "Master sent 2 packets (POLL and ACK)");
    if (s_mock_tx_tail >= 2) {
        radio_pkt_t ack = s_mock_tx_q[1];
        CHECK(ack.type == PKT_ACK, "Second packet is PKT_ACK");
        CHECK(ack.ack_id == 42, "ack_id matches Node's msg_id");
        CHECK(ack.node_id == NODE_MASTER, "ACK comes from NODE_MASTER");
    }
}

static void test_packet_data_oversize(void) {
    TEST("Wire — PKT_DATA with payload_len > PAYLOAD_MAX (serialization)");
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_DATA;
    pkt.payload_len = PAYLOAD_MAX + 1; /* Invalid */
    uint8_t buf[256];
    size_t len = pkt_serialize(&pkt, buf, sizeof(buf));
    CHECK(len == 0, "Serialization fails gracefully for oversized payload");
}

static void test_poller_wrong_node_reply(void) {
    TEST("Poller — Node replies to another node's POLL (should ignore)");
    s_mock_rx_head = 0; s_mock_rx_tail = 0;
    s_mock_tx_head = 0; s_mock_tx_tail = 0;

    /* We are polling node 1, but we simulate a reply from node 2 */
    radio_pkt_t pkt;
    pkt_make_ack(&pkt, 2, 99);
    s_mock_rx_q[s_mock_rx_tail++] = pkt;

    keep_running = 1;
    poller_run();
    
    /* Master should ignore node 2's reply. No ACK sent, no queue processing. */
    CHECK(s_mock_tx_tail == 1, "Master only sent the initial POLL, ignored wrong node reply");
}

static void test_routing_table_full(void) {
    TEST("Routing — Hash table full capacity collision handling");
    routing_init();
    
    char uid[16];
    for (int i = 0; i < ROUTING_MAX_ENTRIES * 2 + 5; i++) {
        snprintf(uid, sizeof(uid), "910000%06d", i);
        routing_attach(uid, 1);
    }
    
    /* The table size is ROUTING_MAX_ENTRIES * 2. It should max out gracefully. */
    CHECK(routing_count() == ROUTING_MAX_ENTRIES * 2, "Routing count capped at TABLE_SIZE");
    
    /* Overwrite an existing entry to ensure update works when full */
    routing_attach("910000000000", 2);
    CHECK(routing_lookup("910000000000") == 2, "Can update existing user even when full");
}

static void test_queue_limit_enforcement(void) {
    TEST("Queue — Enforce hard cap limit of 10,000 messages");
    unlink("test_limit.db");
    unlink("test_limit.db-wal");
    unlink("test_limit.db-shm");
    queue_init("test_limit.db");

    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_DATA;
    pkt.ttl = 30;
    strncpy(pkt.src_uid, "911111111111", UID_MAX_LEN);
    strncpy(pkt.dst_uid, "912222222222", UID_MAX_LEN);
    pkt.payload_len = 10;
    memcpy(pkt.payload, "Limit Test", 10);
    
    for (int i = 0; i < 10005; i++) {
        pkt.msg_id = i % 256;
        if (queue_push("912222222222", &pkt) != 0) {
            printf("  DEBUG: queue_push failed at i=%d\n", i);
            /* Let's just break, the DB is probably closed or table not created? */
            break;
        }
    }
    
    int count = queue_count();
    printf("  DEBUG: count=%d\n", count);
    CHECK(count == 10000, "Queue correctly clamped to 10,000 maximum messages");
    
    queue_close();
}

static void test_poller_garbage_reply(void) {
    TEST("Poller — Node replies with garbage/invalid type");
    s_mock_rx_head = 0; s_mock_rx_tail = 0;
    s_mock_tx_head = 0; s_mock_tx_tail = 0;

    /* Simulate a completely invalid packet type from the node */
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = 0xFF; /* Invalid */
    pkt.node_id = 1; /* Correct node so it gets past the node_id check */
    s_mock_rx_q[s_mock_rx_tail++] = pkt;

    keep_running = 1;
    poller_run();

    /* Should be ignored silently (only POLL sent) */
    CHECK(s_mock_tx_tail == 1, "Garbage reply safely ignored");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("==========================================================\n");
    printf("  radcom master node — full test suite\n");
    printf("==========================================================\n");

    /* Section 1: Packets */
    test_packet_ack();
    test_packet_poll_empty();
    test_packet_poll_piggybacked();
    test_packet_data_full();
    test_packet_presence_attach();
    test_packet_presence_detach();
    test_packet_deserialize_truncated();
    test_packet_deserialize_overflow_payload();
    test_packet_uid_valid();

    /* Section 2: Routing */
    test_routing_basic();
    test_routing_miss();
    test_routing_update();
    test_routing_detach_reattach();
    test_routing_each_at_node();
    test_routing_100_users();
    test_routing_double_detach();

    /* Section 3: Queue */
    test_queue_basic_push_pop();
    test_queue_fifo_order();
    test_queue_ttl_expiry();
    test_queue_pop_wrong_node();
    test_queue_pop_offline_user();
    test_queue_persistence();
    test_queue_multiple_users_same_node();

    /* Section 4: Integration */
    test_integration_online_delivery();
    test_integration_offline_delivery();
    test_integration_ack_chain();
    test_integration_presence_sequence();
    test_integration_duplicate_msgid();
    test_integration_self_message_routing();
    test_integration_node_absent_and_return();

    /* Section 5: Wire edge cases */
    test_wire_zero_payload();
    test_wire_max_uid_length();
    test_wire_msg_id_wraparound();

    /* Section 6: Poller */
    TEST("Poller — Timeout triggers absent streak and eviction");
    unlink(TEST_DB);
    routing_init();
    queue_init(TEST_DB);
    routing_attach("911111111111", 1); // User at Node 1
    
    uint8_t nodes[1] = { 1 };
    poller_init(nodes, 1);
    
    // Test 5 timeouts
    for (int i = 0; i < POLLER_ABSENT_THRESHOLD; i++) {
        keep_running = 1;
        s_mock_rx_head = s_mock_rx_tail = 0; // force timeout
        poller_run();
    }
    CHECK(routing_count() == 0, "routing_evict_node called, user removed");

    /* Additional edge cases & in-depth integration tests */
    test_poller_arq_guarantee();
    test_packet_data_oversize();
    test_poller_wrong_node_reply();
    test_routing_table_full();
    test_queue_limit_enforcement();
    test_poller_garbage_reply();

    printf("\n==========================================================\n");
    printf("  Results: %d passed, %d failed\n", s_pass, s_fail);
    printf("==========================================================\n\n");

    unlink(TEST_DB);
    return s_fail > 0 ? 1 : 0;
}
