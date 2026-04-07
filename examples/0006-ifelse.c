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
 * int32_t abs_diff(int32_t x, int32_t y) {
 *	if (x >= y)
 *		return x - y;
 *	else
 *		return y - x;
 * }
 */
typedef int32_t (*abs_diff_t)(int32_t, int32_t);

void gen_abs_diff(ir_ctx *ctx)
{
	ir_START();
	ir_ref x = ir_PARAM(IR_I32, "x", 1);
	ir_ref y = ir_PARAM(IR_I32, "y", 2);

	/* if (x >= y) */
	ir_ref cond = ir_IF(ir_GE(x, y));

	ir_IF_TRUE(cond);
		ir_ref r1 = ir_SUB_I32(x, y);
		ir_ref end1 = ir_END();

	ir_IF_FALSE(cond);
		ir_ref r2 = ir_SUB_I32(y, x);
		ir_ref end2 = ir_END();

	/* merge both branches */
	ir_MERGE_2(end1, end2);
	ir_ref result = ir_PHI_2(IR_I32, r1, r2);
	ir_RETURN(result);
}

int main(int argc, char **argv)
{
	ir_ctx ctx;

	ir_consistency_check();

	ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
	ctx.ret_type = IR_I32;

	gen_abs_diff(&ctx);

	size_t size;
	void *entry = ir_jit_compile(&ctx, 2, &size);
	if (entry) {
		printf("abs_diff(10, 3) = %d\n", ((abs_diff_t)entry)(10, 3));
		printf("abs_diff(3, 10) = %d\n", ((abs_diff_t)entry)(3, 10));
		printf("abs_diff(5, 5) = %d\n", ((abs_diff_t)entry)(5, 5));
	}

	ir_free(&ctx);

	return 0;
}
