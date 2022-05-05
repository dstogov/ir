CC         = gcc
#CFLAGS     = -O2 -g -Wall -Wno-array-bounds -DIR_TARGET_X64
CFLAGS     = -O0 -g -Wall -DIR_DEBUG -DIR_TARGET_X64
LDFLAGS    = -lm
PHP        = php
LLK        = /home/dmitry/php/llk/llk.php
DASM_ARCH  = x86
DASM_FLAGS = -D X64=1

all: ir ir_test

ir: ir_main.o ir.o ir_strtab.o ir_cfg.o ir_sccp.o ir_gcm.o ir_ra.o ir_$(DASM_ARCH).o \
	ir_load.o ir_save.o ir_emit_c.o ir_dump.o ir_disasm.o ir_gdb.o ir_perf.o ir_check.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ -lcapstone $^

ir_test: ir_test.o ir.o ir_strtab.o ir_cfg.o ir_sccp.o ir_gcm.o ir_ra.o ir_$(DASM_ARCH).o \
	ir_save.o ir_dump.o ir_disasm.o ir_gdb.o ir_perf.o ir_check.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ -lcapstone $^

ir.o: ir.c ir.h ir_private.h ir_fold.h ir_fold_hash.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_cfg.o: ir_cfg.c ir.h ir_private.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_sccp.o: ir_sccp.c ir.h ir_private.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_gcm.o: ir_gcm.c ir.h ir_private.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_ra.o: ir_ra.c ir.h ir_private.h ir_$(DASM_ARCH).h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_strtab.o: ir_strtab.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_save.o: ir_save.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_load.o: ir_load.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_emit_c.o: ir_emit_c.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_dump.o: ir_dump.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_disasm.o: ir_disasm.c ir.h ir_private.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_gdb.o: ir_gdb.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_perf.o: ir_perf.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_check.o: ir_check.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_main.o: ir_main.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<
ir_test.o: ir_test.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<

ir_load.c: ir.g
	$(PHP) $(LLK) ir.g

ir_fold_hash.h: gen_ir_fold_hash ir_fold.h ir.h
	./gen_ir_fold_hash > ir_fold_hash.h
gen_ir_fold_hash: gen_ir_fold_hash.o ir_strtab.o
	$(CC) $(CFLAGS) $(LDFALGS) -o $@ $^
gen_ir_fold_hash.o: gen_ir_fold_hash.c ir.h
	$(CC) $(CFLAGS) -o $@ -c $<

minilua: dynasm/minilua.c
	$(CC) dynasm/minilua.c -lm -o $@
ir_$(DASM_ARCH).c: ir_$(DASM_ARCH).dasc minilua dynasm/*.lua
	./minilua dynasm/dynasm.lua $(DASM_FLAGS) -o $@ ir_$(DASM_ARCH).dasc
ir_$(DASM_ARCH).o: ir_$(DASM_ARCH).c ir.h ir_private.h ir_$(DASM_ARCH).h
	$(CC) $(CFLAGS) -o $@ -c $<

test: ir
	./ir test.ir --dump --save 2>2.log
	./ir test.ir --dot ir.dot
	dot -Tpdf ir.dot -o ir.pdf
	php ir-test.php

clean:
	rm -rf ir ir_test *.o \
	ir_load.c \
	minilua ir_$(DASM_ARCH).c \
	ir_fold_hash.h gen_ir_fold_hash \
	ir.dot ir.pdf 2.log \
	b perf.data perf.data.old perf.data.jitted \
	tests/*.diff tests/*.out tests/*.exp tests/*.ir \
	tests/x86_64/*.diff tests/x86_64/*.out tests/x86_64/*.exp tests/x86_64/*.ir \
	tests/c/*.diff tests/c/*.out tests/c/*.exp tests/c/*.ir
