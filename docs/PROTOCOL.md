# SMP — SOCKS Management Protocol, versión 1.0

> ITBA · Protocolos de Comunicación · TPE 2026/1
> Especificación del **protocolo de monitoreo y configuración** (consigna F7 / NF5).
> Estado: **BORRADOR (MF0)** — el catálogo de comandos está congelado; los detalles de
> `GET-LOG` dependen de un cambio de contrato pendiente con Persona C (ver §10).

Este documento describe, de forma **agnóstica al lenguaje de programación**, un protocolo
de aplicación para administrar y monitorear el servidor proxy SOCKS5 en tiempo de
ejecución, sin reiniciarlo. Es un protocolo **nuevo e independiente** de SOCKS5: corre en
otro socket pasivo, en otro puerto, dentro del mismo proceso. Cualquiera que lea este
documento DEBE poder implementar un cliente o un servidor interoperable.

Las palabras clave "DEBE", "NO DEBE", "DEBERÍA", "PUEDE" y "OPCIONAL" se interpretan según
el [RFC 2119](https://datatracker.ietf.org/doc/html/rfc2119).

---

## 1. Decisiones de diseño y justificación

| Decisión | Elección | Por qué |
|---|---|---|
| **Transporte** | TCP, multiplexado en el mismo selector no bloqueante del proxy. | Confiable y orientado a conexión; reutiliza la infraestructura existente. |
| **Codificación** | **Texto**, líneas terminadas en CRLF, ASCII. | Simple de implementar, depurable con `telnet`/`nc`, y suficiente para un canal administrativo de bajo volumen. Inspirado en POP3/SMTP. |
| **Modelo** | Petición/respuesta sincrónico (un request → una response), sobre una conexión persistente. | El cliente es secuencial; no hace falta pipelining ni IDs de correlación. |
| **Autenticación** | **Token compartido** enviado en el comando `AUTH`. | Suficiente para la primera entrega; el secreto se configura por línea de comandos en servidor y cliente (ver §10). |
| **Estado** | Con estado: la conexión arranca *no autenticada* y pasa a *autenticada* tras un `AUTH` válido. | Evita repetir credenciales en cada comando. |

> El cliente de monitoreo NO es `netcat`: traduce subcomandos ergonómicos
> (p. ej. `client add-user pablito pass1234`) a las líneas de este protocolo, como exige
> la consigna (NF5). Que el wire sea legible es una ventaja de depuración, no un sustituto
> del cliente.

---

## 2. Modelo de la conexión

```
  cliente                                   servidor
    |  ── TCP connect ───────────────────────▶ |
    |  ◀──────── saludo (+OK ... ready) ─────── |   estado: NO_AUTH
    |  ── AUTH <token> ─────────────────────▶  |
    |  ◀──────── +OK authenticated ─────────── |   estado: AUTH
    |  ── <comando> ────────────────────────▶  |
    |  ◀──────── <respuesta> ───────────────── |
    |             ...                           |
    |  ── QUIT ─────────────────────────────▶  |
    |  ◀──────── +OK bye ────────────────────  |
    |  ◀──────── TCP close ──────────────────  |
```

Estados de la sesión (servidor):

- **NO_AUTH** (inicial): sólo se aceptan `AUTH`, `HELP` y `QUIT`. Cualquier otro comando
  DEBE responder `-ERR 3 authentication required`.
- **AUTH**: se aceptan todos los comandos.

El servidor DEBE enviar un **saludo** apenas se establece la conexión TCP, antes de recibir
nada del cliente:

```
+OK SMP/1.0 ready
```

(forma general: `+OK <NAME>/<VERSION> ready`, con `NAME`/`VERSION` de `mgmt_proto.h`).

---

## 3. Framing y sintaxis léxica

- Toda unidad de protocolo es una **línea** terminada en `CRLF` (`\r\n`).
- Una línea (CRLF incluido) NO DEBE superar **1024 octetos**. Si el servidor recibe una
  línea más larga, DEBE responder `-ERR 1 line too long` y PUEDE cerrar la conexión.
- Un **request** es: un *verbo* seguido de cero o más *argumentos*, separados por **un
  espacio** (`SP`, 0x20) cada uno.
- Los **verbos** son case-insensitive (`AUTH` == `auth`). Los **argumentos** son
  case-sensitive (nombres de usuario, contraseñas y tokens distinguen mayúsculas).
- Como los argumentos se separan por espacios, un argumento **NO DEBE** contener espacios
  ni caracteres de control. En particular, nombres de usuario, contraseñas y el token NO
  DEBEN contener `SP`, `CR` ni `LF` (ver Limitaciones, §9).

Gramática (ABNF, [RFC 5234](https://datatracker.ietf.org/doc/html/rfc5234)):

```abnf
request      = verb *( SP argument ) CRLF
verb         = 1*VCHAR
argument     = 1*VCHAR              ; VCHAR = %x21-7E (visible, sin espacios)
SP           = %x20
CRLF         = %x0D %x0A
```

---

## 4. Respuestas

Una respuesta es una de estas tres formas:

### 4.1 Éxito de una línea

```
+OK [texto-libre]
```

### 4.2 Error

```
-ERR <código> <mensaje>
```

`<código>` es un entero decimal de la tabla de §6. `<mensaje>` es texto libre legible para
humanos. El cliente DEBE decidir en base al **código numérico**, no al texto.

### 4.3 Respuesta multilínea

Usada por comandos que devuelven listas (`LIST-USERS`, `GET-LOG`, `HELP`). Estructura:

```
+OK <texto-libre>
<línea de datos 1>
<línea de datos 2>
...
.
```

- La primera línea es un `+OK` normal (PUEDE incluir un resumen, p. ej. la cantidad de
  ítems).
- Le siguen cero o más **líneas de datos**.
- El cuerpo termina con una línea que contiene **únicamente** un punto (`.` + CRLF).
- **Dot-stuffing**: si una línea de datos empieza con `.`, el emisor DEBE anteponerle otro
  `.`; el receptor DEBE removerlo. Esto evita confundir datos con el terminador.

Gramática:

```abnf
response       = ok-line / err-line / multiline
ok-line        = "+OK" [ SP text ] CRLF
err-line       = "-ERR" SP code SP text CRLF
multiline      = ok-line *( data-line CRLF ) "." CRLF
code           = 1*DIGIT
```

---

## 5. Comandos

Notación: **[A]** requiere estado AUTH; **[N]** se acepta también en NO_AUTH.

### 5.1 `AUTH <token>` **[N]**

Autentica el canal administrativo con el token compartido.

- Éxito: `+OK authenticated` → la sesión pasa a estado AUTH.
- Token inválido: `-ERR 4 authentication failed` (la sesión permanece en NO_AUTH; el
  servidor PUEDE limitar la cantidad de intentos).
- Sin argumento o con argumentos de más: `-ERR 5 invalid arguments`.

### 5.2 `LIST-USERS` **[A]**

Lista los usuarios del proxy. Respuesta multilínea, un nombre por línea:

```
+OK 2 users
pablito
jose
.
```

No se exponen contraseñas.

### 5.3 `ADD-USER <name> <pass>` **[A]**

Da de alta un usuario del proxy en runtime.

- Éxito: `+OK user added`
- Store lleno: `-ERR 6 user store full`
- Ya existe: `-ERR 7 user already exists`
- Nombre/contraseña vacío o demasiado largo: `-ERR 5 invalid arguments`

Mapea a `users_add()` (`users.h`). El servidor DEBE traducir `users_result` al código de
error correspondiente (`USERS_FULL`→6, `USERS_ALREADY_EXISTS`→7, `USERS_INVALID`→5).

### 5.4 `DEL-USER <name>` **[A]**

Da de baja un usuario del proxy.

- Éxito: `+OK user removed`
- No existe: `-ERR 8 user not found`

Mapea a `users_remove()`.

### 5.5 `GET-METRICS` **[A]**

Devuelve los contadores volátiles (consigna F6). Respuesta de una línea, `clave=valor`:

```
+OK connections-total=42 connections-current=3 bytes-transferred=190245
```

Las claves son las de §3.4 de este doc (`connections-total`, `connections-current`,
`bytes-transferred`). Mapea a `metrics_get()`.

### 5.6 `GET-CONFIG` **[A]**

Devuelve la configuración mutable actual. Respuesta de una línea, `clave=valor`, con
valores booleanos `on`/`off`:

```
+OK auth-required=on
```

### 5.7 `SET-CONFIG <key> <value>` **[A]**

Cambia un parámetro de configuración en runtime.

| key | value | efecto |
|---|---|---|
| `auth-required` | `on` / `off` | exige (o no) autenticación user/pass en el proxy SOCKS |

- Éxito: `+OK config updated`
- Clave desconocida: `-ERR 9 unknown config key`
- Valor inválido (no `on`/`off`): `-ERR 5 invalid arguments`

Mapea a `config_set_auth_required()`.

> Nota de alcance: esta implementación cubre la **primera entrega**. El sniffer de
> credenciales (clave `dissectors`, consigna F10) es de la segunda entrega y queda fuera.

### 5.8 `GET-LOG` **[A]**

Devuelve el registro de accesos (consigna F8) como respuesta multilínea, una entrada por
línea, con formato tabular separado por TAB:

```
+OK 2 entries
2026-06-27T14:03:11Z	pablito	10.0.0.5:51234	example.com:443	0
2026-06-27T14:05:02Z	-	10.0.0.6:51999	1.1.1.1:80	0
```

Campos (en orden): timestamp ISO-8601 UTC, usuario (`-` si NO-AUTH), `cliente_ip:puerto`,
`destino:puerto`, código REP de SOCKS. **Pendiente (§10):** requiere una API de lectura en
`access_log.h` que hoy no existe.

### 5.9 `HELP` **[N]**

Lista los comandos disponibles. Respuesta multilínea informativa.

### 5.10 `QUIT` **[N]**

Cierra la sesión ordenadamente: `+OK bye` y luego el servidor cierra el TCP.

---

## 6. Códigos de error

| Código | Símbolo (`mgmt_proto.h`) | Significado |
|---|---|---|
| 1 | `MGMT_ERR_SYNTAX` | Línea mal formada o demasiado larga. |
| 2 | `MGMT_ERR_UNKNOWN_CMD` | Verbo desconocido. |
| 3 | `MGMT_ERR_AUTH_REQUIRED` | Comando emitido sin autenticar. |
| 4 | `MGMT_ERR_AUTH_FAILED` | Token inválido. |
| 5 | `MGMT_ERR_INVALID_ARG` | Cantidad o forma de argumentos inválida. |
| 6 | `MGMT_ERR_USERS_FULL` | Store de usuarios lleno. |
| 7 | `MGMT_ERR_USER_EXISTS` | El usuario ya existe. |
| 8 | `MGMT_ERR_USER_NOT_FOUND` | El usuario no existe. |
| 9 | `MGMT_ERR_UNKNOWN_KEY` | Clave de configuración desconocida. |
| 10 | `MGMT_ERR_INTERNAL` | Fallo interno del servidor. |

---

## 7. Ejemplo de sesión completa

```
S: +OK SMP/1.0 ready
C: AUTH s3cr3t-token
S: +OK authenticated
C: ADD-USER pablito pass1234
S: +OK user added
C: LIST-USERS
S: +OK 1 users
S: pablito
S: .
C: GET-METRICS
S: +OK connections-total=42 connections-current=3 bytes-transferred=190245
C: SET-CONFIG auth-required on
S: +OK config updated
C: DEL-USER fantasma
S: -ERR 8 user not found
C: QUIT
S: +OK bye
```

---

## 8. Correctitud de I/O no bloqueante (nota de implementación)

Aunque el protocolo sea de texto, el servidor lo atiende en sockets **no bloqueantes
multiplexados**. La implementación DEBE tolerar **lecturas y escrituras parciales**: una
línea PUEDE llegar fragmentada en varios `recv`, y una respuesta PUEDE no enviarse de una
en un solo `send`. El parser acumula octetos en un `buffer` hasta encontrar `CRLF` y se
reentra cuando llega más data; el escritor avanza el puntero de lectura por los bytes
realmente enviados y rearma el interés de escritura por el resto. El cliente, por su
simpleza, PUEDE usar I/O bloqueante.

---

## 9. Limitaciones conocidas

- Nombres de usuario, contraseñas y token **no pueden contener espacios ni caracteres de
  control**, por ser un protocolo de texto separado por espacios. (RFC 1929 permite
  octetos arbitrarios; acá lo restringimos a `VCHAR`.) Una versión futura PODRÍA usar
  *quoting* o codificación (p. ej. Base64) para levantar esta restricción.
- Sin cifrado: el canal administrativo viaja en claro. Pensado para una red de
  administración confiable; una extensión PODRÍA envolverlo en TLS.
- Token único compartido, sin roles ni auditoría por administrador.

---

## 10. Dependencias pendientes con el resto del grupo

1. **Token del canal admin (Persona C / `args`):** `struct socks5args` (`args.h`) hoy no
   tiene campo para el token de management. Se necesita agregar uno (p. ej. `mng_token`) y
   una opción de línea de comandos. Hasta entonces, `AUTH` se puede probar contra un valor
   compilado por defecto.
2. **Lectura del access log (Persona C / `access_log.h`):** el contrato actual sólo expone
   `access_log_record()` (escritura). `GET-LOG` (§5.8) requiere una API de lectura/iteración.
   Se implementa en MF5; se acuerda la firma con C antes.
```
