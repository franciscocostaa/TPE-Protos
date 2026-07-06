/**
 * mgmt_cmd.c - despacho de comandos del protocolo de monitoreo (ver mgmt_cmd.h).
 *
 * Comandos implementados: AUTH, GET-METRICS, LIST-USERS, ADD-USER, DEL-USER,
 * GET-CONFIG, SET-CONFIG, GET-LOG, HELP, QUIT.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <time.h>      /* gmtime_r, strftime para GET-LOG */

#include "mgmt_cmd.h"
#include "metrics.h"
#include "users.h"
#include "config.h"
#include "access_log.h"

/** Máximo de tokens que parseamos de una línea (verbo + hasta 3 argumentos). */
#define MGMT_CMD_MAX_ARGS 4

/** Buffer temporal para armar una línea de respuesta antes de encolarla. */
#define MGMT_CMD_SCRATCH  512

/** Largo máximo del token administrativo. */
#define MGMT_TOKEN_MAX 255

/**
 * Token por defecto del canal admin: rige hasta que main.c llame a
 * mgmt_cmd_set_token() con el valor de los argumentos. Es un default de
 * desarrollo; en una corrida real DEBE pasarse por línea de comandos.
 */
#define MGMT_TOKEN_DEFAULT "admin"

/* Token vigente. Único hilo (data-path del selector): sin locks. */
static char admin_token[MGMT_TOKEN_MAX + 1] = MGMT_TOKEN_DEFAULT;

/* ------------------------------------------------------------------ *
 *  Helpers de serialización
 * ------------------------------------------------------------------ */

/**
 * Encola la cadena `s` en el buffer de salida. MF1: las respuestas son chicas y
 * entran de sobra; si no hubiera espacio se trunca (se documenta como
 * limitación de MF1; el streaming de respuestas grandes llega con LIST-USERS).
 */
static void
emit_str(buffer *out, const char *s) {
    size_t   space;
    uint8_t *ptr = buffer_write_ptr(out, &space);
    size_t   len = strlen(s);
    if (len > space) {
        len = space;
    }
    memcpy(ptr, s, len);
    buffer_write_adv(out, (ssize_t) len);
}

/**
 * Encola una línea de datos de una respuesta multilínea, con dot-stuffing: si la
 * línea empieza con '.', se le antepone otro '.' para no confundirla con el
 * terminador (ver PROTOCOL.md §4.3). Agrega el CRLF.
 */
static void
emit_data_line(buffer *out, const char *s) {
    if (s[0] == MGMT_MULTILINE_END_CH) {
        emit_str(out, MGMT_MULTILINE_END);   /* '.' extra de stuffing */
    }
    emit_str(out, s);
    emit_str(out, MGMT_CRLF);
}

void
mgmt_cmd_set_token(const char *token) {
    if (token == NULL) {
        return;
    }
    snprintf(admin_token, sizeof(admin_token), "%s", token);
}

/**
 * Compara dos cadenas en tiempo constante (no corta en la primera diferencia)
 * para no filtrar el token por timing. Espeja el criterio de users.c.
 */
static bool
const_time_eq(const char *stored, const char *given) {
    const size_t  ls   = strlen(stored);
    const size_t  lg   = strlen(given);
    unsigned char diff = (unsigned char) (ls ^ lg);
    for (size_t i = 0; i < ls; i++) {
        const char gc = i < lg ? given[i] : 0;
        diff |= (unsigned char) (stored[i] ^ gc);
    }
    return diff == 0;
}

void
mgmt_cmd_emit_greeting(buffer *out) {
    emit_str(out, MGMT_RESP_OK " " MGMT_PROTO_NAME "/" MGMT_PROTO_VERSION
                  " ready" MGMT_CRLF);
}

void
mgmt_cmd_emit_error(buffer *out, mgmt_status code, const char *msg) {
    char scratch[MGMT_CMD_SCRATCH];
    snprintf(scratch, sizeof(scratch), "%s %d %s%s",
             MGMT_RESP_ERR, (int) code, msg, MGMT_CRLF);
    emit_str(out, scratch);
}

/* ------------------------------------------------------------------ *
 *  Parsing de la línea
 * ------------------------------------------------------------------ */

/** Parte `line` in-place por espacios. Devuelve la cantidad de tokens. */
static int
tokenize(char *line, char *argv[], int max) {
    int   argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " ", &save);
         tok != NULL && argc < max;
         tok = strtok_r(NULL, " ", &save)) {
        argv[argc++] = tok;
    }
    return argc;
}

