#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "access_log.h"
#include "netutils.h"
#include "socks5/internal.h"

static unsigned request_parse(struct selector_key *key);
static unsigned request_connect(fd_selector s, struct socks5 *st);
static void request_marshall_reply(struct socks5 *s);

static unsigned
request_parse(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    buffer        *b = &s->read_buffer;

    size_t   avail;
    uint8_t *d = buffer_read_ptr(b, &avail);
    if (avail < 4) {
        return REQUEST_READ;
    }
    const uint8_t ver  = d[0];
    const uint8_t cmd  = d[1];
    const uint8_t atyp = d[3];
    if (ver != SOCKS5_VERSION) {
        return ERROR;
    }
    if (d[2] != 0x00) {
        return ERROR;
    }

    size_t needed;
    switch (atyp) {
        case SOCKS5_ATYP_IPV4:
            needed = 4 + 4 + 2;
            break;
        case SOCKS5_ATYP_IPV6:
            needed = 4 + 16 + 2;
            break;
        case SOCKS5_ATYP_DOMAIN:
            if (avail < 5) {
                return REQUEST_READ;
            }
            needed = 4 + 1 + d[4] + 2;
            break;
        default:
            s->reply_status = SOCKS5_REP_ATYP_NOT_SUPPORTED;
            request_marshall_reply(s);
            selector_set_interest_key(key, OP_WRITE);
            return REQUEST_WRITE;
    }
    if (avail < needed) {
        return REQUEST_READ;
    }

    if (cmd != SOCKS5_CMD_CONNECT) {
        s->reply_status = SOCKS5_REP_COMMAND_NOT_SUPPORTED;
        request_marshall_reply(s);
        buffer_read_adv(b, needed);
        selector_set_interest_key(key, OP_WRITE);
        return REQUEST_WRITE;
    }

    char       host[SOCKS5_FQDN_MAX + 1];
    in_port_t  port_net;
    bool       is_domain = false;
    switch (atyp) {
        case SOCKS5_ATYP_IPV4:
            inet_ntop(AF_INET, &d[4], host, sizeof(host));
            memcpy(&port_net, &d[8], 2);
            break;
        case SOCKS5_ATYP_IPV6:
            inet_ntop(AF_INET6, &d[4], host, sizeof(host));
            memcpy(&port_net, &d[20], 2);
            break;
        default: {
            const uint8_t len = d[4];
            memcpy(host, &d[5], len);
            host[len] = '\0';
            memcpy(&port_net, &d[5 + len], 2);
            is_domain = true;
            break;
        }
    }
    buffer_read_adv(b, needed);
    snprintf(s->dest_port, sizeof(s->dest_port), "%u", ntohs(port_net));
    /* Guardamos el destino pedido (FQDN o IP literal) para access_log; si es
     * dominio tambien se usa para la resolucion DNS. */
    strncpy(s->dest_fqdn, host, sizeof(s->dest_fqdn) - 1);
    s->dest_fqdn[sizeof(s->dest_fqdn) - 1] = '\0';

    if (is_domain) {
        selector_set_interest_key(key, OP_NOOP);
        return REQUEST_RESOLV;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags    = AI_NUMERICHOST;
    if (getaddrinfo(host, s->dest_port, &hints, &s->origin_resolution) != 0
            || s->origin_resolution == NULL) {
        s->reply_status = SOCKS5_REP_GENERAL_FAILURE;
        request_marshall_reply(s);
        selector_set_interest_key(key, OP_WRITE);
        return REQUEST_WRITE;
    }
    s->origin_resolution_current = s->origin_resolution;
    return request_connect(key->s, s);
}

unsigned
request_read(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    buffer        *b = &s->read_buffer;

    size_t   space;
    uint8_t *ptr = buffer_write_ptr(b, &space);
    if (space == 0) {
        return ERROR;
    }
    const ssize_t n = recv(key->fd, ptr, space, 0);
    if (n <= 0) {
        return ERROR;
    }
    buffer_write_adv(b, n);
    return request_parse(key);
}

static void *
request_resolv_blocking(void *arg) {
    struct selector_key *key = arg;
    struct socks5       *s   = ATTACHMENT(key);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(s->dest_fqdn, s->dest_port, &hints, &s->origin_resolution) != 0) {
        s->origin_resolution = NULL;
    }

    selector_notify_block(key->s, key->fd);
    free(key);
    return NULL;
}

void
request_resolv_init(const unsigned state, struct selector_key *key) {
    (void) state;
    struct selector_key *blocking_key = malloc(sizeof(*blocking_key));
    if (blocking_key == NULL) {
        ATTACHMENT(key)->origin_resolution = NULL;
        selector_notify_block(key->s, key->fd);
        return;
    }
    *blocking_key = *key;

    pthread_t tid;
    if (pthread_create(&tid, NULL, request_resolv_blocking, blocking_key) != 0) {
        free(blocking_key);
        ATTACHMENT(key)->origin_resolution = NULL;
        selector_notify_block(key->s, key->fd);
        return;
    }
    pthread_detach(tid);
}

unsigned
request_resolv_done(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    if (s->origin_resolution == NULL) {
        s->reply_status = SOCKS5_REP_HOST_UNREACHABLE;
        request_marshall_reply(s);
        selector_set_interest_key(key, OP_WRITE);
        return REQUEST_WRITE;
    }
    s->origin_resolution_current = s->origin_resolution;
    return request_connect(key->s, s);
}

