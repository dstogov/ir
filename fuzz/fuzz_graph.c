/*
 * IR - Lightweight JIT Compilation Framework
 * (Structured graph fuzz harness)
 * Copyright (C) 2026 IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 *
 * Decodes the input blob into a structurally valid IR function and runs
 * it through the optimization and code generation pipeline. The
 * optimization level is selected at compile time:
 *
 *   FUZZ_GRAPH_O0 -> ir_jit_compile(ctx, 0, ...)
 *   FUZZ_GRAPH_O1 -> ir_jit_compile(ctx, 1, ...)
 *   FUZZ_GRAPH_O2 -> ir_jit_compile(ctx, 2, ...)
 */

#include "ir.h"
#include "ir_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#ifndef _WIN32
# include <unistd.h>
#endif

#define FUZZ_CORPUS_DIR "corpus/graph"
#include "fuzz_corpus.h"

#if !defined(FUZZ_GRAPH_O0) && !defined(FUZZ_GRAPH_O1) && !defined(FUZZ_GRAPH_O2)
# define FUZZ_GRAPH_O2
#endif

#if defined(FUZZ_GRAPH_O0)
# define FUZZ_OPT_LEVEL 0
#elif defined(FUZZ_GRAPH_O1)
# define FUZZ_OPT_LEVEL 1
#else
# define FUZZ_OPT_LEVEL 2
#endif

static jmp_buf fuzz_exit_jmp;
static int fuzz_active = 0;

/*
 * Override __assert_fail so assertions inside IR code do not kill the
 * fuzzer process. Overriding abort() does not work because glibc calls
 * its own internal abort() without going through the PLT.
 */
#ifndef _WIN32
void __assert_fail(const char *expr, const char *file,
	unsigned int line, const char *func)
{
	if (fuzz_active) {
		longjmp(fuzz_exit_jmp, -1);
	}
	fprintf(stderr, "Assertion failed: %s (%s:%u: %s)\n", expr, file, line, func);
	_exit(134);
}
#endif

/* The longjmp on assertion skips pipeline cleanup, so ignore leaks. */
const char *__lsan_default_suppressions(void) { return "leak:ir_"; }
const char *__asan_default_options(void) { return "detect_leaks=0"; }

static const ir_type fuzz_int_types[] = {
	IR_U8, IR_U16, IR_U32, IR_U64,
	IR_I8, IR_I16, IR_I32, IR_I64
};
static const ir_type fuzz_fp_types[] = {
	IR_DOUBLE, IR_FLOAT
};

static const ir_op fuzz_int_bin[] = {
	IR_ADD, IR_SUB, IR_MUL, IR_OR, IR_AND, IR_XOR,
	IR_SHL, IR_SHR, IR_SAR, IR_MIN, IR_MAX
};
static const ir_op fuzz_int_un[] = {
	IR_NEG, IR_NOT
};
static const ir_op fuzz_fp_bin[] = {
	IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MIN, IR_MAX
};
static const ir_op fuzz_fp_un[] = {
	IR_NEG, IR_ABS
};

/* Comparisons used to build branch conditions, valid for int and fp. */
static const ir_op fuzz_cmp[] = {
	IR_EQ, IR_NE, IR_LT, IR_GE, IR_LE, IR_GT
};

#define FUZZ_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define FUZZ_MAX_NODES   4096
#define FUZZ_MAX_PARAMS  4

typedef struct {
	const uint8_t *p;
	size_t         len;
	size_t         pos;
} fuzz_cursor;

static uint8_t fuzz_u8(fuzz_cursor *c)
{
	if (c->pos >= c->len) {
		return 0;
	}
	return c->p[c->pos++];
}

static int fuzz_eof(const fuzz_cursor *c)
{
	return c->pos >= c->len;
}

/*
 * Decode the blob into a valid function:
 *   START
 *   parameters and two seed constants
 *   a sequence of type preserving data ops over earlier nodes with
 *   occasional if/else diamonds that merge through a PHI and simple
 *   natural loops driven by an induction PHI
 *   RETURN of the last value
 */
