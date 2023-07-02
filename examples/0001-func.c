/*
 * IR - Lightweight JIT Compilation Framework
 * (Exmaples package)
 * Copyright (C) 2023 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"
#include "ir_builder.h"
#include <stdlib.h>

#include <stdio.h>

uint8_t hello(void)
{
	printf("I'm Hello\n");
	return 42;
}

/*
 * uint8_t myfunc() {
 * 	return hello();
 * }
 */
typedef uint8_t (*myfunc_t)();

void gen_myfunc(ir_ctx *ctx)
{
	/* Function entry start */
	ir_START();

	/* Load function address. */
	ir_ref addr = ir_CONST_ADDR(hello);
	/* Call function. */
	ir_ref ret = ir_CALL(IR_U8, addr);

	/* Function end, return value */
	ir_RETURN(ret);
}

int main(int argc, char **argv)
{
	ir_ctx ctx = {0};

	ir_consistency_check();

	ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);

	gen_myfunc(&ctx);

	size_t size;
	void *entry = ir_jit_compile(&ctx, 2, &size);
	if (entry) {
		uint8_t i = ((myfunc_t)entry)();
		printf("i=%u\n", i);
	}

	ir_free(&ctx);

	return 0;
}
