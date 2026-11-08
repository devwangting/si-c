CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11

demo: main.c vec.c vec.h
	$(CC) $(CFLAGS) -o demo main.c vec.c

clean:
	rm -f demo

.PHONY: clean
