#include "queue.h"
#include "routing.h"
#include <sqlite3.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static sqlite3 *s_db = NULL;

int queue_init(const char *db_path) {
    if (sqlite3_open(db_path, &s_db) != SQLITE_OK) {
        syslog(LOG_ERR, "queue: sqlite3_open(%s): %s", db_path, sqlite3_errmsg(s_db));
        return -1;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS queue("
        " id          INTEGER PRIMARY KEY,"
        " dst_uid     TEXT NOT NULL,"
        " src_uid     TEXT NOT NULL,"
        " msg_id      INTEGER NOT NULL,"
        " ttl         INTEGER NOT NULL,"
        " payload     BLOB NOT NULL,"
        " payload_len INTEGER NOT NULL,"
        " ttl_expires INTEGER NOT NULL,"
        " created_at  INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS idx_dst ON queue(dst_uid);";
    char *err = NULL;
    if (sqlite3_exec(s_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        syslog(LOG_ERR, "queue: schema: %s", err); sqlite3_free(err); return -1;
    }
    sqlite3_exec(s_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    syslog(LOG_NOTICE, "queue: opened %s", db_path);
    return 0;
}

int queue_push(const char *dst_uid, const radio_pkt_t *pkt) {
    if (!s_db) return -1;
    int ttl_minutes = pkt->ttl ? pkt->ttl : 60;
    int64_t now = (int64_t)time(NULL);
    sqlite3_stmt *st;
    const char *sql =
        "INSERT INTO queue(dst_uid,src_uid,msg_id,ttl,payload,payload_len,ttl_expires,created_at)"
        " VALUES(?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text (st, 1, dst_uid,             -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 2, pkt->src_uid,        -1, SQLITE_STATIC);
    sqlite3_bind_int  (st, 3, pkt->msg_id);
    sqlite3_bind_int  (st, 4, ttl_minutes);
    sqlite3_bind_blob (st, 5, pkt->payload,        pkt->payload_len, SQLITE_STATIC);
    sqlite3_bind_int  (st, 6, pkt->payload_len);
    sqlite3_bind_int64(st, 7, now + (int64_t)ttl_minutes * 60);
    sqlite3_bind_int64(st, 8, now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        syslog(LOG_ERR, "queue_push: %s", sqlite3_errmsg(s_db)); return -1;
    }
    syslog(LOG_INFO, "queue: stored for %s (ttl=%d min)", dst_uid, ttl_minutes);
    
    queue_enforce_limit();
    
    return 0;
}

/* Used by queue_pop_for_node to iterate UIDs at a node */
typedef struct { const char *uid; radio_pkt_t *out; int64_t *row_id; int found; } pop_ctx_t;

static void _try_pop(const char *uid, void *ctx_) {
    pop_ctx_t *ctx = (pop_ctx_t *)ctx_;
    if (ctx->found || !s_db) return;
    int64_t now = (int64_t)time(NULL);
    sqlite3_stmt *st;
    const char *sql =
        "SELECT id,src_uid,msg_id,ttl,payload,payload_len FROM queue"
        " WHERE dst_uid=? AND ttl_expires>? ORDER BY id ASC LIMIT 1";
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text (st, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now);
    if (sqlite3_step(st) == SQLITE_ROW) {
        radio_pkt_t *p = ctx->out;
        memset(p, 0, sizeof(*p));
        p->type = PKT_DATA;
        p->msg_id = (uint8_t)sqlite3_column_int(st, 2);
        p->ttl    = (uint8_t)sqlite3_column_int(st, 3);
        strncpy(p->src_uid, (const char *)sqlite3_column_text(st, 1), UID_MAX_LEN);
        strncpy(p->dst_uid, uid, UID_MAX_LEN);
        int plen = sqlite3_column_int(st, 5);
        if (plen > PAYLOAD_MAX) plen = PAYLOAD_MAX;
        memcpy(p->payload, sqlite3_column_blob(st, 4), plen);
        p->payload_len = (uint8_t)plen;
        *ctx->row_id = sqlite3_column_int64(st, 0); /* proper int64, not stuffed in pkt */
        ctx->found = 1;
    }
    sqlite3_finalize(st);
}

int queue_pop_for_node(uint8_t node_id, radio_pkt_t *out, int64_t *row_id) {
    pop_ctx_t ctx = { NULL, out, row_id, 0 };
    routing_each_at_node(node_id, _try_pop, &ctx);
    return ctx.found;
}

void queue_delete(int64_t row_id) {
    if (!s_db) return;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, "DELETE FROM queue WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, row_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        syslog(LOG_DEBUG, "queue: deleted row %lld", (long long)row_id);
    }
}

void queue_expire(void) {
    if (!s_db) return;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, "DELETE FROM queue WHERE ttl_expires<?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (int64_t)time(NULL));
        sqlite3_step(st);
        int n = sqlite3_changes(s_db);
        sqlite3_finalize(st);
        if (n) syslog(LOG_INFO, "queue: expired %d message(s)", n);
    }
}

int queue_count(void) {
    if (!s_db) return 0;
    sqlite3_stmt *st; int n = 0;
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM queue WHERE ttl_expires>?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (int64_t)time(NULL));
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

void queue_close(void) { if (s_db) { sqlite3_close(s_db); s_db = NULL; } }

/* Test helper: expire ALL rows immediately, regardless of ttl_expires */
void queue_expire_all(void) {
    if (!s_db) return;
    char *err = NULL;
    sqlite3_exec(s_db, "DELETE FROM queue", NULL, NULL, &err);
    if (err) { syslog(LOG_ERR, "queue_expire_all: %s", err); sqlite3_free(err); }
}

void queue_enforce_limit(void) {
    if (!s_db) return;
    /* Hard cap of 10,000 messages in the offline queue */
    int limit = 10000;
    int current = queue_count();
    if (current > limit) {
        int excess = current - limit;
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(s_db, "DELETE FROM queue WHERE id IN (SELECT id FROM queue ORDER BY id ASC LIMIT ?)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, excess);
            sqlite3_step(st);
            int deleted = sqlite3_changes(s_db);
            sqlite3_finalize(st);
            syslog(LOG_WARNING, "queue: enforced limit, deleted %d oldest message(s)", deleted);
        }
    }
}