/* ------------------------------------------------------------------ *
 *  Comandos
 * ------------------------------------------------------------------ */

static mgmt_disposition
cmd_auth(struct mgmt_session *session, int argc, char *argv[], buffer *out) {
    if (argc != 2) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "AUTH takes exactly one argument");
        return MGMT_DISP_CONTINUE;
    }
    if (const_time_eq(admin_token, argv[1])) {
        session->authenticated = true;
        emit_str(out, MGMT_RESP_OK " authenticated" MGMT_CRLF);
    } else {
        session->authenticated = false;
        mgmt_cmd_emit_error(out, MGMT_ERR_AUTH_FAILED, "authentication failed");
    }
    return MGMT_DISP_CONTINUE;
}

static mgmt_disposition
cmd_get_metrics(int argc, char *argv[], buffer *out) {
    (void) argv;
    if (argc != 1) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "GET-METRICS takes no arguments");
        return MGMT_DISP_CONTINUE;
    }
    const struct metrics_snapshot m = metrics_get();
    char scratch[MGMT_CMD_SCRATCH];
    snprintf(scratch, sizeof(scratch), "%s %s=%llu %s=%llu %s=%llu%s",
             MGMT_RESP_OK,
             MGMT_METRIC_CONNECTIONS_TOTAL,   (unsigned long long) m.connections_total,
             MGMT_METRIC_CONNECTIONS_CURRENT, (unsigned long long) m.connections_current,
             MGMT_METRIC_BYTES_TRANSFERRED,   (unsigned long long) m.bytes_transferred,
             MGMT_CRLF);
    emit_str(out, scratch);
    return MGMT_DISP_CONTINUE;
}

/** Traduce un resultado de users.h al código de estado del protocolo. */
static mgmt_status
map_users_result(users_result r) {
    switch (r) {
        case USERS_OK:             return MGMT_OK;
        case USERS_FULL:           return MGMT_ERR_USERS_FULL;
        case USERS_ALREADY_EXISTS: return MGMT_ERR_USER_EXISTS;
        case USERS_NOT_FOUND:      return MGMT_ERR_USER_NOT_FOUND;
        case USERS_INVALID:        return MGMT_ERR_INVALID_ARG;
        default:                   return MGMT_ERR_INTERNAL;
    }
}

/** Mensaje humano-legible para un resultado de users.h con error. */
static const char *
users_result_msg(users_result r) {
    switch (r) {
        case USERS_FULL:           return "user store full";
        case USERS_ALREADY_EXISTS: return "user already exists";
        case USERS_NOT_FOUND:      return "user not found";
        case USERS_INVALID:        return "invalid username or password";
        default:                   return "internal error";
    }
}

static mgmt_disposition
cmd_list_users(int argc, char *argv[], buffer *out) {
    (void) argv;
    if (argc != 1) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "LIST-USERS takes no arguments");
        return MGMT_DISP_CONTINUE;
    }
    const char *names[USERS_MAX];
    const size_t total = users_list(names, USERS_MAX);
    const size_t shown = total < USERS_MAX ? total : USERS_MAX;

    char scratch[MGMT_CMD_SCRATCH];
    snprintf(scratch, sizeof(scratch), "%s %zu users%s", MGMT_RESP_OK, total, MGMT_CRLF);
    emit_str(out, scratch);
    for (size_t i = 0; i < shown; i++) {
        emit_data_line(out, names[i]);
    }
    emit_str(out, MGMT_MULTILINE_END MGMT_CRLF);
    return MGMT_DISP_CONTINUE;
}

static mgmt_disposition
cmd_add_user(int argc, char *argv[], buffer *out) {
    if (argc != 3) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "usage: ADD-USER <name> <pass>");
        return MGMT_DISP_CONTINUE;
    }
    const users_result r = users_add(argv[1], argv[2]);
    if (r == USERS_OK) {
        emit_str(out, MGMT_RESP_OK " user added" MGMT_CRLF);
    } else {
        mgmt_cmd_emit_error(out, map_users_result(r), users_result_msg(r));
    }
    return MGMT_DISP_CONTINUE;
}

