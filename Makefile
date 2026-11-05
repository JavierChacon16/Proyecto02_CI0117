
# Proyecto 2 - CI-0117

CC = gcc

STD ?= $(shell printf 'int main(void){return 0;}\n' | $(CC) -std=c23 -x c -fsyntax-only - >/dev/null 2>&1 && echo c23 || echo c2x)

CFLAGS = -std=$(STD) -Wall -Wextra -pedantic -O2 -pthread -fopenmp
CFLAGS += $(EXTRA)
CPPFLAGS = -Iinclude
LDLIBS = -pthread -fopenmp

BUILD = build

SERVER_SOURCES = src/pipelined.c src/server.c src/scheduler.c src/agents.c src/protocol.c src/util.c
CLIENT_SOURCES = src/pipectl.c src/protocol.c src/util.c

SERVER_OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(SERVER_SOURCES))
CLIENT_OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(CLIENT_SOURCES))
RACE_OBJ = $(patsubst src/%.c,$(BUILD)/race/%.o,$(SERVER_SOURCES))

.PHONY: all test clean race FORCE

all: pipelined pipectl

pipelined: $(SERVER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

pipectl: $(CLIENT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/%.o: src/%.c $(BUILD)/flags
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/race/%.o: src/%.c $(BUILD)/flags
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSIN_PROTECCION -MMD -MP -c $< -o $@

$(BUILD)/flags: FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' '$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(LDLIBS)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp

race: pipelined-race

pipelined-race: $(RACE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_protocol: tests/test_protocol.c $(BUILD)/protocol.o $(BUILD)/util.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_util: tests/test_util.c $(BUILD)/util.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_busy: tests/test_busy.c $(filter-out $(BUILD)/pipelined.o,$(SERVER_OBJ))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_cli: tests/test_cli.c $(BUILD)/util.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_graph: tests/test_graph.c $(BUILD)/graph.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

test: all $(BUILD)/test_protocol $(BUILD)/test_util $(BUILD)/test_busy $(BUILD)/test_cli $(BUILD)/test_graph
	./$(BUILD)/test_protocol
	./$(BUILD)/test_util
	./$(BUILD)/test_busy
	./$(BUILD)/test_cli
	./$(BUILD)/test_graph

clean:
	rm -rf $(BUILD)
	rm -f pipelined pipectl pipelined-race

-include $(wildcard $(BUILD)/*.d $(BUILD)/race/*.d)

