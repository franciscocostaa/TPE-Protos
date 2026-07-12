/**
 * config.c - implementación de la config global (ver config.h).
 *
 * Estructura y getters/setters que consumen el data-path y el mgmt.
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
