# schedplace — user-space thread placement daemon (placement only).
#
# Build products never land inside the repository: the project forbids artifacts in
# the tree (scripts/audit-hygiene.sh enforces it). Override the location with:
#   make BUILD_DIR=/somewhere test
CC ?= gcc
STD := -std=c11
WARN := -Wall -Wextra -Wpedantic -Werror
CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS ?= -O2 -g
LDFLAGS ?=
ALL_CFLAGS := $(STD) $(WARN) $(CFLAGS)

BUILD_DIR ?= $(abspath ../../../.build/perf/schedplace)
BUILD := $(BUILD_DIR)
TEST_TMP_DIR := $(BUILD_DIR)/tmp
BIN := $(BUILD)/schedplace
HEADERS := $(wildcard include/*.h)
LIB_SRCS := $(wildcard lib/*.c)
LIB_OBJS := $(patsubst lib/%.c,$(BUILD)/%.o,$(LIB_SRCS))
MAIN_OBJ := $(BUILD)/main.o
TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))
FAKE := $(BUILD)/fake_sp

PREFIX ?= /usr
SYSCONFDIR ?= /etc
UNITDIR ?= $(PREFIX)/lib/systemd/system
DESTDIR ?=
CONF ?= $(abspath ../../configs/perf/schedplace.conf)

.PHONY: all test clean install uninstall check print-build-dir

all: $(BIN)

$(BUILD):
	@mkdir -p $(BUILD) $(TEST_TMP_DIR)

$(BUILD)/%.o: lib/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c -o $@ $<

$(BUILD)/main.o: main.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c -o $@ $<

$(BIN): $(LIB_OBJS) $(MAIN_OBJ)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

$(FAKE): tests/fake_sp.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -pthread -o $@ $<

$(BUILD)/test_%: tests/test_%.c $(HEADERS) $(LIB_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) -DSP_TEST_TMPDIR='"$(TEST_TMP_DIR)"' $(ALL_CFLAGS) -o $@ $< $(LIB_OBJS) $(LDFLAGS)

check: all

test: all $(FAKE) $(TEST_BINS)
	@mkdir -p $(TEST_TMP_DIR)
	@set -e; for t in $(TEST_BINS); do \
		bin="$$t"; case "$$bin" in /*) ;; *) bin="./$$bin";; esac; \
		echo "== $$(basename $$t)"; \
		SP_BIN=$(abspath $(BIN)) SP_FAKE=$(abspath $(FAKE)) "$$bin"; \
	done
	@echo "ALL TESTS PASSED"

install: all
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/schedplace
	install -Dm644 $(CONF) $(DESTDIR)$(SYSCONFDIR)/schedplace/schedplace.conf
	install -Dm644 schedplace.service $(DESTDIR)$(UNITDIR)/schedplace.service
	@echo "installed: $(DESTDIR)$(PREFIX)/bin/schedplace"
	@echo "config:    $(DESTDIR)$(SYSCONFDIR)/schedplace/schedplace.conf"
	@echo "unit:      $(DESTDIR)$(UNITDIR)/schedplace.service"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/schedplace
	rm -f $(DESTDIR)$(SYSCONFDIR)/schedplace/schedplace.conf
	rm -f $(DESTDIR)$(UNITDIR)/schedplace.service

clean:
	rm -rf $(BUILD) .build schedplace

print-build-dir:
	@echo $(BUILD_DIR)

