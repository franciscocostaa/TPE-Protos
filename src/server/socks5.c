/**
 * socks5.c - máquina de estados que maneja una conexión de un proxy SOCKSv5
 *            con sockets no bloqueantes (RFC 1928, método NO-AUTH).
 *
 * Cada conexión aceptada se modela como una instancia de `struct socks5` con
 * su propia `state_machine`. El selector entrega los eventos de I/O y la MEF
 * decide qué hacer y a qué estado transicionar:
 *
 *   HELLO_READ -> HELLO_WRITE -> REQUEST_READ -> [REQUEST_RESOLV] ->
 *   REQUEST_CONNECTING -> REQUEST_WRITE -> COPY -> DONE
 *
 * El único trabajo bloqueante (getaddrinfo para FQDN) se descarga en un hilo
 * que, al terminar, despierta al selector con `selector_notify_block`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "buffer.h"
#include "stm.h"
#include "selector.h"
#include "socks5.h"
#include "metrics.h"
#include "users.h"
#include "config.h"

/** cantidad máxima de estructuras `socks5` cacheadas para reuso */
#define SOCKS5_POOL_MAX 50

/** longitudes máximas tomadas del protocolo */
#define SOCKS5_FQDN_MAX 0xFF
#define SOCKS5_PORT_STR_MAX 6   /* "65535" + '\0' */

/** constantes de la subnegociación username/password (RFC 1929) */
#define SOCKS5_AUTH_VERSION 0x01
#define SOCKS5_AUTH_SUCCESS 0x00
#define SOCKS5_AUTH_FAILURE 0x01

/** estados de la máquina (el orden DEBE coincidir con el índice en la tabla) */
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

/**
 * Estado de una de las dos mitades del relay bidireccional.
 *
 *   rb: buffer donde se LEE lo que llega por `fd`.
 *   wb: buffer desde donde se ESCRIBE hacia `fd`.
 *   duplex: direcciones todavía abiertas (OP_READ | OP_WRITE).
 *   other: la mitad opuesta.
 */
struct copy {
    int          *fd;
    buffer       *rb;
    buffer       *wb;
    fd_interest   duplex;
    struct copy  *other;
};

/** estructura por conexión: agrupa todo en una única alocación */
struct socks5 {
    /* cliente */
    int                     client_fd;
    struct sockaddr_storage client_addr;
    socklen_t               client_addr_len;

    /* origen (servidor destino) */
    int                     origin_fd;

    /* destino pedido por el cliente */
    char                    dest_fqdn[SOCKS5_FQDN_MAX + 1];
    char                    dest_port[SOCKS5_PORT_STR_MAX];

    /* resolución DNS y su iterador para el fallback de direcciones */
    struct addrinfo        *origin_resolution;
    struct addrinfo        *origin_resolution_current;

    /* código REP a informar al cliente en la respuesta del request */
    uint8_t                 reply_status;

    /* resultado de la autenticación RFC 1929; username se usa luego para logs */
    char                    username[USERS_NAME_MAX + 1];
    bool                    authenticated;
    bool                    auth_success;

    /* máquina de estados */
    struct state_machine    stm;

    /* estados específicos del client_fd */
    union {
        struct { uint8_t method; } hello;
        struct copy                copy;
    } client;

    /* estados específicos del origin_fd */
    union {
        struct copy copy;
    } orig;

    /* buffers de I/O: a = cliente->origen, b = origen->cliente */
    uint8_t                 raw_buff_a[SOCKS5_BUFFER_SIZE];
    uint8_t                 raw_buff_b[SOCKS5_BUFFER_SIZE];
    buffer                  read_buffer;
    buffer                  write_buffer;

    /* conteo de referencias: cantidad de fds registrados que apuntan acá */
    unsigned                references;

    /* enlace para el pool de reuso */
    struct socks5          *next;
};

/* ---- pool de objetos ----------------------------------------------------- */

static struct socks5 *pool      = NULL;
static unsigned       pool_size = 0;

/* ---- declaraciones forward ----------------------------------------------- */

