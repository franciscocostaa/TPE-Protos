/**
 * metrics.c - contadores volátiles (ver metrics.h).
 *
 * Como todo corre en un único hilo no hacen falta atómicos.
 */
#include "metrics.h"

static struct metrics_snapshot m;

void
metrics_init(void) {
    m.connections_total   = 0;
    m.connections_current = 0;
    m.bytes_transferred   = 0;
}

void
metrics_connection_open(void) {
    m.connections_total++;
    m.connections_current++;
}

void
metrics_connection_close(void) {
    if (m.connections_current > 0) {
        m.connections_current--;
    }
}

void
metrics_add_bytes(uint64_t n) {
    m.bytes_transferred += n;
}

struct metrics_snapshot
metrics_get(void) {
    return m;
}