static void fuzz_build(ir_ctx *ctx, fuzz_cursor *c)
{
	ir_ref pool[FUZZ_MAX_NODES];
	uint32_t count = 0;
	bool is_fp;
	ir_type wtype;
	uint8_t b0 = fuzz_u8(c);
	uint8_t nparams;

	if (b0 & 0x80) {
		wtype = fuzz_fp_types[(b0 & 0x7f) % FUZZ_ARRAY_LEN(fuzz_fp_types)];
		is_fp = 1;
	} else {
		wtype = fuzz_int_types[(b0 & 0x7f) % FUZZ_ARRAY_LEN(fuzz_int_types)];
		is_fp = 0;
	}

	ctx->ret_type = wtype;
	_ir_START(ctx);

	nparams = (uint8_t)((fuzz_u8(c) % FUZZ_MAX_PARAMS) + 1);
	for (uint8_t i = 0; i < nparams; i++) {
		char name[8];
		snprintf(name, sizeof(name), "p%u", i);
		pool[count++] = _ir_PARAM(ctx, wtype, name, i + 1);
	}

	for (int i = 0; i < 2; i++) {
		ir_val val;
		val.u64 = 0;
		for (int b = 0; b < 8; b++) {
			val.u64 = (val.u64 << 8) | fuzz_u8(c);
		}
		if (wtype == IR_FLOAT) {
			val.f = (float)(int64_t)val.u64;
		} else if (wtype == IR_DOUBLE) {
			val.d = (double)(int64_t)val.u64;
		}
		pool[count++] = ir_const(ctx, val, wtype);
	}

	while (!fuzz_eof(c) && count < FUZZ_MAX_NODES) {
		uint8_t op_sel = fuzz_u8(c);
		uint8_t s1 = fuzz_u8(c);
		uint8_t s2 = fuzz_u8(c);
		bool unary = (op_sel & 0x80) != 0;
		bool branch = (op_sel & 0x40) != 0;
		bool loop = (op_sel & 0x20) != 0;
		ir_ref a, r;

		a = pool[s1 % count];

		if (branch) {
			/*
			 * Emit an if/else diamond and merge the two sides with a
			 * PHI of the working type. The condition compares two pool
			 * values, and each side feeds an existing value into the
			 * PHI. Data ops float, so back-refs dominate both sides.
			 */
			ir_ref b = pool[s2 % count];
			ir_op cmp = fuzz_cmp[(op_sel & 0x3f) % FUZZ_ARRAY_LEN(fuzz_cmp)];
			ir_ref cond = ir_fold2(ctx, IR_OPT(cmp, IR_BOOL), a, b);
			ir_ref if_ref = _ir_IF(ctx, cond);
			ir_ref t_end, f_end;

			_ir_IF_TRUE(ctx, if_ref);
			t_end = _ir_END(ctx);
			_ir_IF_FALSE(ctx, if_ref);
			f_end = _ir_END(ctx);
			_ir_MERGE_2(ctx, t_end, f_end);
			r = _ir_PHI_2(ctx, wtype, a, b);
		} else if (loop) {
			/*
			 * Emit a simple natural loop. An induction PHI starts from
			 * a pool value, the body advances it with a binary op, and
			 * a comparison decides whether to take the back edge. The
			 * code is never executed, so the trip count is irrelevant,
			 * only that the loop is a valid reducible region. After the
			 * exit the advanced value is available to later records.
			 */
			ir_ref step = pool[s2 % count];
			ir_op body_op = is_fp
				? fuzz_fp_bin[(op_sel & 0x1f) % FUZZ_ARRAY_LEN(fuzz_fp_bin)]
				: fuzz_int_bin[(op_sel & 0x1f) % FUZZ_ARRAY_LEN(fuzz_int_bin)];
			ir_op cmp = fuzz_cmp[(uint8_t)(s1 ^ s2) % FUZZ_ARRAY_LEN(fuzz_cmp)];
			ir_ref loop_ref, iv, next, cond, if_ref, loop_end;

			loop_ref = _ir_LOOP_BEGIN(ctx, _ir_END(ctx));
			iv = _ir_PHI_2(ctx, wtype, a, IR_UNUSED);
			next = ir_fold2(ctx, IR_OPT(body_op, wtype), iv, step);
			cond = ir_fold2(ctx, IR_OPT(cmp, IR_BOOL), next, step);

			if_ref = _ir_IF(ctx, cond);
			_ir_IF_TRUE(ctx, if_ref);
			loop_end = _ir_LOOP_END(ctx);
			_ir_IF_FALSE(ctx, if_ref);

			_ir_MERGE_SET_OP(ctx, loop_ref, 2, loop_end);
			_ir_PHI_SET_OP(ctx, iv, 2, next);
			r = next;
		} else if (unary) {
			ir_op op = is_fp
				? fuzz_fp_un[(op_sel & 0x7f) % FUZZ_ARRAY_LEN(fuzz_fp_un)]
				: fuzz_int_un[(op_sel & 0x7f) % FUZZ_ARRAY_LEN(fuzz_int_un)];
			r = ir_fold1(ctx, IR_OPT(op, wtype), a);
		} else {
			ir_ref b = pool[s2 % count];
			ir_op op = is_fp
				? fuzz_fp_bin[(op_sel & 0x7f) % FUZZ_ARRAY_LEN(fuzz_fp_bin)]
				: fuzz_int_bin[(op_sel & 0x7f) % FUZZ_ARRAY_LEN(fuzz_int_bin)];
			r = ir_fold2(ctx, IR_OPT(op, wtype), a, b);
		}

		pool[count++] = r;
	}

	_ir_RETURN(ctx, pool[count - 1]);
}

static void fuzz_run(ir_ctx *ctx, const uint8_t *data, size_t size)
{
	fuzz_cursor c = { data, size, 0 };
	size_t code_size = 0;
	void *entry;

	fuzz_build(ctx, &c);

	entry = ir_jit_compile(ctx, FUZZ_OPT_LEVEL, &code_size);
	if (entry) {
		ir_mem_unmap(entry, code_size);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	ir_ctx ctx;
	uint32_t flags = IR_FUNCTION;

	if (size < 2 || size > 64 * 1024) {
		return 0;
	}

#if FUZZ_OPT_LEVEL > 0
	flags |= IR_OPT_FOLDING;
#endif

	ir_init(&ctx, flags, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);

	fuzz_active = 1;
	if (setjmp(fuzz_exit_jmp) == 0) {
		fuzz_run(&ctx, data, size);
	}
	fuzz_active = 0;

	ir_free(&ctx);

	return 0;
}
