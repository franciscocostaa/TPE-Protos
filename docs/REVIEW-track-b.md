# Code Review — Track B (protocolo de monitoreo SMP)

> ITBA · Protocolos de Comunicación · TPE 2026/1
> Revisión automática con `ecc:cpp-reviewer` sobre el track de Persona B, tras MF0–MF3.
> Estado: **para discutir en grupo** — los cambios sugeridos NO están aplicados todavía.

## Alcance revisado

- `src/server/mgmt/mgmt.c` — transporte: accept + máquina de estados (stm) + framing de líneas CRLF, tolerante a I/O parcial.
- `src/server/mgmt/mgmt_cmd.c` — dispatch de comandos: AUTH, GET-METRICS, LIST-USERS, ADD-USER, DEL-USER, HELP, QUIT.
- `src/server/mgmt/mgmt_cmd.h`, `src/server/mgmt/mgmt_proto.h` — interfaces y contrato de wire.
- `src/client/client.c` — cliente de management (I/O bloqueante).

La infra de cátedra (`selector`, `stm`, `buffer`) **no** se audita; sólo se usa.

## Veredicto del reviewer: **BLOCK**

Bloquea por los dos hallazgos **CRÍTICOS** (#1 y #2): el manejo de `recv`/`send` no es correcto para I/O no bloqueante porque trata cualquier `-1` como error fatal, sin distinguir `EAGAIN`/`EWOULDBLOCK`/`EINTR`. Esto es exactamente el criterio pass/fail de "correctitud de lecturas/escrituras parciales" de la consigna.

> ⚠️ **Matiz importante para discutir en grupo (lo verificó Persona B):** el `socks5.c` de
> referencia (track A, espejado del código de cátedra) hace **exactamente lo mismo** —
> `recv` con `n <= 0` y `send` con `n == -1` se tratan como fatal, sin chequear `errno`
> (ver `socks5.c:315, 371, 500, 684, 746, 772`). Y tiene el mismo `abort()` en su `done`
> (`socks5.c:294`). O sea: **no es un bug exclusivo de B**, es un patrón que está en todo
> el proyecto. La decisión de fondo (¿lo arreglamos en todos lados?) es de grupo.

---

## CRÍTICO

### #1 — `recv`/`send` con `-1` se trata siempre como error fatal
**Archivo:** `mgmt.c` → `mgmt_on_greeting`, `mgmt_on_read`, `mgmt_on_write`.

En un socket no bloqueante multiplexado, el selector puede despertar el handler de forma
optimista; un `recv`/`send` puede devolver `-1` con `errno == EAGAIN`/`EWOULDBLOCK`, o ser
interrumpido por una señal (`EINTR`). Hoy cualquiera de esos casos **cierra la conexión**.

**Fix sugerido:** incluir `<errno.h>` y, en los tres sitios, reintentar (quedarse en el
mismo estado, dejando que el selector vuelva a invocar el handler) cuando `errno` sea
`EAGAIN`, `EWOULDBLOCK` o `EINTR`; ir a error/cierre sólo en errores reales.

```c
static inline bool io_retryable(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
/* send: */ if (n == -1) return io_retryable() ? MISMO_ESTADO : MGMT_ERROR;
```

### #2 — `mgmt_on_read` mezcla EOF (`n == 0`) con error (`n < 0`)
**Archivo:** `mgmt.c` → `mgmt_on_read` (`if (n <= 0) return MGMT_DONE;`).

Cerrar en ambos casos es razonable **sólo si `n == -1` es un error real**. Con el bug de #1,
un `EAGAIN`/`EINTR` cerraría conexiones válidas.

**Fix sugerido:** separar los tres casos — `n > 0` (avanzar), `n == 0` (EOF → cerrar),
`n == -1` (chequear `errno`: transitorio → reintentar, real → cerrar).

---

## MAYOR

### #4 — `abort()` ante fallo de `selector_unregister_fd`
**Archivo:** `mgmt.c` → `mgmt_done`.

Tirar abajo **todo el proceso** (incluido el proxy SOCKS5) por un fallo de desregistro de
una sola conexión administrativa es agresivo. **Es el patrón de cátedra** (`socks5.c:294`
hace lo mismo), así que puede ser un "fail-fast" intencional de un invariante que nunca
debería romperse en un diseño mono-hilo. Decisión de grupo: dejarlo (coherencia con A) o
loguear + cerrar el fd sin `abort()`.

### #6 — Truncado silencioso acoplado a `USERS_MAX`
**Archivo:** `mgmt_cmd.c` → `emit_str` (trunca si no hay espacio en el write buffer).

Documentado como limitación de MF1. El riesgo concreto: una respuesta truncada **sin CRLF
final** deja al cliente bloqueante esperando un `\n` que no llega. Peor caso de `LIST-USERS`
con `USERS_MAX=10` × 255 bytes + dot-stuffing ≈ 2600 bytes → entra en los 4096. **Pero** si
se sube `USERS_MAX` a 500 (hay un `TODO` en `users.h:17`), deja de entrar y trunca en
silencio.

**Fix sugerido:** un `_Static_assert` que acople `MGMT_WRITE_BUFFER_SIZE` al peor caso de
`USERS_MAX × USERS_NAME_MAX`, para que el build falle si alguien sube el límite; y, a
futuro (MF6), streaming de respuestas multilínea grandes en vez de armarlas enteras.

---

## MENOR / NIT

### #9 — Cliente: no drena la línea cuando excede `CLIENT_LINE_MAX` (MENOR)
**Archivo:** `client.c` → `read_line`.

Si el server manda una línea más larga que el buffer sin `\n`, `read_line` devuelve `false`
dejando el resto en el socket, lo que desincroniza el framing del resto de la sesión.
**Fix:** drenar hasta `\n`/EOF, o cerrar la conexión en ese caso.

### #11 — Cliente: no sanitiza CR/LF embebido en `argv` (MENOR)
**Archivo:** `client.c` → armado de `cmd`.

Un argumento con `\r\n` embebido inyectaría múltiples comandos en la misma conexión
(command smuggling). Riesgo bajo (es un cliente admin local), pero si los argumentos
vinieran de una fuente no confiable sería una vía de inyección. **Fix:** rechazar
argumentos que contengan `\r` o `\n` antes de armar la línea.

### #8 — Sin soporte de espacios en passwords (nota de diseño)
El protocolo es texto separado por espacios, sin quoting → passwords/usuarios no pueden
contener espacios. **Ya documentado** en `PROTOCOL.md §9 (Limitaciones)`. Sin acción.

### #10 — Cliente: `recv` byte a byte en `read_line` (NIT)
Correcto pero ineficiente (un syscall por byte). Aceptable para un cliente de diagnóstico.

---

## Verificado como correcto (no re-auditar)

- `mgmt_passive_accept`: sin fugas de fd en ningún camino de error.
- `mgmt_take_line`: el chequeo de overflow `i >= outsz` es correcto, sin off-by-one.
- Framing parcial en `mgmt_on_greeting` / `mgmt_on_write`: el manejo de envíos parciales
  (`buffer_read_adv` + `buffer_can_read`) es correcto; lo único a corregir es `errno` (#1).
- `mgmt_on_read`: el "intentar extraer línea ya bufferizada antes de leer más" (pipelining)
  está bien.
- `const_time_eq`: comparación en tiempo constante correcta, sin overflow de índices, segura
  frente a tokens largos.
- Sin `strcpy`/`sprintf`/`gets`/`strcat`: todo `snprintf` acotado o `memcpy` con longitud.
- `malloc` verificado contra NULL en `mgmt_new`.

---

## Tabla resumen

| # | Severidad | Archivo | Resumen |
|---|-----------|---------|---------|
| 1 | CRÍTICO | `mgmt.c` | `send`/`recv` con `-1` no distingue `EAGAIN`/`EWOULDBLOCK`/`EINTR` |
| 2 | CRÍTICO | `mgmt.c` | `recv` mezcla EOF y error en `n <= 0` |
| 4 | MAYOR | `mgmt.c` | `abort()` en `mgmt_done` (patrón de cátedra; decidir en grupo) |
| 6 | MAYOR | `mgmt_cmd.c` | Truncado silencioso acoplado a `USERS_MAX` |
| 9 | MENOR | `client.c` | No drena línea que excede el buffer → desincroniza framing |
| 11 | MENOR | `client.c` | No sanitiza CR/LF en `argv` → command smuggling |
| 8 | nota | `mgmt_cmd.c` | Sin espacios en passwords (ya documentado) |
| 10 | nit | `client.c` | `recv` byte a byte (ineficiente, correcto) |

## Decisiones a tomar en grupo

1. **#1/#2 (EAGAIN/EINTR):** ¿se arregla project-wide (también `socks5.c` de A) o se deja?
   Es pass/fail de la consigna → recomendación de B: arreglarlo en todos lados.
2. **#4 (`abort`):** ¿coherencia con el patrón de A, o degradar a log + cierre?
3. **#6 (`USERS_MAX`):** ¿se queda en 10 (entra holgado) o se sube? Si se sube, hace falta
   streaming de multilínea + el `_Static_assert` de guarda.
