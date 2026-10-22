CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc $(shell pkg-config --cflags json-c sqlite3)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -MMD -MP
LDLIBS += $(shell pkg-config --libs json-c sqlite3)
COMMON = build/frame.o build/protocol.o
BIND ?= 127.0.0.1
HOST ?= 127.0.0.1
PORT ?= 5555
DB ?= var/auction.db

.PHONY: all test clean run demo
all: build/server build/client
build/server: build/server.o build/storage.o $(COMMON)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@
build/client: build/client.o build/frame.o
	$(CC) $(CFLAGS) $^ -o $@
build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
test: all
	python3 tests/integration.py
run: all
	@mkdir -p "$(dir $(DB))"
	./build/server "$(BIND)" "$(PORT)" "$(DB)"
demo: all
	bash scripts/demo-clients.sh "$(HOST)" "$(PORT)"
clean:
	rm -f build/*.o build/*.d build/server build/client
-include $(wildcard build/*.d)
