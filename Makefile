
# Proyecto 2 - CI-0117

CC = gcc
CFLAGS = -std=c23 -Wall -Wextra -pedantic -O2 -pthread -fopenmp -Iinclude

BUILD = build
OBJ = $(BUILD)/protocol.o $(BUILD)/util.o

.PHONY: all test clean

all: $(BUILD)/pipelined $(BUILD)/pipectl

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/protocol.o: src/protocol.c include/protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/util.o: src/util.c include/util.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pipelined: src/pipelined.c $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/pipectl: src/pipectl.c $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_protocol: tests/test_protocol.c $(BUILD)/protocol.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_util: tests/test_util.c $(BUILD)/util.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_integration: tests/test_integration.c $(BUILD)/util.o
	$(CC) $(CFLAGS) $^ -o $@

test: all $(BUILD)/test_protocol $(BUILD)/test_util $(BUILD)/test_integration
	./$(BUILD)/test_protocol
	./$(BUILD)/test_util
	./$(BUILD)/test_integration

clean:
	rm -rf $(BUILD)



	
