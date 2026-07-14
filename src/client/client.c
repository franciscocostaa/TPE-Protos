/**
 * client.c - cliente del protocolo de monitoreo SMP (ver docs/PROTOCOL.md).
 *
 * Herramienta de línea de comandos que ABSTRAE el protocolo. El usuario nombra
 * una operación de gestión con un subcomando ergonómico
 *
 *     client 127.0.0.1 8080 -t <token> add-user pablito pass1234
 *
 * y el cliente se encarga de TODA la sesión —saludo, AUTH, envío del comando y
 * cierre con QUIT— de forma interna. El manejo de la sesión NO forma parte de la
 * superficie de comandos: por eso AUTH y QUIT no son invocables por el usuario.
 * Exponerlos habilitaría estados sin sentido (autenticar dos veces, mandar dos
 * QUIT, etc.).
 *
 * La salida se presenta DECODIFICADA y legible: los datos van a stdout, los
 * errores a stderr, y el código de salida es != 0 cuando el servidor responde
 * con error. Es decir, no es "netcat con pasos extra" (RNF5 de la consigna).
 *
 * I/O bloqueante, permitido por la consigna (RNF5) dada la simpleza del cliente.
 *
 * Uso:
 *     client <host> <port> [-t <token>] <comando> [args...]
 *     client -h | --help
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <unistd.h>

#include <netdb.h>
#include <sys/socket.h>

#include "mgmt_proto.h"

#define CLIENT_LINE_MAX MGMT_LINE_MAX

/* ------------------------------------------------------------------ *
 *  Conexión y framing (I/O bloqueante)
 * ------------------------------------------------------------------ */

/** Conecta (bloqueante) a host:port por TCP. Devuelve el fd o -1. */
static int
tcp_connect(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;       /* IPv4 o IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *list;
    const int rc = getaddrinfo(host, port, &hints, &list);
    if (rc != 0) {
        fprintf(stderr, "client: getaddrinfo(%s, %s): %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = list; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;                       /* conectado */
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd == -1) {
        fprintf(stderr, "client: no se pudo conectar a %s:%s\n", host, port);
    }
    return fd;
}

/**
 * Lee una línea terminada en CRLF del socket (bloqueante) y la deja sin el CRLF
 * y NUL-terminada en `line`. Devuelve true si leyó una línea, false en EOF/error.
 */
static bool
read_line(int fd, char *line, size_t size) {
    size_t len = 0;
    char   c;
    while (len < size - 1) {
        const ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) {
            return false;                /* EOF o error */
        }
        if (c == '\n') {
            if (len > 0 && line[len - 1] == '\r') {
                len--;                   /* descartamos el CR del CRLF */
            }
            line[len] = '\0';
            return true;
        }
        line[len++] = c;
    }
    return false;                        /* línea más larga que el máximo */
}

/** Envía `s` completo por el socket bloqueante. Devuelve true si lo logró. */
static bool
send_all(int fd, const char *s) {
    size_t       off = 0;
    const size_t len = strlen(s);
    while (off < len) {
        const ssize_t n = send(fd, s + off, len - off, 0);
        if (n <= 0) {
            return false;
        }
        off += (size_t) n;
    }
    return true;
}

/**
 * ¿La cadena contiene CR o LF? Un argumento con esos bytes inyectaría líneas
 * extra en el protocolo (command smuggling), así que se rechaza antes de armar
 * el comando.
 */
static bool
has_crlf(const char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '\r' || *s == '\n') {
            return true;
        }
    }
    return false;
}

/**
 * Clasificación de la línea de estado de una respuesta.
 */
typedef enum {
    RESP_OK,     /**< empieza con "+OK"                             */
    RESP_ERR,    /**< empieza con "-ERR"                            */
    RESP_PROTO,  /**< EOF o prefijo inesperado (error de protocolo) */
} resp_kind;

