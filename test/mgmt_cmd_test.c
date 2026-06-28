/**
 * mgmt_cmd_test.c - tests del dispatch del protocolo de monitoreo (SMP).
 *
 * Harness propio, SIN dependencias externas (no usa CUnit/check, que no están
 * garantizados en la máquina de corrección). Se compila e incluye directamente
 * la unidad bajo prueba para poder ejercitar también sus funciones estáticas,
 * al estilo de los tests de cátedra (que hacen `#include "buffer.c"`).
 *
 * Correr con: make test
 */
#include <stdio.h>
#include <string.h>

/* Unidad bajo prueba (incluye sus estáticas). */
#include "mgmt/mgmt_cmd.c"

#include "metrics.h"
#include "users.h"
#include "config.h"

/* ---- mini harness -------------------------------------------------------- */

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

/**
 * Despacha `line_in` sobre la sesión `s` y deja la respuesta cruda (con su
 * framing) como C-string en `out`. La línea se copia porque el dispatch la
 * tokeniza in-place.
 */
static const char *
run(struct mgmt_session *s, const char *line_in, char *out, size_t outsz) {
    uint8_t raw[MGMT_LINE_MAX * 8];
    buffer  b;
    buffer_init(&b, sizeof(raw), raw);

    char line[MGMT_LINE_MAX];
    snprintf(line, sizeof(line), "%s", line_in);
    mgmt_cmd_dispatch(s, line, &b);

    size_t   n;
    uint8_t *p = buffer_read_ptr(&b, &n);
    if (n >= outsz) {
        n = outsz - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/* ---- tests --------------------------------------------------------------- */

int
main(void) {
    users_init();
    metrics_init();
    const struct config cfg = {
        .auth_required = true, .dissectors_enabled = false, .io_buffer_size = 4096,
    };
    config_init(&cfg);
    mgmt_cmd_set_token("secret");

    char                out[MGMT_LINE_MAX * 8];
    struct mgmt_session s = { .authenticated = false };

    printf("== gating / auth ==\n");
    run(&s, "GET-METRICS", out, sizeof(out));
    CHECK(strstr(out, "-ERR 3") != NULL, "comando protegido sin auth => ERR 3");

    run(&s, "AUTH wrong", out, sizeof(out));
    CHECK(strstr(out, "-ERR 4") != NULL, "AUTH token incorrecto => ERR 4");
    CHECK(s.authenticated == false, "sesion sigue NO autenticada");

    run(&s, "AUTH secret", out, sizeof(out));
    CHECK(strstr(out, MGMT_RESP_OK) != NULL && s.authenticated, "AUTH correcto => OK + autenticada");

    run(&s, "AUTH", out, sizeof(out));
    CHECK(strstr(out, "-ERR 5") != NULL, "AUTH sin argumento => ERR 5");

    printf("== metrics ==\n");
    run(&s, "GET-METRICS", out, sizeof(out));
    CHECK(strstr(out, "+OK connections-total=") != NULL, "GET-METRICS autenticado => OK");

    printf("== usuarios ==\n");
    run(&s, "ADD-USER pablito pass1234", out, sizeof(out));
    CHECK(strstr(out, MGMT_RESP_OK) != NULL, "ADD-USER nuevo => OK");
    CHECK(users_count() == 1, "users_count == 1 tras alta");

    run(&s, "ADD-USER pablito otra", out, sizeof(out));
    CHECK(strstr(out, "-ERR 7") != NULL, "ADD-USER duplicado => ERR 7");

    run(&s, "ADD-USER soloUno", out, sizeof(out));
    CHECK(strstr(out, "-ERR 5") != NULL, "ADD-USER argc invalido => ERR 5");

    run(&s, "LIST-USERS", out, sizeof(out));
    CHECK(strstr(out, "+OK 1 users") != NULL,   "LIST-USERS encabezado con conteo");
    CHECK(strstr(out, "pablito\r\n") != NULL,   "LIST-USERS incluye el nombre");
    CHECK(strstr(out, "\r\n.\r\n") != NULL,     "LIST-USERS termina en el terminador '.'");

    run(&s, "DEL-USER fantasma", out, sizeof(out));
    CHECK(strstr(out, "-ERR 8") != NULL, "DEL-USER inexistente => ERR 8");

    run(&s, "DEL-USER pablito", out, sizeof(out));
    CHECK(strstr(out, MGMT_RESP_OK) != NULL && users_count() == 0, "DEL-USER existente => OK y baja");

    printf("== config ==\n");
    run(&s, "GET-CONFIG", out, sizeof(out));
    CHECK(strstr(out, "auth-required=on") != NULL, "GET-CONFIG refleja auth-required=on");

    run(&s, "SET-CONFIG auth-required off", out, sizeof(out));
    CHECK(strstr(out, MGMT_RESP_OK) != NULL, "SET-CONFIG auth-required off => OK");
    run(&s, "GET-CONFIG", out, sizeof(out));
    CHECK(strstr(out, "auth-required=off") != NULL, "el cambio de config es efectivo");

    run(&s, "SET-CONFIG foo on", out, sizeof(out));
    CHECK(strstr(out, "-ERR 9") != NULL, "SET-CONFIG clave desconocida => ERR 9");

    run(&s, "SET-CONFIG auth-required quizas", out, sizeof(out));
    CHECK(strstr(out, "-ERR 5") != NULL, "SET-CONFIG valor invalido => ERR 5");

    printf("== varios ==\n");
    run(&s, "FOO", out, sizeof(out));
    CHECK(strstr(out, "-ERR 2") != NULL, "comando desconocido => ERR 2");

    run(&s, "get-metrics", out, sizeof(out));
    CHECK(strstr(out, "+OK connections-total=") != NULL, "verbo case-insensitive");

    /* dot-stuffing: un nombre que empieza con '.' debe viajar con '.' extra */
    users_add(".oculto", "x");
    run(&s, "LIST-USERS", out, sizeof(out));
    CHECK(strstr(out, "..oculto\r\n") != NULL, "dot-stuffing en linea de datos");

    printf("\n%d tests, %d fallaron\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
