# TARGET may be "x86_64" or "x86" or "aarch64"
TARGET     = $(shell uname -m)
OS         = $(shell uname -s)
# BUILD can be "debug" or "release"
BUILD      = release
BUILD_DIR  = .
SRC_DIR    = .
HAVE_LLVM  = no
TESTS      = $(SRC_DIR)/tests

PREFIX     = /usr/local

CC         = gcc
BUILD_CC   = gcc
override CFLAGS += -Wall -Wextra -Wno-unused-parameter
override BUILD_CFLAGS += -Wall -Wextra -Wno-unused-parameter
LDFLAGS    = -lm -ldl
LLK        = llk

ifeq (debug, $(BUILD))
 override CFLAGS += -O0 -g -DIR_DEBUG=1
 override BUILD_CFLAGS += -O2 -g -DIR_DEBUG=1
 MINILUA_CFLAGS = -O0 -g
else ifeq (release, $(BUILD))
 override CFLAGS += -O2 -g
 override BUILD_CFLAGS += -O2 -g
 MINILUA_CFLAGS = -O2 -g
else
 $(error Unsupported build type. BUILD must be 'release' or 'debug')
endif

ifneq (, $(filter x86_64 amd64, $(TARGET)))
  override CFLAGS += -m64 -DIR_TARGET_X64
  override BUILD_CFLAGS += -m64 -DIR_TARGET_X64
  DASM_ARCH  = x86
  DASM_FLAGS = -M -D X64=1
else ifneq (, $(filter x86 i386, $(TARGET)))
  override CFLAGS += -m32 -DIR_TARGET_X86
  override BUILD_CFLAGS += -m32 -DIR_TARGET_X86
  DASM_ARCH  = x86
  DASM_FLAGS = -M
else ifneq (, $(filter aarch64 arm64, $(TARGET)))
# CC= aarch64-linux-gnu-gcc --sysroot=$(HOME)/php/ARM64
  override CFLAGS += -DIR_TARGET_AARCH64
  override BUILD_CFLAGS += -DIR_TARGET_AARCH64
  DASM_ARCH  = aarch64
  DASM_FLAGS = -M
else
 $(error Unsupported target. TRGET must be 'x86_64', 'x86' or 'aarch64')
endif

ifeq (FreeBSD, $(OS))
  CC=cc
  BUILD_CC=$(CC)
  override CFLAGS += -I/usr/local/include
  override BUILD_CFLAGS += -I/usr/local/include
  LDFLAGS += -L/usr/local/lib
else ifeq (NetBSD, $(OS))
  CC=cc
  BUILD_CC=$(CC)
  override CFLAGS += -I/usr/pkg/include
  override BUILD_CFLAGS += -I/usr/pkg/include
  LDFLAGS = -L/usr/pkg/lib -Wl,-rpath,/usr/pkg/lib -lm
endif

ifeq (yes, $(HAVE_LLVM))
  override CFLAGS += -DHAVE_LLVM
  LLVM_OBJS=$(BUILD_DIR)/ir_load_llvm.o
  LLVM_LIBS=-lLLVM
else ifeq (no, $(HAVE_LLVM))
else
 $(error HAVE_LLVM must be 'yes' or 'no')
endif

OBJS_COMMON = $(BUILD_DIR)/ir.o $(BUILD_DIR)/ir_strtab.o $(BUILD_DIR)/ir_cfg.o \
	$(BUILD_DIR)/ir_sccp.o $(BUILD_DIR)/ir_gcm.o $(BUILD_DIR)/ir_ra.o $(BUILD_DIR)/ir_emit.o \
	$(BUILD_DIR)/ir_load.o $(BUILD_DIR)/ir_save.o $(BUILD_DIR)/ir_emit_c.o $(BUILD_DIR)/ir_dump.o \
	$(BUILD_DIR)/ir_disasm.o $(BUILD_DIR)/ir_gdb.o $(BUILD_DIR)/ir_perf.o $(BUILD_DIR)/ir_check.o \
	$(BUILD_DIR)/ir_cpuinfo.o $(BUILD_DIR)/ir_emit_llvm.o $(BUILD_DIR)/ir_mem2ssa.o
OBJS_IR = $(BUILD_DIR)/ir_main.o $(LLVM_OBJS)

all: $(BUILD_DIR) $(BUILD_DIR)/ir $(BUILD_DIR)/tester

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/libir.a: $(OBJS_COMMON)
	ar r $@ $^

$(BUILD_DIR)/ir: $(OBJS_IR) $(BUILD_DIR)/libir.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LLVM_LIBS) -lcapstone

