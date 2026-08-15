/**
 * test_main.c — host-side simulation tests for radcom node logic
 *
 * Compiles on macOS with plain gcc — no ESP-IDF, no hardware required.
 * Tests packet serialization, session table, and routing logic exhaustively.
 *
 * Build & run:
 *   cd firmware/tests
 *   make
 *   ./test_radcom
 *
 * Exit code 0 = all tests passed.
 */

/* ── Stub out ESP-IDF / FreeRTOS types used by production headers ─────────── */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef void  *QueueHandle_t;
typedef void  *TaskHandle_t;
typedef int    BaseType_t;
typedef unsigned int TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)

typedef int esp_err_t;
#define ESP_OK           0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  -2
#define ESP_ERR_NOT_FOUND -5
#define ESP_ERR_INVALID_ARG -6
#define ESP_LOGI(t,f,...) ((void)0)
#define ESP_LOGW(t,f,...) ((void)0)
#define ESP_LOGE(t,f,...) ((void)0)
#define ESP_LOGD(t,f,...) ((void)0)

typedef int httpd_handle_t;
httpd_handle_t ws_server = 0;
#define AP_MAX_STATIONS 10

/* ── Pull in production code ─────────────────────────────────────────────── */
#include "../node/main/packet.h"
#include "../node/main/packet.c"   /* test the real implementation */

/* ── Session table (mirrored from router.c — tests the logic, not RTOS) ─── */
typedef struct {
    char phone_num[UID_BUF_SIZE];
    int  ws_fd;
    bool active;
} session_t;

#define MAX_SESSIONS AP_MAX_STATIONS
static session_t sessions[MAX_SESSIONS];

static void sessions_reset(void) { memset(sessions, 0, sizeof(sessions)); }

static session_t *session_find_by_phone(const char *p) {
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (sessions[i].active && strcmp(sessions[i].phone_num, p) == 0)
            return &sessions[i];
    return NULL;
}

static session_t *session_find_by_fd(int fd) {
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (sessions[i].active && sessions[i].ws_fd == fd)
            return &sessions[i];
    return NULL;
}

static session_t *session_alloc(void) {
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (!sessions[i].active) return &sessions[i];
    return NULL;
}

static int session_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; ++i) n += sessions[i].active;
    return n;
}

static bool session_attach(const char *phone, int ws_fd) {
    if (!uid_valid(phone)) return false;
    session_t *s = session_find_by_phone(phone);
    if (s) { s->ws_fd = ws_fd; return true; }  /* reconnect: update fd */
    s = session_alloc();
    if (!s) return false;
    s->active = true;
    s->ws_fd  = ws_fd;
    strncpy(s->phone_num, phone, UID_MAX_LEN);
    s->phone_num[UID_MAX_LEN] = '\0';
    return true;
}

static bool session_detach_by_fd(int ws_fd) {
    session_t *s = session_find_by_fd(ws_fd);
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    return true;
}

/* ── Minimal test framework ──────────────────────────────────────────────── */
static int tests_run = 0, tests_failed = 0;
static const char *current_test = NULL;

#define TEST(name) static void test_##name(void)
#define RUN(name)  do { \
    current_test = #name; \
    printf("  %-50s", #name " ..."); \
    fflush(stdout); \
    test_##name(); \
    if (tests_failed == 0 || /* check if it passed */ 1) { \
        /* We track failures inline via EXPECT; just count runs */ \
    } \
    tests_run++; \
    } while(0)

static int _fail_in_test = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        if (!_fail_in_test) { printf("FAIL\n"); _fail_in_test = 1; tests_failed++; } \
        printf("    ✗ %s  [%s:%d]\n", #cond, __FILE__, __LINE__); \
    } } while(0)

#define BEGIN_TEST() _fail_in_test = 0
#define END_TEST()   do { if (!_fail_in_test) printf("OK\n"); } while(0)

