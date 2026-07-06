#ifndef MGMT_PROTO_H_9wQ4tZ2kR7mB3xV
#define MGMT_PROTO_H_9wQ4tZ2kR7mB3xV

/**
 * @file mgmt_proto.h
 * @brief Contrato de wire del protocolo de monitoreo (SMP).
 *
 * Es la ÚNICA fuente de verdad de los literales del protocolo (verbos,
 * prefijos de respuesta, claves de configuración, límites y códigos de error).
 * Lo incluyen TANTO el servidor (`mgmt.c`, `mgmt_cmd.c`) COMO el cliente
 * (`client.c`) para serializar/parsear sin "magic strings" duplicados.
 *
 * Header-only a propósito: como el protocolo es de texto, la serialización es
 * trivial (printf/sscanf) y cada lado la hace por su cuenta; lo único que se
 * comparte son estas constantes. Así no hay que linkear un .c común entre los
 * dos binarios (ver decisión de build en docs/PLAN.md).
 *
 * La especificación normativa (estilo-RFC, agnóstica al lenguaje) vive en
 * docs/PROTOCOL.md. Si algo de acá cambia, ese documento DEBE cambiar también.
 */

/* ------------------------------------------------------------------ *
 *  Identidad y versión del protocolo
 * ------------------------------------------------------------------ */

/** Nombre corto del protocolo: SOCKS Management Protocol. */
#define MGMT_PROTO_NAME    "SMP"
/** Versión del protocolo. Aparece en el saludo inicial del servidor. */
#define MGMT_PROTO_VERSION "1.0"

/* ------------------------------------------------------------------ *
 *  Framing
 * ------------------------------------------------------------------ */

/** Toda línea (request o response) termina en CRLF. */
#define MGMT_CRLF "\r\n"

/**
 * Largo máximo de una línea, CRLF incluido. Un request más largo es un error
 * de sintaxis (MGMT_ERR_SYNTAX). Cota: "ADD-USER <255> <255>" entra holgado.
 */
#define MGMT_LINE_MAX 1024

/**
 * En respuestas multilínea (p. ej. LIST-USERS, GET-LOG) el cuerpo termina con
 * una línea que contiene únicamente este carácter. Para evitar ambigüedad, una
 * línea de datos que empiece con '.' se transmite con un '.' extra al inicio
 * (dot-stuffing), igual que en POP3 (RFC 1939 §3).
 */
#define MGMT_MULTILINE_END     "."
#define MGMT_MULTILINE_END_CH  '.'

/* ------------------------------------------------------------------ *
 *  Prefijos de respuesta
 * ------------------------------------------------------------------ */

/** Indicador de éxito al inicio de una respuesta. */
#define MGMT_RESP_OK  "+OK"
/** Indicador de error: va seguido de "<código> <mensaje>". */
#define MGMT_RESP_ERR "-ERR"

/* ------------------------------------------------------------------ *
 *  Verbos (comandos). Case-insensitive en el wire; acá en mayúsculas.
 * ------------------------------------------------------------------ */

#define MGMT_CMD_AUTH        "AUTH"         /* AUTH <token>                  */
#define MGMT_CMD_LIST_USERS  "LIST-USERS"   /* LIST-USERS                    */
#define MGMT_CMD_ADD_USER    "ADD-USER"     /* ADD-USER <name> <pass>        */
#define MGMT_CMD_DEL_USER    "DEL-USER"     /* DEL-USER <name>               */
#define MGMT_CMD_GET_METRICS "GET-METRICS"  /* GET-METRICS                   */
#define MGMT_CMD_GET_CONFIG  "GET-CONFIG"   /* GET-CONFIG                    */
#define MGMT_CMD_SET_CONFIG  "SET-CONFIG"   /* SET-CONFIG <key> <value>      */
#define MGMT_CMD_GET_LOG     "GET-LOG"      /* GET-LOG                       */
#define MGMT_CMD_HELP        "HELP"         /* HELP                          */
#define MGMT_CMD_QUIT        "QUIT"         /* QUIT                          */

/* ------------------------------------------------------------------ *
 *  Claves de GET-CONFIG / SET-CONFIG y sus valores booleanos
 * ------------------------------------------------------------------ */

#define MGMT_CFGKEY_AUTH_REQUIRED "auth-required" /* on|off */
#define MGMT_CFGKEY_DISSECTORS    "dissectors"    /* on|off */

#define MGMT_BOOL_ON  "on"
#define MGMT_BOOL_OFF "off"

/* ------------------------------------------------------------------ *
 *  Claves de la respuesta de GET-METRICS (formato key=value)
 * ------------------------------------------------------------------ */

#define MGMT_METRIC_CONNECTIONS_TOTAL   "connections-total"
#define MGMT_METRIC_CONNECTIONS_CURRENT "connections-current"
#define MGMT_METRIC_BYTES_TRANSFERRED   "bytes-transferred"

/* ------------------------------------------------------------------ *
 *  Códigos de estado / error
 * ------------------------------------------------------------------ */

/**
 * @brief Códigos de estado del protocolo.
 *
 * En una respuesta "-ERR <código> <mensaje>", <código> es uno de estos valores.
 * El <mensaje> es texto libre legible y NO DEBE usarse para tomar decisiones de
 * programa: el cliente DEBE basarse en el código numérico.
 */
typedef enum {
    MGMT_OK                 = 0,  /**< no es un error; reservado             */
    MGMT_ERR_SYNTAX         = 1,  /**< línea mal formada / largo excedido    */
    MGMT_ERR_UNKNOWN_CMD    = 2,  /**< verbo desconocido                     */
    MGMT_ERR_AUTH_REQUIRED  = 3,  /**< comando emitido antes de autenticar   */
    MGMT_ERR_AUTH_FAILED    = 4,  /**< token inválido                        */
    MGMT_ERR_INVALID_ARG    = 5,  /**< cantidad/forma de argumentos inválida */
    MGMT_ERR_USERS_FULL     = 6,  /**< store de usuarios lleno (USERS_MAX)   */
    MGMT_ERR_USER_EXISTS    = 7,  /**< alta de usuario ya existente          */
    MGMT_ERR_USER_NOT_FOUND = 8,  /**< baja de usuario inexistente           */
    MGMT_ERR_UNKNOWN_KEY    = 9,  /**< clave de config desconocida           */
    MGMT_ERR_INTERNAL       = 10, /**< fallo interno del servidor            */
} mgmt_status;

#endif
