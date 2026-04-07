/*
 * IR - Lightweight JIT Compilation Framework
 * (Fuzz harness)
 * Copyright (C) 2026 IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 *
 * Single source for all fuzz targets. The mode is selected at compile
 * time via -DFUZZ_MODE_xxx which determines the argv passed to the
 * IR driver:
 *
 *   FUZZ_MODE_LOAD  -> -fsyntax-only  (parser only)
 *   FUZZ_MODE_O0    -> -O0 --dump-size
 *   FUZZ_MODE_O1    -> -O1 --dump-size
 *   FUZZ_MODE_O2    -> -O2 --dump-size
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

#if !defined(FUZZ_MODE_LOAD) && !defined(FUZZ_MODE_O0) && \
    !defined(FUZZ_MODE_O1) && !defined(FUZZ_MODE_O2)
# define FUZZ_MODE_LOAD
#endif

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
	char filename[64];

	snprintf(filename, sizeof(filename), "fuzz-test-%d.ir", getpid());

#if defined(FUZZ_MODE_LOAD)
	char *argv[] = {"ir", "-fsyntax-only", filename};
	int argc = 3;
#elif defined(FUZZ_MODE_O0)
	char *argv[] = {"ir", "-O0", "--dump-size", filename};
	int argc = 4;
#elif defined(FUZZ_MODE_O1)
	char *argv[] = {"ir", "-O1", "--dump-size", filename};
	int argc = 4;
#elif defined(FUZZ_MODE_O2)
	char *argv[] = {"ir", "-O2", "--dump-size", filename};
	int argc = 4;
#else
# error "Unknown FUZZ_MODE"
#endif

	/* Limit input size to avoid timeouts on huge inputs */
	if (size > 1024 * 1024) {
		return 0;
	}

	if (!fuzz_buf_to_file(data, size, filename)) {
		return 0;
	}

	fuzz_in_load = 1;
	if (setjmp(fuzz_exit_jmp) == 0) {
		_fuzz_main(argc, argv);
	}
	fuzz_in_load = 0;

	unlink(filename);

	return 0;
}
