# Trabajo Práctico Especial — Servidor Proxy SOCKS5 + Protocolo de Monitoreo

**Protocolos de Comunicación — ITBA — 2026/1 — Primera entrega**

> **Grupo 19** · Integrantes: Roman Salerno (65145), Franco Branda (65506), Francisco Costa (65202).
> Repositorio: `github.com/franciscocostaa/TPE-Protos` (se entrega con toda su historia git).

---

## 1. Índice

1. Índice
2. Descripción de los protocolos y aplicaciones desarrolladas
   - 2.1 Aplicación servidor (proxy SOCKS5 + servicio de monitoreo)
   - 2.2 Protocolo de monitoreo (SMP) — descripción estilo RFC
   - 2.3 Aplicación cliente de monitoreo
3. Problemas encontrados durante el diseño y la implementación
4. Limitaciones de la aplicación
5. Posibles extensiones
6. Conclusiones
7. Ejemplos de prueba
8. Guía de instalación
9. Instrucciones de configuración
10. Ejemplos de configuración y monitoreo
11. Documento de diseño (arquitectura)
- Apéndice A: especificación completa del protocolo (`docs/PROTOCOL.md`)

---

## 2. Descripción de los protocolos y aplicaciones desarrolladas

Se desarrollaron **dos aplicaciones** en C11:

- **`server`**: un proxy **SOCKS5** (RFC 1928, comando CONNECT) con autenticación
  usuario/contraseña (RFC 1929), que además expone un **segundo socket pasivo** con un
  **protocolo de monitoreo y configuración propio (SMP)** para administrarlo en caliente.
- **`client`**: un cliente de terminal para hablar el protocolo de monitoreo.

Ambos corren sobre una base de infraestructura no bloqueante (selector/buffer/máquina de
estados) publicada por la cátedra (ver Apéndice y §11).

### 2.1 Aplicación servidor

El proxy atiende **múltiples clientes concurrentes en un único hilo**, multiplexando la
E/S con un *selector* (basado en `select(2)`). Cada conexión de cliente es una **máquina de
estados** independiente que recorre las fases del protocolo SOCKS5:

```
HELLO_READ → HELLO_WRITE → [AUTH_READ → AUTH_WRITE] → REQUEST_READ →
[REQUEST_RESOLV] → REQUEST_CONNECTING → REQUEST_WRITE → COPY → DONE
```

Características implementadas:

- **Negociación de método** (RFC 1928 §3): se ofrece NO-AUTH o USER/PASS según la
  configuración vigente (`auth_required`).
- **Autenticación usuario/contraseña** (RFC 1929): subnegociación completa, tolerante a
  lecturas parciales, validando contra un store de usuarios con **comparación en tiempo
  constante** (evita fugas por *timing*).
- **Salida a IPv4, IPv6 y FQDN**: las direcciones literales se conectan directamente; los
  nombres de dominio se resuelven con `getaddrinfo` en un **hilo auxiliar** (el único uso
  de hilos permitido por la consigna), que al terminar notifica al hilo principal mediante
  una señal, sin realizar ninguna otra E/S.
- **Robustez multi-dirección**: si un FQDN resuelve a varias IPs y una falla, se prueba con
  la siguiente automáticamente.
- **Códigos de respuesta (REP) específicos**: ante un fallo de conexión se informa el
  código más preciso posible mapeando el `errno` real (`ECONNREFUSED` → conexión rechazada,
  `ENETUNREACH` → red inalcanzable, `EHOSTUNREACH`/`ETIMEDOUT` → host inalcanzable, etc.),
  usando "toda la potencia del protocolo".
- **Relay bidireccional** con *backpressure*: dos buffers de tamaño fijo por conexión; se
  lee de un extremo solo si hay lugar y se escribe al otro solo si hay datos, de modo que
  un extremo lento frena al rápido sin acumular memoria sin límite.
- **Métricas, registro de accesos y graceful shutdown** (ver §2.2 y §11).

### 2.2 Protocolo de monitoreo (SMP) — descripción estilo RFC

Se diseñó un protocolo **nuevo e independiente de SOCKS**, que escucha en **otro socket
pasivo, en otro puerto, dentro del mismo proceso** y multiplexado en el mismo selector no
bloqueante. La especificación completa, agnóstica al lenguaje, está en
[`docs/PROTOCOL.md`](./PROTOCOL.md) (Apéndice A). Resumen de las decisiones de diseño:

