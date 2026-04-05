# IR - Lightweight JIT Compilation Framework
# (Fuzzing Makefile)
# Copyright (C) 2026 IR project.
# Authors: Anatol Belski <anbelski@linux.microsoft.com>
#
# Requires clang with libFuzzer support.
#
# Usage:
#   make -f fuzz/Makefile fuzz-load          # parser fuzzer
#   make -f fuzz/Makefile fuzz-pipeline      # optimization pipeline fuzzer
#   make -f fuzz/Makefile corpus             # extract seed corpus from tests
#   make -f fuzz/Makefile clean              # remove fuzz build artifacts
#
# Run:
#   ./fuzz/build/fuzz_load fuzz/corpus/load/
#   ./fuzz/build/fuzz_pipeline fuzz/corpus/pipeline/

# Inherit from parent Makefile defaults
TARGET     ?= x86_64
BUILD      ?= debug
SRC_DIR    ?= .
FUZZ_DIR    = $(SRC_DIR)/fuzz
FUZZ_BUILD  = $(FUZZ_DIR)/build
CORPUS_DIR  = $(FUZZ_DIR)/corpus

# Default to clang for fuzzing
CC         ?= clang

# Base flags
CFLAGS_BASE = -Wall -Wextra -Wno-unused-parameter -g -O1 -DIR_DEBUG=1

# Target flags
ifeq (x86_64, $(TARGET))
  CFLAGS_BASE += -m64 -DIR_TARGET_X64
  DASM_ARCH = x86
endif
ifeq (x86, $(TARGET))
  CFLAGS_BASE += -m32 -DIR_TARGET_X86
  DASM_ARCH = x86
endif
ifeq (aarch64, $(TARGET))
  CFLAGS_BASE += -DIR_TARGET_AARCH64
  DASM_ARCH = aarch64
endif

# Sanitizer flags (always on for fuzzing)
SANITIZE_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all

# libFuzzer flags
FUZZER_FLAGS = -fsanitize=fuzzer

# System libraries
LDFLAGS = -lm
ifneq ($(OS),Windows_NT)
  LDFLAGS += -ldl
endif
LDFLAGS += -lcapstone

# Object list (same as main Makefile)
OBJS_COMMON = $(FUZZ_BUILD)/ir.o $(FUZZ_BUILD)/ir_strtab.o $(FUZZ_BUILD)/ir_cfg.o \
	$(FUZZ_BUILD)/ir_sccp.o $(FUZZ_BUILD)/ir_gcm.o $(FUZZ_BUILD)/ir_ra.o \
	$(FUZZ_BUILD)/ir_emit.o $(FUZZ_BUILD)/ir_load.o $(FUZZ_BUILD)/ir_save.o \
	$(FUZZ_BUILD)/ir_emit_c.o $(FUZZ_BUILD)/ir_dump.o $(FUZZ_BUILD)/ir_disasm.o \
	$(FUZZ_BUILD)/ir_gdb.o $(FUZZ_BUILD)/ir_perf.o $(FUZZ_BUILD)/ir_check.o \
	$(FUZZ_BUILD)/ir_cpuinfo.o $(FUZZ_BUILD)/ir_emit_llvm.o $(FUZZ_BUILD)/ir_mem2ssa.o

# Use pre-built DynASM headers from the main build
BUILD_DIR ?= .

# Check that the compiler supports libFuzzer
HAVE_FUZZER := $(shell echo 'int LLVMFuzzerTestOneInput(const unsigned char *d, long s) { return 0; }' | \
	$(CC) -x c -fsanitize=fuzzer -fsanitize=address - -o /dev/null 2>/dev/null && echo 1 || echo 0)

.PHONY: all fuzz-load fuzz-pipeline corpus clean

ifneq ($(HAVE_FUZZER),1)
  ifneq ($(MAKECMDGOALS),corpus)
    ifneq ($(MAKECMDGOALS),clean)
      $(error libFuzzer not available. Use clang with -fsanitize=fuzzer support)
    endif
  endif
endif

all: fuzz-load fuzz-pipeline

$(FUZZ_BUILD):
	@mkdir -p $(FUZZ_BUILD)

# Common objects, compiled with sanitizers but no fuzzer instrumentation

$(FUZZ_BUILD)/%.o: $(SRC_DIR)/%.c | $(FUZZ_BUILD)
	$(CC) $(CFLAGS_BASE) $(SANITIZE_FLAGS) -I$(BUILD_DIR) -o $@ -c $<

