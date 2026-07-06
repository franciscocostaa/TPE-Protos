#ifndef MGMT_H_6kF3wQz9rT2v
#define MGMT_H_6kF3wQz9rT2v

#include "selector.h"

/**
 * mgmt.h - protocolo de monitoreo/configuración (consigna F7).
 *
 * Es un protocolo NUEVO (no una extensión de SOCKS) que escucha en otro socket
 * pasivo, en otro puerto, dentro del mismo proceso y multiplexado en el mismo
 * selector. Permite manejar usuarios y cambiar configuración en runtime.
 *
 * PLACEHOLDER del commit de bases: por ahora sólo acepta y cierra. La MEF real
 * y la documentación estilo-RFC (docs/PROTOCOL.md) las hace Persona B.
 */

/**
 * Acepta una nueva conexión de management. Pensado como `handle_read` del
 * socket pasivo de mgmt.
 */
void mgmt_passive_accept(struct selector_key *key);

#endif
