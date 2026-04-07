/*
 * IR - Lightweight JIT Compilation Framework
 * (Examples package)
 * Copyright (C) 2026 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"
#include "ir_builder.h"
#include <stdlib.h>

/*
 * int64_t factorial(int64_t n, uintptr_t self) {
 *	if (n <= 1) return 1;
 *	return n * self(n - 1, self);
 * }
 *
 * The function receives its own address as a parameter to
 * recurse without any C globals or trampolines.
 */
typedef int64_t (*factorial_t)(int64_t, uintptr_t);

void gen_factorial(ir_ctx *ctx)
{
	ir_START();
	ir_ref n = ir_PARAM(IR_I64, "n", 1);
	ir_ref self = ir_PARAM(IR_ADDR, "self", 2);

	/* if (n <= 1) */
	ir_ref cond = ir_IF(ir_LE(n, ir_CONST_I64(1)));

	/* true branch: return 1 */
	ir_IF_TRUE(cond);
		ir_RETURN(ir_CONST_I64(1));

	/* false branch: return n * self(n - 1, self) */
	ir_IF_FALSE(cond);
		ir_ref n_minus_1 = ir_SUB_I64(n, ir_CONST_I64(1));
		ir_ref rec = ir_CALL_2(IR_I64, self, n_minus_1, self);
		ir_ref result = ir_MUL_I64(n, rec);
		ir_RETURN(result);
}

int main(int argc, char **argv)
{
	ir_ctx ctx;

	ir_consistency_check();

	ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
	ctx.ret_type = IR_I64;

	gen_factorial(&ctx);

	size_t size;
	void *entry = ir_jit_compile(&ctx, 2, &size);

	if (entry) {
		factorial_t factorial = (factorial_t)entry;
		printf("factorial(1)  = %ld\n", factorial(1, (uintptr_t)entry));
		printf("factorial(5)  = %ld\n", factorial(5, (uintptr_t)entry));
		printf("factorial(10) = %ld\n", factorial(10, (uintptr_t)entry));
		printf("factorial(20) = %ld\n", factorial(20, (uintptr_t)entry));
	}

	ir_free(&ctx);

	return 0;
}
