CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -g
SRC     := src/main.c src/util.c src/lex.c src/parse.c src/check.c src/emit.c
HDR     := src/util.h src/lex.h src/ast.h src/parse.h src/check.h src/emit.h

all: vytoc vytobind

vytoc: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

vytobind: src/vytobind.c src/util.c src/util.h
	$(CC) $(CFLAGS) -o $@ src/vytobind.c src/util.c

.PHONY: all test test-charts test-mobile test-win clean clean-cache

test: vytoc vytobind
	./tests/run_tests.sh

# Split out of `make test`: both are leaf packages nothing else imports, and
# both are expensive because every entry file compiles its own copy of the ui
# stack. Run the matching one after touching lib/vyto/ui/chart.vt or
# lib/vyto/mobile/android/ui.vt, and both before a release.
test-charts: vytoc
	./tests/run_tests_charts.sh

test-mobile: vytoc
	./tests/run_tests_mobile.sh

# Cross-build the Windows-portable slice for windows-x64 and stage it, with its
# goldens and a self-checking run.ps1, into tests/tmp/win-x64/. Runs nothing —
# copy the staging dir to a Windows machine to actually execute it.
# Needs: sudo apt-get install -y gcc-mingw-w64-x86-64
test-win: vytoc
	./tests/run_tests_win.sh

# Every .vyto-cache in the tree, including apps/* which `clean` does not cover
# and the shared object cache at the repo root (also a .vyto-cache, so the same
# find catches it). Run this before regenerating any golden — a stale cache
# validates the previous build, not the current one.
clean-cache:
	find . -name .vyto-cache -prune -exec rm -rf {} +
	rm -rf tests/tmp

clean: clean-cache
	rm -f vytoc vytobind
	rm -rf examples/greeter/native examples/greeter/greeter.vt
