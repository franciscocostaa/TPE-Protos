/**
 * access_log_test.c - tests del registro de accesos (access_log.c).
 *
 * Harness propio, SIN dependencias externas. Incluye la unidad bajo prueba.
 * Como access_log_record() imprime cada acceso a stdout, silenciamos stdout
 * (redirigiéndolo a /dev/null) sólo durante las cargas, y lo restauramos para
 * que se vean los resultados de los CHECK.
 *
 * Correr con: make test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

/* Unidad bajo prueba. */
#include "access_log.c"

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

/* Redirige stdout a /dev/null; devuelve el fd guardado para restaurar luego. */
static int
mute_stdout(void) {
    fflush(stdout);
    const int saved   = dup(STDOUT_FILENO);
    const int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    close(devnull);
    return saved;
}

static void
unmute_stdout(int saved) {
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
}

int
main(void) {
    printf("== access_log ==\n");

    access_log_init();
    CHECK(access_log_count() == 0,   "init deja el log vacio");
    CHECK(access_log_get(0) == NULL, "get fuera de rango => NULL");

    /* un acceso: se guardan todos los campos */
    int saved = mute_stdout();
    access_log_record("ana", "1.2.3.4:5678", "example.com", 443, 0x00);
    unmute_stdout(saved);

    CHECK(access_log_count() == 1, "record => count 1");
    const struct access_log_entry *e = access_log_get(0);
    CHECK(e != NULL,                                        "get(0) no es NULL");
    CHECK(e != NULL && strcmp(e->username, "ana") == 0,     "username guardado");
    CHECK(e != NULL && strcmp(e->client_addr, "1.2.3.4:5678") == 0, "client_addr guardado");
    CHECK(e != NULL && strcmp(e->dest, "example.com") == 0, "dest guardado");
    CHECK(e != NULL && e->dest_port == 443,                 "puerto guardado");
    CHECK(e != NULL && e->rep == 0x00,                      "rep guardado");

    /* username NULL (conexión NO-AUTH) => campo vacío */
    saved = mute_stdout();
    access_log_record(NULL, "9.9.9.9:1", "sitio", 80, 0x05);
    unmute_stdout(saved);
    e = access_log_get(1);
    CHECK(e != NULL && e->username[0] == '\0', "username NULL => campo vacio");
    CHECK(e != NULL && e->rep == 0x05,         "rep 0x05 guardado");

    /* buffer circular: al superar la capacidad se pisan las más viejas */
    access_log_init();
    saved = mute_stdout();
    char dest[32];
    for (int i = 0; i < ACCESS_LOG_CAPACITY + 5; i++) {   /* 0 .. CAPACITY+4 */
        snprintf(dest, sizeof(dest), "host%d", i);
        access_log_record("u", "0.0.0.0:0", dest, (uint16_t) i, 0);
    }
    unmute_stdout(saved);

    CHECK(access_log_count() == ACCESS_LOG_CAPACITY, "el log se topa en CAPACITY (ring buffer)");
    /* cargamos 0..CAPACITY+4; las 5 más viejas (0..4) se pisaron; la más vieja viva es host5 */
    e = access_log_get(0);
    CHECK(e != NULL && strcmp(e->dest, "host5") == 0, "get(0) es la mas vieja viva tras el wrap");
    /* la más reciente es la última cargada: host<CAPACITY+4> */
    e = access_log_get(ACCESS_LOG_CAPACITY - 1);
    char expected_last[32];
    snprintf(expected_last, sizeof(expected_last), "host%d", ACCESS_LOG_CAPACITY + 4);
    CHECK(e != NULL && strcmp(e->dest, expected_last) == 0, "get(ultimo) es la mas reciente");

    printf("\n%d tests, %d fallaron\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
