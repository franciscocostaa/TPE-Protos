/**
 * mgmt.c - TRANSPORTE del protocolo de monitoreo (SMP). Ver mgmt.h / PROTOCOL.md.
 *
 * Dueño: Persona B. Atiende el 2º socket pasivo en el mismo selector no
 * bloqueante que el proxy. Cada conexión administrativa es una instancia de
 * `struct mgmt` manejada por una máquina de estados (stm):
 *
 *   GREETING ─▶ READ ─▶ WRITE ─▶ (READ | DONE)
 *
 *   - GREETING: envía el saludo "+OK SMP/1.0 ready".
 *   - READ:     acumula bytes hasta tener una línea completa (CRLF) y la despacha.
 *   - WRITE:    envía la respuesta encolada; vuelve a READ, o cierra si fue QUIT.
 *
 * Este archivo SOLO se ocupa del transporte y del framing de líneas (tolerante
 * a lecturas/escrituras parciales). La semántica de cada comando vive en
 * mgmt_cmd.c. El wire (literales) vive en mgmt_proto.h.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

#include "mgmt/mgmt.h"
#include "mgmt/mgmt_cmd.h"
#include "mgmt/mgmt_proto.h"
#include "buffer.h"
#include "stm.h"

/** El buffer de lectura debe poder contener una línea completa (CRLF incluido). */
#define MGMT_READ_BUFFER_SIZE  MGMT_LINE_MAX
/** Holgado para respuestas (incluidas las multilínea chicas de MF1). */
#define MGMT_WRITE_BUFFER_SIZE 4096

/** Estados de la máquina. El último (MGMT_ERROR) es `max_state` para el stm. */
enum mgmt_state {
    MGMT_GREETING = 0,
    MGMT_READ,
    MGMT_WRITE,
    MGMT_DONE,
    MGMT_ERROR,
};

/** Estructura por conexión administrativa (una única alocación). */
struct mgmt {
    int                     client_fd;
    struct sockaddr_storage client_addr;
    socklen_t               client_addr_len;

    struct mgmt_session     session;
    struct state_machine    stm;

    /* qué hacer luego de enviar la respuesta en curso (continuar vs cerrar) */
    mgmt_disposition        pending;

    uint8_t                 raw_read [MGMT_READ_BUFFER_SIZE];
    uint8_t                 raw_write[MGMT_WRITE_BUFFER_SIZE];
    buffer                  read_buffer;
    buffer                  write_buffer;
};

#define ATTACHMENT(key) ((struct mgmt *)(key)->data)

/* ---- forward ------------------------------------------------------------- */
static void mgmt_read_handler (struct selector_key *key);
static void mgmt_write_handler(struct selector_key *key);
static void mgmt_close_handler(struct selector_key *key);
static void mgmt_done         (struct selector_key *key);

static const struct state_definition *mgmt_describe_states(void);

/** handler del selector para la conexión administrativa */
static const struct fd_handler mgmt_handler = {
    .handle_read  = mgmt_read_handler,
    .handle_write = mgmt_write_handler,
    .handle_close = mgmt_close_handler,
};

/* ---- ciclo de vida ------------------------------------------------------- */

static struct mgmt *
mgmt_new(const int client_fd) {
    struct mgmt *m = malloc(sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    memset(m, 0, sizeof(*m));
    m->client_fd = client_fd;
    m->pending   = MGMT_DISP_CONTINUE;

    buffer_init(&m->read_buffer,  sizeof(m->raw_read),  m->raw_read);
    buffer_init(&m->write_buffer, sizeof(m->raw_write), m->raw_write);

    /* El saludo se envía apenas el socket esté escribible (estado inicial). */
    mgmt_cmd_emit_greeting(&m->write_buffer);

    m->stm.initial   = MGMT_GREETING;
    m->stm.max_state = MGMT_ERROR;
    m->stm.states    = mgmt_describe_states();
    stm_init(&m->stm);

    return m;
}

void
mgmt_passive_accept(struct selector_key *key) {
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

    struct mgmt *m = mgmt_new(client);
    if (m == NULL) {
        close(client);
        return;
    }
    memcpy(&m->client_addr, &client_addr, client_addr_len);
    m->client_addr_len = client_addr_len;

    /* Arrancamos interesados en ESCRIBIR: lo primero es mandar el saludo. */
    if (selector_register(key->s, client, &mgmt_handler, OP_WRITE, m) != SELECTOR_SUCCESS) {
        free(m);
        close(client);
        return;
    }
}

/** desregistra y cierra el fd (el free ocurre en mgmt_close_handler) */
static void
mgmt_done(struct selector_key *key) {
    const int fd = ATTACHMENT(key)->client_fd;
    if (fd != -1) {
        if (selector_unregister_fd(key->s, fd) != SELECTOR_SUCCESS) {
            abort();
        }
        close(fd);
    }
}

static void
mgmt_close_handler(struct selector_key *key) {
    free(ATTACHMENT(key));
}

/* ---- handlers top-level: delegan en la máquina de estados ---------------- */

static void
mgmt_read_handler(struct selector_key *key) {
    const unsigned st = stm_handler_read(&ATTACHMENT(key)->stm, key);
    if (st == MGMT_DONE || st == MGMT_ERROR) {
        mgmt_done(key);
    }
}

static void
mgmt_write_handler(struct selector_key *key) {
    const unsigned st = stm_handler_write(&ATTACHMENT(key)->stm, key);
    if (st == MGMT_DONE || st == MGMT_ERROR) {
        mgmt_done(key);
    }
}

/* ---- framing de líneas --------------------------------------------------- */

/**
 * Si hay una línea completa (terminada en CRLF) en `b`, la copia sin el CRLF y
 * NUL-terminada en `out`, avanza el buffer y devuelve el largo (>= 0). Si aún
 * no hay línea completa devuelve -1. Si la línea no entra en `out` devuelve -2.
 */
static int
mgmt_take_line(buffer *b, char *out, size_t outsz) {
    size_t   avail;
    uint8_t *data = buffer_read_ptr(b, &avail);
    for (size_t i = 0; i + 1 < avail; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            if (i >= outsz) {
                return -2;               /* no entra (sin contar el NUL) */
            }
            memcpy(out, data, i);
            out[i] = '\0';
            buffer_read_adv(b, (ssize_t) (i + 2));
            return (int) i;
        }
    }
    return -1;                            /* línea incompleta: falta más data */
}

