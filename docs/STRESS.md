# Guía de pruebas de estrés — Proxy SOCKS5

> Cubre el requerimiento no funcional NF3 de la consigna: *"¿Cuál es la máxima cantidad de
> conexiones simultáneas que soporta? ¿Cómo se degrada el throughput?"*. Los resultados van
> a la sección **"Ejemplos de prueba"** del informe.

Todo se corre dentro de un contenedor Linux (en Windows no hay `gcc`). Las pruebas apuntan a
**destinos locales** para no depender de internet ni de terceros.

> **Honestidad sobre la metodología.** Armar estas pruebas tuvo tropiezos reales; los comandos
> de acá son la versión **ya corregida**, y cada sección documenta la trampa en la que caímos
> (van con ⚠️). Los números de "Resultados" son de una **corrida de referencia**: en otro
> hardware cambian los MB/s absolutos, pero se mantiene la **tendencia** y el **techo**.

---

## 0. Setup común

Contenedor con herramientas + compilación:

```bash
docker run --rm -it --ulimit nofile=8192:8192 -v "$PWD:/work" -w /work gcc:latest bash
# dentro del contenedor:
apt-get update -qq && apt-get install -y -qq curl python3 iproute2 procps valgrind
make
```

**Por qué `--ulimit nofile=8192`:** cada conexión SOCKS usa **2 file descriptors** (cliente +
origen). Para 500 conexiones son ~1000 fds. Subimos el límite del SO **a propósito** para que el
techo lo ponga el **programa** (el `FD_SETSIZE` de `select()`, típicamente 1024), no el sistema.

**Dos destinos locales** (hacen falta dos, ya se explica por qué):

```bash
mkdir -p /tmp/www && dd if=/dev/zero of=/tmp/www/big.bin bs=1M count=100   # archivo de 100 MB

# Destino A: SOLO escucha (backlog grande) — para la prueba de MÁXIMO de conexiones.
python3 -c 'import socket,time; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("127.0.0.1",8000)); s.listen(2048); time.sleep(3600)' &

# Destino B: HTTP CONCURRENTE (ThreadingHTTPServer) — para throughput/relay.
# OJO: `python3 -m http.server` es de UN solo hilo y serializa las descargas; para medir
# throughput concurrente hay que usar ThreadingHTTPServer.
cat > /tmp/httpd.py <<'PY'
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
import os
os.chdir("/tmp/www")
class Q(SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
ThreadingHTTPServer(("127.0.0.1",8001), Q).serve_forever()
PY
python3 /tmp/httpd.py &
```

**El servidor bajo prueba** (SOCKS en 1080, mgmt en 8080, un usuario, token):

```bash
./bin/server -p 1080 -P 8080 -u test:test -t secreto >/tmp/server.log 2>&1 &
```

---

## 1. Máximo de conexiones concurrentes  *(F1: ≥ 500)*

**Qué testea:** cuántas conexiones SOCKS **simultáneas** sostiene el proxy antes de rechazar.

**Cómo:** un script abre conexiones (handshake SOCKS completo) **una tras otra, sin cerrarlas**,
guardándolas en una lista, hasta que una falla; el total es el máximo. Usa el **destino A
(listen-only, :8000)**. Guardalo como `maxconns.py`:

```python
import socket, struct
PROXY=("127.0.0.1",1080); USER=b"test"; PW=b"test"; DH=b"127.0.0.1"; DP=8000
def one():
    s=socket.socket(); s.settimeout(4); s.connect(PROXY)
    s.sendall(b"\x05\x01\x02");            assert s.recv(2)[1]==0x02   # HELLO user/pass
    s.sendall(b"\x01"+bytes([len(USER)])+USER+bytes([len(PW)])+PW); assert s.recv(2)[1]==0x00  # AUTH RFC1929
    s.sendall(b"\x05\x01\x00\x03"+bytes([len(DH)])+DH+struct.pack(">H",DP)); assert s.recv(10)[1]==0x00  # CONNECT, REP=0
    return s
socks=[]
try:
    while len(socks) < 1300: socks.append(one())
except Exception: pass
print("maximo de conexiones SOCKS simultaneas:", len(socks))
```

```bash
python3 maxconns.py
```

**Qué mirar / reportar:** el número impreso. Se espera cerca de `FD_SETSIZE/2 ≈ 510` (2 fds por
conexión, menos los pasivos). **Ese techo, dado por `select()`, es la conclusión clave del
informe** — es determinista y reproducible.

