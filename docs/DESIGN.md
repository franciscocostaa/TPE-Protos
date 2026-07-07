# Documento de Diseño — Proxy SOCKS5 + Protocolo de Monitoreo

> ITBA · Protocolos de Comunicación · TPE 2026/1

Este documento es el plano arquitectónico que acordamos *antes* de escribir lógica. Es
intencionalmente preciso en el lenguaje de implementación (C11) para el proxy, y de
estilo RFC / agnóstico al lenguaje para el protocolo de monitoreo que diseñamos nosotros
mismos.

---

## 1. Alcance y decisiones

**Dentro del alcance (primera entrega):**

- Proxy SOCKS5 (RFC 1928) — **solo el comando CONNECT**.
- Autenticación usuario/contraseña (RFC 1929) **y** el método "sin autenticación",
  seleccionado por configuración.
- TCP saliente hacia **IPv4, IPv6 y FQDN** (resuelto a cualquiera de las dos familias).
- Fallback robusto a múltiples direcciones ante fallos de conexión.
- Códigos de respuesta SOCKS completos ante éxito/fallo.
- Métricas volátiles + registro de accesos.
- Un **protocolo de monitoreo/administración separado** en su propio puerto + un
  cliente de terminal.
- Apagado ordenado (SIGTERM/SIGINT).

**Explícitamente fuera de alcance:**

- **BIND** y **UDP ASSOCIATE** → el servidor responde `REP = X'07'` (comando no
  soportado). El diseño deja la capa de request abierta para agregarlos más adelante,
  pero no los construimos ahora.
- Autenticación GSSAPI (no requerida por la consigna).
- Sniffing de credenciales POP3 → **solo para la segunda entrega**, contemplado en el
  diseño pero no construido ahora.

**Restricciones no funcionales clave que guían la arquitectura:**

- **Un solo hilo**, sockets **no bloqueantes**, **multiplexados** mediante un selector.
  El *único* hilo extra permitido es el de resolución DNS (`getaddrinfo`), que no hace
  ningún otro I/O y solo devuelve resultados al hilo principal.
- **La correctitud ante lecturas/escrituras parciales es todo o nada.** Todo camino de
  bytes debe asumir que `read`/`write` pueden mover *menos* bytes de los pedidos, y
  reanudar después.
- Memoria acotada: nunca cargar un stream completo en RAM; buffers de relay de tamaño
  fijo.
- C11 (`-std=c11`), se compila con `make`, convenciones POSIX 1003.1 para la CLI.

---

## 2. Estructura del repositorio y del build

Mantenemos la división existente `server / client / shared` y la hacemos crecer. Árbol
propuesto:

```
src/
  server/        # Proxy SOCKS5 + servidor de monitoreo (un solo proceso)
    main.c               # parseo de argumentos, señales, listeners, arranque del loop del selector
    socks5/
      socks5.c/.h        # máquina de estados por conexión (el corazón)
      negotiation.c/.h   # parser de negociación de método (RFC 1928 §3)
      auth.c/.h          # parser de sub-negociación usuario/contraseña (RFC 1929)
      request.c/.h       # parser de request (RFC 1928 §4) + constructor de respuesta
      connect.c/.h       # connect saliente + fallback sobre lista de direcciones
      relay.c/.h         # copia bidireccional (las dos "mitades")
    mgmt/
      mgmt.c/.h          # máquina de estados del protocolo de monitoreo
    dns/
      resolver.c/.h      # worker asincrónico de getaddrinfo + entrega de resultados
    metrics.c/.h         # contadores (conexiones históricas/concurrentes, bytes)
    access_log.c/.h      # "quién se conectó a dónde y cuándo"
    users.c/.h           # store de usuarios (alta/baja/auth), mutable en runtime
    config.c/.h          # configuración en runtime (timeouts, tamaños de buffer, auth on/off)
  client/        # cliente de terminal de monitoreo (acá el I/O bloqueante está bien)
    main.c
  shared/        # código compartido entre servidor y cliente
    selector.c/.h        # abstracción de loop de eventos no bloqueante
    buffer.c/.h          # buffer de bytes seguro ante lecturas/escrituras parciales
    stm.c/.h             # driver genérico de máquina de estados
    netutils.c/.h        # helpers de sockaddr, sock_blocking_*(), etc.
    args.c/.h            # parseo de CLI estilo POSIX (implementación de referencia una vez publicada)
docs/
  DESIGN.md      # este archivo
  PROTOCOL.md    # especificación estilo RFC de nuestro protocolo de monitoreo (entregable separado)
```

