/**
 * config_test.c - tests de la configuración mutable en runtime (config.c).
 *
 * Harness propio, SIN dependencias externas. Incluye la unidad bajo prueba.
 * Correr con: make test
 */
#include <stdio.h>

/* Unidad bajo prueba. */
#include "config.c"

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
    printf("== config ==\n");

    const struct config initial = {
        .auth_required = true, .dissectors_enabled = false, .io_buffer_size = 4096,
    };
    config_init(&initial);
    struct config c = config_get();
    CHECK(c.auth_required == true && c.dissectors_enabled == false && c.io_buffer_size == 4096,
          "init refleja los valores iniciales");

    /* setters puntuales */
    config_set_auth_required(false);
    CHECK(config_get().auth_required == false, "set_auth_required cambia el valor");

    config_set_dissectors(true);
    CHECK(config_get().dissectors_enabled == true, "set_dissectors cambia el valor");
    CHECK(config_get().auth_required == false,     "set_dissectors no toca auth_required");

    /* init(NULL) => todo en 0 */
    config_init(NULL);
    c = config_get();
    CHECK(c.auth_required == false && c.dissectors_enabled == false && c.io_buffer_size == 0,
          "init(NULL) deja la config en 0");

    /* config_get() devuelve una COPIA: mutarla no afecta la global */
    struct config copy = config_get();
    copy.auth_required = true;
    CHECK(copy.auth_required == true && config_get().auth_required == false,
          "config_get devuelve copia (mutar la copia no afecta la global)");

    printf("\n%d tests, %d fallaron\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
