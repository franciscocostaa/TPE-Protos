#!/bin/sh
# Valgrind exhaustivo pero acotado en tiempo (temporal, NO se commitea).
echo "### setup ###"
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq curl python3 valgrind >/dev/null 2>&1
make >/dev/null 2>&1 && echo "  build OK" || { echo "BUILD FALLO"; exit 1; }

mkdir -p /tmp/www
dd if=/dev/zero of=/tmp/www/big.bin bs=1M count=10 >/dev/null 2>&1
cat > /tmp/httpd.py <<'PY'
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
import os
os.chdir("/tmp/www")
class Q(SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
ThreadingHTTPServer(("127.0.0.1",8001), Q).serve_forever()
PY
python3 /tmp/httpd.py & sleep 1

echo "### server bajo Valgrind (arranca lento) ###"
valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes \
  ./bin/server -p 1080 -P 8080 -u test:test -t secreto >/tmp/vg.log 2>&1 &
sleep 6

echo "### carga bajo Valgrind ###"
NIP=800; NHOST=150
echo "  1) $NIP conexiones por IP literal (auth RFC1929 + request IPv4 + relay + close)"
ok=0; i=0
while [ $i -lt $NIP ]; do
  c=$(curl -s -o /dev/null -w '%{http_code}' -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/)
  [ "$c" = "200" ] && ok=$((ok+1)); i=$((i+1))
done
echo "     -> $ok/$NIP con http_code=200"

echo "  2) $NHOST conexiones por HOSTNAME (ejercita el hilo de DNS + freeaddrinfo)"
ok=0; i=0
while [ $i -lt $NHOST ]; do
  c=$(curl -s -o /dev/null -w '%{http_code}' -x socks5h://test:test@127.0.0.1:1080 http://localhost:8001/)
  [ "$c" = "200" ] && ok=$((ok+1)); i=$((i+1))
done
echo "     -> $ok/$NHOST con http_code=200"

echo "  3) 30 descargas de 10 MB (relay COPY con payload real)"
i=0; while [ $i -lt 30 ]; do curl -s -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:8001/big.bin -o /dev/null; i=$((i+1)); done

echo "  4) 30 conexiones a puerto CERRADO (path de error / REP)"
i=0; while [ $i -lt 30 ]; do curl -s -m 3 -x socks5h://test:test@127.0.0.1:1080 http://127.0.0.1:9999/ -o /dev/null; i=$((i+1)); done

echo "  5) comandos de mgmt (protocolo de monitoreo bajo Valgrind)"
./bin/client 127.0.0.1 8080 -t secreto GET-METRICS | grep -i connections | sed 's/^/     /'
./bin/client 127.0.0.1 8080 -t secreto LIST-USERS >/dev/null
./bin/client 127.0.0.1 8080 -t secreto GET-LOG >/dev/null
./bin/client 127.0.0.1 8080 -t secreto ADD-USER nuevo pass1234 >/dev/null

echo "### apagado graceful (Valgrind vuelca el reporte) ###"
pkill -TERM -f 'bin/server' 2>/dev/null
sleep 12
echo "### RESUMEN VALGRIND (server) ###"
grep -E "total heap usage|in use at exit|definitely lost|indirectly lost|possibly lost|still reachable|ERROR SUMMARY|FILE DESCRIPTORS|Open file" /tmp/vg.log | sed 's/^/  /'
echo "### FIN ###"
