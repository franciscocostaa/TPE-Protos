# TPE-Protos — Proxy SOCKS5 + Protocolo de Monitoreo (SMP)

Trabajo Práctico Especial de Protocolos de Comunicación (ITBA, 2026/1C). Implementa:

- Un **proxy SOCKS5** (RFC 1928) con autenticación usuario/contraseña (RFC 1929), que
  atiende conexiones entrantes y las reenvía a los destinos pedidos (IPv4, IPv6 o nombre
  de dominio).
- Un **protocolo de monitoreo y configuración propio, SMP** (SOCKS Management Protocol),
  servido en un socket y puerto independientes del proxy, para administrar usuarios,
  consultar métricas, leer el registro de accesos y cambiar configuración en runtime sin
  reiniciar el servidor.
- Un **cliente de línea de comandos** para hablar el protocolo SMP contra el servidor.

Ambos servicios corren en un único proceso (`bin/server`), multiplexados con un selector
de I/O no bloqueante sobre un solo hilo.

---

## Integrantes G19

| Nombre | Apellido | Legajo | Email |
|---|---|---|---|
| Roman | Salerno | 65145 | rsalerno@itba.edu.ar |
| Franco | Branda | 65506 | fbranda@itba.edu.ar |
| Francisco | Costa | 65202 | frcosta@itba.edu.ar |

---

## Estructura del proyecto

Ubicación de todo el material de la entrega:

| Contenido | Ubicación |
|---|---|
| Informe de la entrega (PDF) | Raíz del repositorio (`informe.pdf`) |
| Código fuente del servidor | `src/server/` (proxy SOCKS5 en `socks5.c`/`socks5.h`, protocolo de monitoreo en `src/server/mgmt/`, y los módulos `users.c`, `metrics.c`, `config.c`, `access_log.c`) |
| Código fuente del cliente de monitoreo | `src/client/client.c` |
| Código compartido servidor/cliente | `src/shared/` (parser genérico, máquina de estados, selector de I/O, buffer, utilidades de red, parseo de argumentos) |
| Archivos de construcción | `Makefile` y `Makefile.inc` (raíz del repositorio) |
| Artefactos generados (binarios) | `bin/server`, `bin/client` (y binarios de test), generados por `make` |
| Objetos intermedios de compilación | `obj/` (generado por `make`, no se versiona) |
| Tests unitarios | `test/` (harness propio, ver sección "Compilación") |

---

## Compilación

### Requisitos

- `gcc` con soporte de C11.
- `make`.
- Biblioteca de hilos POSIX (`pthread`), habitualmente ya disponible en Linux y macOS.
- Sistema operativo tipo POSIX (probado en Linux y macOS).

### Comandos

Desde la raíz del repositorio:

```sh
make          # compila cliente y servidor (equivalente a: make client server)
make clean    # borra bin/ y obj/
make test     # compila y corre los tests unitarios (users, metrics, config,
              # access_log y el dispatcher de comandos de mgmt)
```

`make` compila todas las fuentes de `src/server`, `src/client` y `src/shared`
(objetos intermedios en `obj/`, reflejando la misma estructura de `src/`) y deja los
binarios finales en:

- `bin/server` — servidor SOCKS5 + monitoreo.
- `bin/client` — cliente de monitoreo.

No hace falta ningún paso de instalación adicional: ambos binarios son autocontenidos y
se ejecutan directamente desde `bin/`.

---

## Ejecución del servidor

```sh
./bin/server [OPCION]...
```