static mgmt_disposition
cmd_del_user(int argc, char *argv[], buffer *out) {
    if (argc != 2) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "usage: DEL-USER <name>");
        return MGMT_DISP_CONTINUE;
    }
    const users_result r = users_remove(argv[1]);
    if (r == USERS_OK) {
        emit_str(out, MGMT_RESP_OK " user removed" MGMT_CRLF);
    } else {
        mgmt_cmd_emit_error(out, map_users_result(r), users_result_msg(r));
    }
    return MGMT_DISP_CONTINUE;
}

/** Interpreta "on"/"off" en *out. Devuelve false si no es ninguno de los dos. */
static bool
parse_bool(const char *s, bool *out) {
    if (strcmp(s, MGMT_BOOL_ON) == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, MGMT_BOOL_OFF) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static mgmt_disposition
cmd_get_config(int argc, char *argv[], buffer *out) {
    (void) argv;
    if (argc != 1) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "GET-CONFIG takes no arguments");
        return MGMT_DISP_CONTINUE;
    }
    const struct config cfg = config_get();
    char scratch[MGMT_CMD_SCRATCH];
    snprintf(scratch, sizeof(scratch), "%s %s=%s %s=%s%s",
             MGMT_RESP_OK,
             MGMT_CFGKEY_AUTH_REQUIRED, cfg.auth_required     ? MGMT_BOOL_ON : MGMT_BOOL_OFF,
             MGMT_CFGKEY_DISSECTORS,    cfg.dissectors_enabled ? MGMT_BOOL_ON : MGMT_BOOL_OFF,
             MGMT_CRLF);
    emit_str(out, scratch);
    return MGMT_DISP_CONTINUE;
}

static mgmt_disposition
cmd_set_config(int argc, char *argv[], buffer *out) {
    if (argc != 3) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "usage: SET-CONFIG <key> <on|off>");
        return MGMT_DISP_CONTINUE;
    }
    const char *key = argv[1];
    if (strcmp(key, MGMT_CFGKEY_AUTH_REQUIRED) != 0
            && strcmp(key, MGMT_CFGKEY_DISSECTORS) != 0) {
        mgmt_cmd_emit_error(out, MGMT_ERR_UNKNOWN_KEY, "unknown config key");
        return MGMT_DISP_CONTINUE;
    }
    bool value;
    if (!parse_bool(argv[2], &value)) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "value must be on or off");
        return MGMT_DISP_CONTINUE;
    }
    if (strcmp(key, MGMT_CFGKEY_AUTH_REQUIRED) == 0) {
        config_set_auth_required(value);
    } else {
        config_set_dissectors(value);
    }
    emit_str(out, MGMT_RESP_OK " config updated" MGMT_CRLF);
    return MGMT_DISP_CONTINUE;
}

static mgmt_disposition
cmd_get_log(int argc, char *argv[], buffer *out) {
    (void) argv;
    if (argc != 1) {
        mgmt_cmd_emit_error(out, MGMT_ERR_INVALID_ARG, "GET-LOG takes no arguments");
        return MGMT_DISP_CONTINUE;
    }
    const size_t total = access_log_count();

    char scratch[MGMT_CMD_SCRATCH];
    snprintf(scratch, sizeof(scratch), "%s %zu entries%s", MGMT_RESP_OK, total, MGMT_CRLF);
    emit_str(out, scratch);

    /* Una línea, peor caso: usuario + cliente + destino máximos + separadores. */
    char line[ACCESS_LOG_USER_MAX + ACCESS_LOG_ADDR_MAX + ACCESS_LOG_DEST_MAX + 64];
    for (size_t i = 0; i < total; i++) {
        const struct access_log_entry *e = access_log_get(i);
        if (e == NULL) {
            break;
        }
        char      ts[sizeof("2026-01-01T00:00:00Z")];
        struct tm tm_utc;
        gmtime_r(&e->time, &tm_utc);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

        /* Formato PROTOCOL.md §5.8: ts \t user \t cliente \t destino:puerto \t REP */
        const int n = snprintf(line, sizeof(line), "%s\t%s\t%s\t%s:%u\t%u",
                               ts,
                               e->username[0]    != '\0' ? e->username    : "-",
                               e->client_addr[0] != '\0' ? e->client_addr : "-",
                               e->dest[0]        != '\0' ? e->dest         : "-",
                               e->dest_port,
                               e->rep);

        /* MF1 no tiene streaming: el write buffer es acotado. Si esta línea más
         * el terminador no entran, cortamos limpio (la respuesta siempre cierra
         * con el '.'). Invariante: al entrar acá siempre quedan >= 3 bytes para
         * el terminador, porque cada línea emitida deja ese margen. */
        size_t space;
        buffer_write_ptr(out, &space);
        const size_t needed = (size_t) n + 1 /*dot-stuffing*/ + 2 /*CRLF*/
                            + 1 + 2 /*terminador '.' + CRLF*/;
        if (n < 0 || space < needed) {
            break;
        }
        emit_data_line(out, line);
    }
    emit_str(out, MGMT_MULTILINE_END MGMT_CRLF);
    return MGMT_DISP_CONTINUE;
}