# Specific dependencies
$(FUZZ_BUILD)/ir.o: $(SRC_DIR)/ir_fold.h $(BUILD_DIR)/ir_fold_hash.h
$(FUZZ_BUILD)/ir_ra.o: $(SRC_DIR)/ir_$(DASM_ARCH).h
$(FUZZ_BUILD)/ir_emit.o: $(SRC_DIR)/ir_$(DASM_ARCH).h $(BUILD_DIR)/ir_emit_$(DASM_ARCH).h

# Fuzzer targets

fuzz-load: $(OBJS_COMMON) $(FUZZ_BUILD)/fuzz_load.o | $(FUZZ_BUILD)
	$(CC) $(CFLAGS_BASE) $(SANITIZE_FLAGS) $(FUZZER_FLAGS) -o $(FUZZ_BUILD)/$@ $^ $(LDFLAGS)
	@echo "Built: $(FUZZ_BUILD)/$@"
	@echo "Run:   $(FUZZ_BUILD)/$@ $(CORPUS_DIR)/load/"

$(FUZZ_BUILD)/fuzz_load.o: $(FUZZ_DIR)/fuzz_load.c $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h | $(FUZZ_BUILD)
	$(CC) $(CFLAGS_BASE) $(SANITIZE_FLAGS) $(FUZZER_FLAGS) -DFUZZ_LIBFUZZER -I$(SRC_DIR) -I$(BUILD_DIR) -o $@ -c $<

fuzz-pipeline: $(OBJS_COMMON) $(FUZZ_BUILD)/fuzz_pipeline.o | $(FUZZ_BUILD)
	$(CC) $(CFLAGS_BASE) $(SANITIZE_FLAGS) $(FUZZER_FLAGS) -o $(FUZZ_BUILD)/$@ $^ $(LDFLAGS)
	@echo "Built: $(FUZZ_BUILD)/$@"
	@echo "Run:   $(FUZZ_BUILD)/$@ $(CORPUS_DIR)/pipeline/"

$(FUZZ_BUILD)/fuzz_pipeline.o: $(FUZZ_DIR)/fuzz_pipeline.c $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h | $(FUZZ_BUILD)
	$(CC) $(CFLAGS_BASE) $(SANITIZE_FLAGS) $(FUZZER_FLAGS) -DFUZZ_LIBFUZZER -I$(SRC_DIR) -I$(BUILD_DIR) -o $@ -c $<

# Corpus extraction

corpus: | $(CORPUS_DIR)/load $(CORPUS_DIR)/pipeline
	@echo "Extracting seed corpus from tests..."
	@count=0; \
	for f in $$(find $(SRC_DIR)/tests -name '*.irt' -type f); do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f" .irt); \
			dir=$$(basename $$(dirname "$$f")); \
			if [ "$$dir" = "tests" ]; then \
				outname="$$base"; \
			else \
				outname="$${dir}_$${base}"; \
			fi; \
			sed -n '/^--CODE--$$/,/^--EXPECT--$$/{ /^--CODE--$$/d; /^--EXPECT--$$/d; p; }' "$$f" \
				> "$(CORPUS_DIR)/load/$${outname}.ir" 2>/dev/null; \
			if [ -s "$(CORPUS_DIR)/load/$${outname}.ir" ]; then \
				count=$$((count + 1)); \
			else \
				rm -f "$(CORPUS_DIR)/load/$${outname}.ir"; \
			fi; \
		fi; \
	done; \
	echo "Extracted $$count seed files to $(CORPUS_DIR)/load/"
	@echo "Creating pipeline corpus (prepend opt-level byte)..."
	@count=0; \
	for f in $(CORPUS_DIR)/load/*.ir; do \
		if [ -f "$$f" ]; then \
			base=$$(basename "$$f"); \
			for lvl in 0 1 2; do \
				printf "\\x0$${lvl}" > "$(CORPUS_DIR)/pipeline/$${lvl}_$${base}"; \
				cat "$$f" >> "$(CORPUS_DIR)/pipeline/$${lvl}_$${base}"; \
				count=$$((count + 1)); \
			done; \
		fi; \
	done; \
	echo "Created $$count pipeline seed files in $(CORPUS_DIR)/pipeline/"

$(CORPUS_DIR)/load:
	@mkdir -p $@

$(CORPUS_DIR)/pipeline:
	@mkdir -p $@

# Cleanup

clean:
	rm -rf $(FUZZ_BUILD)
	rm -rf $(CORPUS_DIR)
