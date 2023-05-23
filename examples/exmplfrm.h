/*
 * IR - Lightweight JIT Compilation Framework
 * (Exmaples package)
 * Copyright (C) 2023 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 *          Tony Su <tao.su@intel.com>
 */

#ifndef USE_CUSTOM_RUN
typedef uint32_t (*myfunc_t)(void);

/* Standard run() function - run IR function without parameters  */
void run_myfunc(myfunc_t func)
{
    if (func) {
        printf("IR func returned: %d\n", func());
    }
}
#endif /* USE_CUSTOM_RUN */

/* Pointing to standard or customary run_myfunc() function. */
void (*p_func)(myfunc_t) = run_myfunc;

int main(int argc, char **argv)
{
	ir_ctx ctx = {0};

	ir_consistency_check();

	ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, 256, 1024);

	gen_myfunc(&ctx);

	size_t size;
	void *entry = ir_jit_compile(&ctx, 2, &size);
	if (entry) {
		p_func(entry);
	}

	ir_free(&ctx);

	return 0;
}

