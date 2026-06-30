# Decisiones de diseno

Este documento registra decisiones de implementacion que afectan el comportamiento
observable del proxy y que conviene justificar en el informe.

## Metricas: bytes transferidos

La metrica `bytes_transferred` cuenta unicamente bytes de payload relayeados por el
proxy SOCKS5 entre el cliente y el servidor destino, sumando ambos sentidos de la
conexion.

No se cuentan bytes de control del protocolo SOCKS5, como negociacion de metodo,
autenticacion RFC 1929, request ni reply. Tampoco se cuentan bytes del protocolo de
monitoreo.

Esta decision hace que la metrica represente trafico util proxificado, que es mas
relevante para monitorear carga real del sistema. La implementacion suma los bytes
cuando `copy_write()` escribe exitosamente hacia cualquiera de los dos sockets del
relay: cliente u origen. De esta forma se contabilizan bytes efectivamente
transferidos al otro extremo del tunel, no solo bytes recibidos y almacenados en un
buffer interno del proxy.
