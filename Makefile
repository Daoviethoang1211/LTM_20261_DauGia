CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc $(shell pkg-config --cflags json-c sqlite3 libsodium)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -MMD -MP
LDLIBS += $(shell pkg-config --libs json-c sqlite3 libsodium) -pthread
COMMON = build/frame.o build/protocol.o
BIND ?= 127.0.0.1
HOST ?= 127.0.0.1
PORT ?= 5555
DB ?= var/auction.db

.PHONY: all test clean run demo client
all: build/server build/client build/libauction_client.a
build/server: build/server.o build/storage.o build/app.o build/auth_worker.o $(COMMON)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@
build/libauction_client.a: build/client_net.o $(COMMON)
	$(AR) rcs $@ $^
build/client: build/menu.o build/libauction_client.a
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@
build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
test: all
	python3 -m unittest discover -s tests -p '*.py' -v
run: all
	@mkdir -p "$(dir $(DB))"
	./build/server "$(BIND)" "$(PORT)" "$(DB)"
demo: all
	bash scripts/demo-clients.sh "$(HOST)" "$(PORT)"
client: all
	./build/client "$(HOST)" "$(PORT)"
clean:
	rm -f build/*.o build/*.d build/*.a build/server build/client
-include $(wildcard build/*.d)
