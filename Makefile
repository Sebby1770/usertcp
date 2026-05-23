CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -O2 -g
LDFLAGS ?=

SRC := src/main.c src/tun.c
OBJ := $(SRC:.c=.o)
BIN := usertcp

.PHONY: all clean run-linux run-macos

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c src/tun.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

# Linux: run scripts/setup-linux.sh first.
run-linux: $(BIN)
	./$(BIN) tun0

# macOS: kernel picks the utun number. Note the name printed by the
# program, then in another terminal run scripts/setup-macos.sh utunN.
run-macos: $(BIN)
	./$(BIN)
