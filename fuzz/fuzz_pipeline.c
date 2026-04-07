/*
 * IR - Lightweight JIT Compilation Framework
 * (Fuzz harness for ir_load - text IR parser)
 * Copyright (C) 2026 IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "../ir_main.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifndef _WIN32
# include <unistd.h>
#else
# include <process.h>
#endif

#define TEST_FILENAME "fuzz-test.ir"

static jmp_buf fuzz_exit_jmp;
static int fuzz_in_load = 0;

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

/* Create a FILE* from a memory buffer. */
static bool fuzz_buf_to_file(const uint8_t *data, size_t size, const char *filename)
{
	FILE *f = fopen(filename, "w+");
	if (!f) {
		return 0;
	}
	if (size > 0) {
		fwrite(data, 1, size, f);
	}
	fclose(f);
	return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *argv[] = {"ir", "-O2", "--dump-size", TEST_FILENAME};

	/* Limit input size to avoid timeouts on huge inputs */
	if (size > 1024 * 1024) {
		return 0;
	}

	if (!fuzz_buf_to_file(data, size, TEST_FILENAME)) {
		return 0;
	}

	fuzz_in_load = 1;
	if (setjmp(fuzz_exit_jmp) == 0) {
		_fuzz_main(4, argv);
	}
	fuzz_in_load = 0;

	unlink(TEST_FILENAME);

	return 0;
}
