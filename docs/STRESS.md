# Guía de pruebas de estrés — Proxy SOCKS5

> Cubre el requerimiento no funcional NF3 de la consigna: *"¿Cuál es la máxima
> cantidad de conexiones simultáneas que soporta? ¿Cómo se degrada el throughput?"*.
> Los resultados de esta guía van a la sección **"Ejemplos de prueba"** del informe.

Todo se corre dentro de un contenedor Linux (en Windows no hay `gcc`). Las pruebas
apuntan a un **destino local** para no depender de internet ni de terceros.

---

## 0. Setup común

Levantá un contenedor con las herramientas y compilá:

```bash
docker run --rm -it --ulimit nofile=8192:8192 \
  -v /c/Users/roman/PROTOS/TPE-Protos:/work -w /work \
  gcc:latest bash
# dentro del contenedor:
apt-get update -qq && apt-get install -y -qq curl python3 iproute2 procps valgrind
make
```

**Por qué `--ulimit nofile=8192`:** cada conexión SOCKS usa **2 file descriptors**
(cliente + origen). Para 500 conexiones son ~1000 fds; el default del sistema (1024)
queda justo. Subimos el límite para que el techo lo ponga el **programa** (el
`FD_SETSIZE` de `select()`, típicamente 1024), no el sistema operativo.

**Destino local** (un servidor HTTP que sirve un archivo grande, para medir throughput):

```bash
mkdir -p /tmp/www && dd if=/dev/zero of=/tmp/www/big.bin bs=1M count=100   # archivo de 100 MB
python3 -m http.server 8000 --directory /tmp/www >/tmp/http.log 2>&1 &
```

**El servidor bajo prueba** (SOCKS en 1080, mgmt en 8080, un usuario, token):

```bash
./bin/server -p 1080 -P 8080 -u test:test -t secreto >/tmp/server.log 2>&1 &
SRV=$(pgrep -f 'bin/server')
```

---

## 1. Máximo de conexiones concurrentes  *(F1: ≥ 500)*

**Qué testea:** cuántas conexiones SOCKS **simultáneas** puede sostener el proxy
antes de empezar a rechazar. Es el número que pide la consigna (mínimo 500).

**Cómo:** un script abre N conexiones a la vez, hace el handshake completo
(HELLO → AUTH → CONNECT a un destino local) y las **mantiene abiertas**, contando
cuántas logró establecer. Guardalo como `stress_conns.py`:

```python
import socket, struct, sys, threading, time

PROXY = ("127.0.0.1", 1080)
USER, PW = b"test", b"test"
DEST_HOST, DEST_PORT = b"127.0.0.1", 8000
N = int(sys.argv[1]) if len(sys.argv) > 1 else 500

def one():
    s = socket.socket(); s.settimeout(10); s.connect(PROXY)
    s.sendall(b"\x05\x01\x02")                                  # HELLO: user/pass
    assert s.recv(2)[1] == 0x02
    s.sendall(b"\x01" + bytes([len(USER)]) + USER + bytes([len(PW)]) + PW)  # AUTH RFC1929
    assert s.recv(2)[1] == 0x00
    s.sendall(b"\x05\x01\x00\x03" + bytes([len(DEST_HOST)]) + DEST_HOST + struct.pack(">H", DEST_PORT))
    assert s.recv(10)[1] == 0x00                                # REP == 0 (éxito)
    return s

ok, fail, socks = [], [0], []
lock = threading.Lock()
def worker():
    try:
        with lock: socks.append(one())
    except Exception:
        with lock: fail[0] += 1
ts = [threading.Thread(target=worker) for _ in range(N)]
for t in ts: t.start()
for t in ts: t.join()
print(f"pedidas={N}  establecidas={len(socks)}  fallidas={fail[0]}")
time.sleep(8)   # las mantenemos abiertas para poder mirar el server
```

**Corré y observá** (en otra terminal del mismo contenedor, o con `&`):

```bash
python3 stress_conns.py 500 &
sleep 3
# ¿cuántas conexiones ve el server en el puerto SOCKS?
ss -tn state established '( sport = :1080 )' | wc -l
# y lo que dicen las métricas del propio proxy:
./bin/client 127.0.0.1 8080 -t secreto GET-METRICS
```

**Qué mirar / reportar:**
- `establecidas` vs `pedidas` del script.
- `connections-current` de `GET-METRICS` (debería coincidir con las vivas).
- Repetí subiendo N (500 → 800 → 1000 → 1100) hasta que empiecen a fallar: ese es
  **el máximo**. Se espera que caiga cerca de `FD_SETSIZE/2 ≈ 500` (por los 2 fds
  por conexión) — **ese límite es la conclusión clave del informe**, y se explica
  por el uso de `select()`.

---

## 2. Throughput y su degradación  *(NF3)*

**Qué testea:** cuántos MB/s mueve el proxy, y **cómo cae** ese número a medida que
aumentan las conexiones concurrentes que compiten por el CPU (recordá: un solo hilo).

**Cómo:** descargar el archivo de 100 MB a través del proxy, primero con 1 conexión
y después con muchas en paralelo, midiendo el tiempo total.