| Opción | Descripción | Valor por defecto |
|---|---|---|
| `-l <SOCKS addr>` | Dirección donde escucha el proxy SOCKS5. | `0.0.0.0` |
| `-p <SOCKS port>` | Puerto donde escucha el proxy SOCKS5. | `1080` |
| `-L <conf addr>` | Dirección donde escucha el servicio de monitoreo (SMP). | `127.0.0.1` |
| `-P <conf port>` | Puerto donde escucha el servicio de monitoreo (SMP). | `8080` |
| `-u <name>:<pass>` | Usuario habilitado para autenticarse contra el proxy SOCKS5. Se puede repetir hasta 10 veces (una por usuario). | ninguno |
| `-t <token>` | Token de autenticación del canal de monitoreo (comando `AUTH` de SMP). | `admin` (valor compilado, ver `src/server/mgmt/mgmt_cmd.c`) |
| `-v` | Imprime la versión y termina. | — |
| `-h` | Imprime la ayuda y termina. | — |

Si no se pasa ningún `-u`, el proxy SOCKS5 no exige autenticación (`auth-required=off`);
en cuanto se carga al menos un usuario con `-u`, el proxy exige autenticación por
defecto. Esto puede cambiarse en runtime con `SET-CONFIG auth-required on|off` (ver
`docs/PROTOCOL.md`, §5.7).

Ejemplo:

```sh
./bin/server -p 1080 -P 8080 -u ana:1234 -t s3cr3t
```

Levanta el proxy SOCKS5 en el puerto 1080 (todas las interfaces), el canal de
monitoreo en `127.0.0.1:8080`, un usuario `ana`/`1234` habilitado para el proxy, y
`s3cr3t` como token de administración.

El proceso corre en primer plano y termina ordenadamente (graceful shutdown,
drenando las conexiones activas) con `SIGINT`/`SIGTERM`; una segunda señal fuerza el
cierre inmediato.

---

## Ejecución del cliente de monitoreo

```sh
./bin/client <host> <port> [-t <token>] <VERB> [args...]
```

- `<host> <port>`: dirección y puerto del servicio de monitoreo del servidor (`-L`/`-P`
  del servidor).
- `-t <token>`: opcional; si se pasa, el cliente se autentica (`AUTH <token>`) antes de
  enviar el comando.
- `<VERB> [args...]`: el comando SMP a ejecutar y sus argumentos, tal como se describen
  en `docs/PROTOCOL.md` (§5): `AUTH`, `LIST-USERS`, `ADD-USER <name> <pass>`,
  `DEL-USER <name>`, `GET-METRICS`, `GET-CONFIG`, `SET-CONFIG <key> <value>`, `GET-LOG`,
  `HELP`, `QUIT`.

El cliente imprime el saludo del servidor, la respuesta del `AUTH` (si corresponde), la
respuesta del comando y, por último, la confirmación de cierre (`+OK bye`).

Ejemplos (contra el servidor levantado más arriba):

```sh
./bin/client 127.0.0.1 8080 -t s3cr3t GET-METRICS
./bin/client 127.0.0.1 8080 -t s3cr3t LIST-USERS
./bin/client 127.0.0.1 8080 -t s3cr3t ADD-USER pablito pass1234
./bin/client 127.0.0.1 8080 -t s3cr3t HELP
```

---

## Prueba rápida de humo

Con el servidor del ejemplo anterior corriendo (`./bin/server -p 1080 -P 8080 -u
ana:1234 -t s3cr3t`), en otra terminal:

**1. Tráfico a través del proxy SOCKS5**, usando las credenciales `ana`/`1234`:

```sh
curl -x socks5h://ana:1234@127.0.0.1:1080 http://ejemplo.com/
```

(`socks5h` hace que sea el proxy, y no el cliente, quien resuelva el nombre de
dominio, ejercitando el `ATYP` de dominio del proxy.)

**2. Consultar el estado del servidor por el canal de monitoreo:**

```sh
./bin/client 127.0.0.1 8080 -t s3cr3t GET-METRICS
```

Debería reportar al menos una conexión total (`connections-total`) tras el `curl`
anterior.

```sh
./bin/client 127.0.0.1 8080 -t s3cr3t LIST-USERS
```

Debería listar a `ana` como único usuario cargado.
