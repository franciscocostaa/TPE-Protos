# SMP — Protocolo de Monitoreo (SOCKS Management Protocol), versión 1.0

Este documento describe el protocolo que diseñamos para administrar y monitorear el
servidor proxy SOCKS5 en tiempo de ejecución, sin reiniciarlo. Es un protocolo nuevo e
independiente de SOCKS: corre en otro socket pasivo, en otro puerto, dentro del mismo
proceso. La idea es que con esta descripción alcance para escribir un cliente (o un
servidor) compatible sin necesidad de mirar nuestro código.

## 1. Transporte y codificación

Usamos **TCP**, porque necesitábamos un transporte confiable y ordenado: el intercambio es
una secuencia de comandos y respuestas —algunas de varias líneas— y no queríamos tener que
lidiar con pérdidas ni reordenamientos, como pasaría con UDP.

El protocolo es **de texto**: cada mensaje es una línea terminada en CRLF (`\r\n`), con
caracteres ASCII. Nos basamos en el estilo de POP3/SMTP (respuestas que empiezan con `+OK`
o `-ERR`, y listas que terminan en una línea con un solo punto). La ventaja es que se
implementa y se depura fácil —incluso se puede probar a mano con `telnet` o `nc`— y para un
canal de administración de poco tráfico no hace falta algo más complejo que eso.

La autenticación es por **token compartido**: un único secreto que el administrador
configura en el servidor y que el cliente envía con el comando `AUTH`. Lo elegimos por
simplicidad y para no mezclar las credenciales de administración con los usuarios del proxy.

## 2. Modelo de conexión

Apenas se establece la conexión TCP, el **servidor habla primero** y manda un saludo:

```
+OK SMP/1.0 ready
```

A partir de ahí la sesión tiene dos estados:

- **No autenticado** (inicial): solo se aceptan `AUTH`, `HELP` y `QUIT`. Cualquier otro
  comando responde `-ERR 3 authentication required`.
- **Autenticado**: se aceptan todos los comandos. Se llega a este estado enviando un `AUTH`
  con el token correcto.

Una sesión típica se ve así:

```
S: +OK SMP/1.0 ready
C: AUTH <token>
S: +OK authenticated
C: <comando>
S: <respuesta>
   ...
C: QUIT
S: +OK bye
   (el servidor cierra la conexión)
```

## 3. Formato de los mensajes

Cada mensaje —tanto los pedidos del cliente como las respuestas del servidor— es una
**línea terminada en CRLF**. Una línea no puede superar los **1024 octetos** (CRLF
incluido); si el servidor recibe una más larga, responde `-ERR 1` y puede cerrar la
conexión.

Un pedido es un **verbo** seguido de cero o más **argumentos**, separados por un espacio:

```
VERBO [arg1 [arg2 ...]]
```

- Los verbos no distinguen mayúsculas de minúsculas (`AUTH` y `auth` son lo mismo).
- Los argumentos sí distinguen mayúsculas (nombres de usuario, contraseñas y token).
- Como los argumentos se separan por espacios, un argumento **no puede contener espacios ni
  caracteres de control**. Por eso los nombres de usuario, contraseñas y el token no pueden
  llevar espacios.

## 4. Respuestas

Hay tres formas de respuesta.

**Éxito de una línea:**

```
+OK [texto opcional]
```

**Error:**

```
-ERR <código> <mensaje>
```

El `<código>` es un número entero (ver sección 6) y el `<mensaje>` es texto libre para que
lo lea una persona. Un cliente debería decidir según el número, no según el texto.

**Respuesta de varias líneas** (para comandos que devuelven listas):

```
+OK <texto opcional>
<línea de datos>
<línea de datos>
...
.
```

El cuerpo termina con una línea que contiene **únicamente un punto**. Para que un dato que
empieza con `.` no se confunda con ese terminador, quien envía le
agrega un `.` extra al principio, y quien recibe se lo saca.

## 5. Comandos

Marcamos con **[A]** los comandos que requieren estar autenticado, y con **[N]** los que
también se aceptan sin autenticar.

