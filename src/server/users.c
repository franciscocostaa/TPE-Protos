/**
 * users.c - store de usuarios en memoria (ver users.h).
 *
 * Implementación funcional del commit de bases: arreglo estático de hasta
 * USERS_MAX entradas. Dueño: Persona C. (A futuro: persistencia opcional.)
 */
#include <string.h>

#include "users.h"

struct user_entry {
    bool in_use;
    char name[USERS_NAME_MAX + 1];
    char pass[USERS_PASS_MAX + 1];
};

static struct user_entry users[USERS_MAX];

void
users_init(void) {
    memset(users, 0, sizeof(users));
}

/**
 * Comparación en tiempo constante: recorre siempre toda la longitud del valor
 * almacenado y no corta en la primera diferencia, para no filtrar información
 * por timing. Longitudes distintas => no coincide.
 */
static bool
const_time_eq(const char *stored, const char *given) {
    const size_t ls = strlen(stored);
    const size_t lg = strlen(given);
    unsigned char diff = (unsigned char) (ls ^ lg);
    for (size_t i = 0; i < ls; i++) {
        const char gc = i < lg ? given[i] : 0;
        diff |= (unsigned char) (stored[i] ^ gc);
    }
    return diff == 0;
}

static struct user_entry *
find(const char *name) {
    for (size_t i = 0; i < USERS_MAX; i++) {
        if (users[i].in_use && strcmp(users[i].name, name) == 0) {
            return &users[i];
        }
    }
    return NULL;
}

static bool
valid_field(const char *s, size_t max) {
    if (s == NULL) {
        return false;
    }
    const size_t len = strlen(s);
    return len > 0 && len <= max;
}

bool
users_authenticate(const char *name, const char *pass) {
    if (name == NULL || pass == NULL) {
        return false;
    }
    const struct user_entry *u = find(name);
    if (u == NULL) {
        return false;
    }
    return const_time_eq(u->pass, pass);
}

users_result
users_add(const char *name, const char *pass) {
    if (!valid_field(name, USERS_NAME_MAX) || !valid_field(pass, USERS_PASS_MAX)) {
        return USERS_INVALID;
    }
    if (find(name) != NULL) {
        return USERS_ALREADY_EXISTS;
    }
    for (size_t i = 0; i < USERS_MAX; i++) {
        if (!users[i].in_use) {
            users[i].in_use = true;
            strcpy(users[i].name, name);
            strcpy(users[i].pass, pass);
            return USERS_OK;
        }
    }
    return USERS_FULL;
}

users_result
users_remove(const char *name) {
    if (name == NULL) {
        return USERS_INVALID;
    }
    struct user_entry *u = find(name);
    if (u == NULL) {
        return USERS_NOT_FOUND;
    }
    memset(u, 0, sizeof(*u));
    return USERS_OK;
}

size_t
users_list(const char **out, size_t max) {
    size_t total = 0;
    for (size_t i = 0; i < USERS_MAX; i++) {
        if (users[i].in_use) {
            if (total < max) {
                out[total] = users[i].name;
            }
            total++;
        }
    }
    return total;
}

size_t
users_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < USERS_MAX; i++) {
        if (users[i].in_use) {
            n++;
        }
    }
    return n;
}