static void     socksv5_read  (struct selector_key *key);
static void     socksv5_write (struct selector_key *key);
static void     socksv5_block (struct selector_key *key);
static void     socksv5_close (struct selector_key *key);
static void     socksv5_done  (struct selector_key *key);

static void     socks5_destroy(struct socks5 *s);

static unsigned request_connect       (fd_selector s, struct socks5 *st);
static void     request_marshall_reply(struct socks5 *s);
static void     copy_init             (struct socks5 *s);
static bool     auth_marshall_reply   (struct socks5 *s, uint8_t status);

/** handler que usan tanto el client_fd como el origin_fd */
static const struct fd_handler socks5_handler = {
    .handle_read  = socksv5_read,
    .handle_write = socksv5_write,
    .handle_block = socksv5_block,
    .handle_close = socksv5_close,
};

/** obtiene la `struct socks5` adjunta a una llave de selección */
#define ATTACHMENT(key) ((struct socks5 *)(key)->data)

/* ---- ciclo de vida del objeto -------------------------------------------- */

static const struct state_definition *socks5_describe_states(void);

/** crea (o reusa del pool) una estructura para una nueva conexión */
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

/** destruye la estructura teniendo en cuenta las referencias y el pool */
static void
socks5_destroy(struct socks5 *s) {
    if (s == NULL) {
        return;
    }
    if (s->references > 1) {
        s->references--;
        return;
    }
    // última referencia: liberamos recursos y devolvemos al pool (o free).
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

/* ---- aceptación de conexiones -------------------------------------------- */

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

/* ---- handlers top-level: delegan en la máquina de estados ---------------- */

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

/** cierra y desregistra ambos fds de la conexión */
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

/* ---- HELLO (negociación de método, RFC 1928 §3) -------------------------- */

static unsigned
hello_read(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    buffer        *b = &s->read_buffer;

    size_t   space;
    uint8_t *ptr = buffer_write_ptr(b, &space);
    if (space == 0) {
        return ERROR;   // saludo más grande que el buffer: cliente inválido
    }
    //busco que se escriban bytes nuveos en el buffer, si no hay bytes nuevos, es porque el cliente cerró la conexión
    const ssize_t n = recv(key->fd, ptr, space, 0);
    if (n <= 0) {
        return ERROR;
    }
    buffer_write_adv(b, n);
    //en esta parte lo q hago es consultar cuantos bytes hay disponibles para leer en el buffer, y si hay menos de 2, es porque faltan VER y NMETHODS, entonces devuelvo HELLO_READ
    size_t   avail;
    uint8_t *data = buffer_read_ptr(b, &avail);
    if (avail < 2) {
        return HELLO_READ;          // faltan VER y NMETHODS
    }
    if (data[0] != SOCKS5_VERSION) {
        return ERROR;
    }
    // el saludo completo tiene: 2 bytes iniciales + nmethods bytes
    const uint8_t nmethods = data[1];
    if (avail < (size_t)(2 + nmethods)) {
        return HELLO_READ;          // faltan métodos por llegar
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
    //ahora si muevo el puntero de lectura del buffer, porque ya leímos el saludo completo
    buffer_read_adv(b, 2 + nmethods);

    const struct config cfg = config_get();
    uint8_t method = SOCKS5_METHOD_NO_ACCEPTABLE;
    if (cfg.auth_required) {
        /* Si la config exige autenticación, no aceptamos NO-AUTH aunque el cliente lo ofrezca. */
        method = user_pass ? SOCKS5_METHOD_USER_PASS : SOCKS5_METHOD_NO_ACCEPTABLE;
    } else if (no_auth) {
        /* Cuando no se exige auth, preferimos el método más simple y compatible. */
        method = SOCKS5_METHOD_NO_AUTH;
    } else if (user_pass) {
        method = SOCKS5_METHOD_USER_PASS;
    }
    s->client.hello.method = method;

    // preparamos la respuesta VER + METHOD
    size_t   wspace;
    uint8_t *w = buffer_write_ptr(&s->write_buffer, &wspace);
    if (wspace < 2) {
        return ERROR;
    }
    w[0] = SOCKS5_VERSION;
    w[1] = method;
    //con esta funcion, le aviso al buffer de escritura que acabo de cargar 2 bytes para enviar
    buffer_write_adv(&s->write_buffer, 2);
    // le digo al selector: no qiuero seguir leyendo, quiero escribir la respuesta al saludo, entonces cambio el interest a OP_WRITE
    //avisame cuando pueda escribirle la respuesta al cliente.
    selector_set_interest_key(key, OP_WRITE);
    return HELLO_WRITE;
}

static unsigned
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
        return HELLO_WRITE;         // faltan bytes por enviar, puede suceder que no se envien todos por eso
    }
    if (s->client.hello.method == SOCKS5_METHOD_NO_ACCEPTABLE) {
        return ERROR;               // no aceptamos ningún método del cliente
    }
    selector_set_interest_key(key, OP_READ);
    if (s->client.hello.method == SOCKS5_METHOD_USER_PASS) {
        return AUTH_READ;
    }
    //situacion no auth, entonces pasamos a leer el request del cliente
    return REQUEST_READ;
}