/* ---- estados ------------------------------------------------------------- */

/** GREETING: envía el saludo inicial y pasa a leer comandos. */
static unsigned
mgmt_on_greeting(struct selector_key *key) {
    struct mgmt *m = ATTACHMENT(key);
    buffer      *b = &m->write_buffer;

    size_t        size;
    uint8_t      *ptr = buffer_read_ptr(b, &size);
    const ssize_t n   = send(key->fd, ptr, size, 0);
    if (n == -1) {
        return MGMT_ERROR;
    }
    buffer_read_adv(b, n);
    if (buffer_can_read(b)) {
        return MGMT_GREETING;            /* falta enviar parte del saludo */
    }
    selector_set_interest_key(key, OP_READ);
    return MGMT_READ;
}

/** READ: acumula bytes, extrae una línea y la despacha. Tolera lecturas parciales. */
static unsigned
mgmt_on_read(struct selector_key *key) {
    struct mgmt *m = ATTACHMENT(key);
    buffer      *b = &m->read_buffer;
    char         line[MGMT_LINE_MAX];

    /* 1) ¿ya quedó una línea completa de un recv anterior? (pipelining) */
    int len = mgmt_take_line(b, line, sizeof(line));
    if (len == -1) {
        /* 2) no: leer del socket */
        size_t   space;
        uint8_t *ptr = buffer_write_ptr(b, &space);
        if (space == 0) {
            /* buffer lleno sin CRLF => línea más larga que el máximo */
            mgmt_cmd_emit_error(&m->write_buffer, MGMT_ERR_SYNTAX, "line too long");
            m->pending = MGMT_DISP_CLOSE;
            selector_set_interest_key(key, OP_WRITE);
            return MGMT_WRITE;
        }
        const ssize_t n = recv(key->fd, ptr, space, 0);
        if (n <= 0) {
            return MGMT_DONE;            /* EOF del cliente o error de socket */
        }
        buffer_write_adv(b, n);

        len = mgmt_take_line(b, line, sizeof(line));
        if (len == -1) {
            return MGMT_READ;            /* sigue incompleta, esperamos más */
        }
    }
    if (len == -2) {
        mgmt_cmd_emit_error(&m->write_buffer, MGMT_ERR_SYNTAX, "line too long");
        m->pending = MGMT_DISP_CLOSE;
        selector_set_interest_key(key, OP_WRITE);
        return MGMT_WRITE;
    }

    m->pending = mgmt_cmd_dispatch(&m->session, line, &m->write_buffer);
    selector_set_interest_key(key, OP_WRITE);
    return MGMT_WRITE;
}

/** WRITE: envía la respuesta encolada. Tolera escrituras parciales. */
static unsigned
mgmt_on_write(struct selector_key *key) {
    struct mgmt *m = ATTACHMENT(key);
    buffer      *b = &m->write_buffer;

    size_t        size;
    uint8_t      *ptr = buffer_read_ptr(b, &size);
    const ssize_t n   = send(key->fd, ptr, size, 0);
    if (n == -1) {
        return MGMT_ERROR;
    }
    buffer_read_adv(b, n);
    if (buffer_can_read(b)) {
        return MGMT_WRITE;               /* falta enviar parte de la respuesta */
    }
    if (m->pending == MGMT_DISP_CLOSE) {
        return MGMT_DONE;
    }
    buffer_compact(b);
    selector_set_interest_key(key, OP_READ);
    return MGMT_READ;
}

/* ---- tabla de estados ---------------------------------------------------- */

static const struct state_definition mgmt_states[] = {
    {
        .state          = MGMT_GREETING,
        .on_write_ready = mgmt_on_greeting,
    }, {
        .state         = MGMT_READ,
        .on_read_ready = mgmt_on_read,
    }, {
        .state          = MGMT_WRITE,
        .on_write_ready = mgmt_on_write,
    }, {
        .state = MGMT_DONE,
    }, {
        .state = MGMT_ERROR,
    },
};

static const struct state_definition *
mgmt_describe_states(void) {
    return mgmt_states;
}