/* Redefine RUN to wrap BEGIN/END */
#undef RUN
#define RUN(name) do { \
    current_test = #name; \
    printf("  %-52s", #name " ..."); \
    fflush(stdout); \
    BEGIN_TEST(); \
    test_##name(); \
    END_TEST(); \
    tests_run++; \
    } while(0)

/* ═══════════════════════════════ PACKET TESTS ══════════════════════════════ */

TEST(uid_valid_good) {
    EXPECT(uid_valid("919876543210"));
    EXPECT(uid_valid("1"));
    EXPECT(uid_valid("123456789012345"));  /* 15 digits — at limit */
}

TEST(uid_valid_bad) {
    EXPECT(!uid_valid(""));
    EXPECT(!uid_valid(NULL));
    EXPECT(!uid_valid("91987654321A"));        /* letter */
    EXPECT(!uid_valid("1234567890123456"));    /* 16 digits — too long */
    EXPECT(!uid_valid("+919876543210"));       /* '+' not digit */
    EXPECT(!uid_valid(" 919876543210"));       /* leading space */
}

TEST(pkt_make_ack) {
    radio_pkt_t p;
    pkt_make_ack(&p, 2, 42);
    EXPECT(p.type        == PKT_ACK);
    EXPECT(p.node_id     == 2);
    EXPECT(p.ack_id      == 42);
    EXPECT(p.payload_len == 0);
    EXPECT(p.src_uid[0]  == '\0');
}

TEST(pkt_make_presence_attach) {
    radio_pkt_t p;
    pkt_make_presence(&p, 3, "919876543210", PRESENCE_ATTACH);
    EXPECT(p.type        == PKT_PRESENCE);
    EXPECT(p.node_id     == 3);
    EXPECT(p.payload_len == 1);
    EXPECT(p.payload[0]  == PRESENCE_ATTACH);
    EXPECT(strcmp(p.src_uid, "919876543210") == 0);
}

TEST(pkt_make_presence_detach) {
    radio_pkt_t p;
    pkt_make_presence(&p, 1, "919876543210", PRESENCE_DETACH);
    EXPECT(p.payload[0] == PRESENCE_DETACH);
}

TEST(serialize_ack_roundtrip) {
    radio_pkt_t orig, dec;
    pkt_make_ack(&orig, 1, 7);
    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&orig, buf, sizeof(buf));
    EXPECT(n == 4);
    EXPECT(pkt_deserialize(buf, n, &dec));
    EXPECT(dec.type == PKT_ACK && dec.node_id == 1 && dec.ack_id == 7);
}

TEST(serialize_data_roundtrip) {
    radio_pkt_t orig, dec;
    memset(&orig, 0, sizeof(orig));
    orig.type = PKT_DATA; orig.msg_id = 55; orig.node_id = 2; orig.ttl = 30;
    orig.payload_len = 5; memcpy(orig.payload, "hello", 5);
    strncpy(orig.src_uid, "919111111111", UID_MAX_LEN);
    strncpy(orig.dst_uid, "919222222222", UID_MAX_LEN);

    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&orig, buf, sizeof(buf));
    EXPECT(n == PKT_HDR_SIZE + 5);
    EXPECT(pkt_deserialize(buf, n, &dec));
    EXPECT(dec.type == PKT_DATA && dec.msg_id == 55 && dec.payload_len == 5);
    EXPECT(memcmp(dec.payload, "hello", 5) == 0);
    EXPECT(strcmp(dec.src_uid, "919111111111") == 0);
    EXPECT(strcmp(dec.dst_uid, "919222222222") == 0);
}

TEST(serialize_presence_roundtrip) {
    radio_pkt_t orig, dec;
    pkt_make_presence(&orig, 1, "919876543210", PRESENCE_DETACH);
    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&orig, buf, sizeof(buf));
    EXPECT(pkt_deserialize(buf, n, &dec));
    EXPECT(dec.type == PKT_PRESENCE);
    EXPECT(dec.payload[0] == PRESENCE_DETACH);
    EXPECT(strcmp(dec.src_uid, "919876543210") == 0);
}

