CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -g
SRC     := src/main.c src/util.c src/lex.c src/parse.c src/check.c src/emit.c
HDR     := src/util.h src/lex.h src/ast.h src/parse.h src/check.h src/emit.h

all: voltc voltbind

voltc: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

voltbind: src/voltbind.c src/util.c src/util.h
	$(CC) $(CFLAGS) -o $@ src/voltbind.c src/util.c

.PHONY: all test clean

test: voltc voltbind
	./tests/run_tests.sh

clean:
	rm -f voltc voltbind
	rm -rf examples/.volt-cache tests/tmp
	rm -rf examples/greeter/native examples/greeter/greeter.vt
