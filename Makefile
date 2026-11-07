
# Proyecto 2 - CI-0117


CC = gcc

STD ?= $(shell printf 'int main(void){return 0;}\n' | $(CC) -std=c23 -x c -fsyntax-only - >/dev/null 2>&1 && echo c23 || echo c2x)

CFLAGS = -std=$(STD) -Wall -Wextra -pedantic -O2 -pthread -fopenmp
CFLAGS += $(EXTRA)

CPPFLAGS = -Iinclude
LDLIBS = -pthread -fopenmp -lm

BUILD = build

# Fase 1: parser propio y modelo v2. El servidor compila todos los
# modulos implementados, aunque SUBMIT seguira pendiente hasta la Fase 4.
# vars/artifacts/logs estan vacios y -pedantic -Werror los rechaza:
# se agregaran en la Fase 3, sin tocar ahora codigo ajeno a Fase 1.
SERVER_SOURCES = src/pipelined.c \
                 src/server.c \
                 src/scheduler.c \
                 src/agents.c \
                 src/protocol.c \
                 src/util.c \
                 src/graph.c \
                 src/model.c \
                 src/yaml_parser.c \
                 src/validator.c \
                 src/script.c \
                 src/tasks.c

CLIENT_SOURCES = src/pipectl.c src/protocol.c src/util.c

SERVER_OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(SERVER_SOURCES))
CLIENT_OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(CLIENT_SOURCES))
RACE_OBJ = $(patsubst src/%.c,$(BUILD)/race/%.o,$(SERVER_SOURCES))

TESTS = $(BUILD)/test_protocol \
        $(BUILD)/test_util \
        $(BUILD)/test_busy \
        $(BUILD)/test_cli \
        $(BUILD)/test_graph \
        $(BUILD)/test_script \
        $(BUILD)/test_tasks \
        $(BUILD)/test_data \
        $(BUILD)/test_motor \
        $(BUILD)/test_yaml \
        $(BUILD)/test_validator

.PHONY: all test clean race test-race FORCE

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
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_util: tests/test_util.c $(BUILD)/util.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_busy: tests/test_busy.c $(filter-out $(BUILD)/pipelined.o,$(SERVER_OBJ))
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_cli: tests/test_cli.c $(BUILD)/util.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_graph: tests/test_graph.c $(BUILD)/graph.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_yaml: tests/test_yaml.c $(BUILD)/yaml_parser.o $(BUILD)/model.o $(BUILD)/graph.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_validator: tests/test_validator.c $(BUILD)/validator.o $(BUILD)/yaml_parser.o $(BUILD)/model.o $(BUILD)/graph.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_submit_yaml: tests/test_submit_yaml.c $(BUILD)/util.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_script: tests/test_script.c $(BUILD)/script.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_tasks: tests/test_tasks.c $(BUILD)/tasks.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_data: tests/test_data.c $(BUILD)/tasks.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

# A1: test_model.c y test_submit_yaml.c son pruebas del contrato v1 y de
# SUBMIT real respectivamente. No se compilan en make test hasta A2.
$(BUILD)/test_motor: tests/test_motor.c $(BUILD)/agents.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

# No se afirma una carrera con SUBMIT aun deshabilitado. La prueba de
# 100 SUBMIT concurrentes debe incorporarse durante las Fases 4-5.
test-race: race
	@echo "PENDIENTE: test-race requiere SUBMIT real (Fase 4/5)"; exit 2

test: all $(TESTS)
	./$(BUILD)/test_protocol
	./$(BUILD)/test_util
	./$(BUILD)/test_busy
	./$(BUILD)/test_cli
	./$(BUILD)/test_graph
	./$(BUILD)/test_script
	./$(BUILD)/test_tasks
	./$(BUILD)/test_data
	./$(BUILD)/test_motor
	./$(BUILD)/test_yaml
	./$(BUILD)/test_validator

clean:
	rm -rf $(BUILD) pipelined pipectl pipelined-race

-include $(wildcard $(BUILD)/*.d $(BUILD)/race/*.d)






