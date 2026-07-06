/**
 * client.c - cliente del protocolo de monitoreo (SMP). Ver docs/PROTOCOL.md.
 *
 * MF1: versión mínima para probar el server de punta a punta. I/O BLOQUEANTE
 * (permitido por la consigna NF5 dada la simpleza del cliente).
 *
 * Uso:
 *     client <host> <port> [-t <token>] <VERB> [args...]
 *
 * Con `-t <token>` autentica (AUTH) antes de enviar el comando. Hace: conecta,
 * imprime el saludo, autentica si corresponde, envía el comando, imprime su
 * respuesta, envía QUIT e imprime el cierre. La traducción de subcomandos
 * ergonómicos (p. ej. `client add-user pablito pass1234`) y las respuestas
 * multilínea llegan en MF3+; por ahora reenvía el verbo tal cual y lee
 * respuestas de una sola línea.
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

/** Lee una línea del socket y la imprime. */
static void
read_print_line(int fd) {
    char line[CLIENT_LINE_MAX];
    if (read_line(fd, line, sizeof(line))) {
        printf("%s\n", line);
    }
}

/**
 * Imprime la respuesta a un comando. Si `multiline` es true, tras la línea de
 * estado lee las líneas de datos hasta el terminador "." (deshaciendo el
 * dot-stuffing). Un "-ERR ..." es siempre de una sola línea, aun para comandos
 * multilínea.
 */
static void
read_print_response(int fd, bool multiline) {
    char line[CLIENT_LINE_MAX];
    if (!read_line(fd, line, sizeof(line))) {
        return;
    }
    printf("%s\n", line);
    if (!multiline || line[0] == '-') {
        return;                          /* respuesta de una línea (o error) */
    }
    while (read_line(fd, line, sizeof(line))) {
        if (strcmp(line, MGMT_MULTILINE_END) == 0) {
            break;                       /* terminador "." */
        }
        const char *p = line;
        if (line[0] == '.' && line[1] == '.') {
            p = line + 1;                /* deshacemos el dot-stuffing */
        }
        printf("%s\n", p);
    }
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

int
main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "uso: %s <host> <port> [-t <token>] <VERB> [args...]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *host = argv[1];
    const char *port = argv[2];

    /* token opcional: "-t <token>" justo después del puerto */
    const char *token   = NULL;
    int         cmd_idx = 3;
    if (argc >= 6 && strcmp(argv[3], "-t") == 0) {
        token   = argv[4];
        cmd_idx = 5;
    }
    if (cmd_idx >= argc) {
        fprintf(stderr, "uso: %s <host> <port> [-t <token>] <VERB> [args...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Ningún argumento (ni el token) puede traer CR/LF: rompería el framing. */
    if (token != NULL && has_crlf(token)) {
        fprintf(stderr, "client: el token no puede contener CR/LF\n");
        return EXIT_FAILURE;
    }
    for (int i = cmd_idx; i < argc; i++) {
        if (has_crlf(argv[i])) {
            fprintf(stderr, "client: los argumentos no pueden contener CR/LF\n");
            return EXIT_FAILURE;
        }
    }

    const int fd = tcp_connect(host, port);
    if (fd == -1) {
        return EXIT_FAILURE;
    }

    /* saludo del servidor */
    read_print_line(fd);

    /* autenticación opcional antes del comando */
    if (token != NULL) {
        char auth[CLIENT_LINE_MAX];
        snprintf(auth, sizeof(auth), "%s %s%s", MGMT_CMD_AUTH, token, MGMT_CRLF);
        if (!send_all(fd, auth)) {
            fprintf(stderr, "client: error enviando AUTH\n");
            close(fd);
            return EXIT_FAILURE;
        }
        read_print_line(fd);             /* respuesta del AUTH */
    }

    /* armamos la línea del comando uniendo argv[cmd_idx..] con espacios */
    char   cmd[CLIENT_LINE_MAX] = {0};
    size_t off = 0;
    for (int i = cmd_idx; i < argc; i++) {
        const int w = snprintf(cmd + off, sizeof(cmd) - off,
                               "%s%s", (i > cmd_idx ? " " : ""), argv[i]);
        if (w < 0 || (size_t) w >= sizeof(cmd) - off) {
            fprintf(stderr, "client: comando demasiado largo\n");
            close(fd);
            return EXIT_FAILURE;
        }
        off += (size_t) w;
    }

    /* ¿el comando devuelve una respuesta multilínea? (verbo en argv[cmd_idx]) */
    const char *verb = argv[cmd_idx];
    const bool  multiline = strcasecmp(verb, MGMT_CMD_LIST_USERS) == 0
                         || strcasecmp(verb, MGMT_CMD_GET_LOG)    == 0
                         || strcasecmp(verb, MGMT_CMD_HELP)       == 0;

    int ret = EXIT_SUCCESS;
    if (!send_all(fd, cmd) || !send_all(fd, MGMT_CRLF)) {
        fprintf(stderr, "client: error enviando el comando\n");
        ret = EXIT_FAILURE;
    } else {
        read_print_response(fd, multiline);
        send_all(fd, MGMT_CMD_QUIT MGMT_CRLF);
        read_print_line(fd);             /* "+OK bye" */
    }

    close(fd);
    return ret;
}
