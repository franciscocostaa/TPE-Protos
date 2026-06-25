# Plan de trabajo y contratos — SOCKS5 Proxy + Protocolo de Monitoreo

> ITBA · Protocolos de Comunicación · TPE 2026/1
> Estado: **BORRADOR para revisar entre los 3** — no escribir lógica hasta acordar §3 (contratos).

Este documento es el **plan de división de tareas** y los **contratos (interfaces)** entre
módulos. La arquitectura conceptual vive en [`DESIGN.md`](./DESIGN.md); acá nos enfocamos en
*quién hace qué* y *cómo no pisarnos*.

---

## 1. Punto de partida real (estado del código hoy)

A diferencia de lo que dice `DESIGN.md` (escrito antes de codear), ya hay base funcionando:

- **Infra de cátedra** en `src/shared/`: `selector`, `stm`, `buffer`, `parser`,
  `parser_utils`, `netutils`. No se toca; se usa.
- **Proxy SOCKS5 NO-AUTH completo** en `src/server/socks5.c` (815 líneas, 1 MEF por
  conexión): `HELLO → REQUEST → RESOLV (DNS en thread) → CONNECTING (con fallback de
  direcciones) → REQUEST_WRITE → COPY`. IPv4/IPv6/FQDN andan.
- **`main.c`** levanta UN socket pasivo (SOCKS) y maneja señales de forma básica.
- **`args/args.c`** (parser de cátedra, con `struct socks5args`: users, mng_addr/port,
  disectors) existe pero **NO está integrado al build**.
- **`client.c`** es un stub.
- Compila con `make` (C11, `-Wall -Wextra -pthread`).

### Gap respecto a la consigna

| # | Requisito | Estado | Dueño propuesto |
|---|---|---|---|
| F2 | Auth user/pass (RFC 1929) | ❌ | A |
| F3-5 | Salida IPv4/IPv6/FQDN, fallback, REP | ✅ | — |
| F6 | Métricas (históricas/concurrentes/bytes) | ❌ | C |
| F7 | Protocolo de monitoreo (2º socket) | ❌ | B |
| F8 | Registro de accesos | ❌ | C |
| F9 | Graceful shutdown correcto (drenar conexiones) | ⚠️ parcial | C |
| F10 | Sniffer POP3 | ❌ 2ª entrega | — |
| NF5 | Cliente de monitoreo (CLI) | ❌ | B |
| NF7 | Integrar `args` | ⚠️ | C (en "bases") |

---

## 2. Layout objetivo y propiedad de archivos

Refactorizamos `socks5.c` monolítico en módulos para que 3 personas trabajen en paralelo
sin conflictos. **Cada archivo tiene un único dueño**; los headers compartidos (§3) se
acuerdan entre todos y luego casi no cambian.

```
src/server/
  main.c              [COMPARTIDO] levanta 2 sockets pasivos, señales, args, loop
  socks5/
    connection.c/.h   [A] orquestador: tabla de estados, ciclo de vida, ATTACHMENT
    hello.c/.h        [A] negociación de método (RFC 1928 §3)
    auth.c/.h         [A] subnegociación user/pass (RFC 1929)      ← NUEVO
    request.c/.h      [A] parse del request + connect/resolv + reply
    relay.c/.h        [A] copy bidireccional con backpressure
  mgmt/
    mgmt.c/.h         [B] MEF del protocolo de monitoreo (2º socket pasivo)
  dns/resolver.c/.h   [A] worker getaddrinfo (extraído de socks5.c actual)
  users.c/.h          [C] store de usuarios            ← CONTRATO (§3.1)
  metrics.c/.h        [C] contadores                   ← CONTRATO (§3.2)
  access_log.c/.h     [C] registro de accesos          ← CONTRATO (§3.3)
  config.c/.h         [C] config mutable en runtime    ← CONTRATO (§3.4)
src/client/
  client.c            [B] CLI del protocolo de monitoreo
src/shared/           [—] infra de cátedra, intacta
args/ → src/shared/   [C] mover e integrar al Makefile
docs/
  DESIGN.md           arquitectura
  PLAN.md             este doc
  PROTOCOL.md         [B] spec estilo-RFC del protocolo de monitoreo
```

