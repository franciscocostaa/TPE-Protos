#ifndef SOCKS5_INTERNAL_H
#define SOCKS5_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <netdb.h>
#include <sys/socket.h>

#include "buffer.h"
#include "selector.h"
#include "socks5.h"
#include "stm.h"
#include "users.h"

/** longitudes maximas tomadas del protocolo */
#define SOCKS5_FQDN_MAX 0xFF
#define SOCKS5_PORT_STR_MAX 6   /* "65535" + '\0' */

/** estados de la maquina (el orden DEBE coincidir con el indice en la tabla) */
enum socks5_state {
    HELLO_READ = 0,
    HELLO_WRITE,
    AUTH_READ,
    AUTH_WRITE,
    REQUEST_READ,
    REQUEST_RESOLV,
    REQUEST_CONNECTING,
    REQUEST_WRITE,
    COPY,
    DONE,
    ERROR,
};

/** Estado de una de las dos mitades del relay bidireccional. */
struct copy {
    int          *fd;
    buffer       *rb;
    buffer       *wb;
    fd_interest   duplex;
    struct copy  *other;
};

/** estructura por conexion: agrupa todo en una unica alocacion */
struct socks5 {
    int                     client_fd;
    struct sockaddr_storage client_addr;
    socklen_t               client_addr_len;

    int                     origin_fd;

    char                    dest_fqdn[SOCKS5_FQDN_MAX + 1];
    char                    dest_port[SOCKS5_PORT_STR_MAX];

    struct addrinfo        *origin_resolution;
    struct addrinfo        *origin_resolution_current;

    uint8_t                 reply_status;

    char                    username[USERS_NAME_MAX + 1];
    bool                    authenticated;
    bool                    auth_success;

    struct state_machine    stm;

    union {
        struct { uint8_t method; } hello;
        struct copy                copy;
    } client;

    union {
        struct copy copy;
    } orig;

    uint8_t                 raw_buff_a[SOCKS5_BUFFER_SIZE];
    uint8_t                 raw_buff_b[SOCKS5_BUFFER_SIZE];
    buffer                  read_buffer;
    buffer                  write_buffer;

    unsigned                references;
    struct socks5          *next;
};

/** handler que usan tanto el client_fd como el origin_fd */
extern const struct fd_handler socks5_handler;

/** obtiene la `struct socks5` adjunta a una llave de seleccion */
#define ATTACHMENT(key) ((struct socks5 *)(key)->data)

unsigned hello_read(struct selector_key *key);
unsigned hello_write(struct selector_key *key);

unsigned auth_read(struct selector_key *key);
unsigned auth_write(struct selector_key *key);

unsigned request_read(struct selector_key *key);
void request_resolv_init(unsigned state, struct selector_key *key);
unsigned request_resolv_done(struct selector_key *key);
unsigned connecting_write(struct selector_key *key);
unsigned request_write(struct selector_key *key);

void copy_init(struct socks5 *s);
unsigned copy_read(struct selector_key *key);
unsigned copy_write(struct selector_key *key);

#endif
