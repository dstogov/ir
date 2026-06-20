# Standalone build for the IR fuzz harnesses.
#
# Kept separate from the top level Makefile so the fuzzing tooling, which
# needs clang and the sanitizer runtimes, never affects the production
# build. All sources are taken from the parent directory and every
# artifact lands under build/.
#
# Usage from this directory:
#   make            build every fuzz target
#   make fuzz-O2
#   make corpus     extract the seed corpus from ../tests
#   make clean

TARGET     = $(shell uname -m)
OS         = $(shell uname -s)

SRC_DIR    = ..
BUILD_DIR  = build

CC         = clang
BUILD_CC   = gcc

CFLAGS        = -Wall -Wextra -Wno-unused-parameter -g -O1 -DIR_DEBUG=1
BUILD_CFLAGS  = -Wall -Wextra -Wno-unused-parameter -O2 -g

UBSAN_IGNORE =
IGNORELIST   = $(if $(wildcard $(UBSAN_IGNORE)),-fsanitize-ignorelist=$(UBSAN_IGNORE))
SANITIZE     = -fsanitize=address,undefined -fno-sanitize-recover=all $(IGNORELIST)
FUZZER       = -fsanitize=fuzzer

ifneq (, $(filter x86_64 amd64, $(TARGET)))
  CFLAGS       += -m64 -DIR_TARGET_X64
  BUILD_CFLAGS += -m64 -DIR_TARGET_X64
  DASM_ARCH  = x86
  DASM_FLAGS = -M -D X64=1
else ifneq (, $(filter x86 i386, $(TARGET)))
  CFLAGS       += -m32 -DIR_TARGET_X86
  BUILD_CFLAGS += -m32 -DIR_TARGET_X86
  DASM_ARCH  = x86
  DASM_FLAGS = -M
else ifneq (, $(filter aarch64 arm64, $(TARGET)))
  CFLAGS       += -DIR_TARGET_AARCH64
  BUILD_CFLAGS += -DIR_TARGET_AARCH64
  DASM_ARCH  = aarch64
  DASM_FLAGS = -M
else
  $(error Unsupported target. TARGET must be 'x86_64', 'x86' or 'aarch64')
endif

COMPILE = $(CC) $(CFLAGS) $(SANITIZE) $(FUZZER) -DFUZZ_LIBFUZZER \
	-I$(SRC_DIR) -I$(BUILD_DIR) -L$(BUILD_DIR)
LIBS = -lir -lcapstone -lm

OBJS = $(addprefix $(BUILD_DIR)/, ir.o ir_strtab.o ir_cfg.o ir_sccp.o ir_gcm.o \
	ir_ra.o ir_emit.o ir_load.o ir_save.o ir_emit_c.o ir_dump.o ir_disasm.o \
	ir_gdb.o ir_perf.o ir_check.o ir_cpuinfo.o ir_emit_llvm.o ir_mem2ssa.o)

TEXT_SRCS = fuzz_ir.c $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h \
	$(SRC_DIR)/ir_main.c $(BUILD_DIR)/libir.a

all: fuzz

fuzz: fuzz-load fuzz-O0 fuzz-O1 fuzz-O2

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZE) $(FUZZER) -I$(BUILD_DIR) -o $@ -c $<

$(BUILD_DIR)/ir.o: $(SRC_DIR)/ir_fold.h $(BUILD_DIR)/ir_fold_hash.h
$(BUILD_DIR)/ir_ra.o: $(SRC_DIR)/ir_$(DASM_ARCH).h
$(BUILD_DIR)/ir_emit.o: $(SRC_DIR)/ir_$(DASM_ARCH).h $(BUILD_DIR)/ir_emit_$(DASM_ARCH).h
$(BUILD_DIR)/ir_gdb.o: $(SRC_DIR)/ir_elf.h
$(BUILD_DIR)/ir_perf.o: $(SRC_DIR)/ir_elf.h
$(BUILD_DIR)/ir_disasm.o: $(SRC_DIR)/ir_elf.h

$(BUILD_DIR)/libir.a: $(OBJS)
	ar r $@ $^

$(BUILD_DIR)/ir_fold_hash.h: $(BUILD_DIR)/gen_ir_fold_hash $(SRC_DIR)/ir_fold.h $(SRC_DIR)/ir.h
	$(BUILD_DIR)/gen_ir_fold_hash < $(SRC_DIR)/ir_fold.h > $@
$(BUILD_DIR)/gen_ir_fold_hash: $(SRC_DIR)/gen_ir_fold_hash.c $(SRC_DIR)/ir_strtab.c $(SRC_DIR)/ir.h | $(BUILD_DIR)
	$(BUILD_CC) $(BUILD_CFLAGS) -o $@ $<

$(BUILD_DIR)/minilua: $(SRC_DIR)/dynasm/minilua.c | $(BUILD_DIR)
	$(BUILD_CC) -O2 -g $(SRC_DIR)/dynasm/minilua.c -lm -o $@
$(BUILD_DIR)/ir_emit_$(DASM_ARCH).h: $(SRC_DIR)/ir_$(DASM_ARCH).dasc $(SRC_DIR)/dynasm/*.lua $(BUILD_DIR)/minilua
	$(BUILD_DIR)/minilua $(SRC_DIR)/dynasm/dynasm.lua $(DASM_FLAGS) -o $@ $(SRC_DIR)/ir_$(DASM_ARCH).dasc

fuzz-load: $(TEXT_SRCS)
	$(COMPILE) -DFUZZ_MODE_LOAD -o $(BUILD_DIR)/$@ fuzz_ir.c $(LIBS)

fuzz-O0: $(TEXT_SRCS)
	$(COMPILE) -DFUZZ_MODE_O0 -o $(BUILD_DIR)/$@ fuzz_ir.c $(LIBS)

fuzz-O1: $(TEXT_SRCS)
	$(COMPILE) -DFUZZ_MODE_O1 -o $(BUILD_DIR)/$@ fuzz_ir.c $(LIBS)

fuzz-O2: $(TEXT_SRCS)
	$(COMPILE) -DFUZZ_MODE_O2 -o $(BUILD_DIR)/$@ fuzz_ir.c $(LIBS)

corpus:
	./make_corpus.sh $(SRC_DIR)/tests corpus

clean:
	rm -rf $(BUILD_DIR)

corpus-clean:
	rm -rf corpus

.PHONY: all fuzz fuzz-load fuzz-O0 fuzz-O1 fuzz-O2 corpus clean corpus-clean