/* ---- AUTH (subnegociación username/password, RFC 1929) ------------------- */

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

static unsigned
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
        return AUTH_READ;           // faltan VER y ULEN
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

    /* El paquete RFC 1929 trae PLEN recién después de UNAME. Esperamos hasta
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
        selector_set_interest_key(key, OP_WRITE); //avisame cuando ewsta listo el cliente para que le escriba
        return AUTH_WRITE;
    }

    const size_t needed = 2 + ulen + 1 + plen;
    if (avail < needed) {
        return AUTH_READ;           // faltan bytes de password
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

static unsigned
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
        return AUTH_WRITE;          // respuesta de auth parcialmente escrita
    }
    if (!s->auth_success) {
        return ERROR;               // RFC 1929: tras failure cerramos la conexión
    }
    selector_set_interest_key(key, OP_READ);
    return REQUEST_READ;
}

/* ---- REQUEST (pedido de conexión, RFC 1928) --------------------------- */

/** intenta parsear el request acumulado en el buffer; tolera lecturas parciales */
static unsigned
request_parse(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    buffer        *b = &s->read_buffer;

    size_t   avail;
    uint8_t *d = buffer_read_ptr(b, &avail);
    if (avail < 4) {
        return REQUEST_READ;        // faltan VER, CMD, RSV, ATYP
    }
    const uint8_t ver  = d[0];
    const uint8_t cmd  = d[1];
    const uint8_t atyp = d[3];
    if (ver != SOCKS5_VERSION) {
        return ERROR;
    }

    // longitud total del request según el tipo de dirección
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
                return REQUEST_READ;    // falta el byte de longitud del FQDN
            }
            needed = 4 + 1 + d[4] + 2;
            break;
        default:
            s->reply_status = SOCKS5_REP_ATYP_NOT_SUPPORTED;
            request_marshall_reply(s); // Arma en el write_buffer la respuesta SOCKS5 del request.
            selector_set_interest_key(key, OP_WRITE); //espero que el fed del cliente este listo para recibir la respuesta de error
            return REQUEST_WRITE;
    }
    if (avail < needed) {
        return REQUEST_READ;        // el request todavía no llegó completo
    }

    // sólo soportamos CONNECT
    if (cmd != SOCKS5_CMD_CONNECT) {
        s->reply_status = SOCKS5_REP_COMMAND_NOT_SUPPORTED;
        request_marshall_reply(s);
        buffer_read_adv(b, needed);
        selector_set_interest_key(key, OP_WRITE);
        return REQUEST_WRITE;
    }
    //es connect, entonces vamos a extraer el host y el puerto del request, para eso necesitamos leer el buffer de lectura, y dependiendo del tipo de dirección (atyp) vamos a extraer los bytes correspondientes al host y al puerto
    // extraemos host y puerto como strings para getaddrinfo
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
        default: { // SOCKS5_ATYP_DOMAIN
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
    //si es un dominio entonces tenemos que resolverlo en otro hilo, porque getaddrinfo puede bloquear, entonces vamos a pasar al estado REQUEST_RESOLV y vamos a crear un hilo que haga la resolución de DNS y cuando termine nos va a avisar con selector_notify_block
    if (is_domain) {
        // resolución asíncrona en otro hilo
        strncpy(s->dest_fqdn, host, sizeof(s->dest_fqdn) - 1);
        selector_set_interest_key(key, OP_NOOP); //avisame cuando termine la resolución de DNS, no quiero seguir leyendo ni escribiendo hasta que termine la resolución
        return REQUEST_RESOLV;
    }

    // IP literal: getaddrinfo con AI_NUMERICHOST no consulta la red (no bloquea)
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

static unsigned
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

/* ---- RESOLV (resolución de DNS en un hilo aparte) ------------------------ */

static void *
request_resolv_blocking(void *arg) {
    struct selector_key *key = arg;
    struct socks5       *s   = ATTACHMENT(key);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Si falla, origin_resolution queda en NULL y lo maneja request_resolv_done.
    if (getaddrinfo(s->dest_fqdn, s->dest_port, &hints, &s->origin_resolution) != 0) {
        s->origin_resolution = NULL;
    }

    selector_notify_block(key->s, key->fd);
    free(key);
    return NULL;
}

static void
request_resolv_init(const unsigned state, struct selector_key *key) {
    (void) state;
    struct selector_key *blocking_key = malloc(sizeof(*blocking_key)); //creamos una copia de la key para pasarla al hilo, porque el hilo va a necesitar la key para notificar al selector cuando termine la resolución de DNS
    if (blocking_key == NULL) {//malloc falló, no podemos lanzar el hilo, entonces no podemos resolver el DNS, entonces ponemos origin_resolution en NULL y notificamos al selector que despierte para que maneje el error
        // sin poder lanzar el hilo, despertamos sin resolución -> error.
        ATTACHMENT(key)->origin_resolution = NULL;
        selector_notify_block(key->s, key->fd);
        return;
    }
    *blocking_key = *key;

    pthread_t tid;
    //crea el hilo que va a ejecutar la función request_resolv_blocking, que va a hacer la resolución de DNS y cuando termine va a notificar al selector que despierte para que maneje el resultado de la resolución
    if (pthread_create(&tid, NULL, request_resolv_blocking, blocking_key) != 0) {
        free(blocking_key);
        ATTACHMENT(key)->origin_resolution = NULL;
        selector_notify_block(key->s, key->fd);
        return;
    }
    pthread_detach(tid);
}

static unsigned
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

/* ---- CONNECTING (conexión no bloqueante con fallback de direcciones) ------ */

/**
 * Intenta conectar a la próxima dirección candidata. Si una falla, prueba con
 * la siguiente (robustez ante FQDN con varias IPs). Registra el origin_fd con
 * OP_WRITE para detectar el fin del connect() no bloqueante.
 *
 * Retorna el próximo estado de la MEF.
 */
static unsigned
request_connect(fd_selector s, struct socks5 *st) {
    while (st->origin_resolution_current != NULL) {
        struct addrinfo *ai          = st->origin_resolution_current;
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
        close(st->origin_fd);
        st->origin_fd = -1;
    }

    // ninguna dirección candidata funcionó
    st->reply_status = SOCKS5_REP_HOST_UNREACHABLE;
    request_marshall_reply(st);
    selector_set_interest(s, st->client_fd, OP_WRITE);
    return REQUEST_WRITE;
}

/** se dispara cuando el origin_fd queda escribible: verifica si el connect anduvo */
static unsigned
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

    // esta dirección falló: la cerramos y probamos con la siguiente.
    selector_unregister_fd(key->s, s->origin_fd);
    close(s->origin_fd);
    s->origin_fd = -1;
    return request_connect(key->s, s);
}