> ⚠️ **Trampa (la aprendimos a los golpes):** el destino A nunca cierra sus conexiones, así que
> al terminar esta prueba el server queda **saturado** con ~510 conexiones medio abiertas
> ocupando todos sus fds. **Antes de las pruebas 2–4 hay que REINICIAR el server** (`pkill -TERM
> -f bin/server; sleep 2; ./bin/server ... &`), si no las siguientes le pegan a un server tapado.

---

## 2. Throughput y su degradación  *(NF3)*

Reiniciá el server primero (por la trampa de arriba). Usa el **destino B (HTTP concurrente, :8001)**.

**Qué testea:** cuántos MB/s mueve el proxy y **cómo caen** con más conexiones concurrentes
(recordá: un solo hilo).

```bash
dl() {   # $1 = descargas en paralelo
  n=$1; start=$(date +%s.%N); pids=""
  i=0; while [ $i -lt "$n" ]; do
    curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/big.bin -o /dev/null & pids="$pids $!"
    i=$((i+1))
  done
  wait $pids       # <-- $pids, NO 'wait' a secas
  end=$(date +%s.%N)
  awk -v n=$n -v s=$start -v e=$end 'BEGIN{t=e-s; printf "  %4d conex | %6.2fs | %8.1f MB/s agregado | %6.1f MB/s por conex\n", n, t, n*100/t, 100/t}'
}
dl 1; dl 10; dl 50; dl 100
```

**Qué mirar / reportar:** la tabla `conexiones → MB/s`. El **agregado hace pico y después
degrada**; el **por conexión se desploma**. Esa curva responde "cómo se degrada el throughput".

> ⚠️ **Dos trampas que caímos:**
> 1. `wait` a secas espera **también a los servidores-destino** (que corren para siempre con
>    `sleep 3600` / `serve_forever`) → el script se **cuelga**. Hay que capturar los PIDs de los
>    `curl` y hacer `wait $pids`.
> 2. Si NO reiniciaste el server tras la Prueba 1, los `curl` fallan al instante (server tapado)
>    y la cuenta da números **físicamente imposibles** (nos dio "14 GB/s"). Eso es basura, no
>    throughput — se descarta.

---

## 3. Fugas de memoria y de descriptores — con Valgrind

**Qué testea:** que tras miles de conexiones el proxy **no acumule** memoria ni file
descriptors (que cada conexión se limpie del todo al cerrarse). Un leak acá mataría el server
con el tiempo.

**La medición confiable es Valgrind.** (Ver más abajo por qué NO usamos el conteo de `/proc`.)
Se ejercitan **todos los caminos**: IP literal (path rápido), hostname (hilo de DNS +
`freeaddrinfo`), relay con datos reales, y errores.

```bash
pkill -TERM -f 'bin/server'; sleep 2
valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes \
  ./bin/server -p 1080 -P 8080 -u test:test -t secreto >/tmp/vg.log 2>&1 &
sleep 5
for i in $(seq 1 800); do curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/ -o /dev/null; done   # IP literal
for i in $(seq 1 150); do curl -s -x socks5h://test:test@127.0.0.1:1080 http://localhost:8001/ -o /dev/null;   done  # hostname -> hilo DNS
for i in $(seq 1 30);  do curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/big.bin -o /dev/null; done  # relay con datos
for i in $(seq 1 30);  do curl -s -m 3 -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:9999/ -o /dev/null;  done   # error (puerto cerrado)
pkill -TERM -f 'bin/server'; sleep 10    # graceful shutdown -> Valgrind vuelca el reporte
grep -E "definitely lost|indirectly lost|possibly lost|ERROR SUMMARY|FILE DESCRIPTORS" /tmp/vg.log
```

**Qué mirar / reportar:** `ERROR SUMMARY: 0 errors` y `FILE DESCRIPTORS: 2 open (2 std) at exit`
(los 2 estándar: stdout y stderr; el stdin lo cierra el `main`). → **sin fugas de memoria ni de
descriptores.**

**Chequeo complementario de drenado (este SÍ es válido):** tras generar carga,
`./bin/client 127.0.0.1 8080 -t secreto GET-METRICS` debe mostrar **`connections-current=0`** →
el server contabiliza y drena correctamente las conexiones.

> ⚠️ **Por qué descartamos el conteo de `/proc/$PID/fd` como evidencia:** lo intentamos y **medía
> el proceso equivocado**. Al reiniciar el server entre pruebas, el viejo puede quedar **colgado
> en su graceful shutdown** (esperando a las conexiones medio abiertas de la Prueba 1, que nunca
> cierran por falta de timeout de inactividad), y `pgrep -f bin/server | head -1` termina
> apuntando a ese proceso viejo. Resultado: un `fds=1022` que **no dice nada** del server que
> estábamos probando. Por eso el snapshot de `/proc` **no es evidencia**; la fuente de verdad
> para fugas es **Valgrind**.

