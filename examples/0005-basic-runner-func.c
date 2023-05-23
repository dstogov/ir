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
 * double myfunc(double x, double y) {
 *	return x - y + .3;
 * }
 */
void gen_myfunc(ir_ctx *ctx)
{
	ir_START();
	ir_ref x = ir_PARAM(IR_DOUBLE, "x", 1);
	ir_ref y = ir_PARAM(IR_DOUBLE, "y", 2);
	ir_ref cr0 = ir_SUB_D(x, y);

	ir_ref cd = ir_CONST_DOUBLE(.3);
	cr0 = ir_ADD_D(cr0, cd);

	ir_RETURN(cr0);
}

#define USE_CUSTOM_RUN
typedef double (*myfunc_t)(double, double);
void run_myfunc(myfunc_t func)
{
	const double N = 2;
	double x, y;
	for (y = -N; y < N; y++) {
		for (x = -N; x < N; x++) {
			double ret = func(x, y);
			printf("%4.1f - %4.1f + 0.3 = %4.1f\n", x, y, ret);
		}
	}
}

/* main() */
#include "exmplfrm.h"