/**
 * Lee la línea de estado y la clasifica. En éxito deja en *rest un puntero al
 * texto que sigue al prefijo ("+OK "/"-ERR "), apuntando dentro de `line`.
 */
static resp_kind
read_status(int fd, char *line, size_t size, const char **rest) {
    if (!read_line(fd, line, size)) {
        return RESP_PROTO;
    }
    const size_t ok_len  = strlen(MGMT_RESP_OK);
    const size_t err_len = strlen(MGMT_RESP_ERR);
    if (strncmp(line, MGMT_RESP_OK, ok_len) == 0) {
        const char *p = line + ok_len;
        while (*p == ' ') { p++; }
        *rest = p;
        return RESP_OK;
    }
    if (strncmp(line, MGMT_RESP_ERR, err_len) == 0) {
        const char *p = line + err_len;
        while (*p == ' ') { p++; }
        *rest = p;
        return RESP_ERR;
    }
    return RESP_PROTO;
}

/**
 * Lee e imprime las líneas de datos de una respuesta multilínea hasta el
 * terminador ".", deshaciendo el dot-stuffing. Antepone `prefix` a cada línea.
 */
static void
print_data_lines(int fd, const char *prefix) {
    char line[CLIENT_LINE_MAX];
    while (read_line(fd, line, sizeof(line))) {
        if (strcmp(line, MGMT_MULTILINE_END) == 0) {
            break;                       /* terminador "." */
        }
        const char *p = line;
        if (line[0] == '.' && line[1] == '.') {
            p = line + 1;                /* deshacemos el dot-stuffing */
        }
        printf("%s%s\n", prefix, p);
    }
}

/* ------------------------------------------------------------------ *
 *  Tabla de comandos y presentación
 * ------------------------------------------------------------------ */

struct cmd_ctx;  /* fwd */

/**
 * Muestra la respuesta a un comando que devolvió "+OK". `rest` es el texto que
 * sigue a "+OK "; para respuestas multilínea lee las líneas de datos de `fd`.
 * Devuelve 0 si la presentó bien.
 */
typedef int (*present_fn)(int fd, const char *rest, const struct cmd_ctx *ctx);

/** Descripción de un comando de la superficie del cliente. */
struct cli_command {
    const char *name;       /**< subcomando ergonómico que tipea el usuario */
    const char *wire_verb;  /**< verbo SMP que se envía al servidor         */
    int         min_args;   /**< mínimo de argumentos tras el subcomando    */
    int         max_args;   /**< máximo de argumentos tras el subcomando    */
    bool        needs_auth; /**< requiere -t <token>                        */
    present_fn  present;    /**< cómo mostrar la respuesta +OK              */
    const char *usage;      /**< firma para los mensajes de error           */
};

/** Contexto que reciben los presentadores (para referenciar los argumentos). */
struct cmd_ctx {
    const struct cli_command *cmd;
    int                       argc;   /**< args tras el subcomando */
    char                    **argv;   /**< args tras el subcomando */
};

/** Etiqueta legible para una métrica conocida; si no la conoce, la clave cruda. */
static const char *
metric_label(const char *key) {
    if (strcmp(key, MGMT_METRIC_CONNECTIONS_TOTAL) == 0)   { return "Conexiones totales     "; }
    if (strcmp(key, MGMT_METRIC_CONNECTIONS_CURRENT) == 0) { return "Conexiones concurrentes"; }
    if (strcmp(key, MGMT_METRIC_BYTES_TRANSFERRED) == 0)   { return "Bytes transferidos     "; }
    return key;
}