```bash
dl() {   # $1 = cantidad de descargas en paralelo
  local n=$1 start end
  start=$(date +%s.%N)
  for i in $(seq 1 "$n"); do
    curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8000/big.bin -o /dev/null &
  done
  wait
  end=$(date +%s.%N)
  awk -v n="$n" -v t="$(echo "$end-$start" | bc)" \
    'BEGIN{ printf "  %4d conexiones  |  %6.2f s  |  %7.1f MB/s (agregado)\n", n, t, n*100/t }'
}
echo "throughput (100 MB por descarga):"
dl 1
dl 10
dl 50
dl 100
```

**Qué mirar / reportar:**
- El MB/s **agregado** en cada nivel. Con 1 conexión tenés el pico; con más, el
  agregado sube un poco y después **se aplana o cae** (el hilo único se satura).
- Armá una tabla `conexiones → MB/s` y un párrafo explicando la curva. La respuesta
  a "cómo se degrada el throughput" **es esa curva**.

---

## 3. Estabilidad bajo carga: fugas de fds y de memoria

**Qué testea:** que después de miles de conexiones **el proxy no acumule** file
descriptors ni memoria (que cada conexión se limpie del todo al cerrarse). Un leak
acá haría que el server muera con el tiempo — es justo lo que la cátedra busca.

**Cómo:** medir fds y memoria (RSS) del proceso **antes y después** de un ciclo de
muchas conexiones cortas.

```bash
snap() { echo "  fds=$(ls /proc/$SRV/fd | wc -l)  RSS=$(ps -o rss= -p $SRV | tr -d ' ') KB"; }
echo "antes: $(snap)"
for i in $(seq 1 5000); do
  curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8000/ -o /dev/null
done
sleep 1
echo "despues: $(snap)"
```

**Qué mirar / reportar:**
- `fds` **antes ≈ después** (los pasivos + selector, un puñado). Si crece de forma
  sostenida → leak de descriptores.
- `RSS` estable (no crece linealmente con la cantidad de conexiones) → sin leak de
  memoria. El proxy usa un **pool** de estructuras, así que la memoria debería
  estabilizarse.

**Versión con Valgrind** (más contundente para el informe):

```bash
kill $SRV
valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes \
  ./bin/server -p 1080 -P 8080 -u test:test -t secreto >/tmp/vg.log 2>&1 &
SRV=$(pgrep -f 'bin/server')
for i in $(seq 1 200); do curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8000/ -o /dev/null; done
kill -TERM $SRV          # graceful shutdown: valgrind vuelca el reporte al salir
sleep 2; tail -30 /tmp/vg.log
```
Reportá el resumen: "definitely lost: 0 bytes" y "open file descriptors" al salir.

---

## 4. Robustez: conexiones lentas / lecturas parciales

**Qué testea:** que un cliente que manda el handshake **de a un byte, muy lento**, no
rompa ni cuelgue al proxy (correctitud de lecturas parciales, que es pass/fail en la
consigna), y que muchos clientes lentos a la vez no lo tumben.

**Cómo:** un script que manda el saludo SOCKS byte por byte con pausas. Guardalo como
`slow_client.py`:

```python
import socket, time, sys
s = socket.socket(); s.connect(("127.0.0.1", 1080))
for b in b"\x05\x01\x02":            # HELLO byte a byte
    s.sendall(bytes([b])); time.sleep(0.5)
print("method:", s.recv(2))
# ... seguiría con AUTH y REQUEST igual de lento ...
time.sleep(2)
```

```bash
for i in $(seq 1 50); do python3 slow_client.py & done; wait
# mientras corren, el server DEBE seguir atendiendo clientes normales:
curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8000/ -o /dev/null -w "cliente normal: %{http_code}\n"
```

**Qué mirar / reportar:**
- El proxy sigue respondiendo a clientes normales mientras hay 50 lentos (no se
  bloquea en ninguno → confirma el modelo no bloqueante multiplexado).
- Ninguna conexión lenta produce crash ni cierre prematuro.
- **Limitación conocida a documentar:** el proxy **no** tiene timeout de inactividad,
  así que un cliente lento ocupa una conexión indefinidamente (un slowloris podría
  agotar las 500). Mencionarlo en "Limitaciones" del informe; el fix (timeouts de
  idle en el selector) es una posible extensión.

---

## Qué incluir en el informe (sección de estrés)

1. **Máximo de conexiones** (Prueba 1): el número medido y la explicación
   (`select()` → `FD_SETSIZE`, 2 fds por conexión).
2. **Curva de throughput** (Prueba 2): tabla `conexiones → MB/s` + análisis de la
   degradación (hilo único).
3. **Estabilidad** (Prueba 3): fds y memoria estables; salida de Valgrind sin leaks.
4. **Robustez** (Prueba 4): sigue atendiendo bajo clientes lentos; limitación del
   timeout de inactividad.
5. **Entorno de prueba**: aclarar hardware, que es dentro de Docker, `ulimit`, etc.
   (los números dependen del entorno; lo importante es la **tendencia** y el techo).
