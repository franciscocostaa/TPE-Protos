/**
 * config.c - implementación mínima de la config global (ver config.h).
 *
 * STUB del commit de bases: estructura y getters/setters listos para que el
 * data-path y mgmt los consuman. Dueño: Persona C.
 */
#include <string.h>

#include "config.h"

static struct config current;

void
config_init(const struct config *initial) {
    if (initial != NULL) {
        current = *initial;
    } else {
        memset(&current, 0, sizeof(current));
    }
}

struct config
config_get(void) {
    return current;
}

void
config_set_auth_required(bool v) {
    current.auth_required = v;
}
