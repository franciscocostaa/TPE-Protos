/**
 * mgmt.c - PLACEHOLDER del protocolo de monitoreo (ver mgmt.h).
 *
 * Dueño: Persona B. Acá va la máquina de estados del protocolo de
 * configuración: autenticación del canal admin, alta/baja de usuarios
 * (users.h), lectura de métricas (metrics.h) y cambios de config (config.h).
 *
 * Por ahora acepta la conexión y la cierra, para tener el punto de entrada
 * registrado en el selector sin bloquear el resto del desarrollo.
 */
#include <stdio.h>
#include <unistd.h>

#include <sys/socket.h>

#include "mgmt/mgmt.h"

void
mgmt_passive_accept(struct selector_key *key) {
    struct sockaddr_storage addr;
    socklen_t               addr_len = sizeof(addr);

    const int fd = accept(key->fd, (struct sockaddr *) &addr, &addr_len);
    if (fd == -1) {
        return;
    }
    /* TODO(B): registrar la conexión con la MEF del protocolo de monitoreo.
     * Mientras tanto la cerramos para no dejar fds colgados. */
    fprintf(stderr, "[mgmt] conexión recibida (protocolo aún no implementado)\n");
    close(fd);
}
