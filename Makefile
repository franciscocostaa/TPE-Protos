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

.PHONY: all server client clean