| Decisión | Elección | Justificación |
|---|---|---|
| **Transporte** | TCP, multiplexado en el mismo selector del proxy | Confiable; reutiliza la infraestructura no bloqueante. |
| **Codificación** | **Texto**, líneas terminadas en CRLF, ASCII | Simple de implementar y depurar; suficiente para un canal administrativo de bajo volumen. Inspirado en POP3/SMTP. |
| **Autenticación** | Token compartido (comando `AUTH`), configurable por línea de comandos | Simple para la primera entrega; separa el canal admin del proxy. |
| **Respuestas** | `+OK`/`-ERR <código>`; respuestas multilínea terminadas en `.` con *dot-stuffing* | Formato inequívoco y parseable, al estilo POP3. |

**Comandos implementados:** `AUTH`, `GET-METRICS`, `LIST-USERS`, `ADD-USER`, `DEL-USER`,
`GET-CONFIG`, `SET-CONFIG` (clave `auth-required`), `GET-LOG`, `HELP`,
`QUIT`. Los comandos que manejan usuarios, config o log requieren autenticación previa.

El protocolo permite **manejar usuarios y cambiar la configuración en tiempo de ejecución
sin reiniciar el servidor**, y consultar métricas y el registro de accesos.

### 2.3 Aplicación cliente de monitoreo

`client` es una aplicación de terminal (I/O bloqueante, permitido por su simpleza) que
habla el protocolo SMP: se encarga del *handshake*, la autenticación con token, el framing
CRLF y el *dot-stuffing* de las respuestas multilínea. **No es netcat**: el usuario invoca
verbos ergonómicos (p. ej. `client 127.0.0.1 8080 -t <token> ADD-USER pablito pass1234`) y
el cliente traduce eso al protocolo.

---

## 3. Problemas encontrados durante el diseño y la implementación

- **División e integración entre 3 personas.** Se refactorizó la lógica en módulos con
  *dueño único* y contratos (headers) acordados, para trabajar en paralelo sin pisarse. La
  integración final de los tres *tracks* (SOCKS core, monitoreo, observabilidad/infra)
  requirió coordinar los puntos de contacto.
- **Cableado de contratos.** Los módulos de observabilidad (métricas, registro de accesos)
  se implementaron antes que sus consumidores. Hubo que cablearlos en el data-path: el
  conteo de bytes en el relay y el `access_log_record` en el punto donde se decide la
  respuesta al request.
- **`GET-LOG` y el contrato del registro de accesos.** El comando de consulta del log
  requería una **API de lectura** en el registro de accesos que inicialmente no existía
  (solo escribía a `stdout`). Se acordó y agregó un buffer circular en memoria con una
  interfaz de conteo/iteración, desacoplando quién escribe (data-path) de quién lee (mgmt).
- **Correctitud de E/S no bloqueante.** Una primera versión trataba **cualquier** retorno
  `-1` de `recv`/`send` como cierre de conexión. En sockets no bloqueantes, `EAGAIN`,
  `EWOULDBLOCK` e `EINTR` significan "reintentar", no "error": se corrigió para distinguir
  esos casos de un EOF (`n == 0`) o un error real.
- **Códigos REP genéricos.** Al principio todo fallo de conexión reportaba
  `HOST_UNREACHABLE`. Se agregó el mapeo de `errno` a códigos REP específicos.

---

## 4. Limitaciones de la aplicación

- **Techo de ~510 conexiones concurrentes.** El selector usa `select(2)`, limitado por
  `FD_SETSIZE` (típicamente 1024 descriptores). Como cada conexión usa 2 descriptores
  (cliente + origen), el máximo práctico es ≈ 510 (cumple el mínimo de 500 de la consigna,
  pero es un techo duro).
- **Sin timeout de inactividad.** Una conexión que se queda "colgada" (un cliente lento que
  no cierra) ocupa su lugar indefinidamente. Esto habilita un ataque tipo *slowloris* y
  puede **demorar el graceful shutdown** (que espera a que drenen las conexiones); la
  segunda señal fuerza el apagado.
- **Volatilidad.** Las métricas, el registro de accesos (buffer circular de las últimas
  1024 entradas) y los usuarios son **volátiles**: se pierden al reiniciar. La consigna lo
  permite para las métricas; los usuarios se siembran por línea de comandos y por el
  protocolo de monitoreo.
