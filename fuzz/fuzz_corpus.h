/*
 * IR - Lightweight JIT Compilation Framework
 * (Shared fuzz corpus helpers)
 * Copyright (C) 2026 IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#ifndef IR_FUZZ_CORPUS_H
#define IR_FUZZ_CORPUS_H

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <direct.h>
# define FUZZ_MKDIR(p) _mkdir(p)
#else
# include <sys/stat.h>
# define FUZZ_MKDIR(p) mkdir((p), 0755)
#endif

#ifndef FUZZ_CORPUS_DIR
# error "Define FUZZ_CORPUS_DIR before including fuzz_corpus.h"
#endif

/* Create each path component, ignoring already existing dirs. */
static void fuzz_mkpath(const char *path)
{
	char tmp[256];
	size_t len = strlen(path);
	char *p;

	if (len == 0 || len >= sizeof(tmp)) {
		return;
	}
	memcpy(tmp, path, len + 1);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			FUZZ_MKDIR(tmp);
			*p = '/';
		}
	}
	FUZZ_MKDIR(tmp);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	int i, n;
	char **na;

	for (i = 1; i < *argc; i++) {
		if ((*argv)[i][0] != '-') {
			return 0;
		}
	}

	fuzz_mkpath(FUZZ_CORPUS_DIR);

	n = *argc;
	na = (char **)malloc((size_t)(n + 2) * sizeof(*na));
	if (!na) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		na[i] = (*argv)[i];
	}
	na[n] = (char *)FUZZ_CORPUS_DIR;
	na[n + 1] = NULL;
	*argv = na;
	*argc = n + 1;

	return 0;
}

#endif /* IR_FUZZ_CORPUS_H */
