/**
 * main.c - servidor proxy SOCKSv5 concurrente (sockets no bloqueantes).
 *
 * Monta un socket pasivo y atiende TODAS las conexiones en un único hilo
 * mediante el selector (multiplexación no bloqueante). El único trabajo
 * bloqueante (resolución de DNS con getaddrinfo) se descarga en hilos
 * auxiliares que notifican al selector cuando terminan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "selector.h"
#include "socks5.h"

#define DEFAULT_SOCKS_PORT        1080
#define MAX_PENDING_CONNECTIONS   20
#define SELECTOR_INITIAL_ELEMENTS 1024
#define SELECTOR_TIMEOUT_SECONDS  10

/** señal interna que usa el selector para despertarse (p. ej. fin de DNS) */
#define SELECTOR_SIGNAL SIGALRM

static bool terminated = false;

static void
sigterm_handler(const int signal) {
    printf("Señal %d recibida, cerrando el servidor.\n", signal);
    terminated = true;
}

/** convierte un argumento de línea de comandos en un puerto válido */
static unsigned short
parse_port(const char *s) {
    char *end = NULL;
    errno = 0;
    const long port = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || port < 0 || port > USHRT_MAX) {
        fprintf(stderr, "Puerto inválido: %s\n", s);
        exit(EXIT_FAILURE);
    }
    return (unsigned short) port;
}

int
main(const int argc, const char **argv) {
    unsigned short socks_port = DEFAULT_SOCKS_PORT;
    if (argc == 2) {
        socks_port = parse_port(argv[1]);
    } else if (argc > 2) {
        fprintf(stderr, "Uso: %s [puerto]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // No leemos de stdin.
    close(STDIN_FILENO);

    // Un write a un socket cerrado debe fallar con EPIPE, no matar el proceso.
    signal(SIGPIPE, SIG_IGN);

    const char     *err_msg  = NULL;
    selector_status ss       = SELECTOR_SUCCESS;
    fd_selector     selector = NULL;
    int             server   = -1;

    // Socket pasivo IPv6 dual-stack: atiende clientes IPv4 e IPv6.
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_any;
    addr.sin6_port   = htons(socks_port);

    server = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (server < 0) {
        err_msg = "no se pudo crear el socket pasivo";
        goto finally;
    }

    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    if (bind(server, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        err_msg = "fallo en bind()";
        goto finally;
    }
    if (listen(server, MAX_PENDING_CONNECTIONS) < 0) {
        err_msg = "fallo en listen()";
        goto finally;
    }

    fprintf(stdout, "Servidor SOCKSv5 escuchando en el puerto TCP %u\n", socks_port);

    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);

    if (selector_fd_set_nio(server) == -1) {
        err_msg = "no se pudo poner el socket pasivo en modo no bloqueante";
        goto finally;
    }

    const struct selector_init conf = {
        .signal         = SELECTOR_SIGNAL,
        .select_timeout = { .tv_sec = SELECTOR_TIMEOUT_SECONDS, .tv_nsec = 0 },
    };
    if (selector_init(&conf) != 0) {
        err_msg = "no se pudo inicializar el selector";
        goto finally;
    }

    selector = selector_new(SELECTOR_INITIAL_ELEMENTS);
    if (selector == NULL) {
        err_msg = "no se pudo crear el selector";
        goto finally;
    }

    const struct fd_handler passive = {
        .handle_read  = socksv5_passive_accept,
        .handle_write = NULL,
        .handle_block = NULL,
        .handle_close = NULL,
    };
    ss = selector_register(selector, server, &passive, OP_READ, NULL);
    if (ss != SELECTOR_SUCCESS) {
        err_msg = "no se pudo registrar el socket pasivo";
        goto finally;
    }

    while (!terminated) {
        err_msg = NULL;
        ss = selector_select(selector);
        if (ss != SELECTOR_SUCCESS) {
            err_msg = "error durante el selector_select";
            goto finally;
        }
    }

finally:
    if (ss != SELECTOR_SUCCESS) {
        fprintf(stderr, "%s: %s\n",
                err_msg ? err_msg : "",
                ss == SELECTOR_IO ? strerror(errno) : selector_error(ss));
    } else if (err_msg != NULL) {
        perror(err_msg);
    }

    const int ret = (err_msg == NULL) ? EXIT_SUCCESS : EXIT_FAILURE;
    if (selector != NULL) {
        selector_destroy(selector);
    }
    selector_close();
    socksv5_pool_destroy();
    if (server >= 0) {
        close(server);
    }
    return ret;
}
