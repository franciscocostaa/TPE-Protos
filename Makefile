include ./Makefile.inc

SERVER_SOURCE=$(wildcard src/server/*.c)
CLIENT_SOURCE=$(wildcard src/client/*.c)
SHARED_SOURCE=$(wildcard src/shared/*.c)

OUTPUT_FOLDER=bin

SERVER_OUTPUT_FILE=$(OUTPUT_FOLDER)/server
CLIENT_OUTPUT_FILE=$(OUTPUT_FOLDER)/client

all: server client

server:
	mkdir -p $(OUTPUT_FOLDER)
	$(COMPILER) $(COMPILER_FLAGS) $(SERVER_SOURCE) $(SHARED_SOURCE) -o $(SERVER_OUTPUT_FILE)

client:
	mkdir -p $(OUTPUT_FOLDER)
	$(COMPILER) $(COMPILER_FLAGS) $(CLIENT_SOURCE) $(SHARED_SOURCE) -o $(CLIENT_OUTPUT_FILE)

clean:
	rm -rf $(OUTPUT_FOLDER)

.PHONY: all server client clean