$(OBJS_COMMON): $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h

$(BUILD_DIR)/ir_main.o: $(SRC_DIR)/ir.h
$(BUILD_DIR)/ir.o: $(SRC_DIR)/ir_fold.h $(BUILD_DIR)/ir_fold_hash.h
$(BUILD_DIR)/ir_ra.o: $(SRC_DIR)/ir_$(DASM_ARCH).h
$(BUILD_DIR)/ir_emit.o: $(SRC_DIR)/ir_$(DASM_ARCH).h $(BUILD_DIR)/ir_emit_$(DASM_ARCH).h
$(BUILD_DIR)/ir_gdb.o: $(SRC_DIR)/ir_elf.h
$(BUILD_DIR)/ir_perf.o: $(SRC_DIR)/ir_elf.h
$(BUILD_DIR)/ir_disasm.o: $(SRC_DIR)/ir_elf.h
$(BUILD_DIR)/ir_load_llvm.o: $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h $(SRC_DIR)/ir_builder.h

$(SRC_DIR)/ir_load.c: $(SRC_DIR)/ir.g
	$(LLK) ir.g

$(BUILD_DIR)/ir_fold_hash.h: $(BUILD_DIR)/gen_ir_fold_hash $(SRC_DIR)/ir_fold.h $(SRC_DIR)/ir.h
	$(BUILD_DIR)/gen_ir_fold_hash < $(SRC_DIR)/ir_fold.h > $(BUILD_DIR)/ir_fold_hash.h
$(BUILD_DIR)/gen_ir_fold_hash: $(SRC_DIR)/gen_ir_fold_hash.c $(SRC_DIR)/ir_strtab.c $(SRC_DIR)/ir.h
	$(BUILD_CC) $(BUILD_CFLAGS) $(LDFALGS) -o $@ $<

$(BUILD_DIR)/minilua: $(SRC_DIR)/dynasm/minilua.c
	$(BUILD_CC) $(MINILUA_CFLAGS) $(SRC_DIR)/dynasm/minilua.c -lm -o $@
