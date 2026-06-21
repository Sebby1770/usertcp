CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -O2 -g
LDFLAGS ?=

# The protocol core (everything except the TUN glue and the entry point).
# Shared by the binary and the host-side test harness.
CORE := src/net.c src/stack.c src/tcp.c src/app.c
SRC  := src/main.c src/tun.c $(CORE)
OBJ  := $(SRC:.c=.o)
BIN  := usertcp

TEST_SRC := tests/test_stack.c $(CORE)
TEST_BIN := tests/test_stack

.PHONY: all clean test run-linux run-macos

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Host-side protocol tests: no TUN, no root. Built directly from sources.
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

# Linux: run scripts/setup-linux.sh first.
run-linux: $(BIN)
	./$(BIN) tun0

# macOS: kernel picks the utun number. Note the name printed by the
# program, then in another terminal run scripts/setup-macos.sh utunN.
run-macos: $(BIN)
	./$(BIN)
