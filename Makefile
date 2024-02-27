# TARGET may be "x86_64" or "x86" or "aarch64"
TARGET     = x86_64
OS         = $(shell uname -s)
# BUILD can be "debug" or "release"
BUILD      = debug
BUILD_DIR  = .
SRC_DIR    = .
HAVE_LLVM  = no
EXAMPLES_SRC_DIR = $(SRC_DIR)/examples
EXAMPLES_BUILD_DIR = $(BUILD_DIR)/examples

CC         = gcc
BUILD_CC   = gcc
override CFLAGS += -Wall -Wextra -Wno-unused-parameter
override BUILD_CFLAGS += -Wall -Wextra -Wno-unused-parameter
LDFLAGS    = -lm -ldl
PHP        = php
LLK        = llk
#LLK        = $(PHP) $(HOME)/php/llk/llk.php

ifeq (debug, $(BUILD))
 override CFLAGS += -O0 -g -DIR_DEBUG=1
 override BUILD_CFLAGS += -O2 -g -DIR_DEBUG=1
 MINILUA_CFLAGS = -O0 -g
endif
ifeq (release, $(BUILD))
 override CFLAGS += -O2 -g
 override BUILD_CFLAGS += -O2 -g
 MINILUA_CFLAGS = -O2 -g
endif

ifeq (x86_64, $(TARGET))
  override CFLAGS += -m64 -DIR_TARGET_X64
  override BUILD_CFLAGS += -m64 -DIR_TARGET_X64
  DASM_ARCH  = x86
  DASM_FLAGS = -M -D X64=1
endif
ifeq (x86, $(TARGET))
  override CFLAGS += -m32 -DIR_TARGET_X86
  override BUILD_CFLAGS += -m32 -DIR_TARGET_X86
  DASM_ARCH  = x86
  DASM_FLAGS = -M
endif
ifeq (aarch64, $(TARGET))
  CC= aarch64-linux-gnu-gcc --sysroot=$(HOME)/php/ARM64
  override CFLAGS += -DIR_TARGET_AARCH64
  override BUILD_CFLAGS += -DIR_TARGET_AARCH64
  DASM_ARCH  = aarch64
  DASM_FLAGS = -M
endif
ifeq (FreeBSD, $(OS))
  CC=cc
  BUILD_CC=$(CC)
  override CFLAGS += -I/usr/local/include
  override BUILD_CFLAGS += -I/usr/local/include
  LDFLAGS += -L/usr/local/lib
endif

ifeq (NetBSD, $(OS))
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
endif

OBJS_COMMON = $(BUILD_DIR)/ir.o $(BUILD_DIR)/ir_strtab.o $(BUILD_DIR)/ir_cfg.o \
	$(BUILD_DIR)/ir_sccp.o $(BUILD_DIR)/ir_gcm.o $(BUILD_DIR)/ir_ra.o $(BUILD_DIR)/ir_emit.o \
	$(BUILD_DIR)/ir_load.o $(BUILD_DIR)/ir_save.o $(BUILD_DIR)/ir_emit_c.o $(BUILD_DIR)/ir_dump.o \
	$(BUILD_DIR)/ir_disasm.o $(BUILD_DIR)/ir_gdb.o $(BUILD_DIR)/ir_perf.o $(BUILD_DIR)/ir_check.o \
	$(BUILD_DIR)/ir_cpuinfo.o $(BUILD_DIR)/ir_emit_llvm.o
OBJS_IR = $(BUILD_DIR)/ir_main.o $(LLVM_OBJS)
EXAMPLE_EXES = $(EXAMPLES_BUILD_DIR)/mandelbrot \
	$(EXAMPLES_BUILD_DIR)/0001-basic \
	$(EXAMPLES_BUILD_DIR)/0002-while \
	$(EXAMPLES_BUILD_DIR)/0003-pointer \
	$(EXAMPLES_BUILD_DIR)/0004-func \
	$(EXAMPLES_BUILD_DIR)/0005-basic-runner-func

all: $(BUILD_DIR) $(BUILD_DIR)/ir $(BUILD_DIR)/tester

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(EXAMPLES_BUILD_DIR):
	@mkdir -p $(EXAMPLES_BUILD_DIR)

$(BUILD_DIR)/ir: $(OBJS_COMMON) $(OBJS_IR)
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

$(EXAMPLE_EXES): $(EXAMPLES_BUILD_DIR)/$(notdir %): $(EXAMPLES_SRC_DIR)/$(notdir %.c)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $< -o $@ $(OBJS_COMMON) $(LDFLAGS) -lcapstone

$(BUILD_DIR)/tester: $(SRC_DIR)/tools/tester.c
	$(CC) $(BUILD_CFLAGS) -o $@ $<

test: $(BUILD_DIR)/ir $(BUILD_DIR)/tester
	$(BUILD_DIR)/tester --test-cmd $(BUILD_DIR)/ir --target $(TARGET) --default-args "--save" \
		--test-extension ".irt" --code-extension ".ir" $(SRC_DIR)/tests

test-ci: $(BUILD_DIR)/ir $(BUILD_DIR)/tester
	$(BUILD_DIR)/tester --test-cmd $(BUILD_DIR)/ir --target $(TARGET) --default-args "--save" \
		--test-extension ".irt" --code-extension ".ir" --show-diff $(SRC_DIR)/tests

examples: $(OBJS_COMMON) $(EXAMPLES_BUILD_DIR) $(EXAMPLE_EXES)

clean:
	rm -rf $(BUILD_DIR)/ir $(BUILD_DIR)/*.o \
	$(BUILD_DIR)/minilua $(BUILD_DIR)/ir_emit_$(DASM_ARCH).h \
	$(BUILD_DIR)/ir_fold_hash.h $(BUILD_DIR)/gen_ir_fold_hash \
	$(EXAMPLE_EXES) \
	$(BUILD_DIR)/tester
	find $(SRC_DIR)/tests -type f -name '*.diff' -delete
	find $(SRC_DIR)/tests -type f -name '*.out' -delete
	find $(SRC_DIR)/tests -type f -name '*.exp' -delete
	find $(SRC_DIR)/tests -type f -name '*.ir' -delete
