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
 * void myfunc(uint32_t *i) {
 *	(*i)++;
 * }
 */
typedef uint32_t (*myfunc_t)(uint32_t*);

void gen_myfunc(ir_ctx *ctx)
{
	/* Function entry start */
	ir_START();
	/* Declare function parameters */
	ir_ref i_ptr = ir_PARAM(IR_U32, "i", 1);

	/* Dereference the argument pointer value, increment and store back. */
	ir_ref i_val = ir_LOAD_U32(i_ptr);
	ir_ref i_inc_val = ir_ADD_U32(i_val, ir_CONST_U32(1));
	ir_STORE(i_ptr, i_inc_val);

	/* Function end, declared void */
	ir_RETURN(IR_UNUSED);
}

int main(int argc, char **argv)
{
	ir_ctx ctx;

	ir_consistency_check();

	ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);

	gen_myfunc(&ctx);

	size_t size;
	void *entry = ir_jit_compile(&ctx, 2, &size);
	if (entry) {
		uint32_t i = 42;
		printf("i=%u ", i);
		((myfunc_t)entry)(&i);
		printf("i=%u\n", i);
	}

	ir_free(&ctx);

	return 0;
}
