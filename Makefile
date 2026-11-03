
# Proyecto 2 - Servidor de pipelines concurrente
# CI-0117 Programacion Paralela y Concurrente

CC = gcc
CFLAGS = -std=c2x -Wall -Wextra -pedantic -O2 -pthread -fopenmp -Iinclude

SRC_DIR = src
BUILD_DIR = build

# Modulos implementados actualmente
OBJS = $(BUILD_DIR)/protocol.o $(BUILD_DIR)/util.o

.PHONY: all test clean

all: $(OBJS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/protocol.o: $(SRC_DIR)/protocol.c include/protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/util.o: $(SRC_DIR)/util.c include/util.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	@echo "Compilacion completada."
	@echo "Pruebas funcionales pendientes de implementar."

clean:
	rm -rf $(BUILD_DIR)