---

## 4. Robustez: conexiones lentas / lecturas parciales

**Qué testea:** que un cliente que manda el handshake **de a un byte, muy lento**, no rompa ni
cuelgue al proxy (correctitud de lecturas parciales, pass/fail en la consigna), y que muchos
clientes lentos a la vez no lo tumben. Usa el **destino B**.

```python
# slow_client.py
import socket, time
s = socket.socket(); s.settimeout(8); s.connect(("127.0.0.1", 1080))
for b in b"\x05\x01\x02":            # HELLO byte a byte
    s.sendall(bytes([b])); time.sleep(0.4)
s.recv(2); time.sleep(3); s.close()
```

```bash
pids=""; i=0; while [ $i -lt 20 ]; do python3 slow_client.py & pids="$pids $!"; i=$((i+1)); done
sleep 1
curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/ -o /dev/null -w "cliente normal: %{http_code}\n"
wait $pids
```

**Qué mirar / reportar:** el cliente normal responde `200` mientras hay 20 lentos colgados → el
modelo no bloqueante multiplexado sigue atendiendo sin trabarse, y el parser del HELLO tolera 1
byte por vez.

> **Alcance honesto de esta prueba:** solo gotea la fase HELLO (no AUTH ni REQUEST), son solo 20
> clientes, y una sola muestra del cliente normal. Prueba "no se bloquea", **no** prueba
> resistencia a un slowloris grande. **Limitación a documentar:** sin timeout de inactividad, un
> cliente lento ocupa una conexión indefinidamente (y puede demorar el graceful shutdown; la 2ª
> señal fuerza el cierre). El fix (timeouts de idle) es una posible extensión.

---

## Resultados obtenidos (corrida de referencia)

Corrida en Docker (`gcc:latest`, `--ulimit nofile=8192`), destinos locales. Los números
absolutos dependen del hardware; lo importante es la **tendencia** y el **techo**.

**1. Máximo de conexiones concurrentes: 510.** Coincide con lo esperado (`FD_SETSIZE`=1024 ÷ 2
fds por conexión, menos los pasivos). **Supera el mínimo de 500.** Determinista y reproducible.

**2. Throughput (archivo de 100 MB, destino concurrente):**

| Conexiones | Tiempo | Agregado | Por conexión |
|---|---|---|---|
| 1   | 0.42 s | 237 MB/s | 237 MB/s |
| 10  | 1.18 s | 847 MB/s |  85 MB/s |
| 50  | 6.29 s | 795 MB/s |  16 MB/s |
| 100 | 14.3 s | 700 MB/s |   7 MB/s |

El agregado hace pico (~800 MB/s) con 10–50 conexiones y después degrada (700 con 100); el
throughput por conexión cae de 237 a 7 MB/s al competir por el único hilo.
**Advertencia:** medido sobre *loopback* con un archivo de ceros → mide el **overhead interno
del relay**, no throughput de red real. Es un **techo**; leer por su tendencia, no por el valor
absoluto.

**3. Fugas (Valgrind):** `ERROR SUMMARY: 0 errors` y `FILE DESCRIPTORS: 2 open (2 std) at exit`
→ **sin fugas de memoria ni de descriptores**. Chequeo de drenado (válido): tras 2161 conexiones,
`connections-current=0` y `bytes-transferred ≈ 16.8 GB`.
*(El snapshot de `/proc/PID/fd` se descartó por medir el proceso equivocado; ver §3.)*

**4. Robustez:** con 20 clientes lentos colgados, un cliente normal responde `http_code=200` →
el modelo no bloqueante sigue atendiendo. (Alcance limitado; no cubre slowloris grande.)

---

## Qué incluir en el informe (sección de estrés)

1. **Máximo de conexiones** (Prueba 1): el número medido (510) y la explicación (`select()` →
   `FD_SETSIZE`, 2 fds por conexión).
2. **Curva de throughput** (Prueba 2): tabla `conexiones → MB/s` + análisis de la degradación
   (hilo único), **con la advertencia de que es loopback (techo, no red real)**.
3. **Fugas** (Prueba 3): salida de **Valgrind** sin leaks (la medición confiable). No usar el
   snapshot de `/proc` como evidencia.
4. **Robustez** (Prueba 4): sigue atendiendo bajo clientes lentos; **limitación del timeout de
   inactividad** (slowloris) como posible extensión.
5. **Entorno**: aclarar que es dentro de Docker, con `--ulimit`, sobre loopback. Los números
   dependen del entorno; lo que vale es la **tendencia** y el **techo** (510).
