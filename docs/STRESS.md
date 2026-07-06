# Guía de pruebas de estrés — Proxy SOCKS5

> Cubre el requerimiento no funcional NF3 de la consigna: *"¿Cuál es la máxima cantidad de
> conexiones simultáneas que soporta? ¿Cómo se degrada el throughput?"*. Los resultados van
> a la sección **"Ejemplos de prueba"** del informe.

Todo se corre dentro de un contenedor Linux (en Windows no hay `gcc`). Las pruebas apuntan a
**destinos locales** para no depender de internet ni de terceros. Los números de "Resultados"
son de una **corrida de referencia**: en otro hardware cambian los MB/s absolutos, pero se
mantienen la **tendencia** y el **techo**.

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
origen). Subimos el límite del SO para que el techo lo ponga el **programa** (el `FD_SETSIZE`
de `select()`, típicamente 1024), no el sistema operativo.

**Dos destinos locales:** uno solo para *aceptar* conexiones (prueba de máximo de conexiones) y
otro HTTP concurrente que *sirve datos* (throughput/relay):

```bash
mkdir -p /tmp/www && dd if=/dev/zero of=/tmp/www/big.bin bs=1M count=100   # archivo de 100 MB

# Destino A (:8000): solo escucha con backlog grande — para la prueba de máximo de conexiones.
python3 -c 'import socket,time; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("127.0.0.1",8000)); s.listen(2048); time.sleep(3600)' &

# Destino B (:8001): HTTP concurrente (ThreadingHTTPServer) — para throughput y relay.
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
hasta que una falla; el total es el máximo. Usa el **destino A (:8000)**. Guardalo como
`maxconns.py`:

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

> Al terminar esta prueba, el server queda con esas conexiones abiertas; **reinicialo antes de
> las pruebas 2–4** (`pkill -TERM -f bin/server; sleep 2; ./bin/server -p 1080 -P 8080 -u
> test:test -t secreto & sleep 1`).

---

## 2. Throughput y su degradación  *(NF3)*

Usa el **destino B (:8001)**.

**Qué testea:** cuántos MB/s mueve el proxy y **cómo caen** con más conexiones concurrentes
(recordá: un solo hilo).

```bash
dl() {   # $1 = descargas en paralelo
  n=$1; start=$(date +%s.%N); pids=""
  i=0; while [ $i -lt "$n" ]; do
    curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/big.bin -o /dev/null & pids="$pids $!"
    i=$((i+1))
  done
  wait $pids
  end=$(date +%s.%N)
  awk -v n=$n -v s=$start -v e=$end 'BEGIN{t=e-s; printf "  %4d conex | %6.2fs | %8.1f MB/s agregado | %6.1f MB/s por conex\n", n, t, n*100/t, 100/t}'
}
dl 1; dl 10; dl 50; dl 100
```

**Qué mirar / reportar:** la tabla `conexiones → MB/s`. El **agregado hace pico y después
degrada**; el **por conexión se desploma**. Esa curva responde "cómo se degrada el throughput".

---

## 3. Fugas de memoria y de descriptores  (Valgrind)

**Qué testea:** que tras miles de conexiones el proxy **no acumule** memoria ni file
descriptors (que cada conexión se limpie del todo al cerrarse). Se ejercitan **todos los
caminos**: IP literal (path rápido), hostname (hilo de DNS + `freeaddrinfo`), relay con datos y
errores.

```bash
pkill -TERM -f 'bin/server'; sleep 2
valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes \
  ./bin/server -p 1080 -P 8080 -u test:test -t secreto >/tmp/vg.log 2>&1 &
sleep 5
for i in $(seq 1 500); do curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/ -o /dev/null; done   # IP literal
for i in $(seq 1 100); do curl -s -x socks5h://test:test@127.0.0.1:1080 http://localhost:8001/ -o /dev/null;   done  # hostname -> hilo DNS
for i in $(seq 1 20);  do curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/big.bin -o /dev/null; done  # relay con datos
for i in $(seq 1 20);  do curl -s -m 3 -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:9999/ -o /dev/null;  done   # error (puerto cerrado)
pkill -TERM -f 'bin/server'; sleep 10    # graceful shutdown -> Valgrind vuelca el reporte
grep -E "total heap usage|in use at exit|definitely lost|ERROR SUMMARY|FILE DESCRIPTORS" /tmp/vg.log
```

**Qué mirar / reportar:** `ERROR SUMMARY: 0 errors` y `FILE DESCRIPTORS: 2 open (2 std) at exit`
(los 2 estándar: stdout y stderr; el stdin lo cierra el `main`) → **sin fugas de memoria ni de
descriptores**. Chequeo complementario de drenado:
`./bin/client 127.0.0.1 8080 -t secreto GET-METRICS` debe mostrar **`connections-current=0`**.

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

> **Limitación a documentar en el informe:** sin timeout de inactividad, un cliente lento ocupa
> una conexión indefinidamente (un *slowloris* podría agotar las ~510 conexiones y demorar el
> graceful shutdown; la 2ª señal lo fuerza). El fix (timeouts de idle en el selector) es una
> posible extensión.

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
**Nota:** medido sobre *loopback* con un archivo de ceros → representa el **overhead interno del
relay** (un techo), no throughput de red real. Se lee por su **tendencia**, no por el valor
absoluto.

**3. Fugas (Valgrind, 640 conexiones ejercitando todos los caminos):** `total heap usage:
1.377 allocs, 1.377 frees` (todo lo reservado se liberó), `in use at exit: 0 bytes`,
`FILE DESCRIPTORS: 2 open (2 std) at exit` y `ERROR SUMMARY: 0 errors` → **sin fugas de memoria
ni de descriptores**. Chequeo de drenado: `connections-current=0` tras las 640 conexiones.

**4. Robustez:** con 20 clientes lentos colgados, un cliente normal responde `http_code=200` →
el modelo no bloqueante sigue atendiendo.

---