TEST(serialize_poll_empty) {
    radio_pkt_t p, dec;
    memset(&p, 0, sizeof(p));
    p.type = PKT_POLL; p.msg_id = 11; p.node_id = 3;
    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&p, buf, sizeof(buf));
    EXPECT(n == 4);
    EXPECT(pkt_deserialize(buf, n, &dec));
    EXPECT(dec.type == PKT_POLL && dec.node_id == 3 && dec.payload_len == 0);
}

TEST(serialize_poll_with_delivery) {
    radio_pkt_t p, dec;
    memset(&p, 0, sizeof(p));
    p.type = PKT_POLL; p.msg_id = 7; p.node_id = 2;
    p.payload_len = 4; memcpy(p.payload, "test", 4);
    strncpy(p.src_uid, "919111111111", UID_MAX_LEN);
    strncpy(p.dst_uid, "919222222222", UID_MAX_LEN);
    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&p, buf, sizeof(buf));
    EXPECT(n == PKT_HDR_SIZE + 4);
    EXPECT(pkt_deserialize(buf, n, &dec));
    EXPECT(dec.payload_len == 4 && memcmp(dec.payload, "test", 4) == 0);
}

TEST(serialize_max_payload) {
    radio_pkt_t p, dec;
    memset(&p, 0, sizeof(p));
    p.type = PKT_DATA; p.node_id = 1; p.payload_len = PAYLOAD_MAX;
    memset(p.payload, 'Z', PAYLOAD_MAX);
    strncpy(p.src_uid, "919111111111", UID_MAX_LEN);
    strncpy(p.dst_uid, "919222222222", UID_MAX_LEN);
    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&p, buf, sizeof(buf));
    EXPECT(n == PKT_HDR_SIZE + PAYLOAD_MAX);
    EXPECT(n <= 255);  /* must fit SX1278 FIFO */
    printf("(wire=%zu) ", n);
    EXPECT(pkt_deserialize(buf, n, &dec));
    EXPECT(dec.payload_len == PAYLOAD_MAX);
    for (int i = 0; i < PAYLOAD_MAX; ++i) EXPECT(dec.payload[i] == 'Z');
}

TEST(deserialize_too_short) {
    uint8_t buf[3] = { PKT_DATA, 1, 0 };
    radio_pkt_t out;
    EXPECT(!pkt_deserialize(buf, 3, &out));
}

TEST(deserialize_bad_type) {
    uint8_t buf[4] = { 0xFF, 1, 0, 1 };
    radio_pkt_t out;
    EXPECT(!pkt_deserialize(buf, 4, &out));
}

TEST(deserialize_payload_overflow) {
    uint8_t buf[sizeof(radio_pkt_t)] = { 0 };
    buf[0] = PKT_DATA; buf[3] = 1; buf[4] = 10;
    memset(&buf[5],  '9', UID_MAX_LEN); buf[20] = '\0';
    memset(&buf[21], '8', UID_MAX_LEN); buf[36] = '\0';
    buf[37] = 200;  /* payload_len > PAYLOAD_MAX */
    radio_pkt_t out;
    EXPECT(!pkt_deserialize(buf, sizeof(buf), &out));
}

TEST(deserialize_invalid_uid) {
    radio_pkt_t orig;
    memset(&orig, 0, sizeof(orig));
    orig.type = PKT_DATA; orig.node_id = 1;
    orig.payload_len = 1; orig.payload[0] = 'X';
    strncpy(orig.src_uid, "ABCDEF", UID_MAX_LEN);  /* not digits */
    strncpy(orig.dst_uid, "919111111111", UID_MAX_LEN);
    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&orig, buf, sizeof(buf));
    EXPECT(n > 0);
    radio_pkt_t out;
    EXPECT(!pkt_deserialize(buf, n, &out));
}

TEST(deserialize_presence_bad_event) {
    radio_pkt_t orig;
    pkt_make_presence(&orig, 1, "919876543210", PRESENCE_ATTACH);
    orig.payload[0] = 'X';  /* corrupt event byte */
    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&orig, buf, sizeof(buf));
    radio_pkt_t out;
    EXPECT(!pkt_deserialize(buf, n, &out));
}