- **Cliente de monitoreo.** Cumple el requisito de fondo (no es netcat; maneja auth,
  framing y dot-stuffing), pero el host y el puerto se pasan como argumentos posicionales
  en lugar de opciones POSIX con nombre.

---

## 5. Posibles extensiones

- **`epoll`/`kqueue`** detrás de la misma interfaz de selector para superar el límite de
  `FD_SETSIZE` y escalar más allá de las ~510 conexiones.
- **Timeouts de inactividad** (reaping de conexiones ociosas) para mitigar *slowloris* y
  hacer el graceful shutdown acotado en el tiempo.
- **Sniffer de credenciales POP3** estilo ettercap (segunda entrega, consigna F10).
- **Persistencia** opcional de usuarios y del registro de accesos a archivo.
- **Métricas adicionales** (bytes por sentido, tasa de fallos de conexión, etc.).

---

## 6. Conclusiones

El trabajo permitió implementar un servidor concurrente real bajo restricciones estrictas:
**un único hilo, E/S no bloqueante multiplexada, y correctitud ante lecturas/escrituras
parciales**. La disciplina de modelar cada conexión como una máquina de estados y de tratar
los `EAGAIN`/`EWOULDBLOCK` como flujo de control esperado (no como error) resultó central
para la robustez. El diseño de un protocolo de aplicación propio (SMP) y su documentación
"estilo RFC" ejercitó la capacidad de especificar de forma implementable e inequívoca. Las
pruebas de estrés confirmaron el comportamiento esperado (techo de conexiones dado por
`select`, degradación de throughput por el hilo único) y las herramientas de análisis
(Valgrind, tests unitarios) dieron confianza sobre la ausencia de fugas.

---

## 7. Ejemplos de prueba

Metodología completa y reproducible en [`docs/STRESS.md`](./STRESS.md). Resumen:

**Tests unitarios (`make test`):** 70 casos en verde, cubriendo los módulos puros
(`users`, `metrics`, `config`, `access_log`, incluido el *wraparound* del buffer circular)
y el dispatch del protocolo de monitoreo (gating por auth, comandos, dot-stuffing).

**Pruebas de integración** (con `curl --socks5`): conexión con auth a destinos IPv4, IPv6 y
FQDN; descarga de archivos grandes (relay); credenciales inválidas rechazadas; consultas por
el cliente de monitoreo; graceful shutdown.

**Pruebas de estrés** (en Docker, destino local):

| Métrica | Resultado |
|---|---|
| Máximo de conexiones concurrentes | **510** (supera el mínimo de 500; techo por `FD_SETSIZE`) |
| Throughput 1 conexión | 237 MB/s |
| Throughput 10 / 50 / 100 conexiones (agregado) | 847 / 795 / 700 MB/s |
| Throughput por conexión (1 → 100) | 237 → 7 MB/s (degrada por el hilo único) |
| Valgrind (memoria + descriptores) | `0 errors`, sin fugas de fds |

> **Nota metodológica:** las cifras de throughput se midieron sobre *loopback* con un archivo
> de ceros, dentro de un contenedor Docker. Representan el **overhead interno del relay** (un
> techo), no throughput de red real; deben leerse por su **tendencia** (pico y degradación) más
> que por su valor absoluto. El número de conexiones (510) es determinista y reproducible.

---

## 8. Guía de instalación

**Requisitos:** un entorno POSIX (Linux/macOS) con `gcc` (C11), `make` y `pthreads`. En
Windows se recomienda WSL o un contenedor Docker.

```bash
git clone <repo> && cd TPE-Protos
make                 # genera bin/server y bin/client
make test            # (opcional) corre la suite de tests
make clean           # limpia binarios y objetos
```

Artefactos generados: `bin/server` y `bin/client`.

Compilación reproducible en contenedor:
```bash
docker run --rm -v "$PWD:/work" -w /work gcc:latest make
```

---

## 9. Instrucciones de configuración

Ambos binarios siguen las convenciones de línea de comandos POSIX. Opciones del **servidor**:

| Opción | Descripción | Default |
|---|---|---|
| `-p <puerto>` | Puerto del proxy SOCKS | `1080` |
| `-P <puerto>` | Puerto del servicio de monitoreo | `8080` |
| `-l <dir>` | Dirección de escucha del proxy SOCKS | todas las interfaces |
| `-L <dir>` | Dirección de escucha del monitoreo | `127.0.0.1` (loopback) |
| `-u <user>:<pass>` | Usuario del proxy (hasta 10) | — |
| `-t <token>` | Token del canal administrativo | (default compilado) |
| `-h` / `-v` | Ayuda / versión | — |

