/**
 * access_log.c - registro de accesos (ver access_log.h).
 *
 * Cada acceso se imprime a stdout (auditoría en vivo) y se guarda en un buffer
 * circular en memoria con las últimas ACCESS_LOG_CAPACITY entradas, para que el
 * protocolo de monitoreo (GET-LOG) las pueda leer. Formato del stdout (tabs):
 *
 *   <timestamp ISO-8601>  <username>  <client_addr>  <dest>:<port>  REP=<rep>
 *
 * Todo corre en un único hilo (el mismo selector atiende el proxy y el mgmt),
 * así que el buffer no necesita locks. Dueño: Persona C.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "access_log.h"

/* Buffer circular: `entries_next` es dónde se escribe la próxima entrada;
 * `entries_size` cuántas hay guardadas (crece hasta ACCESS_LOG_CAPACITY y ahí
 * se estabiliza, pisando siempre la más vieja). Volátil por especificación. */
static struct access_log_entry entries[ACCESS_LOG_CAPACITY];
static size_t                  entries_size = 0;
static size_t                  entries_next = 0;

void
access_log_init(void) {
    entries_size = 0;
    entries_next = 0;
}

/* Copia segura de `src` a `dst` (buffer de `dstsize`) garantizando el '\0'
 * final. `src == NULL` => cadena vacía. */
static void
copy_field(char *dst, size_t dstsize, const char *src) {
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstsize - 1);
    dst[dstsize - 1] = '\0';
}

void
access_log_record(const char *username,
                  const char *client_addr,
                  const char *dest, uint16_t dest_port,
                  uint8_t rep) {
    /* 1. Guardamos la entrada en el slot actual del buffer circular. */
    struct access_log_entry *e = &entries[entries_next];
    e->time = time(NULL);
    copy_field(e->username,    sizeof(e->username),    username);
    copy_field(e->client_addr, sizeof(e->client_addr), client_addr);
    copy_field(e->dest,        sizeof(e->dest),        dest);
    e->dest_port = dest_port;
    e->rep       = rep;

    /* Avanzamos el puntero de escritura (circular) y crecemos hasta el tope. */
    entries_next = (entries_next + 1) % ACCESS_LOG_CAPACITY;
    if (entries_size < ACCESS_LOG_CAPACITY) {
        entries_size++;
    }

    /* 2. Además lo imprimimos a stdout, como antes (misma línea tabular). */
    char ts[sizeof("2026-01-01T00:00:00Z")];
    struct tm tm_utc;
    gmtime_r(&e->time, &tm_utc);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    printf("%s\t%s\t%s\t%s:%u\tREP=0x%02X\n",
           ts,
           e->username[0]    != '\0' ? e->username    : "-",
           e->client_addr[0] != '\0' ? e->client_addr : "-",
           e->dest[0]        != '\0' ? e->dest         : "-",
           e->dest_port,
           e->rep);
    fflush(stdout);
}

size_t
access_log_count(void) {
    return entries_size;
}

const struct access_log_entry *
access_log_get(size_t index) {
    if (index >= entries_size) {
        return NULL;
    }
    /* La entrada lógica 0 es la más antigua. Con el buffer lleno, la más antigua
     * está en `entries_next` (el slot que se pisaría). Fórmula sin underflow: */
    const size_t phys = (entries_next + ACCESS_LOG_CAPACITY - entries_size + index)
                        % ACCESS_LOG_CAPACITY;
    return &entries[phys];
}
