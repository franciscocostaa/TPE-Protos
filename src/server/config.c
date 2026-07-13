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
