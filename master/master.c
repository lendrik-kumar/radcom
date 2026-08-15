#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/stat.h>
#include "radio.h"
#include "routing.h"
#include "queue.h"
#include "poller.h"
#include "packet.h"

volatile int keep_running = 1;

static void sig_handler(int sig) { (void)sig; keep_running = 0; }

int main(void) {
    openlog("radcom-master", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_NOTICE, "radcom master starting");

    struct sigaction sa;
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* Important: NO SA_RESTART, we want poll() to return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGHUP, SIG_IGN); /* ignore hangup from terminal */

    /* Radio init — retry 3x with 1s delay */
    int ok = 0;
    for (int i = 0; i < 3 && !ok; i++) {
        if (radio_init() == 0) { ok = 1; break; }
        syslog(LOG_WARNING, "radio_init failed (attempt %d/3)", i + 1);
        sleep(1);
    }
    if (!ok) {
        syslog(LOG_CRIT, "fatal: SX1278 init failed after 3 attempts — exiting");
        closelog();
        return 1;
    }

    routing_init();

    /* Ensure DB directory exists */
    mkdir("/var/lib/radcom", 0700);

    const char *db_path = "/var/lib/radcom/queue.db";
    if (queue_init(db_path) < 0) {
        syslog(LOG_CRIT, "fatal: queue_init(%s) failed", db_path);
        closelog();
        return 1;
    }
    syslog(LOG_NOTICE, "queue open: %d messages pending", queue_count());

    /* ── Node list ──
     * Add node IDs as you deploy more hardware.
     * node_count=1 for initial bench test with Node 1 only. */
    uint8_t nodes[] = { 1, 2, 3, 4, 5 };
    int node_count = 1;

    poller_init(nodes, node_count);
    radio_start_rx();
    poller_run(); /* blocks until SIGINT/SIGTERM */

    syslog(LOG_NOTICE, "shutting down");
    queue_close();
    closelog();
    return 0;
}