static uint8_t
errno_to_rep(const int e) {
    switch (e) {
        case ECONNREFUSED: return SOCKS5_REP_CONNECTION_REFUSED;
        case ENETUNREACH:  return SOCKS5_REP_NETWORK_UNREACHABLE;
        case EHOSTUNREACH: return SOCKS5_REP_HOST_UNREACHABLE;
        case ETIMEDOUT:    return SOCKS5_REP_HOST_UNREACHABLE;
        default:           return SOCKS5_REP_GENERAL_FAILURE;
    }
}

static unsigned
request_connect(fd_selector s, struct socks5 *st) {
    while (st->origin_resolution_current != NULL) {
        struct addrinfo *ai           = st->origin_resolution_current;
        st->origin_resolution_current = ai->ai_next;

        st->origin_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (st->origin_fd < 0) {
            continue;
        }
        if (selector_fd_set_nio(st->origin_fd) == -1) {
            close(st->origin_fd);
            st->origin_fd = -1;
            continue;
        }
        if (connect(st->origin_fd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS) {
            st->references++;
            if (selector_register(s, st->origin_fd, &socks5_handler, OP_WRITE, st) != SELECTOR_SUCCESS) {
                st->references--;
                close(st->origin_fd);
                st->origin_fd = -1;
                continue;
            }
            selector_set_interest(s, st->client_fd, OP_NOOP);
            return REQUEST_CONNECTING;
        }
        st->reply_status = errno_to_rep(errno);
        close(st->origin_fd);
        st->origin_fd = -1;
    }

    if (st->reply_status == SOCKS5_REP_SUCCEEDED) {
        st->reply_status = SOCKS5_REP_HOST_UNREACHABLE;
    }
    request_marshall_reply(st);
    selector_set_interest(s, st->client_fd, OP_WRITE);
    return REQUEST_WRITE;
}

unsigned
connecting_write(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);

    int       error = 0;
    socklen_t len   = sizeof(error);
    if (getsockopt(key->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        error = errno;
    }

    if (error == 0) {
        s->reply_status = SOCKS5_REP_SUCCEEDED;
        request_marshall_reply(s);
        selector_set_interest(key->s, s->origin_fd, OP_NOOP);
        selector_set_interest(key->s, s->client_fd, OP_WRITE);
        return REQUEST_WRITE;
    }

    s->reply_status = errno_to_rep(error);
    selector_unregister_fd(key->s, s->origin_fd);
    close(s->origin_fd);
    s->origin_fd = -1;
    return request_connect(key->s, s);
}

static void
log_access(struct socks5 *s) {
    char client[SOCKADDR_TO_HUMAN_MIN];
    sockaddr_to_human(client, sizeof(client), (struct sockaddr *) &s->client_addr);
    access_log_record(s->username[0]  != '\0' ? s->username  : NULL,
                      client,
                      s->dest_fqdn[0] != '\0' ? s->dest_fqdn : NULL,
                      (uint16_t) strtol(s->dest_port, NULL, 10),
                      s->reply_status);
}

static void
request_marshall_reply(struct socks5 *s) {
    buffer *b = &s->write_buffer;
    buffer_reset(b);

    size_t   space;
    uint8_t *p = buffer_write_ptr(b, &space);
    size_t   i = 0;
    (void) space;

    p[i++] = SOCKS5_VERSION;
    p[i++] = s->reply_status;
    p[i++] = 0x00;

    struct sockaddr_storage bnd;
    socklen_t               bnd_len = sizeof(bnd);
    if (s->reply_status == SOCKS5_REP_SUCCEEDED && s->origin_fd != -1
            && getsockname(s->origin_fd, (struct sockaddr *) &bnd, &bnd_len) == 0
            && bnd.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (struct sockaddr_in6 *) &bnd;
        p[i++] = SOCKS5_ATYP_IPV6;
        memcpy(&p[i], &a->sin6_addr, 16); i += 16;
        memcpy(&p[i], &a->sin6_port,  2); i += 2;
    } else if (s->reply_status == SOCKS5_REP_SUCCEEDED && s->origin_fd != -1
            && bnd.ss_family == AF_INET) {
        const struct sockaddr_in *a = (struct sockaddr_in *) &bnd;
        p[i++] = SOCKS5_ATYP_IPV4;
        memcpy(&p[i], &a->sin_addr, 4); i += 4;
        memcpy(&p[i], &a->sin_port, 2); i += 2;
    } else {
        p[i++] = SOCKS5_ATYP_IPV4;
        memset(&p[i], 0, 4 + 2); i += 4 + 2;
    }
    buffer_write_adv(b, i);

    log_access(s);
}

unsigned
request_write(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    buffer        *b = &s->write_buffer;

    size_t   size;
    uint8_t *ptr = buffer_read_ptr(b, &size);
    const ssize_t n = send(s->client_fd, ptr, size, 0);
    if (n == -1) {
        return ERROR;
    }
    buffer_read_adv(b, n);
    if (buffer_can_read(b)) {
        return REQUEST_WRITE;
    }
    if (s->reply_status != SOCKS5_REP_SUCCEEDED) {
        return DONE;
    }

    copy_init(s);
    selector_set_interest(key->s, s->client_fd, OP_READ);
    selector_set_interest(key->s, s->origin_fd, OP_READ);
    return COPY;
}
