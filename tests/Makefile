# Host tests for nodeTrix: the real engine, step sequencers, WAV loader and slot
# files, compiled for this computer. Runs on macOS and Linux (no devkitPro needed).
#   make -C tests
SRC   := ../sequencer/source
SHIM  := ../tools/ui_render/shim
CC    ?= cc
CFLAGS ?= -O2 -g -Wall -Wno-unused-function
BIN   := build/run_tests
TESTS := t.c test_dsp.c test_graph.c test_mod.c test_store.c
CODE  := $(SRC)/synth.c $(SRC)/modseq.c $(SRC)/store.c $(SRC)/samples.c

test: $(BIN)
	@rm -rf build/work && mkdir -p build/work
	cd build/work && ../run_tests

$(BIN): $(TESTS) t.h $(CODE) $(SRC)/*.h $(SHIM)/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -I. -I$(SHIM) -I$(SRC) -I../common -DSTORE_DIR=\"store\" -DOLD_STORE_DIR=\"old\" \
	    $(TESTS) $(CODE) -lm -o $@

clean:
	rm -rf build

.PHONY: test clean
