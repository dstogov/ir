# Standalone build for the IR fuzz harnesses.
#
# Kept separate from the top level Makefile so the fuzzing tooling, which
# needs clang and the sanitizer runtimes, never affects the production
# build. All sources are taken from the parent directory and every
# artifact lands under build/.
#
# Usage from this directory:
#   make            build every fuzz target
#   make fuzz-text-O2
#   make fuzz-graph-O2
#   make corpus     extract the seed corpus from ../tests
#   make clean

SRC_DIR    = ..
BUILD_DIR  = build

CC         = clang

CFLAGS        = -Wall -Wextra -Wno-unused-parameter -g -O1 -DIR_DEBUG=1

UBSAN_IGNORE =
IGNORELIST   = $(if $(wildcard $(UBSAN_IGNORE)),-fsanitize-ignorelist=$(UBSAN_IGNORE))
SANITIZE     = -fsanitize=address,undefined -fno-sanitize-recover=all $(IGNORELIST)
FUZZER       = -fsanitize=fuzzer

COMPILE = $(CC) $(CFLAGS) $(SANITIZE) $(FUZZER) -DFUZZ_LIBFUZZER \
	-I$(SRC_DIR) -I$(BUILD_DIR) -L$(BUILD_DIR)

LIBS = -lir -lcapstone -lm

TEXT_SRCS = fuzz_text.c fuzz_corpus.h $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h \
	$(SRC_DIR)/ir_main.c $(BUILD_DIR)/libir.a
GRAPH_SRCS = fuzz_graph.c fuzz_corpus.h $(SRC_DIR)/ir.h $(SRC_DIR)/ir_builder.h \
	$(BUILD_DIR)/libir.a

all: fuzz

fuzz: fuzz-text-load fuzz-text-O0 fuzz-text-O1 fuzz-text-O2 fuzz-graph-O0 fuzz-graph-O1 fuzz-graph-O2

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/libir.a: $(SRC_DIR)/ir.c $(SRC_DIR)/ir_strtab.c $(SRC_DIR)/ir_cfg.c \
	$(SRC_DIR)/ir_sccp.c $(SRC_DIR)/ir_gcm.c $(SRC_DIR)/ir_ra.c $(SRC_DIR)/ir_emit.c \
	$(SRC_DIR)/ir_load.c $(SRC_DIR)/ir_save.c $(SRC_DIR)/ir_emit_c.c $(SRC_DIR)/ir_dump.c \
	$(SRC_DIR)/ir_disasm.c $(SRC_DIR)/ir_gdb.c $(SRC_DIR)/ir_perf.c $(SRC_DIR)/ir_check.c \
	$(SRC_DIR)/ir_cpuinfo.c $(SRC_DIR)/ir_emit_llvm.c $(SRC_DIR)/ir_mem2ssa.c \
	$(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h $(SRC_DIR)/ir_x86.h $(SRC_DIR)/ir_aarch64.h \
	$(SRC_DIR)/ir_elf.h $(SRC_DIR)/ir_builder.h  \
	$(SRC_DIR)/ir_fold.h $(SRC_DIR)/ir_x86.dasc $(SRC_DIR)/ir_aarch64.dasc | $(BUILD_DIR)
	make -f $(SRC_DIR)/Makefile $(BUILD_DIR)/libir.a \
		CC=$(CC) SRC_DIR=$(SRC_DIR) BUILD_DIR=$(BUILD_DIR) \
		CFLAGS="$(CFLAGS) $(SANITIZE) $(FUZZER) -DFUZZ_LIBFUZZER"

fuzz-text-load: $(TEXT_SRCS)
	$(COMPILE) -DFUZZ_MODE_LOAD -o $(BUILD_DIR)/$@ fuzz_text.c $(LIBS)

fuzz-text-O0: $(TEXT_SRCS)
	$(COMPILE) -DFUZZ_MODE_O0 -o $(BUILD_DIR)/$@ fuzz_text.c $(LIBS)

fuzz-text-O1: $(TEXT_SRCS)
	$(COMPILE) -DFUZZ_MODE_O1 -o $(BUILD_DIR)/$@ fuzz_text.c $(LIBS)

fuzz-text-O2: $(TEXT_SRCS)
	$(COMPILE) -DFUZZ_MODE_O2 -o $(BUILD_DIR)/$@ fuzz_text.c $(LIBS)

fuzz-graph-O0: $(GRAPH_SRCS)
	$(COMPILE) -DFUZZ_GRAPH_O0 -o $(BUILD_DIR)/$@ fuzz_graph.c $(LIBS)

fuzz-graph-O1: $(GRAPH_SRCS)
	$(COMPILE) -DFUZZ_GRAPH_O1 -o $(BUILD_DIR)/$@ fuzz_graph.c $(LIBS)

fuzz-graph-O2: $(GRAPH_SRCS)
	$(COMPILE) -DFUZZ_GRAPH_O2 -o $(BUILD_DIR)/$@ fuzz_graph.c $(LIBS)

corpus:
	./make_corpus.sh $(SRC_DIR)/tests corpus

clean:
	rm -rf $(BUILD_DIR)

corpus-clean:
	rm -rf corpus/text

.PHONY: all fuzz fuzz-text-load fuzz-text-O0 fuzz-text-O1 fuzz-text-O2 \
	fuzz-graph-O0 fuzz-graph-O1 fuzz-graph-O2 corpus clean corpus-clean