### `AUTH <token>` — [N]
Autentica la sesión.
- Éxito: `+OK authenticated` (la sesión pasa a autenticada).
- Token incorrecto: `-ERR 4 authentication failed`.
- Falta el token o sobran argumentos: `-ERR 5 invalid arguments`.

### `LIST-USERS` — [A]
Lista los usuarios del proxy, uno por línea. No se muestran las contraseñas.

```
+OK 2 users
pablito
jose
.
```

### `ADD-USER <nombre> <contraseña>` — [A]
Da de alta un usuario del proxy.
- Éxito: `+OK user added`.
- No hay lugar para más usuarios: `-ERR 6 user store full`.
- El usuario ya existe: `-ERR 7 user already exists`.
- Nombre o contraseña vacíos o demasiado largos: `-ERR 5 invalid arguments`.

### `DEL-USER <nombre>` — [A]
Da de baja un usuario del proxy.
- Éxito: `+OK user removed`.
- No existe: `-ERR 8 user not found`.

### `GET-METRICS` — [A]
Devuelve los contadores del servidor en una línea, como pares `clave=valor`:

```
+OK connections-total=42 connections-current=3 bytes-transferred=190245
```

### `GET-CONFIG` — [A]
Devuelve la configuración que se puede cambiar en runtime, con valores `on`/`off`:

```
+OK auth-required=on
```

### `SET-CONFIG <clave> <valor>` — [A]
Cambia un parámetro en runtime. La clave disponible es `auth-required` (`on`/`off`), que
activa o desactiva la exigencia de usuario y contraseña en el proxy.
- Éxito: `+OK config updated`.
- Clave desconocida: `-ERR 9 unknown config key`.
- Valor inválido (no es `on` ni `off`): `-ERR 5 invalid arguments`.

### `GET-LOG` — [A]
Devuelve el registro de accesos, una entrada por línea, con los campos separados por TAB:

```
+OK 2 entries
2026-06-27T14:03:11Z	pablito	10.0.0.5:51234	example.com:443	0
2026-06-27T14:05:02Z	-	10.0.0.6:51999	1.1.1.1:80	0
.
```

Los campos, en orden, son: timestamp en ISO-8601 UTC, usuario (`-` si la conexión no se
autenticó), cliente `ip:puerto`, destino `dirección:puerto`, y el código de respuesta de
SOCKS.

### `HELP` — [N]
Devuelve la lista de comandos disponibles, como respuesta de varias líneas.

### `QUIT` — [N]
Cierra la sesión ordenadamente: el servidor responde `+OK bye` y cierra la conexión.

## 6. Códigos de error

| Código | Significado |
|---|---|
| 1 | Línea mal formada o demasiado larga |
| 2 | Verbo desconocido |
| 3 | Comando enviado sin autenticar |
| 4 | Token inválido |
| 5 | Cantidad o forma de argumentos inválida |
| 6 | No hay lugar para más usuarios |
| 7 | El usuario ya existe |
| 8 | El usuario no existe |
| 9 | Clave de configuración desconocida |
| 10 | Error interno del servidor |

## 7. Ejemplo de sesión completa

```
S: +OK SMP/1.0 ready
C: AUTH s3cr3t
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

## 8. Nota sobre la implementación

Aunque el protocolo sea de texto, del lado del servidor se atiende con sockets no
bloqueantes, así que la implementación tiene que tolerar mensajes que llegan partidos en
varios pedazos y respuestas que no se pueden enviar de una sola vez. El cliente, por ser
simple y secuencial, puede usar I/O bloqueante.

Un cliente puede ofrecerle al usuario comandos más cómodos que estos verbos (por ejemplo,
`add-user pablito pass1234` en vez de escribir el `AUTH` y el `ADD-USER` a mano) y
traducirlos internamente a los mensajes de este protocolo; eso queda del lado del cliente y
no forma parte de la especificación.

## 9. Limitaciones conocidas

- Los nombres de usuario, contraseñas y el token no pueden contener espacios ni caracteres
  de control, por tratarse de un protocolo de texto separado por espacios.
- El canal no está cifrado: está pensado para una red de administración de confianza. Una
  versión futura podría envolverlo en TLS.
- Hay un único token compartido, sin roles ni distinción por administrador.
