#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/socket.h>

#include "metrics.h"
#include "socks5/internal.h"

void
copy_init(struct socks5 *s) {
    struct copy *c = &s->client.copy;
    c->fd     = &s->client_fd;
    c->rb     = &s->read_buffer;
    c->wb     = &s->write_buffer;
    c->duplex = OP_READ | OP_WRITE;
    c->other  = &s->orig.copy;

    struct copy *o = &s->orig.copy;
    o->fd     = &s->origin_fd;
    o->rb     = &s->write_buffer;
    o->wb     = &s->read_buffer;
    o->duplex = OP_READ | OP_WRITE;
    o->other  = &s->client.copy;
}

static struct copy *
copy_ptr(struct socks5 *s, const int fd) {
    return (fd == s->client_fd) ? &s->client.copy : &s->orig.copy;
}

static void
copy_compute_interests(fd_selector s, struct copy *c) {
    fd_interest interest = OP_NOOP;
    if ((c->duplex & OP_READ)  && buffer_can_write(c->rb)) {
        interest |= OP_READ;
    }
    if ((c->duplex & OP_WRITE) && buffer_can_read(c->wb)) {
        interest |= OP_WRITE;
    }
    selector_set_interest(s, *c->fd, interest);
}

unsigned
copy_read(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct copy   *c = copy_ptr(s, key->fd);

    size_t   size;
    uint8_t *ptr = buffer_write_ptr(c->rb, &size);
    const ssize_t n = recv(key->fd, ptr, size, 0);
    if (n > 0) {
        buffer_write_adv(c->rb, n);

        /*
         * los bytes recien leidos ya estan en el buffer que debe
         * escribir el otro fd. Intentamos enviarlos ahora para evitar esperar
         * otra vuelta de pselect() solo para recibir el evento OP_WRITE.
         */
        if (*c->other->fd != -1 && buffer_can_read(c->other->wb)) {
            struct selector_key other_key = *key;
            other_key.fd = *c->other->fd;

            /*
             * Si el socket no esta listo, copy_write() no bloquea: deja los
             * bytes pendientes y luego copy_compute_interests() mantiene OP_WRITE.
             */
            if (copy_write(&other_key) == DONE) {
                return DONE;
            }
        }
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        c->duplex &= ~OP_READ;
        if (*c->other->fd != -1) {
            shutdown(*c->other->fd, SHUT_WR);
            c->other->duplex &= ~OP_WRITE;
        }
    }
    copy_compute_interests(key->s, c);
    copy_compute_interests(key->s, c->other);
    if (c->duplex == OP_NOOP && c->other->duplex == OP_NOOP) {
        return DONE;
    }
    return COPY;
}

unsigned
copy_write(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct copy   *c = copy_ptr(s, key->fd);

    size_t   size;
    uint8_t *ptr = buffer_read_ptr(c->wb, &size);
    const ssize_t n = send(key->fd, ptr, size, 0);
    if (n > 0) {
        buffer_read_adv(c->wb, n);
        metrics_add_bytes((uint64_t)n);
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        c->duplex &= ~OP_WRITE;
        if (*c->other->fd != -1) {
            shutdown(*c->other->fd, SHUT_RD);
            c->other->duplex &= ~OP_READ;
        }
    }
    copy_compute_interests(key->s, c);
    copy_compute_interests(key->s, c->other);
    if (c->duplex == OP_NOOP && c->other->duplex == OP_NOOP) {
        return DONE;
    }
    return COPY;
}