$(BUILD_DIR)/ir_emit_$(DASM_ARCH).h: $(SRC_DIR)/ir_$(DASM_ARCH).dasc $(SRC_DIR)/dynasm/*.lua $(BUILD_DIR)/minilua
	$(BUILD_DIR)/minilua $(SRC_DIR)/dynasm/dynasm.lua $(DASM_FLAGS) -o $@ $(SRC_DIR)/ir_$(DASM_ARCH).dasc

$(OBJS_COMMON) $(OBJS_IR): $(BUILD_DIR)/$(notdir %.o): $(SRC_DIR)/$(notdir %.c)
	$(CC) $(CFLAGS) -I$(BUILD_DIR) -o $@ -c $<

$(BUILD_DIR)/tester: $(SRC_DIR)/tools/tester.c
	$(CC) $(BUILD_CFLAGS) -o $@ $<

test: $(BUILD_DIR)/ir $(BUILD_DIR)/tester
	$(BUILD_DIR)/tester --test-cmd $(BUILD_DIR)/ir --target $(TARGET) --default-args "--save" \
		--test-extension ".irt" --code-extension ".ir" $(TESTS)

test-ci: $(BUILD_DIR)/ir $(BUILD_DIR)/tester
	$(BUILD_DIR)/tester --test-cmd $(BUILD_DIR)/ir --target $(TARGET) --default-args "--save" \
		--test-extension ".irt" --code-extension ".ir" --show-diff $(TESTS)

clean:
	rm -rf $(BUILD_DIR)/ir $(BUILD_DIR)/libir.a $(BUILD_DIR)/*.o \
	$(BUILD_DIR)/minilua $(BUILD_DIR)/ir_emit_$(DASM_ARCH).h \
	$(BUILD_DIR)/ir_fold_hash.h $(BUILD_DIR)/gen_ir_fold_hash \
	$(BUILD_DIR)/tester
	find $(SRC_DIR)/tests -type f -name '*.diff' -delete
	find $(SRC_DIR)/tests -type f -name '*.out' -delete
	find $(SRC_DIR)/tests -type f -name '*.exp' -delete
	find $(SRC_DIR)/tests -type f -name '*.ir' -delete

install: $(BUILD_DIR)/ir $(BUILD_DIR)/libir.a
	install -m a+rx $(BUILD_DIR)/ir $(PREFIX)/bin
	install -m a+r $(BUILD_DIR)/libir.a $(PREFIX)/lib
	install -m a+r $(SRC_DIR)/ir.h $(SRC_DIR)/ir_builder.h $(SRC_DIR)/ir_private.h \
		$(SRC_DIR)/ir_x86.h $(SRC_DIR)/ir_aarch64.h $(PREFIX)/include

uninstall:
	rm $(PREFIX)/bin/ir
	rm $(PREFIX)/lib/libir.a
	rm $(PREFIX)/include/ir.h
	rm $(PREFIX)/include/ir_builder.h
	rm $(PREFIX)/include/ir_private.h
	rm $(PREFIX)/include/ir_x86.h
	rm $(PREFIX)/include/ir_aarch64.h

FUZZ_DIR       = $(SRC_DIR)/fuzz
FUZZ_BUILD     = $(FUZZ_DIR)/build
FUZZ_CORPUS    = $(FUZZ_DIR)/corpus
FUZZ_CFLAGS    = -Wall -Wextra -Wno-unused-parameter -g -O1 -DIR_DEBUG=1
FUZZ_SANITIZE  = -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_FUZZER    = -fsanitize=fuzzer
FUZZ_CC        = clang

FUZZ_SRCS = $(FUZZ_DIR)/fuzz_ir.c $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h \
	$(SRC_DIR)/ir_main.c $(FUZZ_BUILD)/libir.a
FUZZ_COMPILE = $(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SANITIZE) $(FUZZ_FUZZER) -DFUZZ_LIBFUZZER \
	-I$(SRC_DIR) -I$(FUZZ_BUILD) -L$(FUZZ_BUILD)
FUZZ_LIBS = -lir -lcapstone -lm
FUZZ_OBJS = $(patsubst $(BUILD_DIR)/%,$(FUZZ_BUILD)/%,$(OBJS_COMMON))

$(FUZZ_BUILD):
	@mkdir -p $@

$(FUZZ_BUILD)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h | $(FUZZ_BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SANITIZE) $(FUZZ_FUZZER) -I$(BUILD_DIR) -o $@ -c $<

$(FUZZ_BUILD)/ir.o: $(SRC_DIR)/ir_fold.h $(BUILD_DIR)/ir_fold_hash.h
$(FUZZ_BUILD)/ir_ra.o: $(SRC_DIR)/ir_$(DASM_ARCH).h
$(FUZZ_BUILD)/ir_emit.o: $(SRC_DIR)/ir_$(DASM_ARCH).h $(BUILD_DIR)/ir_emit_$(DASM_ARCH).h
$(FUZZ_BUILD)/ir_gdb.o: $(SRC_DIR)/ir_elf.h
$(FUZZ_BUILD)/ir_perf.o: $(SRC_DIR)/ir_elf.h
$(FUZZ_BUILD)/ir_disasm.o: $(SRC_DIR)/ir_elf.h

$(FUZZ_BUILD)/libir.a: $(FUZZ_OBJS)
	ar r $@ $^

fuzz-load: $(FUZZ_SRCS)
	$(FUZZ_COMPILE) -DFUZZ_MODE_LOAD -o $(FUZZ_BUILD)/$@ $(FUZZ_DIR)/fuzz_ir.c $(FUZZ_LIBS)

fuzz-O0: $(FUZZ_SRCS)
	$(FUZZ_COMPILE) -DFUZZ_MODE_O0 -o $(FUZZ_BUILD)/$@ $(FUZZ_DIR)/fuzz_ir.c $(FUZZ_LIBS)

fuzz-O1: $(FUZZ_SRCS)
	$(FUZZ_COMPILE) -DFUZZ_MODE_O1 -o $(FUZZ_BUILD)/$@ $(FUZZ_DIR)/fuzz_ir.c $(FUZZ_LIBS)

fuzz-O2: $(FUZZ_SRCS)
	$(FUZZ_COMPILE) -DFUZZ_MODE_O2 -o $(FUZZ_BUILD)/$@ $(FUZZ_DIR)/fuzz_ir.c $(FUZZ_LIBS)

fuzz: fuzz-load fuzz-O0 fuzz-O1 fuzz-O2

fuzz-corpus:
	$(FUZZ_DIR)/make_corpus.sh $(SRC_DIR)/tests $(FUZZ_CORPUS)

fuzz-clean:
	rm -rf $(FUZZ_BUILD)

fuzz-corpus-clean:
	rm -rf $(FUZZ_CORPUS)

.PHONY: fuzz fuzz-load fuzz-O0 fuzz-O1 fuzz-O2 fuzz-corpus fuzz-clean fuzz-corpus-clean
