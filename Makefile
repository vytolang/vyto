CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -g
SRC     := src/main.c src/util.c src/lex.c src/parse.c src/check.c src/emit.c
HDR     := src/util.h src/lex.h src/ast.h src/parse.h src/check.h src/emit.h

all: vytoc vytobind

vytoc: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

vytobind: src/vytobind.c src/util.c src/util.h
	$(CC) $(CFLAGS) -o $@ src/vytobind.c src/util.c

.PHONY: all test clean

test: vytoc vytobind
	./tests/run_tests.sh

clean:
	rm -f vytoc vytobind
	rm -rf examples/.vyto-cache tests/tmp tests/ui/.vyto-cache
	rm -rf tests/fixtures/libpath/.vyto-cache tests/fixtures/libpath/shadow/.vyto-cache
	rm -rf examples/greeter/native examples/greeter/greeter.vt