**Nota de build (acción pendiente):** `Makefile.inc` hoy tiene
`COMPILER_FLAGS=-Wall -pedantic -g`. La consigna exige C11, así que hay que agregar
`-std=c11` y una macro de feature-test (p. ej. `-D_POSIX_C_SOURCE=200809L`) para
`getaddrinfo`, `sigaction`, etc. También conviene `-Wextra` y un perfil de debug con
`-fsanitize=address` para desarrollo (no para el build que se entrega).

**Sobre código de terceros:** la cátedra publica material de la materia (en particular
un `selector` + `buffer` + `stm` no bloqueantes). Reutilizarlo está *permitido citando
la fuente*. Vamos a usar las versiones publicadas tal cual (con atribución) o a escribir
las nuestras con la misma forma — a confirmar antes de codear. De cualquier manera, la
**lógica de SOCKS5 es nuestra**.

---

## 3. Arquitectura central — un hilo, un selector

Todo es un único loop de eventos. No hay **ninguna llamada bloqueante** en el data
path. El loop es dueño de un conjunto de file descriptors, cada uno con un conjunto de
intereses (lectura/escritura) y un handler. Pseudocódigo:

```
selector = selector_new()
register(socks_listen_fd,  READ, accept_socks)
register(mgmt_listen_fd,   READ, accept_mgmt)
register(dns_notify_fd,    READ, on_dns_result)   # self-pipe / eventfd
while running:
    selector_select(selector, timeout)   # el ÚNICO lugar donde bloqueamos, con timeout
    for each ready fd: dispatch its handler          # para cada fd listo: despachar su handler
    run expired timeouts (idle connection reaping)   # correr timeouts vencidos (limpieza de conexiones idle)
```

El selector está respaldado por `select(2)` por portabilidad (la consigna apunta a un
entorno POSIX; `select` alcanza para las 500 conexiones requeridas, y el selector de la
cátedra lo usa). Si el profiling lo exige, se puede cambiar el backend detrás de la
misma interfaz.

**Por qué un self-pipe / `eventfd` para DNS:** el hilo worker de DNS no puede tocar
nuestros sockets. Cuando termina `getaddrinfo`, escribe un byte en un pipe que el
selector observa; el hilo principal se despierta, lee el resultado de una cola, y
retoma la máquina de estados de esa conexión. Este es el único uso de threading,
cuidadosamente acotado.

---

## 4. La abstracción de buffer (correctitud de I/O parcial)

Un `buffer` es un arreglo de bytes de tamaño fijo con punteros de `read`/`write`, que
expone:

```
buffer_write_ptr(b, &n)  -> dónde hacer recv(), y cuánto lugar hay (n)
buffer_write_adv(b, k)   -> efectivamente recibimos k bytes
buffer_read_ptr(b, &n)   -> desde dónde hacer send(), y cuánto queda pendiente (n)
buffer_read_adv(b, k)    -> efectivamente enviamos k bytes
buffer_can_read/write()  -> predicados
buffer_compact()         -> recupera el espacio ya consumido
```

Todo parser consume de un buffer y **debe tolerar quedarse sin bytes a mitad de un
mensaje** — devuelve "necesito más datos" y se re-entra cuando llega el próximo pedazo.
Todo escritor **debe tolerar un `send` corto** — avanza el puntero de lectura por los
bytes efectivamente escritos y rearma el interés de `WRITE` por el resto. Este es el
mecanismo que hace que el I/O parcial sea correcto *por construcción*, en vez de a
fuerza de parches ad-hoc.

---

## 5. Máquina de estados de la conexión SOCKS5

Cada conexión de cliente aceptada es una instancia de máquina de estados (manejada por
`stm`). Los estados, mapeados directamente a RFC 1928 / 1929:

```
                       (lectura)         (lectura/escritura)
NEGOTIATION_READ ───▶ NEGOTIATION_WRITE ───▶ AUTH_READ ───▶ AUTH_WRITE
   |  lista de métodos RFC1928 §3   responde X'05',method   sub-neg RFC1929
   |                                                              |
   ▼                                                              ▼
REQUEST_READ ───▶ RESOLVING ───▶ CONNECTING ───▶ REQUEST_WRITE ───▶ RELAY
 parsea req      DNS async     prueba lista de   responde REP,...   copia ambas
 RFC1928 §4      (si FQDN)     direcc., fallback  bind addr          direcciones
                                si falla                               |
                                                                        ▼
                                                                    DONE / ERROR
```

- **NEGOTIATION:** lee `VER, NMETHODS, METHODS`; elige `NO_AUTH (0x00)` o
  `USER_PASS (0x02)` según config; responde `VER=0x05, METHOD`. Si ninguno es
  aceptable → `0xFF` y cierra.
