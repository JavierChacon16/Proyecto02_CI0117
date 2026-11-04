

# Proyecto 2 - CI-0117
CC = gcc
CFLAGS = -std=c23 -Wall -Wextra -pedantic -O2 -pthread -fopenmp -Iinclude

SRC_DIR = src
BUILD_DIR = build
TEST_DIR = tests
OBJS = $(BUILD_DIR)/protocol.o $(BUILD_DIR)/util.o
TESTS = $(BUILD_DIR)/test_protocol $(BUILD_DIR)/test_util

.PHONY: all test clean

# Hasta implementar pipelined y pipectl, compila los modulos existentes.
all: $(OBJS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/protocol.o: $(SRC_DIR)/protocol.c include/protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/util.o: $(SRC_DIR)/util.c include/util.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_protocol: $(TEST_DIR)/test_protocol.c $(BUILD_DIR)/protocol.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_util: $(TEST_DIR)/test_util.c $(BUILD_DIR)/util.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

test: all $(TESTS)
	./$(BUILD_DIR)/test_protocol
	./$(BUILD_DIR)/test_util

clean:
	rm -rf $(BUILD_DIR)

	