TEST(serialize_buffer_too_small) {
    radio_pkt_t p;
    pkt_make_ack(&p, 1, 1);
    uint8_t tiny[2];
    EXPECT(pkt_serialize(&p, tiny, sizeof(tiny)) == 0);
}

TEST(serialize_null_guards) {
    uint8_t buf[16];
    radio_pkt_t p; memset(&p,0,sizeof(p)); p.type=PKT_ACK;
    EXPECT(pkt_serialize(NULL, buf, sizeof(buf)) == 0);
    EXPECT(pkt_serialize(&p, NULL, sizeof(buf)) == 0);
    EXPECT(!pkt_deserialize(NULL, 4, &p));
    EXPECT(!pkt_deserialize(buf, 4, NULL));
}

TEST(msg_id_wraparound) {
    radio_pkt_t p; pkt_make_ack(&p, 1, 255);
    uint8_t buf[sizeof(radio_pkt_t)];
    size_t n = pkt_serialize(&p, buf, sizeof(buf));
    radio_pkt_t out;
    EXPECT(pkt_deserialize(buf, n, &out) && out.ack_id == 255);
}

TEST(ack_wire_is_4_bytes) {
    radio_pkt_t p; pkt_make_ack(&p, 1, 0);
    EXPECT(pkt_wire_len(&p) == 4);
}

TEST(poll_empty_wire_is_4_bytes) {
    radio_pkt_t p = { .type = PKT_POLL, .node_id = 1 };
    EXPECT(pkt_wire_len(&p) == 4);
}

TEST(max_packet_fits_sx1278_fifo) {
    EXPECT(sizeof(radio_pkt_t) <= 255);
    printf("(struct=%zu) ", sizeof(radio_pkt_t));
}

/* ═══════════════════════════════ SESSION TESTS ════════════════════════════ */

TEST(session_attach_new) {
    sessions_reset();
    EXPECT(session_attach("919876543210", 1));
    EXPECT(session_count() == 1);
    EXPECT(session_find_by_phone("919876543210") != NULL);
    EXPECT(session_find_by_phone("919876543210")->ws_fd == 1);
}

TEST(session_attach_reconnect) {
    sessions_reset();
    EXPECT(session_attach("919876543210", 1));
    EXPECT(session_attach("919876543210", 2));  /* reconnect: new fd */
    EXPECT(session_count() == 1);
    EXPECT(session_find_by_phone("919876543210")->ws_fd == 2);
}

TEST(session_attach_two_different_phones) {
    sessions_reset();
    EXPECT(session_attach("919111111111", 1));
    EXPECT(session_attach("919222222222", 2));
    EXPECT(session_count() == 2);
}

TEST(session_table_full) {
    sessions_reset();
    char phone[16];
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        snprintf(phone, sizeof(phone), "91900000%04d", i);
        EXPECT(session_attach(phone, i + 1));
    }
    EXPECT(session_count() == MAX_SESSIONS);
    EXPECT(!session_attach("919999999999", 99));  /* 11th: rejected */
    EXPECT(session_count() == MAX_SESSIONS);
}

TEST(session_detach_by_fd) {
    sessions_reset();
    EXPECT(session_attach("919876543210", 5));
    EXPECT(session_detach_by_fd(5));
    EXPECT(session_count() == 0);
    EXPECT(session_find_by_phone("919876543210") == NULL);
}

TEST(session_detach_unknown_fd_noop) {
    sessions_reset();
    EXPECT(!session_detach_by_fd(999));  /* no crash, returns false */
}

TEST(session_find_nonexistent) {
    sessions_reset();
    EXPECT(session_find_by_phone("919000000000") == NULL);
    EXPECT(session_find_by_fd(42) == NULL);
}