static mgmt_disposition
cmd_help(buffer *out) {
    emit_str(out, MGMT_RESP_OK " available commands" MGMT_CRLF);
    emit_str(out, MGMT_CMD_AUTH        MGMT_CRLF);
    emit_str(out, MGMT_CMD_GET_METRICS MGMT_CRLF);
    emit_str(out, MGMT_CMD_LIST_USERS  MGMT_CRLF);
    emit_str(out, MGMT_CMD_ADD_USER    MGMT_CRLF);
    emit_str(out, MGMT_CMD_DEL_USER    MGMT_CRLF);
    emit_str(out, MGMT_CMD_GET_CONFIG  MGMT_CRLF);
    emit_str(out, MGMT_CMD_SET_CONFIG  MGMT_CRLF);
    emit_str(out, MGMT_CMD_GET_LOG     MGMT_CRLF);
    emit_str(out, MGMT_CMD_HELP        MGMT_CRLF);
    emit_str(out, MGMT_CMD_QUIT        MGMT_CRLF);
    emit_str(out, MGMT_MULTILINE_END   MGMT_CRLF);
    return MGMT_DISP_CONTINUE;
}

static mgmt_disposition
cmd_quit(buffer *out) {
    emit_str(out, MGMT_RESP_OK " bye" MGMT_CRLF);
    return MGMT_DISP_CLOSE;
}

/* ------------------------------------------------------------------ *
 *  Dispatch
 * ------------------------------------------------------------------ */

mgmt_disposition
mgmt_cmd_dispatch(struct mgmt_session *session, char *line, buffer *out) {
    char *argv[MGMT_CMD_MAX_ARGS];
    const int argc = tokenize(line, argv, MGMT_CMD_MAX_ARGS);
    if (argc == 0) {
        mgmt_cmd_emit_error(out, MGMT_ERR_SYNTAX, "empty command");
        return MGMT_DISP_CONTINUE;
    }

    const char *verb = argv[0];

    /* Comandos disponibles SIN autenticar (ver PROTOCOL.md §5, marca [N]). */
    if (strcasecmp(verb, MGMT_CMD_AUTH) == 0) {
        return cmd_auth(session, argc, argv, out);
    }
    if (strcasecmp(verb, MGMT_CMD_HELP) == 0) {
        return cmd_help(out);
    }
    if (strcasecmp(verb, MGMT_CMD_QUIT) == 0) {
        return cmd_quit(out);
    }

    /* De acá en más, todo requiere sesión autenticada. */
    if (!session->authenticated) {
        mgmt_cmd_emit_error(out, MGMT_ERR_AUTH_REQUIRED, "authentication required");
        return MGMT_DISP_CONTINUE;
    }

    if (strcasecmp(verb, MGMT_CMD_GET_METRICS) == 0) {
        return cmd_get_metrics(argc, argv, out);
    }
    if (strcasecmp(verb, MGMT_CMD_LIST_USERS) == 0) {
        return cmd_list_users(argc, argv, out);
    }
    if (strcasecmp(verb, MGMT_CMD_ADD_USER) == 0) {
        return cmd_add_user(argc, argv, out);
    }
    if (strcasecmp(verb, MGMT_CMD_DEL_USER) == 0) {
        return cmd_del_user(argc, argv, out);
    }
    if (strcasecmp(verb, MGMT_CMD_GET_CONFIG) == 0) {
        return cmd_get_config(argc, argv, out);
    }
    if (strcasecmp(verb, MGMT_CMD_SET_CONFIG) == 0) {
        return cmd_set_config(argc, argv, out);
    }
    if (strcasecmp(verb, MGMT_CMD_GET_LOG) == 0) {
        return cmd_get_log(argc, argv, out);
    }

    mgmt_cmd_emit_error(out, MGMT_ERR_UNKNOWN_CMD, "unknown command");
    return MGMT_DISP_CONTINUE;
}
