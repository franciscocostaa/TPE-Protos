/**
 * users_test.c - tests del store de usuarios (users.c).
 *
 * Harness propio, SIN dependencias externas (mismo estilo que mgmt_cmd_test.c).
 * Incluye la unidad bajo prueba como fuente para ejercitar sus estáticas.
 *
 * Correr con: make test
 */
#include <stdio.h>
#include <string.h>

/* Unidad bajo prueba (incluye sus estáticas). */
#include "users.c"

static int tests_run    = 0;
static int tests_failed = 0;

#define CHECK(cond, name)                          \
    do {                                           \
        tests_run++;                               \
        if (cond) {                                \
            printf("  ok   %s\n", (name));         \
        } else {                                   \
            tests_failed++;                        \
            printf("  FAIL %s\n", (name));         \
        }                                          \
    } while (0)

int
main(void) {
    printf("== users ==\n");

    users_init();
    CHECK(users_count() == 0, "init deja el store vacio");

    /* alta básica */
    CHECK(users_add("ana", "pass1") == USERS_OK,              "alta nueva => OK");
    CHECK(users_count() == 1,                                 "count == 1 tras alta");
    CHECK(users_add("ana", "otra") == USERS_ALREADY_EXISTS,   "alta duplicada => ALREADY_EXISTS");
    CHECK(users_count() == 1,                                 "duplicado no incrementa count");

    /* validación de campos */
    CHECK(users_add("", "x")   == USERS_INVALID, "nombre vacio => INVALID");
    CHECK(users_add("bob", "") == USERS_INVALID, "pass vacio => INVALID");
    CHECK(users_add(NULL, "x") == USERS_INVALID, "nombre NULL => INVALID");

    /* autenticación */
    CHECK(users_authenticate("ana", "pass1")   == true,  "auth correcto => true");
    CHECK(users_authenticate("ana", "malo")    == false, "auth pass incorrecto => false");
    CHECK(users_authenticate("noexiste", "x")  == false, "auth usuario inexistente => false");
    CHECK(users_authenticate(NULL, NULL)       == false, "auth con NULL => false");

    /* baja */
    CHECK(users_remove("noexiste") == USERS_NOT_FOUND, "baja inexistente => NOT_FOUND");
    CHECK(users_remove("ana")      == USERS_OK,        "baja existente => OK");
    CHECK(users_count() == 0,                          "count == 0 tras baja");
    CHECK(users_authenticate("ana", "pass1") == false, "usuario dado de baja ya no autentica");

    /* llenar hasta el máximo */
    users_init();
    char name[16];
    bool all_ok = true;
    for (int i = 0; i < USERS_MAX; i++) {
        snprintf(name, sizeof(name), "u%d", i);
        if (users_add(name, "p") != USERS_OK) {
            all_ok = false;
        }
    }
    CHECK(all_ok && users_count() == USERS_MAX, "se pueden cargar USERS_MAX usuarios");
    CHECK(users_add("unomas", "p") == USERS_FULL, "el usuario USERS_MAX+1 => FULL");

    /* listado con buffer más chico que el total: devuelve el total real */
    const char *names[3];
    const size_t total = users_list(names, 3);
    CHECK(total == USERS_MAX, "list devuelve el total real aunque max sea menor");

    /* límites de longitud (RFC 1929: hasta 255) */
    users_init();
    char maxname[USERS_NAME_MAX + 1];
    memset(maxname, 'a', USERS_NAME_MAX);
    maxname[USERS_NAME_MAX] = '\0';
    CHECK(users_add(maxname, "p") == USERS_OK, "nombre de 255 chars (limite) => OK");

    char toolong[USERS_NAME_MAX + 2];
    memset(toolong, 'a', USERS_NAME_MAX + 1);
    toolong[USERS_NAME_MAX + 1] = '\0';
    CHECK(users_add(toolong, "p") == USERS_INVALID, "nombre de 256 chars => INVALID");

    printf("\n%d tests, %d fallaron\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