/* ---- REQUEST_WRITE (respuesta al cliente, RFC 1928 §6) ------------------- */

/** arma la respuesta del request en write_buffer (VER REP RSV ATYP BND.ADDR PORT) */
static void
request_marshall_reply(struct socks5 *s) {
    buffer *b = &s->write_buffer;
    buffer_reset(b);

    size_t   space;
    uint8_t *p = buffer_write_ptr(b, &space);
    size_t   i = 0;

    p[i++] = SOCKS5_VERSION;
    p[i++] = s->reply_status;
    p[i++] = 0x00;  // RSV

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
        // error o no pudimos averiguar la dirección: IPv4 0.0.0.0:0
        p[i++] = SOCKS5_ATYP_IPV4;
        memset(&p[i], 0, 4 + 2); i += 4 + 2;
    }
    buffer_write_adv(b, i);
}

static unsigned
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
        return REQUEST_WRITE;       // faltan bytes de la respuesta
    }
    if (s->reply_status != SOCKS5_REP_SUCCEEDED) {
        return DONE;                // ya informamos el error: cerramos
    }

    copy_init(s);
    selector_set_interest(key->s, s->client_fd, OP_READ);
    selector_set_interest(key->s, s->origin_fd, OP_READ);
    return COPY;
}

/* ---- COPY (relay bidireccional con backpressure) ------------------------- */

