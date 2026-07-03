/**
 * main.c - servidor proxy SOCKSv5 + servicio de monitoreo (sockets no bloqueantes).
 *
 * Monta DOS sockets pasivos en un único proceso:
 *   - SOCKS5  (data-path del proxy)        -> socksv5_passive_accept
 *   - mgmt    (protocolo de monitoreo)     -> mgmt_passive_accept
 *
 * Ambos se atienden en un único hilo mediante el selector (multiplexación no
 * bloqueante). El único trabajo bloqueante (resolución de DNS con getaddrinfo)
 * se descarga en hilos auxiliares que notifican al selector cuando terminan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "args.h"
#include "selector.h"
#include "socks5.h"
#include "mgmt/mgmt.h"
#include "mgmt/mgmt_cmd.h"
#include "users.h"
#include "metrics.h"
#include "config.h"
#include "access_log.h"

#define MAX_PENDING_CONNECTIONS   20 //maybe should be more, but for now is ok, we can change it later if we need to
#define SELECTOR_INITIAL_ELEMENTS 1024
#define SELECTOR_TIMEOUT_SECONDS  10

/** señal interna que usa el selector para despertarse (p. ej. fin de DNS) */
#define SELECTOR_SIGNAL SIGALRM

/**
 * Cuenta de señales de terminación recibidas:
 *   0 -> operación normal
 *   1 -> graceful shutdown: dejamos de aceptar y drenamos conexiones
 *  >=2 -> apagado forzado
 */
static volatile sig_atomic_t terminate_count = 0;

static void
sigterm_handler(const int signal) {
    (void) signal;
    terminate_count++;
}

/**
 * Crea un socket pasivo IPv6 dual-stack (atiende clientes IPv4 e IPv6) ligado a
 * `bind_addr` en el puerto dado. `bind_addr` puede ser NULL / "0.0.0.0" / "::"
 * (todas las interfaces), una IPv6 literal, o una IPv4 literal (se representa
 * como IPv4-mapped). Devuelve el fd o -1 (con *err_msg seteado).
 */
static int
create_passive_socket(const char *bind_addr, const unsigned short port, const char **err_msg) {
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(port);

    /* Resolvemos la dirección de bind sobre el socket IPv6 dual-stack. */
    if (bind_addr == NULL || strcmp(bind_addr, "0.0.0.0") == 0
            || strcmp(bind_addr, "::") == 0 || strcmp(bind_addr, "*") == 0) {
        addr.sin6_addr = in6addr_any;                    /* todas las interfaces */
    } else if (inet_pton(AF_INET6, bind_addr, &addr.sin6_addr) == 1) {
        /* IPv6 literal: ya quedó cargada en sin6_addr. */
    } else {
        struct in_addr v4;
        if (inet_pton(AF_INET, bind_addr, &v4) != 1) {
            *err_msg = "dirección de bind inválida (esperaba IPv4 o IPv6 literal)";
            return -1;
        }
        /* IPv4 literal -> IPv4-mapped IPv6 (::ffff:a.b.c.d) para el socket dual-stack. */
        memset(&addr.sin6_addr, 0, sizeof(addr.sin6_addr));
        addr.sin6_addr.s6_addr[10] = 0xff;
        addr.sin6_addr.s6_addr[11] = 0xff;
        memcpy(&addr.sin6_addr.s6_addr[12], &v4, sizeof(v4));
    }

    const int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        *err_msg = "no se pudo crear el socket pasivo";
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    /* Forzamos dual-stack explícito (en algunas plataformas viene v6-only). */
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &(int){0}, sizeof(int));

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) { //with bind we associate the socket with the address and port, so the kernel knows that when a packet arrives for that address and port, it should be delivered to this socket
        *err_msg = "fallo en bind()";
        close(fd);
        return -1;
    }
    if (listen(fd, MAX_PENDING_CONNECTIONS) < 0) { //maybe we should change 
        *err_msg = "fallo en listen()";
        close(fd);
        return -1;
    }
    if (selector_fd_set_nio(fd) == -1) {
        *err_msg = "no se pudo poner el socket pasivo en modo no bloqueante";
        close(fd);
        return -1;
    }
    return fd;
    
}

