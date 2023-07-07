# BUILD can be "debug" or "release"
BUILD      = debug
BUILD_DIR  = .
SRC_DIR    = .
EXAMPLES_SRC_DIR = $(SRC_DIR)/examples
EXAMPLES_BUILD_DIR = $(BUILD_DIR)/examples

BUILD_CC   = gcc
CFLAGS     = -Wall -Wextra -Wno-unused-parameter
LDFLAGS    = -lm -ldl
PHP        = php
LLK        = llk
#LLK        = $(PHP) $(HOME)/php/llk/llk.php

ifeq (debug, $(BUILD))
 CFLAGS += -O0 -g -DIR_DEBUG=1
endif
ifeq (release, $(BUILD))
 CFLAGS += -O2 -g
endif

processor :=
ifndef CROSS_COMPILE
  processor := $(shell uname -m)
  CC  ?= gcc
  CXX ?= g++
  ifeq ($(strip $(processor)+$(TARGET)),x86_64+x86)
    check_x86 := $(shell echo | cpp -m32 -dM - | grep "__i386__" >/dev/null && echo 1)
    ifeq ($(check_x86),1)
      CFLAGS += -m32
      processor = i386
    else
      $(error "gcc-multilib is not available. Check the installation or specify with CROSS_COMPILE.")
    endif
  endif
else # CROSS_COMPILE was set
  CC  := $(CROSS_COMPILE)gcc
  CXX := $(CROSS_COMPILE)g++
  check_aarch64 := $(shell echo | $(CROSS_COMPILE)cpp -dM - | grep "__aarch64__" >/dev/null && echo 1)
  ifeq ($(check_aarch64),1)
    processor = aarch64
  endif
  check_x64 := $(shell echo | $(CROSS_COMPILE)cpp -dM - | grep "__x86_64__" >/dev/null && echo 1)
  ifeq ($(check_x64),1)
    processor = x86_64
  endif
  check_x86 := $(shell echo | $(CROSS_COMPILE)cpp -dM - | grep "__i386__" >/dev/null && echo 1)
  ifeq ($(check_x86),1)
    processor = i386
  endif
  ifdef SYSROOT
    CC += --sysroot=$(SYSROOT)
  endif
endif

# Follow platform-specific configurations
ifeq ($(processor),$(filter $(processor),aarch64 arm64))
  CFLAGS    += -D IR_TARGET_AARCH64
  DASM_ARCH  = aarch64
  DASM_FLAGS =
else ifeq ($(processor),$(filter $(processor),x86_64))
  CFLAGS    += -D IR_TARGET_X64
  DASM_ARCH  = x86
  DASM_FLAGS = -D X64=1
else ifeq ($(processor),$(filter $(processor),i386 i686))
  CFLAGS    += -D IR_TARGET_X86
  DASM_ARCH  = x86
  DASM_FLAGS =
else
  $(error Unsupported architecture)
endif

OBJS_COMMON = $(BUILD_DIR)/ir.o $(BUILD_DIR)/ir_strtab.o $(BUILD_DIR)/ir_cfg.o \
	$(BUILD_DIR)/ir_sccp.o $(BUILD_DIR)/ir_gcm.o $(BUILD_DIR)/ir_ra.o $(BUILD_DIR)/ir_emit.o \
	$(BUILD_DIR)/ir_load.o $(BUILD_DIR)/ir_save.o $(BUILD_DIR)/ir_emit_c.o $(BUILD_DIR)/ir_dump.o \
	$(BUILD_DIR)/ir_disasm.o $(BUILD_DIR)/ir_gdb.o $(BUILD_DIR)/ir_perf.o $(BUILD_DIR)/ir_check.o \
	$(BUILD_DIR)/ir_cpuinfo.o
OBJS_IR = $(BUILD_DIR)/ir_main.o
OBJS_IR_TEST = $(BUILD_DIR)/ir_test.o
EXAMPLE_EXES = $(EXAMPLES_BUILD_DIR)/0001-basic $(EXAMPLES_BUILD_DIR)/0001-while $(EXAMPLES_BUILD_DIR)/0005-basic-runner-func \
	       $(EXAMPLES_BUILD_DIR)/0001-pointer $(EXAMPLES_BUILD_DIR)/0001-func

all: $(BUILD_DIR) $(BUILD_DIR)/ir $(BUILD_DIR)/ir_test

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(EXAMPLES_BUILD_DIR):
	@mkdir -p $(EXAMPLES_BUILD_DIR)