static int
present_metrics(int fd, const char *rest, const struct cmd_ctx *ctx) {
    (void) fd;
    (void) ctx;
    printf("Métricas del servidor:\n");
    char  buf[CLIENT_LINE_MAX];
    snprintf(buf, sizeof(buf), "%s", rest);
    char *save = NULL;
    for (char *tok = strtok_r(buf, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        printf("  %s  %s\n", metric_label(tok), eq + 1);
    }
    return 0;
}

static int
present_config(int fd, const char *rest, const struct cmd_ctx *ctx) {
    (void) fd;
    (void) ctx;
    printf("Configuración en runtime:\n");
    char  buf[CLIENT_LINE_MAX];
    snprintf(buf, sizeof(buf), "%s", rest);
    char *save = NULL;
    for (char *tok = strtok_r(buf, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        printf("  %s: %s\n", tok, eq + 1);
    }
    return 0;
}

static int
present_users(int fd, const char *rest, const struct cmd_ctx *ctx) {
    (void) ctx;
    printf("Usuarios (%d):\n", atoi(rest));   /* rest = "<N> users" */
    print_data_lines(fd, "  ");
    return 0;
}

static int
present_log(int fd, const char *rest, const struct cmd_ctx *ctx) {
    (void) ctx;
    printf("Accesos registrados (%d):\n", atoi(rest));   /* rest = "<N> entries" */
    char line[CLIENT_LINE_MAX];
    while (read_line(fd, line, sizeof(line))) {
        if (strcmp(line, MGMT_MULTILINE_END) == 0) {
            break;
        }
        const char *p = line;
        if (line[0] == '.' && line[1] == '.') {
            p = line + 1;                       /* deshacemos el dot-stuffing */
        }
        /* Campos TSV (PROTOCOL.md §5.8): ts, usuario, cliente, destino:puerto, REP. */
        char  copy[CLIENT_LINE_MAX];
        snprintf(copy, sizeof(copy), "%s", p);
        char *save = NULL;
        char *ts   = strtok_r(copy, "\t", &save);
        char *user = strtok_r(NULL, "\t", &save);
        char *cli  = strtok_r(NULL, "\t", &save);
        char *dst  = strtok_r(NULL, "\t", &save);
        char *rep  = strtok_r(NULL, "\t", &save);
        if (ts != NULL && user != NULL && cli != NULL && dst != NULL && rep != NULL) {
            printf("  %s  usuario=%s  %s -> %s  (rep %s)\n", ts, user, cli, dst, rep);
        } else {
            printf("  %s\n", p);                /* formato inesperado: crudo */
        }
    }
    return 0;
}

static int
present_add_user(int fd, const char *rest, const struct cmd_ctx *ctx) {
    (void) fd;
    (void) rest;
    printf("Usuario '%s' agregado.\n", ctx->argv[0]);
    return 0;
}

static int
present_del_user(int fd, const char *rest, const struct cmd_ctx *ctx) {
    (void) fd;
    (void) rest;
    printf("Usuario '%s' eliminado.\n", ctx->argv[0]);
    return 0;
}

static int
present_set_config(int fd, const char *rest, const struct cmd_ctx *ctx) {
    (void) fd;
    (void) rest;
    printf("Configuración actualizada: %s = %s\n", ctx->argv[0], ctx->argv[1]);
    return 0;
}

/**
 * Superficie de comandos del cliente. A propósito NO incluye AUTH ni QUIT (los
 * gestiona el cliente) ni HELP (se resuelve localmente con `help`/-h).
 */
static const struct cli_command COMMANDS[] = {
    { "metrics",    MGMT_CMD_GET_METRICS, 0, 0, true, present_metrics,    "metrics" },
    { "users",      MGMT_CMD_LIST_USERS,  0, 0, true, present_users,      "users" },
    { "add-user",   MGMT_CMD_ADD_USER,    2, 2, true, present_add_user,   "add-user <nombre> <pass>" },
    { "del-user",   MGMT_CMD_DEL_USER,    1, 1, true, present_del_user,   "del-user <nombre>" },
    { "get-config", MGMT_CMD_GET_CONFIG,  0, 0, true, present_config,     "get-config" },
    { "set-config", MGMT_CMD_SET_CONFIG,  2, 2, true, present_set_config, "set-config <clave> <valor>" },
    { "log",        MGMT_CMD_GET_LOG,     0, 0, true, present_log,        "log" },
};

/** Busca un comando por su nombre ergonómico (case-insensitive). */
static const struct cli_command *
find_command(const char *name) {
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcasecmp(name, COMMANDS[i].name) == 0) {
            return &COMMANDS[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 *  Uso y sesión
 * ------------------------------------------------------------------ */

static void
usage(FILE *f, const char *prog) {
    fprintf(f,
        "uso: %s <host> <port> [-t <token>] <comando> [args...]\n"
        "     %s -h | --help\n"
        "\n"
        "Cliente del protocolo de monitoreo SMP: se conecta al canal de\n"
        "administración, ejecuta UN comando y muestra el resultado.\n"
        "\n"
        "Opciones:\n"
        "  -t <token>   token de administración (requerido por los comandos)\n"
        "  -h, --help   muestra esta ayuda y termina\n"
        "\n"
        "Comandos:\n"
        "  metrics                     métricas del servidor\n"
        "  users                       lista los usuarios SOCKS5\n"
        "  add-user <nombre> <pass>    agrega un usuario\n"
        "  del-user <nombre>           elimina un usuario\n"
        "  get-config                  configuración modificable en runtime\n"
        "  set-config <clave> <valor>  cambia una opción en runtime\n"
        "  log                         registro de accesos\n"
        "\n"
        "Ejemplo:\n"
        "  %s 127.0.0.1 8080 -t s3cr3t add-user pablito pass1234\n",
        prog, prog, prog);
}

/**
 * Ejecuta la sesión completa sobre una conexión ya abierta: saludo, AUTH
 * (interno) si hay token, un comando y cierre con QUIT. Devuelve
 * EXIT_SUCCESS/EXIT_FAILURE.
 */
static int
run_session(int fd, const char *token, const struct cli_command *cmd,
            int cmd_argc, char **cmd_args) {
    char        line[CLIENT_LINE_MAX];
    const char *rest = NULL;

    /* saludo del servidor (no se muestra: es parte de la sesión) */
    if (read_status(fd, line, sizeof(line), &rest) != RESP_OK) {
        fprintf(stderr, "client: saludo inesperado del servidor\n");
        return EXIT_FAILURE;
    }

    /* AUTH interno */
    if (token != NULL) {
        char auth[CLIENT_LINE_MAX];
        snprintf(auth, sizeof(auth), "%s %s%s", MGMT_CMD_AUTH, token, MGMT_CRLF);
        if (!send_all(fd, auth) ||
            read_status(fd, line, sizeof(line), &rest) != RESP_OK) {
            fprintf(stderr, "client: autenticación fallida\n");
            return EXIT_FAILURE;
        }
    }

    /* armamos la línea de comando: "<VERB> arg1 arg2 ..." */
    char wire[CLIENT_LINE_MAX];
    int  w = snprintf(wire, sizeof(wire), "%s", cmd->wire_verb);
    for (int i = 0; i < cmd_argc && w > 0 && (size_t) w < sizeof(wire); i++) {
        w += snprintf(wire + w, sizeof(wire) - (size_t) w, " %s", cmd_args[i]);
    }
    if (w < 0 || (size_t) w >= sizeof(wire)) {
        fprintf(stderr, "client: comando demasiado largo\n");
        return EXIT_FAILURE;
    }
    if (!send_all(fd, wire) || !send_all(fd, MGMT_CRLF)) {
        fprintf(stderr, "client: error enviando el comando\n");
        return EXIT_FAILURE;
    }

    /* respuesta al comando */
    int ret;
    struct cmd_ctx ctx = { .cmd = cmd, .argc = cmd_argc, .argv = cmd_args };
    switch (read_status(fd, line, sizeof(line), &rest)) {
        case RESP_OK:
            ret = cmd->present(fd, rest, &ctx) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
            break;
        case RESP_ERR: {
            char      *end  = NULL;
            const long code = strtol(rest, &end, 10);
            while (*end == ' ') { end++; }
            fprintf(stderr, "client: comando rechazado (código %ld): %s\n", code, end);
            ret = EXIT_FAILURE;
            break;
        }
        default:
            fprintf(stderr, "client: respuesta inesperada del servidor\n");
            ret = EXIT_FAILURE;
            break;
    }

    /* cierre ordenado */
    send_all(fd, MGMT_CMD_QUIT MGMT_CRLF);
    read_line(fd, line, sizeof(line));   /* "+OK bye" (descartado) */
    return ret;
}

int
main(int argc, char *argv[]) {
    const char *prog = argv[0];

    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(stdout, prog);
        return EXIT_SUCCESS;
    }
    if (argc < 4) {
        usage(stderr, prog);
        return EXIT_FAILURE;
    }

    const char *host = argv[1];
    const char *port = argv[2];

    /* token opcional: "-t <token>" justo después del puerto */
    const char *token = NULL;
    int         idx   = 3;
    if (strcmp(argv[3], "-t") == 0) {
        if (argc < 6) {
            usage(stderr, prog);
            return EXIT_FAILURE;
        }
        token = argv[4];
        idx   = 5;
    }

    const char *name     = argv[idx];
    char      **cmd_args = &argv[idx + 1];
    const int   cmd_argc = argc - (idx + 1);

    /* `help` como comando: ayuda local, sin conectar. */
    if (strcasecmp(name, "help") == 0) {
        usage(stdout, prog);
        return EXIT_SUCCESS;
    }

    const struct cli_command *cmd = find_command(name);
    if (cmd == NULL) {
        if (strcasecmp(name, MGMT_CMD_AUTH) == 0 || strcasecmp(name, MGMT_CMD_QUIT) == 0) {
            fprintf(stderr, "client: '%s' lo maneja el cliente internamente, no es un comando\n", name);
        } else {
            fprintf(stderr, "client: comando desconocido '%s' (probá `%s -h`)\n", name, prog);
        }
        return EXIT_FAILURE;
    }

    /* validaciones ANTES de tocar la red (fail-fast) */
    if (cmd_argc < cmd->min_args) {
        fprintf(stderr, "client: faltan argumentos para '%s'. uso: %s\n", cmd->name, cmd->usage);
        return EXIT_FAILURE;
    }
    if (cmd_argc > cmd->max_args) {
        fprintf(stderr, "client: '%s' recibió argumentos de más. uso: %s\n", cmd->name, cmd->usage);
        for (int i = 0; i < cmd_argc; i++) {
            if (strcmp(cmd_args[i], "-t") == 0) {
                fprintf(stderr, "client: la opción -t <token> va antes del comando: %s <host> <port> -t <token> %s\n",
                        prog, cmd->name);
                break;
            }
        }
        return EXIT_FAILURE;
    }
    if (cmd->needs_auth && token == NULL) {
        fprintf(stderr, "client: el comando '%s' requiere autenticación: pasá -t <token>\n", cmd->name);
        return EXIT_FAILURE;
    }
    if (token != NULL && has_crlf(token)) {
        fprintf(stderr, "client: el token no puede contener CR/LF\n");
        return EXIT_FAILURE;
    }
    for (int i = 0; i < cmd_argc; i++) {
        if (has_crlf(cmd_args[i])) {
            fprintf(stderr, "client: los argumentos no pueden contener CR/LF\n");
            return EXIT_FAILURE;
        }
    }

    const int fd = tcp_connect(host, port);
    if (fd == -1) {
        return EXIT_FAILURE;
    }
    const int ret = run_session(fd, token, cmd, cmd_argc, cmd_args);
    close(fd);
    return ret;
}
