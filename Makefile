CC = cc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS = -DDEFAULT_INCLUDE_DIR='"$(CURDIR)/include"'

.PHONY: all test clean
all: build/4c

build/4c: src/4c.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

test: build/4c
	python3 tests/test.py

clean:
	rm -rf build
