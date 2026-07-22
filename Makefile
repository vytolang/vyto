CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -g
SRC     := src/main.c src/util.c src/lex.c src/parse.c src/check.c src/emit.c
HDR     := src/util.h src/lex.h src/ast.h src/parse.h src/check.h src/emit.h

all: vytoc vytobind

vytoc: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

vytobind: src/vytobind.c src/util.c src/util.h
	$(CC) $(CFLAGS) -o $@ src/vytobind.c src/util.c

.PHONY: all test clean clean-cache

test: vytoc vytobind
	./tests/run_tests.sh

# Every .vyto-cache in the tree, including apps/* which `clean` does not cover.
# Run this before regenerating any golden — a stale cache validates the previous
# build, not the current one.
clean-cache:
	find . -name .vyto-cache -prune -exec rm -rf {} +
	rm -rf tests/tmp

clean: clean-cache
	rm -f vytoc vytobind
	rm -rf examples/greeter/native examples/greeter/greeter.vt