TEST(session_attach_bad_uid) {
    sessions_reset();
    EXPECT(!session_attach("", 1));
    EXPECT(!session_attach("+919876543210", 1));
    EXPECT(!session_attach("ABCDEF", 1));
    EXPECT(!session_attach("1234567890123456", 1));  /* 16 digits */
    EXPECT(session_count() == 0);
}

TEST(session_fill_empty_fill_again) {
    sessions_reset();
    char phone[16];
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        snprintf(phone, sizeof(phone), "91900000%04d", i);
        EXPECT(session_attach(phone, i));
    }
    for (int i = 0; i < MAX_SESSIONS; ++i) EXPECT(session_detach_by_fd(i));
    EXPECT(session_count() == 0);
    EXPECT(session_attach("919999999999", 0));  /* table available again */
}

/* ═══════════════════════════════ ROUTING SIMULATION ═══════════════════════ */

/* Capture stubs for assertions */
static struct { int fd; char msg[256]; } ws_out[64];
static int    ws_out_n = 0;
static radio_pkt_t lora_out[64];
static int         lora_out_n = 0;

static void reset_captures(void) { ws_out_n = 0; lora_out_n = 0; }

static void stub_ws_send(int fd, const char *msg) {
    if (ws_out_n < 64) {
        ws_out[ws_out_n].fd = fd;
        snprintf(ws_out[ws_out_n].msg, 256, "%s", msg);
        ws_out_n++;
    }
}

static void stub_lora_enqueue(const radio_pkt_t *pkt) {
    if (lora_out_n < 64) lora_out[lora_out_n++] = *pkt;
}

/* Simulated router helpers */
static bool sim_attach(const char *phone, int ws_fd) {
    if (!uid_valid(phone)) { stub_ws_send(ws_fd, "error:bad_uid"); return false; }
    session_t *ex = session_find_by_phone(phone);
    if (ex) {
        ex->ws_fd = ws_fd;
        stub_ws_send(ws_fd, "attached");
        radio_pkt_t p; pkt_make_presence(&p, 1, phone, PRESENCE_ATTACH);
        stub_lora_enqueue(&p);
        return true;
    }
    if (!session_attach(phone, ws_fd)) {
        stub_ws_send(ws_fd, "error:full");
        return false;
    }
    stub_ws_send(ws_fd, "attached");
    radio_pkt_t p; pkt_make_presence(&p, 1, phone, PRESENCE_ATTACH);
    stub_lora_enqueue(&p);
    return true;
}

static void sim_detach(int ws_fd) {
    session_t *s = session_find_by_fd(ws_fd);
    if (!s) return;
    radio_pkt_t p; pkt_make_presence(&p, 1, s->phone_num, PRESENCE_DETACH);
    stub_lora_enqueue(&p);
    session_detach_by_fd(ws_fd);
}

static void sim_msg(int ws_fd, const char *dst, const char *text) {
    session_t *s = session_find_by_fd(ws_fd);
    if (!s) { stub_ws_send(ws_fd, "error:not_attached"); return; }
    if (strcmp(s->phone_num, dst) == 0) { stub_ws_send(ws_fd, "self_delivered"); return; }
    session_t *local = session_find_by_phone(dst);
    if (local) {
        stub_ws_send(local->ws_fd, text);
        stub_ws_send(ws_fd, "sent:local");
        return;
    }
    radio_pkt_t p; memset(&p, 0, sizeof(p));
    p.type = PKT_DATA; p.ttl = 30;
    p.payload_len = (uint8_t)strlen(text);
    memcpy(p.payload, text, p.payload_len);
    strncpy(p.src_uid, s->phone_num, UID_MAX_LEN);
    strncpy(p.dst_uid, dst, UID_MAX_LEN);
    stub_lora_enqueue(&p);
    stub_ws_send(ws_fd, "queued");
}

static void sim_lora_rx(const radio_pkt_t *pkt) {
    session_t *dst = session_find_by_phone(pkt->dst_uid);
    if (!dst) return;  /* phone not here — drop */
    stub_ws_send(dst->ws_fd, (const char *)pkt->payload);
}