static void
copy_init(struct socks5 *s) {
    struct copy *c = &s->client.copy;
    c->fd     = &s->client_fd;
    c->rb     = &s->read_buffer;    // lo que llega del cliente -> hacia el origen
    c->wb     = &s->write_buffer;   // lo que viene del origen   -> hacia el cliente
    c->duplex = OP_READ | OP_WRITE;
    c->other  = &s->orig.copy;

    struct copy *o = &s->orig.copy;
    o->fd     = &s->origin_fd;
    o->rb     = &s->write_buffer;   // lo que llega del origen   -> hacia el cliente
    o->wb     = &s->read_buffer;    // lo que viene del cliente  -> hacia el origen
    o->duplex = OP_READ | OP_WRITE;
    o->other  = &s->client.copy;
}

static struct copy *
copy_ptr(struct socks5 *s, const int fd) {
    return (fd == s->client_fd) ? &s->client.copy : &s->orig.copy;
}

/** recalcula el interés de una mitad según el espacio/datos de sus buffers */
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

static unsigned
copy_read(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct copy   *c = copy_ptr(s, key->fd);

    size_t   size;
    uint8_t *ptr = buffer_write_ptr(c->rb, &size);
    const ssize_t n = recv(key->fd, ptr, size, 0);
    if (n <= 0) {
        // EOF o error de lectura: cerramos esta lectura y la escritura del otro
        c->duplex &= ~OP_READ;
        if (*c->other->fd != -1) {
            shutdown(*c->other->fd, SHUT_WR);
            c->other->duplex &= ~OP_WRITE;
        }
    } else {
        buffer_write_adv(c->rb, n);
    }
    copy_compute_interests(key->s, c);
    copy_compute_interests(key->s, c->other);
    if (c->duplex == OP_NOOP && c->other->duplex == OP_NOOP) {
        return DONE;
    }
    return COPY;
}

static unsigned
copy_write(struct selector_key *key) {
    struct socks5 *s = ATTACHMENT(key);
    struct copy   *c = copy_ptr(s, key->fd);

    size_t   size;
    uint8_t *ptr = buffer_read_ptr(c->wb, &size);
    const ssize_t n = send(key->fd, ptr, size, 0);
    if (n == -1) {
        c->duplex &= ~OP_WRITE;
        if (*c->other->fd != -1) {
            shutdown(*c->other->fd, SHUT_RD);
            c->other->duplex &= ~OP_READ;
        }
    } else {
        buffer_read_adv(c->wb, n);
        metrics_add_bytes((uint64_t)n);
    }
    copy_compute_interests(key->s, c);
    copy_compute_interests(key->s, c->other);
    if (c->duplex == OP_NOOP && c->other->duplex == OP_NOOP) {
        return DONE;
    }
    return COPY;
}

/* ---- tabla de estados ---------------------------------------------------- */

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
