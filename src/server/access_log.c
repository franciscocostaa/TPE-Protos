/**
 * access_log.c - registro de accesos (ver access_log.h).
 *
 * Implementación del commit de bases: una línea estructurada por acceso a
 * stdout. Formato (separado por tabs):
 *
 *   <timestamp ISO-8601>  <username>  <client_addr>  <dest>:<port>  REP=<rep>
 *
 * A futuro se puede redirigir a un archivo configurable. Dueño: Persona C.
 */
#include <stdio.h>
#include <time.h>

#include "access_log.h"

void
access_log_record(const char *username,
                  const char *client_addr,
                  const char *dest, uint16_t dest_port,
                  uint8_t rep) {
    char ts[sizeof("2026-01-01T00:00:00Z")];
    const time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    printf("%s\t%s\t%s\t%s:%u\tREP=0x%02X\n",
           ts,
           username    != NULL ? username    : "-",
           client_addr != NULL ? client_addr : "-",
           dest        != NULL ? dest        : "-",
           dest_port,
           rep);
    fflush(stdout);
}
