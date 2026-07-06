#ifndef SOCKS5_H_5cZ0mQk3aXr8tWv2
#define SOCKS5_H_5cZ0mQk3aXr8tWv2

#include "selector.h"

/**
 * socks5.h - interfaz pública del proxy SOCKSv5 (RFC 1928) y constantes del
 *            protocolo. La lógica vive en una máquina de estados por conexión
 *            (socks5.c), manejada de forma no bloqueante por el selector.
 */

/** tamaño de los buffers de I/O por conexión (bytes) */
#define SOCKS5_BUFFER_SIZE 4096

/** versión del protocolo */
#define SOCKS5_VERSION 0x05

/* métodos de autenticación (handshake inicial, RFC 1928 §3) */
#define SOCKS5_METHOD_NO_AUTH       0x00
#define SOCKS5_METHOD_USER_PASS     0x02
#define SOCKS5_METHOD_NO_ACCEPTABLE 0xFF

/* comandos (request, RFC 1928 §4) */
#define SOCKS5_CMD_CONNECT       0x01
#define SOCKS5_CMD_BIND          0x02
#define SOCKS5_CMD_UDP_ASSOCIATE 0x03

/* tipos de dirección (ATYP) */
#define SOCKS5_ATYP_IPV4   0x01
#define SOCKS5_ATYP_DOMAIN 0x03
#define SOCKS5_ATYP_IPV6   0x04

/* códigos de respuesta (REP, RFC 1928 §6) */
#define SOCKS5_REP_SUCCEEDED             0x00
#define SOCKS5_REP_GENERAL_FAILURE       0x01
#define SOCKS5_REP_NOT_ALLOWED           0x02
#define SOCKS5_REP_NETWORK_UNREACHABLE   0x03
#define SOCKS5_REP_HOST_UNREACHABLE      0x04
#define SOCKS5_REP_CONNECTION_REFUSED    0x05
#define SOCKS5_REP_TTL_EXPIRED           0x06
#define SOCKS5_REP_COMMAND_NOT_SUPPORTED 0x07
#define SOCKS5_REP_ATYP_NOT_SUPPORTED    0x08

/**
 * Acepta una nueva conexión entrante sobre el socket pasivo y la registra en
 * el selector con su propia máquina de estados. Pensado para usarse como
 * `handle_read` del socket pasivo.
 */
void
socksv5_passive_accept(struct selector_key *key);

/**
 * Libera el pool de estructuras reutilizables. Llamar una vez al terminar.
 */
void
socksv5_pool_destroy(void);

#endif
