/*
 * IR - Lightweight JIT Compilation Framework
 * (Exmaples package)
 * Copyright (C) 2023 by IR project.
 * Authors: Tony Su <tao.su@intel.com>
 */

#include "ir.h"
#include "ir_builder.h"
#include <stdlib.h>

/* Doc: Equivalent C code here */
/*
 * int32_t myfunc(int32_t x, int32_t y) {
 *	return x - y;
 * }
 */

// IR function to compile to native code
// Do NOT change function signature
void gen_myfunc(ir_ctx *ctx)
{
	ir_START();
	ir_ref ret = ir_CONST_I32(0);
	ir_RETURN(ret);
}

/* Usage: custom and standard run_myfunc()
 *   If your IR function above supports parameters, e.g.
 *       int32_t myfunc(int32_t, int32_t);
 *   a customary run_myfunc() (see template below) should be implemented in
 *   order to pass in your own parameters and run the function.
 *
 *   Otherwise, your IR function is assumed to be parameter-less and
 *   return a value of uint32_t, just like
 *      uint32_t myfunc(void)
 *   In this case, simply remove the run_myfunc() template below and
 *   by default use standard run_myfunc() function provided by 'exmplfrm.h'.
 */
// define USE_STANDARD_RUN
#define USE_CUSTOM_RUN
typedef int32_t (*myfunc_t)(int32_t, int32_t);
void run_myfunc(myfunc_t func)
{
	if (func) {
		printf("42 - 24 = %d\n", ((myfunc_t)func)(42, 24));
	}
}

// Import example framework -
//   standard run_myfunc() and main() functions
#include "exmplfrm.h"