> Nota: el refactor de `socks5.c` en `connection/hello/request/relay` lo hace **A** como
> primer paso de su track, para no romper a nadie. Hasta que lo haga, el `socks5.c` actual
> sigue compilando y sirviendo de referencia.

---

## 3. Contratos (interfaces) — **acordar esto ANTES de codear**

Son los headers que cruzan dueños. Si alguno cambia después, se avisa al grupo. Son
sketches para discutir, no definitivos.

### 3.1 `users.h` — store de usuarios (dueño C; consumen A y B)

```c
#ifndef USERS_H
#define USERS_H
#include <stdbool.h>
#include <stddef.h>

#define USERS_MAX            10
#define USERS_NAME_MAX       255   /* ULEN de RFC 1929 */
#define USERS_PASS_MAX       255   /* PLEN de RFC 1929 */

typedef enum {
    USERS_OK = 0,
    USERS_FULL,            /* se alcanzó USERS_MAX */
    USERS_ALREADY_EXISTS,
    USERS_NOT_FOUND,
    USERS_INVALID,         /* nombre/pass vacío o demasiado largo */
} users_result;

/* Inicializa el store vacío. Llamar una vez al arrancar. */
void users_init(void);

/* Valida credenciales (lo usa auth.c, RFC 1929). Comparación constante. */
bool users_authenticate(const char *name, const char *pass);

/* Alta/baja en runtime (lo usa mgmt). */
users_result users_add(const char *name, const char *pass);
users_result users_remove(const char *name);

/* Listado: copia hasta 'max' nombres a 'out', devuelve la cantidad total. */
size_t users_list(const char **out, size_t max);
size_t users_count(void);
#endif
```

### 3.2 `metrics.h` — contadores (dueño C; consumen connection/relay y mgmt)

```c
#ifndef METRICS_H
#define METRICS_H
#include <stdint.h>

struct metrics_snapshot {
    uint64_t connections_total;     /* históricas */
    uint64_t connections_current;   /* concurrentes ahora */
    uint64_t bytes_transferred;     /* suma de ambos sentidos del relay */
};

void metrics_init(void);

/* Lo llama connection.c al aceptar y al cerrar una conexión SOCKS. */
void metrics_connection_open(void);
void metrics_connection_close(void);

/* Lo llama relay.c cada vez que copia bytes. */
void metrics_add_bytes(uint64_t n);

/* Lo lee mgmt para responder. */
struct metrics_snapshot metrics_get(void);
#endif
```

### 3.3 `access_log.h` — registro de accesos (dueño C; consume connection)

```c
#ifndef ACCESS_LOG_H
#define ACCESS_LOG_H
#include <stdint.h>

/* Se registra una entrada cuando una conexión SOCKS queda establecida (o falla
 * el request). 'username' puede ser NULL si la conexión es NO-AUTH.
 * 'dest' es el FQDN o la IP literal que pidió el cliente. 'rep' es el código REP. */
void access_log_record(const char *username,
                       const char *client_addr,
                       const char *dest, uint16_t dest_port,
                       uint8_t rep);
#endif
```

### 3.4 `config.h` — config mutable en runtime (dueño C; consumen todos)

```c
#ifndef CONFIG_H
#define CONFIG_H
#include <stdbool.h>
#include <stdint.h>

struct config {
    bool     auth_required;     /* false = se permite NO-AUTH */
    bool     dissectors_enabled;
    uint32_t io_buffer_size;    /* informativo por ahora */
    /* timeouts, etc. a futuro */
};

void           config_init(const struct config *initial);
struct config  config_get(void);       /* lectura barata (1 hilo, sin locks) */
void           config_set_auth_required(bool v);
void           config_set_dissectors(bool v);
#endif
```

