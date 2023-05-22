/*
 * IR - Lightweight JIT Compilation Framework
 * (Exmaples package)
 * Copyright (C) 2023 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"
#include "ir_builder.h"
#include <stdlib.h>

/*
 * int32_t myfunc(int32_t x, int32_t y) {
 *	return x - y;
 * }
 */
typedef int32_t (*myfunc_t)(int32_t, int32_t);

void gen_myfunc(ir_ctx *ctx)
{
	ir_START();
	ir_ref x = ir_PARAM(IR_I32, "x", 1);
	ir_ref y = ir_PARAM(IR_I32, "y", 2);
	ir_ref cr = ir_SUB_I32(x, y);
	ir_RETURN(cr);
}

int main(int argc, char **argv)
{
	ir_ctx ctx = {0};

	ir_consistency_check();

	ir_init(&ctx, IR_FUNCTION, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);

	gen_myfunc(&ctx);

	ir_build_def_use_lists(&ctx);
	ir_build_cfg(&ctx);
	ir_match(&ctx);
	ir_assign_virtual_registers(&ctx);
	ir_compute_dessa_moves(&ctx);

	size_t size;
	void *entry = ir_emit_code(&ctx, &size);
	if (entry) {
		printf("42 - 24 = %d\n", ((myfunc_t)entry)(42, 24));
	}

	ir_free(&ctx);

	return 0;
}