$(BUILD_DIR)/ir: $(OBJS_COMMON) $(OBJS_IR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcapstone

$(BUILD_DIR)/ir_test: $(OBJS_COMMON) $(OBJS_IR_TEST)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcapstone

$(OBJS_COMMON): $(SRC_DIR)/ir.h $(SRC_DIR)/ir_private.h

$(BUILD_DIR)/ir_main.o: $(SRC_DIR)/ir.h
$(BUILD_DIR)/ir_test.o: $(SRC_DIR)/ir.h $(SRC_DIR)/ir_builder.h
$(BUILD_DIR)/ir.o: $(SRC_DIR)/ir_fold.h $(BUILD_DIR)/ir_fold_hash.h
$(BUILD_DIR)/ir_ra.o: $(SRC_DIR)/ir_$(DASM_ARCH).h
$(BUILD_DIR)/ir_emit.o: $(SRC_DIR)/ir_$(DASM_ARCH).h $(BUILD_DIR)/ir_emit_$(DASM_ARCH).h
$(BUILD_DIR)/ir_gdb.o: $(SRC_DIR)/ir_elf.h
$(BUILD_DIR)/ir_perf.o: $(SRC_DIR)/ir_elf.h
$(BUILD_DIR)/ir_disasm.o: $(SRC_DIR)/ir_elf.h

$(SRC_DIR)/ir_load.c: $(SRC_DIR)/ir.g
	$(LLK) ir.g

$(BUILD_DIR)/ir_fold_hash.h: $(BUILD_DIR)/gen_ir_fold_hash $(SRC_DIR)/ir_fold.h $(SRC_DIR)/ir.h
	$(BUILD_DIR)/gen_ir_fold_hash < $(SRC_DIR)/ir_fold.h > $(BUILD_DIR)/ir_fold_hash.h
$(BUILD_DIR)/gen_ir_fold_hash: $(SRC_DIR)/gen_ir_fold_hash.c $(SRC_DIR)/ir_strtab.c $(SRC_DIR)/ir.h
	$(BUILD_CC) $(CFLAGS) $(LDFALGS) -o $@ $^

$(BUILD_DIR)/minilua: $(SRC_DIR)/dynasm/minilua.c
	$(BUILD_CC) $(SRC_DIR)/dynasm/minilua.c -lm -o $@
$(BUILD_DIR)/ir_emit_$(DASM_ARCH).h: $(SRC_DIR)/ir_$(DASM_ARCH).dasc $(SRC_DIR)/dynasm/*.lua $(BUILD_DIR)/minilua
	$(BUILD_DIR)/minilua $(SRC_DIR)/dynasm/dynasm.lua $(DASM_FLAGS) -o $@ $(SRC_DIR)/ir_$(DASM_ARCH).dasc

$(OBJS_COMMON) $(OBJS_IR) $(OBJS_IR_TEST): $(BUILD_DIR)/$(notdir %.o): $(SRC_DIR)/$(notdir %.c)
	$(CC) $(CFLAGS) -I$(BUILD_DIR) -o $@ -c $<

$(EXAMPLE_EXES): $(EXAMPLES_BUILD_DIR)/$(notdir %): $(EXAMPLES_SRC_DIR)/$(notdir %.c)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $< -o $@ $(OBJS_COMMON) $(LDFLAGS) -lcapstone

$(BUILD_DIR)/ir-test: $(SRC_DIR)/ir-test.cxx
	$(CXX) -O3 -std=c++17 $(SRC_DIR)/ir-test.cxx -o $(BUILD_DIR)/ir-test

test: $(BUILD_DIR)/ir $(BUILD_DIR)/ir-test
	$(BUILD_DIR)/ir $(SRC_DIR)/test.ir --dump --save 2>$(BUILD_DIR)/test.log
	$(BUILD_DIR)/ir $(SRC_DIR)/test.ir --dot $(BUILD_DIR)/ir.dot
	dot -Tpdf $(BUILD_DIR)/ir.dot -o $(BUILD_DIR)/ir.pdf
	$(BUILD_DIR)/ir-test $(SRC_DIR)/tests

test-ci: $(BUILD_DIR)/ir $(BUILD_DIR)/ir-test
	$(BUILD_DIR)/ir-test --show-diff $(SRC_DIR)/tests

examples: $(OBJS_COMMON) $(EXAMPLES_BUILD_DIR) $(EXAMPLE_EXES)

clean:
	rm -rf $(BUILD_DIR)/ir $(BUILD_DIR)/ir_test $(BUILD_DIR)/*.o \
	$(BUILD_DIR)/minilua $(BUILD_DIR)/ir_emit_$(DASM_ARCH).h \
	$(BUILD_DIR)/ir_fold_hash.h $(BUILD_DIR)/gen_ir_fold_hash \
	$(BUILD_DIR)/ir.dot $(BUILD_DIR)/ir.pdf $(BUILD_DIR)/test.log \
	$(BUILD_DIR)/ir-test
	find $(SRC_DIR)/tests -type f -name '*.diff' -delete
	find $(SRC_DIR)/tests -type f -name '*.out' -delete
	find $(SRC_DIR)/tests -type f -name '*.exp' -delete
	find $(SRC_DIR)/tests -type f -name '*.ir' -delete