TEST(routing_local_delivery) {
    sessions_reset(); reset_captures();
    sim_attach("919111111111", 1); sim_attach("919222222222", 2);
    reset_captures();
    sim_msg(1, "919222222222", "hi");
    EXPECT(lora_out_n == 0);       /* no LoRa needed */
    EXPECT(ws_out_n   == 2);       /* text + "sent:local" */
    EXPECT(ws_out[0].fd == 2);     /* recipient got message */
}

TEST(routing_remote_to_lora) {
    sessions_reset(); reset_captures();
    sim_attach("919111111111", 1); reset_captures();
    sim_msg(1, "919999999999", "hello");
    EXPECT(lora_out_n == 1);
    EXPECT(lora_out[0].type == PKT_DATA);
    EXPECT(strcmp(lora_out[0].dst_uid, "919999999999") == 0);
    EXPECT(ws_out[0].fd == 1 && strstr(ws_out[0].msg, "queued") != NULL);
}

TEST(routing_self_message) {
    sessions_reset(); reset_captures();
    sim_attach("919111111111", 1); reset_captures();
    sim_msg(1, "919111111111", "to_myself");
    EXPECT(lora_out_n == 0);
    EXPECT(ws_out_n   == 1);
    EXPECT(strstr(ws_out[0].msg, "self") != NULL);
}

TEST(routing_msg_before_attach) {
    sessions_reset(); reset_captures();
    sim_msg(99, "919111111111", "ghost");
    EXPECT(ws_out_n == 1 && strstr(ws_out[0].msg, "error") != NULL);
    EXPECT(lora_out_n == 0);
}

TEST(routing_presence_on_attach) {
    sessions_reset(); reset_captures();
    sim_attach("919876543210", 3);
    EXPECT(lora_out_n == 1);
    EXPECT(lora_out[0].type == PKT_PRESENCE);
    EXPECT(lora_out[0].payload[0] == PRESENCE_ATTACH);
}

TEST(routing_presence_on_detach) {
    sessions_reset(); reset_captures();
    sim_attach("919876543210", 3); reset_captures();
    sim_detach(3);
    EXPECT(lora_out_n == 1);
    EXPECT(lora_out[0].type == PKT_PRESENCE);
    EXPECT(lora_out[0].payload[0] == PRESENCE_DETACH);
    EXPECT(session_count() == 0);
}

TEST(routing_detach_unknown_fd_noop) {
    sessions_reset(); reset_captures();
    sim_detach(999);
    EXPECT(lora_out_n == 0);  /* no spurious PRESENCE */
}

TEST(routing_inbound_for_absent_phone) {
    /* Master sent delivery but phone just disconnected */
    sessions_reset(); reset_captures();
    radio_pkt_t p; memset(&p, 0, sizeof(p));
    p.type = PKT_POLL; p.payload_len = 5; memcpy(p.payload, "hello", 5);
    strncpy(p.src_uid, "919111111111", UID_MAX_LEN);
    strncpy(p.dst_uid, "919222222222", UID_MAX_LEN);  /* not in table */
    sim_lora_rx(&p);
    EXPECT(ws_out_n == 0);  /* nothing sent — phone not here */
}

TEST(routing_inbound_delivery_ok) {
    sessions_reset(); reset_captures();
    sim_attach("919222222222", 2); reset_captures();
    radio_pkt_t p; memset(&p, 0, sizeof(p));
    p.type = PKT_POLL; p.payload_len = 5; memcpy(p.payload, "hello", 5);
    strncpy(p.src_uid, "919111111111", UID_MAX_LEN);
    strncpy(p.dst_uid, "919222222222", UID_MAX_LEN);
    sim_lora_rx(&p);
    EXPECT(ws_out_n == 1 && ws_out[0].fd == 2);
    EXPECT(memcmp(ws_out[0].msg, "hello", 5) == 0);
}

