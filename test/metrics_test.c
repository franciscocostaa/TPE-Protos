/**
 * metrics_test.c - tests de los contadores volátiles (metrics.c).
 *
 * Harness propio, SIN dependencias externas. Incluye la unidad bajo prueba.
 * Correr con: make test
 */
#include <stdio.h>

/* Unidad bajo prueba. */
#include "metrics.c"

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
    printf("== metrics ==\n");

    metrics_init();
    struct metrics_snapshot m = metrics_get();
    CHECK(m.connections_total == 0 && m.connections_current == 0 && m.bytes_transferred == 0,
          "init deja todos los contadores en 0");

    /* aperturas: suben históricas y concurrentes */
    metrics_connection_open();
    metrics_connection_open();
    m = metrics_get();
    CHECK(m.connections_total == 2 && m.connections_current == 2, "2 open => total 2, current 2");

    /* cierre: baja concurrentes, NO históricas */
    metrics_connection_close();
    m = metrics_get();
    CHECK(m.connections_total == 2 && m.connections_current == 1, "close baja current pero no total");

    /* bytes: se acumulan */
    metrics_add_bytes(100);
    metrics_add_bytes(50);
    m = metrics_get();
    CHECK(m.bytes_transferred == 150, "add_bytes acumula");

    /* no underflow: cerrar más veces que las abiertas deja current en 0 */
    metrics_connection_close();   /* current: 1 -> 0 */
    metrics_connection_close();   /* current: 0 -> 0 (no debe dar -1) */
    m = metrics_get();
    CHECK(m.connections_current == 0, "current no baja de 0 (sin underflow)");
    CHECK(m.connections_total == 2,   "total no se ve afectado por los cierres de más");

    printf("\n%d tests, %d fallaron\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
