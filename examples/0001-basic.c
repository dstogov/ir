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

void gen_myfunc(ir_ctx *ctx)
{
	ir_START();
	ir_ref x = ir_PARAM(IR_I32, "x", 1);
	ir_ref y = ir_PARAM(IR_I32, "y", 2);
	ir_ref cr = ir_SUB_I32(x, y);
	ir_RETURN(cr);
}

#define USE_CUSTOM_RUN
typedef int32_t (*myfunc_t)(int32_t, int32_t);
void run_myfunc(myfunc_t func)
{
	if (func) {
		printf("42 - 24 = %d\n", ((myfunc_t)func)(42, 24));
	}
}

#include "exmplfrm.h"
