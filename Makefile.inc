COMPILER=gcc

COMPILER_FLAGS=-std=c11 -pedantic -Wall -Wextra -g -pthread -Isrc/shared/include -Isrc/server/include -Isrc/server/mgmt/include -Isrc/shared -Isrc/server

# Para exponer las funciones POSIX/BSD (sigaction, pthread_kill, getaddrinfo, ...)
# bajo -std=c11 cada SO necesita algo distinto:
#  - Linux: definir _POSIX_C_SOURCE.
#  - macOS: el nivel por defecto ya las expone; definir _POSIX_C_SOURCE o
#    _DARWIN_C_SOURCE las OCULTA, así que no agregamos feature-macro.
ifneq ($(shell uname -s),Darwin)
	COMPILER_FLAGS+=-D_POSIX_C_SOURCE=200809L
endif