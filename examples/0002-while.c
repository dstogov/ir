/*
 * IR - Lightweight JIT Compilation Framework
 * (Examples package)
 * Copyright (C) 2023 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"
#include "ir_builder.h"
#include <stdlib.h>

/*
 * int32_t myfunc() {
 * 	int32_t i = 0;
 * 	while (i++ < 42);
 *	return i;
 * }
 */
typedef int32_t (*myfunc_t)(void);

void gen_myfunc(ir_ctx *ctx)
{
	/* Function entry start */
	ir_START();
	/* Declare loop counter. */
	ir_ref i = ir_COPY_I32(ir_CONST_I32(0));
	ir_ref loop = ir_LOOP_BEGIN(ir_END());
		ir_ref phi_i_1 = ir_PHI_2(IR_I32, i, IR_UNUSED);
		ir_ref i_2 = ir_ADD_I32(phi_i_1, ir_CONST_I32(1));
		ir_ref cond = ir_IF(ir_LT(phi_i_1, ir_CONST_I32(42)));
			ir_IF_TRUE(cond);
				/* close loop */
				ir_MERGE_SET_OP(loop, 2, ir_LOOP_END());
				ir_PHI_SET_OP(phi_i_1, 2, i_2);
			ir_IF_FALSE(cond);

	/* Function end */
	ir_RETURN(i_2);
}

int main(int argc, char **argv)
{
	ir_ctx ctx;

	ir_consistency_check();

	ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
	ctx.ret_type = IR_I32;

	gen_myfunc(&ctx);

	size_t size;
	void *entry = ir_jit_compile(&ctx, 2, &size);
	if (entry) {
		printf("%d\n", ((myfunc_t)entry)());
	}

	ir_free(&ctx);

	return 0;
}