### 3.5 Contrato del protocolo de monitoreo (dueño B → `PROTOCOL.md`)

No es un header C sino un **contrato de wire**. B define en `PROTOCOL.md`:
transporte (TCP, multiplexado en el mismo selector), encoding (binario length-prefixed vs
texto — ver decisión en §5), autenticación del canal admin, y los comandos:
`auth`, `list-users`, `add-user`, `del-user`, `get-metrics`, `set-config`, `get-log`.
El servidor (`mgmt.c`) consume `users.h`, `metrics.h`, `config.h`, `access_log.h`.

---

## 4. División de tareas

Cada uno es dueño de su carpeta; tocan los contratos sólo de a uno y avisando.

- **Persona A — SOCKS core**
  1. Refactor de `socks5.c` → `socks5/{connection,hello,request,relay}` + `dns/resolver`
     (sin cambiar comportamiento; deja todo compilando).
  2. Implementar `auth.c` (RFC 1929) + estado `AUTH` en la MEF, gobernado por
     `config.auth_required` y `users_authenticate()`.
  3. Hooks a `metrics`/`access_log` en los puntos del relay/connection.

- **Persona B — Monitoreo**
  1. Diseñar el protocolo y escribir `PROTOCOL.md` (entregable estilo-RFC).
  2. `mgmt.c`: 2º socket pasivo + MEF del protocolo, consumiendo los contratos de C.
  3. `client.c`: CLI ergonómica (`client add-user pablito pass1234`), I/O bloqueante OK.

- **Persona C — Observabilidad e infra**
  1. **Commit de "bases"** (§6) que desbloquea a A y B.
  2. Implementar `users`, `metrics`, `access_log`, `config`.
  3. Integrar `args/` al build; graceful shutdown correcto (drenar conexiones).
  4. Tests de los módulos puros.

---

## 5. Decisiones abiertas (resolver en grupo antes de §3 final)

1. **Wire del protocolo de monitoreo:** binario length-prefixed (más "en el espíritu" del
   curso, hay que documentar serialización) vs texto por líneas (más fácil de debuggear).
   Recomendación: **binario** — justificarlo en el informe.
2. **Auth del canal admin:** token compartido vs tabla de admins. Recomendación: token
   simple para la 1ª entrega.
3. **Persistencia de usuarios:** sólo volátil + seed por CLI/`args`, o archivo. Las
   métricas SÍ pueden ser volátiles por consigna; usuarios probablemente también.
4. **`USERS_MAX`:** `args.h` lo fija en 10. ¿Lo dejamos o lo subimos?

---

## 6. Primer paso: commit de "bases" (lo hace C, lo revisan los 3)

Objetivo: que el repo quede con los contratos y el 2º socket en su lugar, **todo
compilando**, para que A y B arranquen en paralelo sin bloquearse.

1. Mover `args/` a `src/shared/` e integrarlo al `Makefile`; reemplazar el `parse_port`
   ad-hoc de `main.c` por `parse_args`.
2. Abrir el **2º socket pasivo** (mgmt) en `main.c` con un handler placeholder.
3. Crear `users.h/.c`, `metrics.h/.c`, `access_log.h/.c`, `config.h/.c` con la interfaz de
   §3 y una implementación mínima (stubs que compilen y linkeen).
4. Arreglar graceful shutdown: al recibir señal, desregistrar los sockets pasivos (dejar de
   aceptar) y seguir el loop hasta que no queden conexiones; 2ª señal = salida forzada.

Cada hito se revisa con `ecc:cpp-reviewer` antes de mergear.

---

## 7. Reglas de coordología (para no pisarnos)

- Una rama por persona/feature; PRs chicos; `main` siempre compila.
- Los headers de §3 son "interfaz pública": cambiarlos requiere avisar al grupo.
- Nadie toca `src/shared/` (cátedra) salvo bug puntual consensuado.
- Correr `make` antes de cada commit; tests verdes antes de mergear.
