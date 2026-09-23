# pzpdir: PZP directory archives (spec ../../doc/pzpd-spec.md)
#
#   make            pzpdir (release), dpzpdir (debug + ASan), spzpdir (SIMD), libpzpdir.so, libpzpdir.a
#   make test       unit tests + CLI round-trip tests
#   make test-py    Python binding tests (pzp.pzpdir; needs numpy, PIL for the image test)
#   make fuzz       truncation / bit-flip fuzzing under ASan
#   make doc        Doxygen HTML + PDF (scripts/refreshDoxygen.sh)
#   make check-client   compile with the DataLoader's exact flag sets

CC      ?= gcc
PZP_DIR  = ../..
WARN     = -Wall -Wextra
BASE     = -D_GNU_SOURCE -pthread -I$(PZP_DIR) $(WARN)
RELEASE  = -O3 -march=native -mtune=native $(BASE)
DEBUG    = -O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined $(BASE)
SIMD     = -O3 -mavx2 -DINTEL_OPTIMIZATIONS $(BASE)
LIBS     = -lzstd -llz4 -lm

LIB_SRC  = $(filter-out pzpdir_cli.c,$(wildcard pzpdir_*.c))
LIB_DEPS = $(LIB_SRC) pzpdir.h pzpdir_internal.h pzpdir_unicode.h third_party/xxhash.h $(PZP_DIR)/pzp.h
CLI_SRC  = pzpdir_cli.c

.PHONY: all clean test test-py fuzz doc check-client bench

all: pzpdir dpzpdir spzpdir libpzpdir.so libpzpdir.a

pzpdir: $(LIB_DEPS) $(CLI_SRC)
	$(CC) $(RELEASE) $(LIB_SRC) $(CLI_SRC) -o $@ $(LIBS)

dpzpdir: $(LIB_DEPS) $(CLI_SRC)
	$(CC) $(DEBUG) $(LIB_SRC) $(CLI_SRC) -o $@ $(LIBS)

spzpdir: $(LIB_DEPS) $(CLI_SRC)
	$(CC) $(SIMD) $(LIB_SRC) $(CLI_SRC) -o $@ $(LIBS)

libpzpdir.so: $(LIB_DEPS)
	$(CC) -shared -fPIC $(RELEASE) $(LIB_SRC) -o $@ $(LIBS)

libpzpdir.a: $(LIB_DEPS)
	rm -rf obj_a && mkdir obj_a
	for f in $(LIB_SRC); do $(CC) -c -fPIC $(RELEASE) $$f -o obj_a/$${f%.c}.o || exit 1; done
	ar rcs $@ obj_a/*.o
	rm -rf obj_a

tests/test_pzpdir: tests/test_pzpdir.c $(LIB_DEPS)
	$(CC) $(DEBUG) -Wno-unused-function $(LIB_SRC) tests/test_pzpdir.c -o $@ $(LIBS)

tests/fuzz_pzpdir: tests/fuzz_pzpdir.c $(LIB_DEPS)
	$(CC) $(DEBUG) $(LIB_SRC) tests/fuzz_pzpdir.c -o $@ $(LIBS)

client_smoke: scripts/client_smoke.c $(LIB_DEPS)
	$(CC) $(DEBUG) $(LIB_SRC) scripts/client_smoke.c -o $@ $(LIBS)

bench_dataloader_replay: scripts/bench_dataloader_replay.c $(LIB_DEPS)
	$(CC) $(RELEASE) $(LIB_SRC) scripts/bench_dataloader_replay.c -o $@ $(LIBS)

bench_pzpdir_early: scripts/bench_pzpdir_early.c $(LIB_DEPS)
	$(CC) $(RELEASE) $(LIB_SRC) scripts/bench_pzpdir_early.c -o $@ $(LIBS) -ljpeg -lpng

test: tests/test_pzpdir dpzpdir
	./tests/test_pzpdir
	./tests/run_cli_tests.sh ./dpzpdir

test-py: libpzpdir.so
	python3 tests/test_pzpdir_py.py

fuzz: tests/fuzz_pzpdir
	./tests/fuzz_pzpdir

bench: bench_pzpdir_early

doc:
	./scripts/refreshDoxygen.sh

check-client:
	./scripts/check_client_build.sh

clean:
	rm -f pzpdir dpzpdir spzpdir libpzpdir.so libpzpdir.a tests/test_pzpdir tests/fuzz_pzpdir bench_pzpdir_early bench_dataloader_replay client_smoke
	rm -rf doc/html doc/latex PZPDIR.pdf
