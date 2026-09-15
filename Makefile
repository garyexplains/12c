CC = cc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS = -DDEFAULT_INCLUDE_DIR='"$(CURDIR)/include"'

.PHONY: all test test-sanitize test-dhrystone dhrystone clean
all: build/4c

build/4c: src/4c.c src/softfloat.h src/asm_comments.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

test: build/4c
	python3 tests/test.py

test-dhrystone: build/4c
	python3 tests/test.py CompilerTests.test_dhrystone_integer_core CompilerTests.test_dhrystone_unchanged CompilerTests.test_dhrystone_unchanged_deterministic_timing

dhrystone: build/dhry

build/dhry.s: build/4c examples/dhry.c include/stdio.h include/stdlib.h include/stdbool.h include/string.h include/time.h
	./build/4c --target linux examples/dhry.c -o $@

build/dhry: build/dhry.s
	$(CC) $< -o $@

build/4c-sanitize: src/4c.c src/softfloat.h src/asm_comments.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all $< -o $@

test-sanitize: build/4c-sanitize
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 TEST_COMPILER=$(CURDIR)/build/4c-sanitize python3 tests/test.py

clean:
	rm -rf build
