include ./Makefile.inc

# Se busca recursivamente para soportar subdirectorios (socks5/, mgmt/, dns/, ...)
SERVER_SOURCES=$(shell find src/server -name '*.c')
CLIENT_SOURCES=$(shell find src/client -name '*.c')
SHARED_SOURCES=$(shell find src/shared -name '*.c')

SERVER_OBJECTS=$(SERVER_SOURCES:src/%.c=obj/%.o)
CLIENT_OBJECTS=$(CLIENT_SOURCES:src/%.c=obj/%.o)
SHARED_OBJECTS=$(SHARED_SOURCES:src/%.c=obj/%.o)

OUTPUT_FOLDER=bin
OBJECTS_FOLDER=obj

SERVER_OUTPUT_FILE=$(OUTPUT_FOLDER)/server
CLIENT_OUTPUT_FILE=$(OUTPUT_FOLDER)/client

all: client server
server: $(SERVER_OUTPUT_FILE)
client: $(CLIENT_OUTPUT_FILE)

$(SERVER_OUTPUT_FILE): $(SERVER_OBJECTS) $(SHARED_OBJECTS)
	mkdir -p $(OUTPUT_FOLDER)
	$(COMPILER) $(COMPILER_FLAGS) $(SERVER_OBJECTS) $(SHARED_OBJECTS) -o $(SERVER_OUTPUT_FILE)

$(CLIENT_OUTPUT_FILE): $(CLIENT_OBJECTS) $(SHARED_OBJECTS)
	mkdir -p $(OUTPUT_FOLDER)
	$(COMPILER) $(COMPILER_FLAGS) $(CLIENT_OBJECTS) $(SHARED_OBJECTS) -o $(CLIENT_OUTPUT_FILE)

obj/%.o: src/%.c
	mkdir -p $(@D)
	$(COMPILER) $(COMPILER_FLAGS) -c $< -o $@

clean:
	rm -rf $(OUTPUT_FOLDER)
	rm -rf $(OBJECTS_FOLDER)

# ---- tests ---------------------------------------------------------------
# Harness propio (sin CUnit/check, que no están garantizados en la máquina de
# corrección). Cada test incluye la unidad bajo prueba como fuente para poder
# ejercitar también sus funciones estáticas.
#
#  - Tests de módulos puros (users/metrics/config/access_log): autocontenidos,
#    no necesitan enlazar nada más.
#  - Test del dispatch de mgmt: enlaza los módulos que consume + la infra.
MGMT_TEST_BIN=$(OUTPUT_FOLDER)/mgmt_cmd_test
MGMT_TEST_DEPS=obj/server/metrics.o obj/server/users.o obj/server/config.o obj/server/access_log.o

UNIT_TESTS=$(OUTPUT_FOLDER)/users_test $(OUTPUT_FOLDER)/metrics_test \
           $(OUTPUT_FOLDER)/config_test $(OUTPUT_FOLDER)/access_log_test

test: $(UNIT_TESTS) $(MGMT_TEST_BIN)
	./$(OUTPUT_FOLDER)/users_test
	./$(OUTPUT_FOLDER)/metrics_test
	./$(OUTPUT_FOLDER)/config_test
	./$(OUTPUT_FOLDER)/access_log_test
	./$(MGMT_TEST_BIN)

$(MGMT_TEST_BIN): test/mgmt_cmd_test.c $(MGMT_TEST_DEPS) $(SHARED_OBJECTS)
	mkdir -p $(OUTPUT_FOLDER)
	$(COMPILER) $(COMPILER_FLAGS) test/mgmt_cmd_test.c $(MGMT_TEST_DEPS) $(SHARED_OBJECTS) -o $(MGMT_TEST_BIN)

# Tests de módulos puros: autocontenidos (el .c bajo prueba se incluye en el test).
$(OUTPUT_FOLDER)/%_test: test/%_test.c
	mkdir -p $(OUTPUT_FOLDER)
	$(COMPILER) $(COMPILER_FLAGS) $< -o $@

.PHONY: all server client clean test