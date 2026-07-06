# Licencia y atribución de código — TPE-Protos

Este documento declara la **autoría, el origen y las condiciones de uso** de todo
el código que forma parte de esta entrega. Su objetivo es dejar constancia
explícita de qué escribió el grupo y qué se reutilizó de terceros, en
cumplimiento del requerimiento no funcional **NF8** de la consigna, que regula el
uso de librerías o fragmentos de código de terceros.

Grupo 19 — Protocolos de Comunicación (72.07), ITBA, cuatrimestre 2026/1.

---

## 1. Resumen

| Componente | Origen | Condición |
|---|---|---|
| Lógica del proxy SOCKS5 y del canal de monitoreo (`src/server/`) | **Grupo 19** (original) | Autoría propia |
| Cliente de monitoreo (`src/client/`) | **Grupo 19** (original) | Autoría propia |
| Diseño del protocolo de monitoreo SMP y su documentación | **Grupo 19** (original) | Autoría propia |
| Infraestructura de I/O y utilidades (`src/shared/`) | **Cátedra** (material del curso) | Uso permitido por NF8, con atribución |
| Parser de argumentos (`args.c` / `args.h`) | **Cátedra**, adaptado por el grupo | Uso permitido por NF8, con atribución |
| Bibliotecas del sistema (libc, sockets POSIX, `pthread`) | Plataforma / POSIX | No son terceros en el sentido de NF8 |

No se utilizó **ninguna biblioteca externa de terceros** más allá del material
publicado por la cátedra. En consecuencia, no hubo que solicitar aprobación de la
cátedra por el foro (NF8, incisos 2 y 3), que aplica a librerías externas.

---

## 2. Código original del grupo

Es la parte central del trabajo y resuelve **las cuestiones de fondo** de la
consigna. Fue escrito íntegramente por el Grupo 19:

- **Servidor SOCKS5** (`src/server/`):
  - `main.c` — armado de los dos sockets pasivos, ciclo del selector y
    *graceful shutdown*.
  - `socks5.c`, `socks5.h` — máquina de estados de la conexión SOCKS5
    (negociación de método, autenticación RFC 1929, parseo del request,
    resolución de nombres en hilo aparte, conexión con *fallback* de
    direcciones y relay bidireccional).
  - `mgmt/mgmt.c`, `mgmt/mgmt_cmd.c`, `mgmt/mgmt.h`, `mgmt/mgmt_cmd.h`,
    `mgmt/mgmt_proto.h` — transporte, despacho de comandos y contrato de wire
    del **protocolo de monitoreo SMP** diseñado por el grupo.
  - `metrics.c/.h`, `users.c/.h`, `config.c/.h`, `access_log.c/.h` — métricas,
    gestión de usuarios, configuración en runtime y registro de accesos.
- **Cliente de monitoreo** (`src/client/client.c`).
- **Pruebas propias** (`test/`): `users_test.c`, `metrics_test.c`,
  `config_test.c`, `access_log_test.c`, `mgmt_cmd_test.c`.
- **Documentación de diseño** (`docs/`), incluida la especificación del
  protocolo SMP en estilo RFC.

**Términos.** Este código es trabajo académico del Grupo 19, entregado para su
evaluación en la materia. Los autores conservan su autoría. Si en el futuro el
grupo decidiera publicarlo, podría hacerlo bajo una licencia aprobada por la
Open Source Initiative (por ejemplo MIT o BSD-3-Clause); dicha decisión no forma
parte de esta entrega.

> Nota: el archivo `src/shared/shared.c` / `shared.h` es un *stub* residual del
> armado inicial del proyecto y no contiene lógica relevante.

---

## 3. Código provisto por la cátedra

La cátedra publica, durante la cursada, código de **infraestructura genérica**
que la consigna autoriza expresamente a reutilizar (NF8, último párrafo:
*"Está permitido utilizar código publicado por los docentes durante la cursada
actual, siempre que se atribuya correctamente."*). Ese código vive en
`src/shared/`:

| Archivo | Qué aporta |
|---|---|
| `selector.c` / `selector.h` | Multiplexor de I/O no bloqueante (basado en `pselect`). |
| `buffer.c` / `buffer.h` | Buffer de lectura/escritura con acceso directo para I/O. |
| `stm.c` / `stm.h` | Motor genérico de máquinas de estado. |
| `parser.c` / `parser.h` | Motor genérico de parsers/lexers. |
| `parser_utils.c` / `parser_utils.h` | *Factory* de parsers típicos. |
| `netutils.c` / `netutils.h` | Utilidades de red (p. ej. `sockaddr` a texto). |
| `tests.h` | *Helpers* mínimos para las pruebas unitarias. |
| `args.c` / `args.h` | Parser de argumentos oficial de la cátedra, **adaptado** por el grupo (se agregaron opciones y campos propios: usuarios, dirección/puerto de monitoreo y token de administración). |

Las pruebas `buffer_test.c`, `parser_test.c`, `parser_utils_test.c`,
`selector_test.c`, `stm_test.c` y `netutils_test.c` también provienen del
material de cátedra.

**Por qué su uso es válido según NF8.** Estas piezas son *infraestructura*: no
implementan SOCKS5, ni la autenticación, ni el protocolo de monitoreo, ni las
métricas o el registro de accesos. Es decir, **no resuelven las cuestiones de
fondo** del trabajo (NF8, inciso 1); esas las implementó el grupo (sección 2).
Su reutilización está explícitamente permitida por la consigna, y queda
atribuida en este documento y en la historia del repositorio git (fueron
incorporadas originalmente como *patches* provistos por la cátedra, aplicados con
`git am`).

**Licencia.** El material de cátedra se distribuye para uso en el curso y su
reutilización se rige por la autorización de la consigna citada arriba. El
identificador de licencia exacto bajo el que la cátedra publica estas utilidades
es el que figura en su fuente de origen (campus / repositorio de la cátedra); ver
sección 5.

---

## 4. Bibliotecas del sistema

El proyecto enlaza únicamente contra la biblioteca estándar de C y funciones
POSIX de la plataforma (sockets de Berkeley, `getaddrinfo`, `getopt`,
`pthread`). No son dependencias de terceros en el sentido de NF8: son parte del
sistema operativo y del estándar del lenguaje.

Sobre el uso de **hilos**: `pthread` se usa exclusivamente para descargar la
resolución de nombres (`getaddrinfo`) en un hilo auxiliar que solo resuelve y
despierta al hilo principal —la **única** excepción al modelo de un solo hilo que
la consigna permite— y de forma interna por el selector de la cátedra. Ningún
otro I/O usa hilos.

---

## 5. Verificación pendiente

Para cerrar formalmente este documento, el grupo debe:

1. **Citar el identificador de licencia exacto** del material de cátedra
   reutilizado (`selector`, `buffer`, `stm`, `parser`, `parser_utils`,
   `netutils`, `tests.h`, `args`), tomándolo de la fuente publicada por la
   cátedra, y agregarlo a la sección 3.
2. **Reemplazar el texto provisional** `"AQUI VA LA LICENCIA"` que hoy imprime la
   opción `-v` del servidor (en `src/shared/args.c`) por una línea real —por
   ejemplo, una referencia a este archivo `LICENCIA.md`.