- **AUTH (solo si se eligió USER_PASS):** lee `VER=0x01, ULEN, UNAME, PLEN, PASSWD`;
  valida contra el store de usuarios; responde `VER=0x01, STATUS` (`0x00` ok, si no,
  cierra).
- **REQUEST:** lee `VER, CMD, RSV, ATYP, DST.ADDR, DST.PORT`. Si `CMD != CONNECT` →
  responde `REP=0x07`. Bifurca según `ATYP`: IPv4 / IPv6 → conecta directamente;
  DOMAINNAME → entra a `RESOLVING`.
- **RESOLVING:** entrega el FQDN al worker de DNS; deja la conexión parqueada (sin
  interés de fd) hasta que el self-pipe nos despierte con una lista de addrinfo.
- **CONNECTING:** `connect()` no bloqueante a la primera dirección; ante
  `EINPROGRESS` observa `WRITE`; ante fallo (`SO_ERROR`) avanza a la **siguiente
  dirección** de la lista (requisito de robustez). Cuando se agota la lista, responde
  con el `REP` más específico posible (host unreachable `0x04`, connection refused
  `0x05`, network unreachable `0x03`, general `0x01`).
- **REQUEST_WRITE:** envía la respuesta (`REP=0x00` + dirección/puerto bindeado) y
  entra al relay.
- **RELAY:** ver §6.

Cada estado tiene: `on_arrival`, `on_read_ready`, `on_write_ready`, `on_block`,
`on_departure`.

---

## 6. El relay — dos mitades acopladas

Una vez establecida, una sesión tiene dos sockets (cliente `C`, origen `O`) y **dos
buffers**: `C→O` y `O→C`. El registro de intereses está guiado por los datos:

- Queremos **leer** de `C` solo si el buffer `C→O` tiene lugar.
- Queremos **escribir** en `O` solo si el buffer `C→O` tiene bytes pendientes.
- Simétricamente para la dirección `O→C`.

Esto aplica **backpressure** de forma natural: un origen lento nos frena de leer de un
cliente rápido (sin buffering ilimitado — satisface el requisito de memoria acotada).
Los bytes transferidos se contabilizan acá para las métricas. Un half-close (`EOF` de
un lado) cierra la dirección correspondiente con `shutdown()` y termina la sesión una
vez que ambas direcciones drenaron.

---

## 7. Resolución DNS (el único hilo permitido)

- Los pedidos de FQDN se encolan a un worker chico (un hilo dedicado, o
  `getaddrinfo_a`).
- El worker llama a `getaddrinfo` (que puede bloquear) y encola
  `(conn_id, struct addrinfo*)` en una cola de resultados, y después escribe un byte en
  el self-pipe del selector.
- El hilo principal drena la cola al despertarse y retoma cada conexión parqueada.
- El worker no toca **ningún socket** y no hace **ningún otro I/O** — exactamente como
  lo permite la consigna. Sugerimos `AI_ADDRCONFIG` y pedimos ambas familias para que
  la lista de fallback (§5 CONNECTING) quede poblada.

---

## 8. Protocolo de monitoreo/administración (diseño propio — entregable separado)

Un segundo socket pasivo en su propio puerto, mismo proceso, mismo selector. **No** es
una extensión de SOCKS. La especificación completa a nivel de bytes va en
`docs/PROTOCOL.md`; acá la intención de diseño de alto nivel:

- **Transporte:** TCP, multiplexado en el mismo loop no bloqueante que el proxy.
- **Codificación:** *propuesta*: request/response **binario** compacto (frames con
  largo prefijado) para el wire, con el **cliente** traduciendo comandos ergonómicos
  (`client add-user pablito pass1234`) a frames. Vamos a justificar binario-vs-texto en
  el informe. (Abierto a discusión — un protocolo de texto por líneas es más simple de
  depurar; binario está más "en el espíritu" de la materia. Decisión pendiente.)
- **Capacidades:** autenticar; listar/agregar/quitar usuarios del proxy; alternar el
  método de auth; leer métricas (conexiones históricas, conexiones concurrentes,
  bytes); leer/setear configuración en runtime (timeouts, tamaños de buffer);
  consultar/seguir el registro de accesos.
- **Auth:** el canal de administración tiene sus propias credenciales de admin
  (decisión pendiente: token de secreto compartido vs. tabla de usuarios admin). El
  cliente maneja el handshake de autenticación; *no* es netcat-friendly por diseño
  (según la prohibición explícita de la consigna).
- El cliente **puede usar I/O bloqueante** (es simple y secuencial).

---

## 9. Módulos transversales

