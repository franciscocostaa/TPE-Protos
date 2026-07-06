#ifndef USERS_H_3pZ9kWq1nB6t
#define USERS_H_3pZ9kWq1nB6t

#include <stdbool.h>
#include <stddef.h>

/**
 * users.h - registro de usuarios del proxy (autenticación RFC 1929).
 *
 * Store en memoria, mutable en tiempo de ejecución vía el protocolo de
 * monitoreo. Lo consumen `auth` (data-path SOCKS) y `mgmt`.
 *
 * CONTRATO COMPARTIDO — ver docs/PLAN.md §3.1. Cambiar esta interfaz requiere
 * avisar al grupo.
 */

#define USERS_MAX       10 //TODO: deberia ser 500 no?
#define USERS_NAME_MAX  255   /* ULEN máximo en RFC 1929 */
#define USERS_PASS_MAX  255   /* PLEN máximo en RFC 1929 */

typedef enum {
    USERS_OK = 0,
    USERS_FULL,             /* se alcanzó USERS_MAX                 */
    USERS_ALREADY_EXISTS,
    USERS_NOT_FOUND,
    USERS_INVALID,          /* nombre/pass vacío o demasiado largo  */
} users_result;

/** Inicializa el store vacío. Llamar una vez al arrancar. */
void users_init(void);

/**
 * Valida credenciales (lo usa auth.c). Comparación en tiempo constante para
 * no filtrar información por timing. Devuelve true si (name, pass) coincide.
 */
bool users_authenticate(const char *name, const char *pass);

/** Alta en runtime (lo usa mgmt). */
users_result users_add(const char *name, const char *pass);

/** Baja en runtime (lo usa mgmt). */
users_result users_remove(const char *name);

/**
 * Copia hasta `max` punteros a nombres en `out`. Devuelve la cantidad total de
 * usuarios (puede ser mayor que `max`). Los punteros son válidos hasta el
 * próximo add/remove.
 */
size_t users_list(const char **out, size_t max);

/** Cantidad actual de usuarios registrados. */
size_t users_count(void);

#endif
