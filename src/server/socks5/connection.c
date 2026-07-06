#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netdb.h>
#include <sys/socket.h>

#include "metrics.h"
#include "socks5/internal.h"

/** cantidad maxima de estructuras `socks5` cacheadas para reuso */
#define SOCKS5_POOL_MAX 50

static struct socks5 *pool      = NULL;
static unsigned       pool_size = 0;

static void socksv5_read (struct selector_key *key);
static void socksv5_write(struct selector_key *key);
static void socksv5_block(struct selector_key *key);
static void socksv5_close(struct selector_key *key);
static void socksv5_done (struct selector_key *key);

static void socks5_destroy(struct socks5 *s);
static const struct state_definition *socks5_describe_states(void);

const struct fd_handler socks5_handler = {
    .handle_read  = socksv5_read,
    .handle_write = socksv5_write,
    .handle_block = socksv5_block,
    .handle_close = socksv5_close,
};

static struct socks5 *
socks5_new(const int client_fd) {
    struct socks5 *s;
    if (pool != NULL) {
        s    = pool;
        pool = s->next;
        pool_size--;
    } else {
        s = malloc(sizeof(*s));
        if (s == NULL) {
            return NULL;
        }
    }
    memset(s, 0, sizeof(*s));

    s->client_fd  = client_fd;
    s->origin_fd  = -1;
    s->references = 1;

    buffer_init(&s->read_buffer,  sizeof(s->raw_buff_a), s->raw_buff_a);
    buffer_init(&s->write_buffer, sizeof(s->raw_buff_b), s->raw_buff_b);

    s->stm.initial   = HELLO_READ;
    s->stm.max_state = ERROR;
    s->stm.states    = socks5_describe_states();
    stm_init(&s->stm);

    return s;
}

static void
socks5_destroy(struct socks5 *s) {
    if (s == NULL) {
        return;
    }
    if (s->references > 1) {
        s->references--;
        return;
    }
    if (s->origin_resolution != NULL) {
        freeaddrinfo(s->origin_resolution);
        s->origin_resolution = NULL;
    }
    if (pool_size < SOCKS5_POOL_MAX) {
        s->next   = pool;
        pool      = s;
        pool_size++;
    } else {
        free(s);
    }
}

void
socksv5_pool_destroy(void) {
    struct socks5 *s = pool;
    while (s != NULL) {
        struct socks5 *next = s->next;
        free(s);
        s = next;
    }
    pool      = NULL;
    pool_size = 0;
}

void
socksv5_passive_accept(struct selector_key *key) {
    struct sockaddr_storage client_addr;
    socklen_t               client_addr_len = sizeof(client_addr);

    const int client = accept(key->fd, (struct sockaddr *) &client_addr, &client_addr_len);
    if (client == -1) {
        return;
    }
    if (selector_fd_set_nio(client) == -1) {
        close(client);
        return;
    }

    struct socks5 *s = socks5_new(client);
    if (s == NULL) {
        close(client);
        return;
    }
    memcpy(&s->client_addr, &client_addr, client_addr_len);
    s->client_addr_len = client_addr_len;

    if (selector_register(key->s, client, &socks5_handler, OP_READ, s) != SELECTOR_SUCCESS) {
        socks5_destroy(s);
        close(client);
        return;
    }
    metrics_connection_open();
}

static void
socksv5_read(struct selector_key *key) {
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const unsigned st = stm_handler_read(stm, key);
    if (st == ERROR || st == DONE) {
        socksv5_done(key);
    }
}

static void
socksv5_write(struct selector_key *key) {
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const unsigned st = stm_handler_write(stm, key);
    if (st == ERROR || st == DONE) {
        socksv5_done(key);
    }
}

static void
socksv5_block(struct selector_key *key) {
    struct state_machine *stm = &ATTACHMENT(key)->stm;
    const unsigned st = stm_handler_block(stm, key);
    if (st == ERROR || st == DONE) {
        socksv5_done(key);
    }
}

static void
socksv5_close(struct selector_key *key) {
    socks5_destroy(ATTACHMENT(key));
}

static void
socksv5_done(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    metrics_connection_close();
    const int fds[] = { s->client_fd, s->origin_fd };
    for (unsigned i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (fds[i] != -1) {
            if (selector_unregister_fd(key->s, fds[i]) != SELECTOR_SUCCESS) {
                abort();
            }
            close(fds[i]);
        }
    }
}

static const struct state_definition socks5_states[] = {
    {
        .state         = HELLO_READ,
        .on_read_ready = hello_read,
    }, {
        .state          = HELLO_WRITE,
        .on_write_ready = hello_write,
    }, {
        .state         = AUTH_READ,
        .on_read_ready = auth_read,
    }, {
        .state          = AUTH_WRITE,
        .on_write_ready = auth_write,
    }, {
        .state         = REQUEST_READ,
        .on_read_ready = request_read,
    }, {
        .state          = REQUEST_RESOLV,
        .on_arrival     = request_resolv_init,
        .on_block_ready = request_resolv_done,
    }, {
        .state          = REQUEST_CONNECTING,
        .on_write_ready = connecting_write,
    }, {
        .state          = REQUEST_WRITE,
        .on_write_ready = request_write,
    }, {
        .state          = COPY,
        .on_read_ready  = copy_read,
        .on_write_ready = copy_write,
    }, {
        .state = DONE,
    }, {
        .state = ERROR,
    },
};

static const struct state_definition *
socks5_describe_states(void) {
    return socks5_states;
}