int
main(const int argc, char **argv) {
    struct socks5args args;
    parse_args(argc, argv, &args);

    /* Inicialización de los módulos compartidos (ver docs/PLAN.md). */
    users_init();
    metrics_init();
    access_log_init();
    for (int i = 0; i < MAX_USERS; i++) {
        if (args.users[i].name != NULL) {
            users_add(args.users[i].name, args.users[i].pass);
        }
    }
    const struct config initial_cfg = {
        /* si se cargaron usuarios, por defecto exigimos autenticación */
        .auth_required      = users_count() > 0, //check if this approach is right
        .dissectors_enabled = args.disectors_enabled, //think its for the second part of the project, but we can leave it as is
        .io_buffer_size     = SOCKS5_BUFFER_SIZE, 
    };
    config_init(&initial_cfg);

    /* Token del canal de administración: si no se pasó -t, mgmt usa su default. */
    mgmt_cmd_set_token(args.mng_token);

    /* No leemos de stdin. */
    close(STDIN_FILENO);
    /* Un write a un socket cerrado debe fallar con EPIPE, no matar el proceso. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);

    const char     *err_msg     = NULL;
    selector_status ss          = SELECTOR_SUCCESS;
    fd_selector     selector    = NULL;
    int             socks_fd    = -1;
    int             mgmt_fd     = -1;
    bool            accepting   = true; //this is great for graceful shutdown, we can use it to stop accepting new connections when we receive a signal

    socks_fd = create_passive_socket(args.socks_addr, args.socks_port, &err_msg);
    if (socks_fd < 0) {
        goto finally;
    }
    mgmt_fd = create_passive_socket(args.mng_addr, args.mng_port, &err_msg);
    if (mgmt_fd < 0) {
        goto finally;
    }

    fprintf(stdout, "Servidor SOCKSv5 escuchando en el puerto TCP %u\n", args.socks_port);
    fprintf(stdout, "Servicio de monitoreo escuchando en el puerto TCP %u\n", args.mng_port);

    const struct selector_init conf = {
        .signal         = SELECTOR_SIGNAL,
        .select_timeout = { .tv_sec = SELECTOR_TIMEOUT_SECONDS, .tv_nsec = 0 },
    };
    if (selector_init(&conf) != 0) {
        err_msg = "no se pudo inicializar el selector";
        goto finally; //marcelo wouldnt be proud about this :), i like it
    }

    selector = selector_new(SELECTOR_INITIAL_ELEMENTS);
    if (selector == NULL) {
        err_msg = "no se pudo crear el selector";
        goto finally;
    }
    //this functions are the handlers for the passive sockets, they are called when a new connection is accepted
    const struct fd_handler socks_passive = { .handle_read = socksv5_passive_accept };
    const struct fd_handler mgmt_passive  = { .handle_read = mgmt_passive_accept };
    //we pass the selector, the fd of the passive socket, the handler and the interest (OP_READ) to the selector_register function, so it can register the passive socket and call the handler when a new connection is accepted
    ss = selector_register(selector, socks_fd, &socks_passive, OP_READ, NULL);
    if (ss != SELECTOR_SUCCESS) {
        err_msg = "no se pudo registrar el socket pasivo SOCKS";
        goto finally;
    }
    ss = selector_register(selector, mgmt_fd, &mgmt_passive, OP_READ, NULL);
    if (ss != SELECTOR_SUCCESS) {
        err_msg = "no se pudo registrar el socket pasivo de monitoreo";
        goto finally;
    }

    /*
     * Loop principal con graceful shutdown (consigna F9):
     *  - 1ª señal: dejamos de aceptar (desregistramos los sockets pasivos) y
     *    seguimos iterando hasta que no queden conexiones vivas.
     *  - 2ª señal: apagado forzado inmediato.
     */
    while (true) {
        if (terminate_count >= 2) {
            fprintf(stderr, "Segunda señal recibida: apagado forzado.\n");
            break;
        }
        if (terminate_count >= 1 && accepting) {
            fprintf(stderr, "Señal recibida: dejando de aceptar conexiones, "
                            "esperando a que terminen las activas...\n");
            selector_unregister_fd(selector, socks_fd);
            selector_unregister_fd(selector, mgmt_fd);
            close(socks_fd); socks_fd = -1;
            close(mgmt_fd);  mgmt_fd  = -1;
            accepting = false;
        }
        if (!accepting && metrics_get().connections_current == 0) {
            break;  // todas las conexiones drenaron
        }

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
    if (socks_fd >= 0) {
        close(socks_fd);
    }
    if (mgmt_fd >= 0) {
        close(mgmt_fd);
    }
    return ret;
}
