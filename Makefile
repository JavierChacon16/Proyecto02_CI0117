
# Proyecto 2 - CI-0117

CC = gcc
CFLAGS = -std=c23 -Wall -Wextra -pedantic -O2 -pthread -fopenmp -Iinclude

SRC_DIR = src
BUILD_DIR = build
TEST_DIR = tests

OBJS = $(BUILD_DIR)/protocol.o $(BUILD_DIR)/util.o

TESTS = $(BUILD_DIR)/test_protocol $(BUILD_DIR)/test_util

.PHONY: all test clean

# Compilacion de los modulos y el cliente
all: $(OBJS) $(BUILD_DIR)/pipectl

# Crear directorio de compilacion
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compilar protocolo
$(BUILD_DIR)/protocol.o: $(SRC_DIR)/protocol.c include/protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compilar utilidades de red
$(BUILD_DIR)/util.o: $(SRC_DIR)/util.c include/util.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compilar cliente
$(BUILD_DIR)/pipectl: $(SRC_DIR)/pipectl.c $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# Compilar pruebas del protocolo
$(BUILD_DIR)/test_protocol: $(TEST_DIR)/test_protocol.c $(BUILD_DIR)/protocol.o
	$(CC) $(CFLAGS) $^ -o $@

# Compilar pruebas de utilidades
$(BUILD_DIR)/test_util: $(TEST_DIR)/test_util.c $(BUILD_DIR)/util.o
	$(CC) $(CFLAGS) $^ -o $@

# Ejecutar pruebas
test: all $(TESTS)
	./$(BUILD_DIR)/test_protocol
	./$(BUILD_DIR)/test_util

# Limpiar archivos compilados
clean:
	rm -rf $(BUILD_DIR)


	