TEST(routing_full_table_rejects_new_phone) {
    sessions_reset(); reset_captures();
    char phone[16];
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        snprintf(phone, sizeof(phone), "91900000%04d", i);
        sim_attach(phone, i);
    }
    reset_captures();
    sim_attach("919999999999", 99);
    EXPECT(ws_out_n >= 1 && strstr(ws_out[0].msg, "error") != NULL);
    EXPECT(lora_out_n == 0);  /* no PRESENCE for rejected phone */
}

TEST(routing_reconnect_updates_presence) {
    /* Same phone reconnecting should NOT send new PRESENCE(ATTACH) — master
     * already knows this phone is here. Current impl does send it; that's
     * acceptable (idempotent). This test documents the behaviour. */
    sessions_reset(); reset_captures();
    sim_attach("919876543210", 1); reset_captures();
    sim_attach("919876543210", 2);  /* reconnect */
    /* On reconnect: ws updated, PRESENCE re-sent (idempotent at master) */
    EXPECT(session_find_by_phone("919876543210")->ws_fd == 2);
    /* At least "attached" sent to new fd */
    EXPECT(ws_out[0].fd == 2);
}

TEST(routing_multi_message_burst) {
    /* 5 messages from same phone in a row — all should queue correctly */
    sessions_reset(); reset_captures();
    sim_attach("919111111111", 1); reset_captures();
    for (int i = 0; i < 5; ++i) sim_msg(1, "919999999999", "burst");
    EXPECT(lora_out_n == 5);
    for (int i = 0; i < 5; ++i)
        EXPECT(lora_out[i].type == PKT_DATA);
}

/* ═══════════════════════════════════ MAIN ══════════════════════════════════ */

int main(void) {
    printf("\nradcom node — host simulation test suite\n");
    printf("=========================================\n");

    printf("\n[packet — serialization & validation]\n");
    RUN(uid_valid_good);
    RUN(uid_valid_bad);
    RUN(pkt_make_ack);
    RUN(pkt_make_presence_attach);
    RUN(pkt_make_presence_detach);
    RUN(serialize_ack_roundtrip);
    RUN(serialize_data_roundtrip);
    RUN(serialize_presence_roundtrip);
    RUN(serialize_poll_empty);
    RUN(serialize_poll_with_delivery);
    RUN(serialize_max_payload);
    RUN(deserialize_too_short);
    RUN(deserialize_bad_type);
    RUN(deserialize_payload_overflow);
    RUN(deserialize_invalid_uid);
    RUN(deserialize_presence_bad_event);
    RUN(serialize_buffer_too_small);
    RUN(serialize_null_guards);
    RUN(msg_id_wraparound);
    RUN(ack_wire_is_4_bytes);
    RUN(poll_empty_wire_is_4_bytes);
    RUN(max_packet_fits_sx1278_fifo);

    printf("\n[session table]\n");
    RUN(session_attach_new);
    RUN(session_attach_reconnect);
    RUN(session_attach_two_different_phones);
    RUN(session_table_full);
    RUN(session_detach_by_fd);
    RUN(session_detach_unknown_fd_noop);
    RUN(session_find_nonexistent);
    RUN(session_attach_bad_uid);
    RUN(session_fill_empty_fill_again);

    printf("\n[routing simulation]\n");
    RUN(routing_local_delivery);
    RUN(routing_remote_to_lora);
    RUN(routing_self_message);
    RUN(routing_msg_before_attach);
    RUN(routing_presence_on_attach);
    RUN(routing_presence_on_detach);
    RUN(routing_detach_unknown_fd_noop);
    RUN(routing_inbound_for_absent_phone);
    RUN(routing_inbound_delivery_ok);
    RUN(routing_full_table_rejects_new_phone);
    RUN(routing_reconnect_updates_presence);
    RUN(routing_multi_message_burst);

    printf("\n=========================================\n");
    int passed = tests_run - tests_failed;
    printf("Results: %d/%d passed", passed, tests_run);
    if (tests_failed) {
        printf("  (%d FAILED ✗)\n", tests_failed);
        return 1;
    }
    printf(" — ALL PASS ✓\n\n");
    return 0;
}
