COMPILER=gcc

# Flags comunes a todos los perfiles (sin optimización ni símbolos de debug).
COMPILER_FLAGS=-std=c11 -pedantic -Wall -Wextra -pthread -Isrc/shared/include -Isrc/server/include -Isrc/server/mgmt/include -Isrc/shared -Isrc/server

# Perfil de compilación:
#   - release (por defecto): optimizado (-O2) y SIN símbolos de debug.
#   - debug   (make DEBUG=1): sin optimizar y con -g para gdb/valgrind.
ifeq ($(DEBUG),1)
	COMPILER_FLAGS+=-O0 -g
else
	COMPILER_FLAGS+=-O2
endif

# Para exponer las funciones POSIX/BSD (sigaction, pthread_kill, getaddrinfo, ...)
# bajo -std=c11 cada SO necesita algo distinto:
#  - Linux: definir _POSIX_C_SOURCE.
#  - macOS: el nivel por defecto ya las expone; definir _POSIX_C_SOURCE o
#    _DARWIN_C_SOURCE las OCULTA, así que no agregamos feature-macro.
ifneq ($(shell uname -s),Darwin)
	COMPILER_FLAGS+=-D_POSIX_C_SOURCE=200809L
endif