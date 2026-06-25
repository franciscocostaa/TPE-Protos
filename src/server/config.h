#ifndef CONFIG_H_8sQ2mVx7pLr4
#define CONFIG_H_8sQ2mVx7pLr4

#include <stdbool.h>
#include <stdint.h>

/**
 * config.h - configuración del servidor mutable en tiempo de ejecución.
 *
 * Vive una única instancia global. Como todo el data-path corre en un solo
 * hilo, las lecturas/escrituras no necesitan locks. El protocolo de monitoreo
 * (mgmt) muta esta config; el data-path (socks5) la lee.
 *
 * CONTRATO COMPARTIDO — ver docs/PLAN.md §3.4. Cambiar esta interfaz requiere
 * avisar al grupo.
 */
struct config {
    bool     auth_required;       /* false => se acepta el método NO-AUTH */
    bool     dissectors_enabled;  /* sniffer de credenciales (2ª entrega)  */
    uint32_t io_buffer_size;      /* informativo por ahora                 */
};

/** Inicializa la config global con los valores iniciales (típicamente de args). */
void config_init(const struct config *initial);

/** Devuelve una copia de la config actual (lectura barata). */
struct config config_get(void);

void config_set_auth_required(bool v);
void config_set_dissectors(bool v);

#endif