El servicio de monitoreo escucha **por defecto solo en loopback** por seguridad; para
administrarlo remotamente hay que pedirlo explícitamente con `-L 0.0.0.0`.

---

## 10. Ejemplos de configuración y monitoreo

**Levantar el servidor** con un usuario y un token de admin:
```bash
./bin/server -p 1080 -P 8080 -u pablito:pass1234 -t miTokenSecreto
```

**Usar el proxy** (con `curl`):
```bash
curl -x socks5h://pablito:pass1234@localhost:1080 http://example.com
```

**Administrar por el cliente de monitoreo:**
```bash
./bin/client 127.0.0.1 8080 -t miTokenSecreto GET-METRICS
./bin/client 127.0.0.1 8080 -t miTokenSecreto ADD-USER juan clave5678
./bin/client 127.0.0.1 8080 -t miTokenSecreto LIST-USERS
./bin/client 127.0.0.1 8080 -t miTokenSecreto SET-CONFIG auth-required on
./bin/client 127.0.0.1 8080 -t miTokenSecreto GET-LOG
```

**Formato del registro de accesos** (una línea por acceso, separada por tabs), tanto a
`stdout` como consultable por `GET-LOG`:
```
2026-07-04T17:38:00Z    pablito    ::ffff:127.0.0.1:59036    example.com:80    REP=0x00
```
(timestamp ISO-8601 UTC · usuario · cliente ip:puerto · destino:puerto · código REP)

---

## 11. Documento de diseño (arquitectura)

El diseño arquitectónico detallado está en [`docs/DESIGN.md`](./DESIGN.md). Resumen:

**Principio central: un proceso, un hilo, un selector.** Toda la E/S es no bloqueante y se
multiplexa en un único *event loop*. El único hilo adicional permitido es el de resolución
de DNS (`getaddrinfo`), que no hace otra E/S y solo notifica su resultado al hilo principal
mediante una señal (técnica `pselect`).

**Capas:**

```
                 main.c  (levanta 2 sockets pasivos, señales, graceful shutdown)
                /                                   \
       socks5.c  [track A]                    mgmt/  [track B]
   (MEF por conexión SOCKS5)          (MEF del protocolo SMP + cliente)
                \                                   /
        users · metrics · access_log · config   [track C]
              (módulos transversales compartidos)
                              |
        selector · stm · buffer · parser · netutils · args  (infra de cátedra)
```

- **`selector`**: envuelve `select(2)` con un patrón *master/slave* de `fd_set` y una
  tabla fd→handler; despacha eventos de lectura/escritura y las notificaciones del hilo de
  DNS.
- **`stm`**: motor genérico de máquinas de estado; cada estado devuelve el próximo estado.
- **`buffer`**: buffer de bytes con punteros de lectura/escritura y compactación, que hace
  correcta **por construcción** la E/S parcial.
- **Módulos transversales (track C):** `users` (store con comparación en tiempo constante),
  `metrics` (contadores volátiles), `access_log` (buffer circular + salida a stdout),
  `config` (config mutable en runtime, sin locks por ser un único hilo). Se ubican en una
  capa compartida porque los consumen **tanto** el proxy **como** el protocolo de monitoreo.

**Decisiones de diseño documentadas** (ver [`docs/DESIGN_DECISIONS.md`](./DESIGN_DECISIONS.md)):
p. ej., la métrica de bytes cuenta únicamente el *payload* efectivamente relayeado (ambos
sentidos), no los bytes de control del protocolo, por ser más representativa de la carga
útil del sistema.

---

## Apéndice A — Especificación del protocolo de monitoreo

La especificación completa, en estilo RFC y agnóstica al lenguaje, se encuentra en
[`docs/PROTOCOL.md`](./PROTOCOL.md) y forma parte de esta entrega (se recomienda incluirla
como anexo al generar el PDF final).

---

> **Nota para el grupo (borrador):** al generar el PDF final (`informe.pdf` en la raíz, como
> indica el README), concatenar `PROTOCOL.md` como Apéndice A. Conviene revisar/expandir §6
> (conclusiones) con la experiencia personal del grupo. Los números de estrés de §7 son de
> una corrida de referencia; conviene re-correr en el entorno de entrega y actualizarlos.
