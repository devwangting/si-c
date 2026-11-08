CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Wpedantic
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Isrc
# macOS hides SIGWINCH behind _DARWIN_C_SOURCE when _POSIX_C_SOURCE is set.
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif
LDFLAGS ?=
LDLIBS  ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC = src/util.c src/text.c src/syntax.c src/index.c src/term.c src/app.c \
      src/render.c src/main.c
OBJ = $(SRC:.c=.o)
CORE = src/util.c src/text.c src/syntax.c src/index.c
UI_CORE = $(CORE) src/term.c src/app.c src/render.c

all: si

si: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -c -o $@ $<

test_core: tests/test_core.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ \
		tests/test_core.c $(CORE) $(LDLIBS)

ui_smoke: tests/ui_smoke.c $(UI_CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ \
		tests/ui_smoke.c $(UI_CORE) $(LDLIBS)

test_keys: tests/test_keys.c src/term.c src/util.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -std=c11 -o $@ \
		tests/test_keys.c src/term.c src/util.c $(LDLIBS)

test: test_core test_keys ui_smoke
	./test_core tests/fixtures/demo
	./test_keys
	./ui_smoke tests/fixtures/demo > /dev/null

clean:
	rm -f si test_core test_keys ui_smoke $(OBJ)

install: si
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 si $(DESTDIR)$(BINDIR)/si

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/si

.PHONY: all clean test install uninstall
