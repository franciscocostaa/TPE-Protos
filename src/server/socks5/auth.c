#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "socks5/internal.h"

/** constantes de la subnegociacion username/password (RFC 1929) */
#define SOCKS5_AUTH_VERSION 0x01
#define SOCKS5_AUTH_SUCCESS 0x00
#define SOCKS5_AUTH_FAILURE 0x01

static bool
auth_marshall_reply(struct socks5 *s, uint8_t status) {
    buffer *b = &s->write_buffer;
    buffer_reset(b);

    size_t   space;
    uint8_t *p = buffer_write_ptr(b, &space);
    if (space < 2) {
        return false;
    }
    p[0] = SOCKS5_AUTH_VERSION;
    p[1] = status;
    buffer_write_adv(b, 2);
    return true;
}

unsigned
auth_read(struct selector_key *key) {
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
        return AUTH_READ;
    }

    const uint8_t ver  = data[0];
    const uint8_t ulen = data[1];
    if (ver != SOCKS5_AUTH_VERSION || ulen == 0) {
        s->auth_success = false;
        if (!auth_marshall_reply(s, SOCKS5_AUTH_FAILURE)) {
            return ERROR;
        }
        selector_set_interest_key(key, OP_WRITE);
        return AUTH_WRITE;
    }

    /* El paquete RFC 1929 trae PLEN recien despues de UNAME. Esperamos hasta
     * tener ese byte para calcular el largo total sin leer fuera del buffer. */
    if (avail < (size_t)(2 + ulen + 1)) {
        return AUTH_READ;
    }

    const uint8_t plen = data[2 + ulen];
    if (plen == 0) {
        s->auth_success = false;
        if (!auth_marshall_reply(s, SOCKS5_AUTH_FAILURE)) {
            return ERROR;
        }
        selector_set_interest_key(key, OP_WRITE);
        return AUTH_WRITE;
    }

    const size_t needed = 2 + ulen + 1 + plen;
    if (avail < needed) {
        return AUTH_READ;
    }

    char username[USERS_NAME_MAX + 1];
    char password[USERS_PASS_MAX + 1];
    memcpy(username, &data[2], ulen);
    username[ulen] = '\0';
    memcpy(password, &data[2 + ulen + 1], plen);
    password[plen] = '\0';

    s->auth_success = users_authenticate(username, password);
    s->authenticated = s->auth_success;
    if (s->auth_success) {
        strncpy(s->username, username, sizeof(s->username) - 1);
    }
    buffer_read_adv(b, needed);

    if (!auth_marshall_reply(s, s->auth_success ? SOCKS5_AUTH_SUCCESS : SOCKS5_AUTH_FAILURE)) {
        return ERROR;
    }
    selector_set_interest_key(key, OP_WRITE);
    return AUTH_WRITE;
}

unsigned
auth_write(struct selector_key *key) {
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
        return AUTH_WRITE;
    }
    if (!s->auth_success) {
        return ERROR;
    }
    selector_set_interest_key(key, OP_READ);
    return REQUEST_READ;
}
