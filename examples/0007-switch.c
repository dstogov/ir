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
 * int32_t classify(int32_t x) {
 *	switch (x) {
 *		case 1:  return 10;
 *		case 2:  return 20;
 *		case 3:  return 30;
 *		default: return -1;
 *	}
 * }
 */
typedef int32_t (*classify_t)(int32_t);

void gen_classify(ir_ctx *ctx)
{
	ir_START();
	ir_ref x = ir_PARAM(IR_I32, "x", 1);

	ir_ref sw = ir_SWITCH(x);

	/* case 1 */
	ir_CASE_VAL(sw, ir_CONST_I32(1));
	ir_ref r1 = ir_COPY_I32(ir_CONST_I32(10));
	ir_ref end1 = ir_END();

	/* case 2 */
	ir_CASE_VAL(sw, ir_CONST_I32(2));
	ir_ref r2 = ir_COPY_I32(ir_CONST_I32(20));
	ir_ref end2 = ir_END();

	/* case 3 */
	ir_CASE_VAL(sw, ir_CONST_I32(3));
	ir_ref r3 = ir_COPY_I32(ir_CONST_I32(30));
	ir_ref end3 = ir_END();

	/* default */
	ir_CASE_DEFAULT(sw);
	ir_ref r4 = ir_COPY_I32(ir_CONST_I32(-1));
	ir_ref end4 = ir_END();

	/* merge all cases */
	ir_ref ends[] = {end1, end2, end3, end4};
	ir_MERGE_N(4, ends);
	ir_ref vals[] = {r1, r2, r3, r4};
	ir_ref result = ir_PHI_N(IR_I32, 4, vals);
	ir_RETURN(result);
}

int main(int argc, char **argv)
{
	ir_ctx ctx;

	ir_consistency_check();

	ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
	ctx.ret_type = IR_I32;

	gen_classify(&ctx);

	size_t size;
	void *entry = ir_jit_compile(&ctx, 2, &size);
	if (entry) {
		printf("classify(1) = %d\n", ((classify_t)entry)(1));
		printf("classify(2) = %d\n", ((classify_t)entry)(2));
		printf("classify(3) = %d\n", ((classify_t)entry)(3));
		printf("classify(4) = %d\n", ((classify_t)entry)(4));
		printf("classify(0) = %d\n", ((classify_t)entry)(0));
	}

	ir_free(&ctx);

	return 0;
}
