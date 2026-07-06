#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "config.h"
#include "socks5/internal.h"

unsigned
hello_read(struct selector_key *key) {
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

    size_t   avail;
    uint8_t *data = buffer_read_ptr(b, &avail);
    if (avail < 2) {
        return HELLO_READ;
    }
    if (data[0] != SOCKS5_VERSION) {
        return ERROR;
    }

    const uint8_t nmethods = data[1];
    if (avail < (size_t)(2 + nmethods)) {
        return HELLO_READ;
    }

    bool no_auth   = false;
    bool user_pass = false;
    for (uint8_t i = 0; i < nmethods; i++) {
        if (data[2 + i] == SOCKS5_METHOD_NO_AUTH) {
            no_auth = true;
        } else if (data[2 + i] == SOCKS5_METHOD_USER_PASS) {
            user_pass = true;
        }
    }
    buffer_read_adv(b, 2 + nmethods);

    const struct config cfg = config_get();
    uint8_t method = SOCKS5_METHOD_NO_ACCEPTABLE;
    if (cfg.auth_required) {
        /* Si la config exige autenticacion, no aceptamos NO-AUTH aunque el cliente lo ofrezca. */
        method = user_pass ? SOCKS5_METHOD_USER_PASS : SOCKS5_METHOD_NO_ACCEPTABLE;
    } else if (no_auth) {
        /* Cuando no se exige auth, preferimos el metodo mas simple y compatible. */
        method = SOCKS5_METHOD_NO_AUTH;
    } else if (user_pass) {
        method = SOCKS5_METHOD_USER_PASS;
    }
    s->client.hello.method = method;

    size_t   wspace;
    uint8_t *w = buffer_write_ptr(&s->write_buffer, &wspace);
    if (wspace < 2) {
        return ERROR;
    }
    w[0] = SOCKS5_VERSION;
    w[1] = method;
    buffer_write_adv(&s->write_buffer, 2);

    selector_set_interest_key(key, OP_WRITE);
    return HELLO_WRITE;
}

unsigned
hello_write(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    buffer        *b = &s->write_buffer;

    size_t   size;
    uint8_t *ptr = buffer_read_ptr(b, &size);
    const ssize_t n = send(key->fd, ptr, size, 0);
    if (n == -1) {
        return ERROR;
    }
    buffer_read_adv(b, n);
    if (buffer_can_read(b)) {
        return HELLO_WRITE;
    }
    if (s->client.hello.method == SOCKS5_METHOD_NO_ACCEPTABLE) {
        return ERROR;
    }
    selector_set_interest_key(key, OP_READ);
    if (s->client.hello.method == SOCKS5_METHOD_USER_PASS) {
        return AUTH_READ;
    }
    return REQUEST_READ;
}