- **users:** store en memoria, mutable en runtime vía el protocolo de mgmt. Las
  contraseñas se comparan en tiempo constante. (Persistencia opcional — las métricas
  pueden ser volátiles; los usuarios probablemente se cargan por CLI + altas por
  mgmt.)
- **metrics:** contadores simples actualizados en el data path; leídos por mgmt.
  Volátiles según la especificación.
- **access_log:** agrega un registro estructurado por cada conexión establecida —
  timestamp, usuario, dirección del cliente, destino (FQDN/IP + puerto), bytes,
  resultado. Pensado para responder a la consulta de "queja externa".
- **config:** un único struct leído por el data path, mutado solo por el hilo
  principal (no hacen falta locks — es single-threaded), expuesto vía mgmt.

---

## 10. Señales y apagado ordenado

- `SIGTERM` / `SIGINT` manejadas vía `sigaction`; el handler solo setea un flag
  `volatile sig_atomic_t` (y/o escribe el self-pipe para romper el `select`). Sin
  trabajo real dentro del handler.
- Con el flag activo: dejar de aceptar (cerrar/desregistrar los fd de listen), dejar
  drenar las sesiones en curso, y salir prolijamente (liberar buffers, cerrar fds,
  hacer join del worker de DNS).
- Una **segunda** señal puede forzar la salida inmediata.

---

## 11. CLI y convenciones POSIX

Ambos binarios siguen las convenciones de utilidades IEEE 1003.1 (el parser de
argumentos de referencia, una vez publicado, entra en `shared/args`). Opciones
previstas para el servidor: puerto SOCKS, puerto de management, dirección de bind,
usuarios iniciales, destino del log, tamaño de buffer, `-h`/`-v`. El cliente toma
host/puerto de management + un verbo de subcomando.

---

## 12. Filosofía de manejo de errores (la regla de "nada de parches débiles")

- Se chequea el valor de retorno de cada syscall; `EAGAIN/EWOULDBLOCK/EINPROGRESS/EINTR`
  son *flujo de control esperado*, no errores, y se manejan explícitamente.
- Los fallos se propagan a la máquina de estados de SOCKS, que emite el **código REP
  más específico** posible en vez de uno genérico (la consigna pide usar "toda la
  potencia del protocolo").
- Nada de `catch-and-continue` silencioso; ninguna suposición de tamaño fijo que
  "usualmente" se cumple. Los parsers están guiados por longitud y son acotados.
- La propiedad de los recursos es explícita por conexión; el teardown libera todo
  exactamente una vez.

---

## 13. Testing y estrés

- Tests unitarios para los parsers puros (negotiation/auth/request) — están guiados
  por tablas y por input parcial, así que podemos alimentarlos byte a byte para probar
  la correctitud ante lecturas parciales.
- Tests de interoperabilidad contra un cliente SOCKS5 real (`curl --socks5`,
  navegadores).
- Estrés: escalar conexiones concurrentes por encima de 500, medir la degradación de
  throughput y la cantidad máxima de conexiones sostenidas, para la sección de estrés
  requerida en el informe.

---

## 14. Orden de construcción sugerido (hitos)

Cada hito es revisable de forma independiente; corremos `ecc:cpp-reviewer` después de
cada uno.

1. **Fundación:** arreglar `Makefile.inc` (C11 + flags), incorporar `selector` +
   `buffer` + `stm`, un echo server trivial que pruebe que el loop no bloqueante
   funciona.
2. **Negociación SOCKS + auth:** negociación de método y usuario/contraseña RFC 1929
   contra un store de usuarios estático. Todavía sin relay.
3. **Request + CONNECT (IPv4/IPv6) + relay:** data path completo hacia destinos con IP
   literal.
4. **DNS + fallback:** hilo resolver asincrónico, destinos FQDN, robustez
   multi-dirección.
5. **Métricas + access log + apagado ordenado.**
6. **Protocolo de monitoreo + cliente** (y `docs/PROTOCOL.md`).
7. **Hardening + tests de estrés + informe.**

---

## 15. Preguntas abiertas (necesitan una decisión antes de/al momento de codear)

1. **Origen de selector/buffer:** ¿reutilizamos el `selector`/`buffer`/`stm` publicado
   por la cátedra (con atribución) o escribimos el nuestro con la misma interfaz?
2. **Formato de wire de monitoreo:** ¿frames binarios con largo prefijado vs. texto por
   líneas?
3. **Modelo de auth de monitoreo:** ¿token de admin compartido vs. tabla de usuarios
   admin?
4. **Persistencia de usuarios:** ¿puramente volátil + cargados por CLI, o persistidos
   en un archivo?
