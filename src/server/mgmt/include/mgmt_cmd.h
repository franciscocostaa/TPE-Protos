#ifndef MGMT_CMD_H_4bN8xQ1wZ6rT2kV
#define MGMT_CMD_H_4bN8xQ1wZ6rT2kV

#include <stdbool.h>
#include <stdint.h>

#include "buffer.h"
#include "mgmt_proto.h"

/**
 * @file mgmt_cmd.h
 * @brief Despacho de comandos del protocolo de monitoreo (SMP).
 *
 * Separa la SEMÁNTICA del protocolo (qué hace cada comando, contra qué módulo
 * del servidor habla) del TRANSPORTE (`mgmt.c`: selector, stm, framing de
 * líneas). El transporte le entrega una línea ya desframeada y este módulo
 * encola la respuesta completa (con su framing de wire) en el buffer de salida.
 *
 * Agregar un comando nuevo (ADD-USER, GET-CONFIG, ...) es agregar un caso acá;
 * el transporte no cambia.
 */

/**
 * @brief Qué debe hacer el transporte una vez enviada la respuesta encolada.
 */
typedef enum {
    MGMT_DISP_CONTINUE,  /**< seguir atendiendo comandos en esta conexión       */
    MGMT_DISP_CLOSE,     /**< cerrar la sesión tras enviar la respuesta (QUIT)  */
} mgmt_disposition;

/**
 * @brief Estado de la sesión que el dispatcher lee/muta entre comandos.
 */
struct mgmt_session {
    bool authenticated;  /**< true tras un AUTH válido; gatea los comandos protegidos */
};

/**
 * @brief Fija el token del canal administrativo (lo compara el comando AUTH).
 *
 * Si no se llama nunca, rige un default compilado (ver mgmt_cmd.c). Pensado para
 * que main.c lo cablee desde los argumentos de línea de comandos.
 *
 * @param token Nuevo token. Si es NULL no se modifica el valor vigente.
 */
void mgmt_cmd_set_token(const char *token);

/**
 * @brief Encola el saludo inicial del servidor. Lo llama el transporte al aceptar.
 *
 * @param out Buffer de salida donde se encola el saludo ("+OK SMP/1.0 ready\r\n").
 */
void mgmt_cmd_emit_greeting(buffer *out);

/**
 * @brief Encola una respuesta de error con el framing del protocolo.
 *
 * Produce la línea "-ERR <code> <msg>\r\n". Expuesto para que el transporte
 * reporte errores de framing (p. ej. línea demasiado larga) sin duplicar el
 * formato del wire.
 *
 * @param out  Buffer de salida donde se encola la respuesta.
 * @param code Código de estado a reportar (ver ::mgmt_status).
 * @param msg  Mensaje humano-legible que acompaña al código.
 */
void mgmt_cmd_emit_error(buffer *out, mgmt_status code, const char *msg);

/**
 * @brief Parsea una línea de comando y encola la respuesta completa.
 *
 * @param session Estado de la sesión; el dispatcher lo lee y muta (p. ej. AUTH
 *                setea el flag de autenticación).
 * @param line    Línea sin CRLF, NUL-terminada y modificable in-place (se
 *                tokeniza destructivamente).
 * @param out     Buffer de salida donde se encola la respuesta (con su framing).
 * @return ::MGMT_DISP_CONTINUE si la conexión sigue, ::MGMT_DISP_CLOSE si debe
 *         cerrarse tras enviar la respuesta.
 */
mgmt_disposition mgmt_cmd_dispatch(struct mgmt_session *session,
                                   char *line, buffer *out);

#endif
