#ifndef METRICS_H_7nR2vKp5xQ8w
#define METRICS_H_7nR2vKp5xQ8w

#include <stdint.h>

/**
 * metrics.h - contadores volátiles del servidor (consigna F6).
 *
 * Los actualiza el data-path (connection/relay) y los lee el protocolo de
 * monitoreo. Volátiles por especificación: se pierden al reiniciar.
 *
 * CONTRATO COMPARTIDO — ver docs/PLAN.md §3.2.
 */
struct metrics_snapshot {
    uint64_t connections_total;     /* conexiones históricas        */
    uint64_t connections_current;   /* conexiones concurrentes ahora */
    uint64_t bytes_transferred;     /* bytes en ambos sentidos del relay */
};

void metrics_init(void);

/** Lo llama connection.c al aceptar una conexión SOCKS. */
void metrics_connection_open(void);

/** Lo llama connection.c al cerrar una conexión SOCKS. */
void metrics_connection_close(void);

/** Lo llama relay.c cada vez que copia `n` bytes. */
void metrics_add_bytes(uint64_t n);

/** Snapshot atómico-suficiente (un solo hilo) para mgmt. */
struct metrics_snapshot metrics_get(void);

#endif
