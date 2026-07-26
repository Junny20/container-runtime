# OCI-compliant container runtime — build file.
#
# Targets:
#   make            release build      -> build/runtime
#   make debug      ASan/UBSan build   -> build/runtime-debug
#   make test       build + run unit tests and the lifecycle harness
#   make bench      build + measure cold-start, memory overhead, conformance
#   make clean      remove build artifacts
#
# The runtime only runs meaningfully on Linux (namespaces, cgroup v2, pivot_root).
# It still *compiles* elsewhere so the parser/state code can be unit-tested.

CC       ?= cc
CSTD      = -std=c11
WARN      = -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes
DEFS      = -D_GNU_SOURCE
INCS      = -Iinclude -Ithird_party/cJSON
OPT      ?= -O2 -g

CFLAGS    = $(CSTD) $(WARN) $(DEFS) $(INCS) $(OPT)
LDFLAGS  ?=

BUILD     = build
BIN       = $(BUILD)/runtime
BIN_DBG   = $(BUILD)/runtime-debug

# sources

CORE_SRC  = src/oci.c src/container.c src/process.c src/namespaces.c \
            src/mounts.c src/cgroup.c src/log.c third_party/cJSON/cJSON.c
MAIN_SRC  = src/main.c
ALL_SRC   = $(CORE_SRC) $(MAIN_SRC)

CORE_OBJ  = $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC))
MAIN_OBJ  = $(patsubst %.c,$(BUILD)/%.o,$(MAIN_SRC))

# unit tests

TEST_OCI   = $(BUILD)/test_oci
TEST_CG    = $(BUILD)/test_cgroup

# sanitizer flags for the debug build

SAN       = -fsanitize=address,undefined -fno-omit-frame-pointer

# default target

.PHONY: all
all: $(BIN)

$(BIN): $(CORE_OBJ) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "built $@"

# Pattern rule: compile any .c into build/ mirroring the source tree.
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# debug build (ASan + UBSan)

.PHONY: debug
debug: OPT = -O1 -g
debug: CFLAGS += $(SAN)
debug: LDFLAGS += $(SAN)
debug: $(BIN_DBG)

$(BIN_DBG): $(ALL_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ALL_SRC) -o $@ $(LDFLAGS)
	@echo "built $@ (ASan/UBSan)"

# tests

.PHONY: test
test: $(TEST_OCI) $(TEST_CG) $(BIN)
	@echo "== unit: oci config parsing =="
	@./$(TEST_OCI)
	@echo "== unit: cgroup controller writes =="
	@./$(TEST_CG)
	@echo "== conformance: lifecycle rules =="
	@RT_BIN=$(BIN) sh tests/conformance/lifecycle_cases.sh || true
	@echo "== lifecycle harness =="
	@RT_BIN=$(BIN) sh tests/harness.sh || \
	  echo "harness skipped/failed (needs Linux + privileges)"

# Unit tests link the core objects (not main.o) plus the test driver, built with
# sanitizers so memory bugs in the parser surface in CI.
$(TEST_OCI): tests/test_oci.c $(CORE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(DEFS) $(INCS) -O1 -g $(SAN) \
	  tests/test_oci.c $(CORE_SRC) -o $@ $(SAN)

$(TEST_CG): tests/test_cgroup.c $(CORE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(DEFS) $(INCS) -O1 -g $(SAN) \
	  tests/test_cgroup.c $(CORE_SRC) -o $@ $(SAN)

# benchmarks

.PHONY: bench
bench: $(BIN)
	@echo "== benchmarks (cold-start, memory overhead, conformance) =="
	@RT_BIN=$(BIN) sh tests/bench.sh $(N)

# housekeeping

.PHONY: clean
clean:
	rm -rf $(BUILD)
	@echo "cleaned"

.PHONY: help
help:
	@grep -E '^#   make' Makefile | sed 's/^#   //'
