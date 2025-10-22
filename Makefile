# TARGET may be "x86_64" or "x86" or "aarch64"
TARGET     = x86_64
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
# CC= aarch64-linux-gnu-gcc --sysroot=$(HOME)/php/ARM64
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
