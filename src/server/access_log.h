#ifndef ACCESS_LOG_H_2dT8mYw4qL1k
#define ACCESS_LOG_H_2dT8mYw4qL1k

#include <stdint.h>

/**
 * access_log.h - registro de accesos (consigna F8).
 *
 * Se registra una entrada cuando una conexión SOCKS queda establecida (o cuando
 * el request falla). Pensado para responder "¿quién se conectó a tal sitio y
 * cuándo?" ante una queja externa.
 *
 * CONTRATO COMPARTIDO — ver docs/PLAN.md §3.3.
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

#endif
