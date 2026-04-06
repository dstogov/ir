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
CC          = clang

# Base flags
CFLAGS_BASE = -Wall -Wextra -Wno-unused-parameter -g -O1 -DIR_DEBUG=1

# Sanitizer flags (always on for fuzzing)
SANITIZE_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all

# libFuzzer flags
FUZZER_FLAGS = -fsanitize=fuzzer

# Check that the compiler supports libFuzzer
#HAVE_FUZZER := $(shell echo 'int LLVMFuzzerTestOneInput(const unsigned char *d, long s) { return 0; }' | \
#       $(CC) -x c -fsanitize=fuzzer -fsanitize=address - -o /dev/null 2>/dev/null && echo 1 || echo 0)

.PHONY: all fuzz-load fuzz-pipeline corpus clean

all: fuzz-load fuzz-pipeline

$(FUZZ_BUILD)/libir.a:
	@mkdir -p $(FUZZ_BUILD)
	make $(FUZZ_BUILD)/libir.a \
		TARGET=$(TARGET) BUILD_DIR=$(FUZZ_BUILD) BUILD=$(BUILD) \
		CC=$(CC) CFLAGS="$(CFLAGS_BASE) $(SANITIZE_FLAGS) $(FUZZER_FLAGS)"

# Fuzzer targets
fuzz-load: $(FUZZ_DIR)/fuzz_load.c $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h $(FUZZ_BUILD)/libir.a
	$(CC) $(CFLAGS_BASE) $(SANITIZE_FLAGS) $(FUZZER_FLAGS) -DFUZZ_LIBFUZZER \
		-I$(SRC_DIR) -I$(FUZZ_BUILD) -L$(FUZZ_BUILD) -o $(FUZZ_BUILD)/$@ $< -lir
	@echo "Built: $(FUZZ_BUILD)/$@"
	@echo "Run:   $(FUZZ_BUILD)/$@ $(CORPUS_DIR)/load/"

fuzz-pipeline: $(FUZZ_DIR)/fuzz_pipeline.c $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h $(FUZZ_BUILD)/libir.a
	$(CC) $(CFLAGS_BASE) $(SANITIZE_FLAGS) $(FUZZER_FLAGS) -DFUZZ_LIBFUZZER \
		-I$(SRC_DIR) -I$(FUZZ_BUILD) -L$(FUZZ_BUILD) -o $(FUZZ_BUILD)/$@ $< -lir
	@echo "Built: $(FUZZ_BUILD)/$@"
	@echo "Run:   $(FUZZ_BUILD)/$@ $(CORPUS_DIR)/pipeline/"

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
