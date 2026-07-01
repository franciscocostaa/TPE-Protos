#ifndef ACCESS_LOG_H_2dT8mYw4qL1k
#define ACCESS_LOG_H_2dT8mYw4qL1k

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/**
 * access_log.h - registro de accesos (consigna F8).
 *
 * Se registra una entrada cuando una conexión SOCKS queda establecida (o cuando
 * el request falla). Pensado para responder "¿quién se conectó a tal sitio y
 * cuándo?" ante una queja externa.
 *
 * Cada acceso se imprime a stdout Y se guarda en un buffer circular en memoria
 * con las últimas ACCESS_LOG_CAPACITY entradas, para que el protocolo de
 * monitoreo (mgmt, GET-LOG) las pueda consultar. Volátil: se pierde al reiniciar.
 *
 * CONTRATO COMPARTIDO — ver docs/PLAN.md §3.3 y docs/PROTOCOL.md §5.8.
 */

/** capacidad del buffer circular: se conservan las últimas N entradas */
#define ACCESS_LOG_CAPACITY  1024
/** cotas de los campos de texto (sin contar el '\0' final) */
#define ACCESS_LOG_USER_MAX  255   /* ULEN de RFC 1929           */
#define ACCESS_LOG_ADDR_MAX  63    /* "ipv6:puerto" entra holgado */
#define ACCESS_LOG_DEST_MAX  255   /* FQDN máximo                 */

/**
 * Una entrada del registro. Es lo que consume `GET-LOG` de mgmt para armar su
 * respuesta. Los strings guardan su propia copia (no punteros prestados).
 */
struct access_log_entry {
    time_t   time;                               /* instante del acceso (UTC)     */
    char     username[ACCESS_LOG_USER_MAX + 1];  /* "" si la conexión fue NO-AUTH */
    char     client_addr[ACCESS_LOG_ADDR_MAX + 1];
    char     dest[ACCESS_LOG_DEST_MAX + 1];      /* FQDN o IP literal pedida      */
    uint16_t dest_port;
    uint8_t  rep;                                /* código REP de SOCKS           */
};

/** Reinicia el registro (vacío). Llamar una vez al arrancar. */
void access_log_init(void);

/**
 * Registra un acceso: lo imprime a stdout y lo agrega al buffer en memoria.
 *
 * @param username    usuario autenticado, o NULL si la conexión es NO-AUTH.
 * @param client_addr dirección del cliente en formato humano (ip:puerto).
 * @param dest        FQDN o IP literal que pidió el cliente.
 * @param dest_port   puerto destino (host byte order).
 * @param rep         código REP de SOCKS (0x00 = éxito).
 */
void access_log_record(const char *username,
                       const char *client_addr,
                       const char *dest, uint16_t dest_port,
                       uint8_t rep);

/** Cantidad de entradas disponibles ahora mismo (0..ACCESS_LOG_CAPACITY). */
size_t access_log_count(void);

/**
 * Devuelve la entrada lógica `index` (0 = la más antigua guardada), o NULL si
 * `index >= access_log_count()`. El puntero apunta a memoria interna y es válido
 * hasta el próximo access_log_record(). Pensado para que mgmt itere GET-LOG.
 */
const struct access_log_entry *access_log_get(size_t index);

#endif
