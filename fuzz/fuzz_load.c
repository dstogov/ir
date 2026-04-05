/*
 * IR - Lightweight JIT Compilation Framework
 * (Fuzz harness for ir_load - text IR parser)
 * Copyright (C) 2026 IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"
#include "ir_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifndef _WIN32
# include <unistd.h>
#else
# include <process.h>
#endif

/*
 * The generated parser ir_load.c calls exit(2) on parse errors.
 * To survive this during fuzzing, exit() is intercepted via setjmp/longjmp.
 */
static jmp_buf fuzz_exit_jmp;
static int fuzz_in_load = 0;

void exit(int status)
{
	if (fuzz_in_load) {
		longjmp(fuzz_exit_jmp, status ? status : -1);
	}
	_exit(status);
}

/*
 * Override __assert_fail so that assertions inside IR code don't kill
 * the fuzzer process.  Overriding abort() is not possible because glibc
 * calls its own internal abort() without going through the PLT.
 */
#ifndef _WIN32
void __assert_fail(const char *expr, const char *file,
	unsigned int line, const char *func)
{
	if (fuzz_in_load) {
		longjmp(fuzz_exit_jmp, -1);
	}
	fprintf(stderr, "Assertion failed: %s (%s:%u: %s)\n", expr, file, line, func);
	_exit(134);
}
#endif

/* Suppress leak detection, longjmp skips parser cleanup intentionally. */
const char *__lsan_default_suppressions(void) { return "leak:ir_"; }
const char *__asan_default_options(void) { return "detect_leaks=0"; }

/* Minimal loader callbacks */

static bool fuzz_func_init(ir_loader *loader, ir_ctx *ctx, const char *name)
{
	ir_init(ctx, loader->default_func_flags, 256, 1024);
	return 1;
}

static bool fuzz_func_process(ir_loader *loader, ir_ctx *ctx, const char *name)
{
	return 1;
}

static bool fuzz_external_sym_dcl(ir_loader *loader, const char *name, uint32_t flags)
{
	return 1;
}

static bool fuzz_external_func_dcl(ir_loader *loader, const char *name,
	uint32_t flags, ir_type ret_type, uint32_t params_count, const uint8_t *param_types)
{
	return 1;
}

static bool fuzz_forward_func_dcl(ir_loader *loader, const char *name,
	uint32_t flags, ir_type ret_type, uint32_t params_count, const uint8_t *param_types)
{
	return 1;
}

static bool fuzz_sym_dcl(ir_loader *loader, const char *name, uint32_t flags, size_t size)
{
	return 1;
}

static bool fuzz_sym_data(ir_loader *loader, ir_type type, uint32_t count, const void *data)
{
	return 1;
}

static bool fuzz_sym_data_str(ir_loader *loader, const char *str, size_t len)
{
	return 1;
}

static bool fuzz_sym_data_pad(ir_loader *loader, size_t offset)
{
	return 1;
}

static bool fuzz_sym_data_ref(ir_loader *loader, ir_op op, const char *ref, uintptr_t offset)
{
	return 1;
}

static bool fuzz_sym_data_end(ir_loader *loader, uint32_t flags)
{
	return 1;
}

static void *fuzz_resolve_sym_name(ir_loader *loader, const char *name, uint32_t flags)
{
	return NULL;
}

static bool fuzz_has_sym(ir_loader *loader, const char *name)
{
	return 0;
}

static bool fuzz_add_sym(ir_loader *loader, const char *name, void *addr)
{
	return 1;
}

static void fuzz_init_loader(ir_loader *loader)
{
	memset(loader, 0, sizeof(*loader));
	loader->default_func_flags = IR_FUNCTION | IR_OPT_FOLDING | IR_OPT_CFG;
	loader->external_sym_dcl   = fuzz_external_sym_dcl;
	loader->external_func_dcl  = fuzz_external_func_dcl;
	loader->forward_func_dcl   = fuzz_forward_func_dcl;
	loader->sym_dcl            = fuzz_sym_dcl;
	loader->sym_data           = fuzz_sym_data;
	loader->sym_data_str       = fuzz_sym_data_str;
	loader->sym_data_pad       = fuzz_sym_data_pad;
	loader->sym_data_ref       = fuzz_sym_data_ref;
	loader->sym_data_end       = fuzz_sym_data_end;
	loader->func_init          = fuzz_func_init;
	loader->func_process       = fuzz_func_process;
	loader->resolve_sym_name   = fuzz_resolve_sym_name;
	loader->has_sym            = fuzz_has_sym;
	loader->add_sym            = fuzz_add_sym;
	loader->add_label          = fuzz_add_sym;
}

/* Create a FILE* from a memory buffer. */
static FILE *fuzz_buf_to_file(const uint8_t *data, size_t size)
{
#ifndef _WIN32
	return fmemopen((void *)data, size, "rb");
#else
	FILE *f = tmpfile();
	if (!f) {
		return NULL;
	}
	if (size > 0) {
		fwrite(data, 1, size, f);
		rewind(f);
	}
	return f;
#endif
}

#ifdef FUZZ_LIBFUZZER

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	ir_loader loader;
	FILE *f;

	/* Limit input size to avoid timeouts on huge inputs */
	if (size > 1024 * 1024) {
		return 0;
	}

	f = fuzz_buf_to_file(data, size);
	if (!f) {
		return 0;
	}

	fuzz_init_loader(&loader);
	ir_loader_init();

	fuzz_in_load = 1;
	if (setjmp(fuzz_exit_jmp) == 0) {
		ir_load(&loader, f);
	}
	fuzz_in_load = 0;

	ir_loader_free();
	fclose(f);

	return 0;
}

#else
#error "Define FUZZ_LIBFUZZER"
#endif
