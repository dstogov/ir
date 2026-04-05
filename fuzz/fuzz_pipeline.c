/*
 * IR - Lightweight JIT Compilation Framework
 * (Fuzz harness for optimization pipeline)
 * Copyright (C) 2026 IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"
#include "ir_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifndef _WIN32
# include <unistd.h>
#else
# include <process.h>
#endif

/*
 * Intercept exit() from the parser. ir_load.c calls exit(2) on errors.
 */
static jmp_buf fuzz_exit_jmp;
static int fuzz_in_load = 0;

void exit(int status)
{
	if (fuzz_in_load) {
		longjmp(fuzz_exit_jmp, status ? status : -1);
	}
	_exit(status);
}

#ifndef _WIN32
void __assert_fail(const char *expr, const char *file,
	unsigned int line, const char *func)
{
	if (fuzz_in_load) {
		longjmp(fuzz_exit_jmp, -1);
	}
	fprintf(stderr, "Assertion failed: %s (%s:%u: %s)\n", expr, file, line, func);
	_exit(134);
}
#endif

const char *__asan_default_options(void) { return "detect_leaks=0"; }

static int fuzz_opt_level = 2;

static bool fuzz_func_init(ir_loader *loader, ir_ctx *ctx, const char *name)
{
	ir_init(ctx, loader->default_func_flags, 256, 1024);
	return 1;
}

static bool fuzz_func_process(ir_loader *loader, ir_ctx *ctx, const char *name)
{
	/* Determine return type from first RETURN instruction */
	ir_ref ref = ctx->ir_base[1].op1;
	while (ref) {
		ir_insn *insn = &ctx->ir_base[ref];
		if (insn->op == IR_RETURN) {
			ctx->ret_type = insn->op2 ? ctx->ir_base[insn->op2].type : IR_VOID;
			break;
		}
		ref = insn->op3;
	}

	/* Run full optimization pipeline based on opt_level */
	ir_build_def_use_lists(ctx);

	if (fuzz_opt_level > 0) {
		ir_mem2ssa(ctx);
	}
	if (fuzz_opt_level > 1) {
		ir_sccp(ctx);
	}

	ir_build_cfg(ctx);

	if (fuzz_opt_level > 0) {
		ir_build_dominators_tree(ctx);
		ir_find_loops(ctx);
		ir_gcm(ctx);
		ir_schedule(ctx);
	}

	/* Verify IR integrity after optimization */
	ir_check(ctx);

	ir_match(ctx);
	ir_assign_virtual_registers(ctx);

	if (fuzz_opt_level > 0) {
		ir_compute_live_ranges(ctx);
		ir_coalesce(ctx);
		ir_reg_alloc(ctx);
		ir_schedule_blocks(ctx);
	} else {
		ir_compute_dessa_moves(ctx);
	}

	return 1;
}

static bool fuzz_stub_true_sym(ir_loader *loader, const char *name, uint32_t flags) { return 1; }
static bool fuzz_stub_true_func(ir_loader *loader, const char *name,
	uint32_t flags, ir_type ret_type, uint32_t params_count, const uint8_t *param_types) { return 1; }
static bool fuzz_stub_true_data(ir_loader *loader, ir_type type, uint32_t count, const void *data) { return 1; }
static bool fuzz_stub_true_str(ir_loader *loader, const char *str, size_t len) { return 1; }
static bool fuzz_stub_true_pad(ir_loader *loader, size_t offset) { return 1; }
static bool fuzz_stub_true_ref(ir_loader *loader, ir_op op, const char *ref, uintptr_t offset) { return 1; }
static bool fuzz_stub_true_end(ir_loader *loader, uint32_t flags) { return 1; }
static bool fuzz_stub_true_sym_dcl(ir_loader *loader, const char *name, uint32_t flags, size_t size) { return 1; }
static void *fuzz_stub_null(ir_loader *loader, const char *name, uint32_t flags) { return NULL; }
static bool fuzz_stub_false(ir_loader *loader, const char *name) { return 0; }
static bool fuzz_stub_add(ir_loader *loader, const char *name, void *addr) { return 1; }

static void fuzz_init_loader(ir_loader *loader)
{
	memset(loader, 0, sizeof(*loader));
	loader->default_func_flags = IR_FUNCTION | IR_OPT_FOLDING | IR_OPT_CFG | IR_OPT_CODEGEN | IR_OPT_MEM2SSA;
	loader->external_sym_dcl   = fuzz_stub_true_sym;
	loader->external_func_dcl  = fuzz_stub_true_func;
	loader->forward_func_dcl   = fuzz_stub_true_func;
	loader->sym_dcl            = fuzz_stub_true_sym_dcl;
	loader->sym_data           = fuzz_stub_true_data;
	loader->sym_data_str       = fuzz_stub_true_str;
	loader->sym_data_pad       = fuzz_stub_true_pad;
	loader->sym_data_ref       = fuzz_stub_true_ref;
	loader->sym_data_end       = fuzz_stub_true_end;
	loader->func_init          = fuzz_func_init;
	loader->func_process       = fuzz_func_process;
	loader->resolve_sym_name   = fuzz_stub_null;
	loader->has_sym            = fuzz_stub_false;
	loader->add_sym            = fuzz_stub_add;
	loader->add_label          = fuzz_stub_add;
}

static FILE *fuzz_buf_to_file(const uint8_t *data, size_t size)
{
#ifndef _WIN32
	return fmemopen((void *)data, size, "rb");
#else
	FILE *f = tmpfile();
	if (!f) {
		return NULL;
	}
	if (size > 0) {
		fwrite(data, 1, size, f);
		rewind(f);
	}
	return f;
#endif
}

#ifdef FUZZ_LIBFUZZER

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	ir_loader loader;
	FILE *f;

	if (size > 1024 * 1024) {
		return 0;
	}

	/* First byte selects optimization level (0, 1, 2) */
	if (size < 1) {
		return 0;
	}
	fuzz_opt_level = data[0] % 3;
	data++;
	size--;

	f = fuzz_buf_to_file(data, size);
	if (!f) {
		return 0;
	}

	fuzz_init_loader(&loader);
	ir_loader_init();

	fuzz_in_load = 1;
	if (setjmp(fuzz_exit_jmp) == 0) {
		ir_load(&loader, f);
	}
	fuzz_in_load = 0;

	ir_loader_free();
	fclose(f);

	return 0;
}

#else
#error "Define FUZZ_LIBFUZZER"
#endif
