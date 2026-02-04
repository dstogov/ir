/*
 * IR - Lightweight JIT Compilation Framework
 * (Test runner implementation)
 * Copyright (C) 2023 by IR project.
 * Authors: Dmitry Stogov <dmitry@php.net>, Anatol Belski <anbelski@linux.microsoft.com>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#ifdef _WIN32
# include <io.h>
# include <windows.h>
# pragma warning(disable : 4996)
# define PATH_SEP '\\'
# define DEFAULT_DIFF_CMD "fc"
# define S_IRUSR _S_IREAD
# define S_IWUSR _S_IWRITE
#else
# include <sys/wait.h>
# include <dirent.h>
# include <unistd.h>
# include <signal.h>
# if defined(__linux__) || defined(__sun)
#  include <alloca.h>
# endif
# define PATH_SEP '/'
# define DEFAULT_DIFF_CMD "diff --strip-trailing-cr -u"
# define O_BINARY 0
#endif

typedef enum _color {GREEN, YELLOW, RED} color;

typedef struct _test {
	int   id;
	char *name;
	char *target;
	char *args;
	char *code;
	char *expect;
	char *xfail;
} test;

static int colorize = 1;

static const char *test_cmd = NULL;
static const char *target = NULL;
static const char *default_args = NULL;
static const char *additional_args = NULL;
static const char *diff_cmd = NULL;

static const char *test_extension = NULL;
static int test_extension_len = 0;

static const char *code_extension = NULL;
static int code_extension_len = 0;

static char **files = NULL;
static int files_count = 0;
static int files_limit = 0;

static void print_color(const char *s, color c)
{
	if (colorize) {
		switch (c) {
			case GREEN:
				printf("\x1b[1;32m%s\x1b[0m", s);
				return;
			case YELLOW:
				printf("\x1b[1;33m%s\x1b[0m", s);
				return;
			case RED:
				printf("\x1b[1;31m%s\x1b[0m", s);
				return;
		}
	}
	printf("%s", s);
}

static void init_console(void)
{
#ifdef _WIN32
	if (colorize) {
		DWORD mode, new_mode = ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		GetConsoleMode(h, &mode);
		SetConsoleMode(h, mode | new_mode);
	}
#endif
}

static test *parse_file(const char *filename, int id)
{
	test *t;
	char *buf, **section, **last_section = NULL;
	int fd;
	size_t start, i, size;
	struct stat stat_buf;

	if (stat(filename, &stat_buf) != 0) {
		return NULL;
	}
	size = stat_buf.st_size;
	fd = open(filename, O_RDONLY | O_BINARY, 0);
	if (fd < 0) {
		return NULL;
	}
	t = malloc(sizeof(test) + size + 1);
	if (!t) {
		return NULL;
	}
	buf = (char*)t + sizeof(test);
	if ((size_t)read(fd, buf, size) != size) {
		free(t);
		return NULL;
	}
	close(fd);
	memset(t, 0, sizeof(test));
	buf[size] = 0;
	i = 0;
	while (i < size) {
		start = i;
		while (i < size && buf[i] != '\r' && buf[i] != '\n') i++;
		if (i - start        == strlen("--TEST--")    && memcmp(buf + start, "--TEST--",   strlen("--TEST--")) == 0) {
			section = &t->name;
		} else if (i - start == strlen("--ARGS--")    && memcmp(buf + start, "--ARGS--",   strlen("--ARGS--")) == 0) {
			section = &t->args;
		} else if (i - start == strlen("--CODE--")    && memcmp(buf + start, "--CODE--",   strlen("--CODE--")) == 0) {
			section = &t->code;
		} else if (i - start == strlen("--EXPECT--")  && memcmp(buf + start, "--EXPECT--", strlen("--EXPECT--")) == 0) {
			section = &t->expect;
		} else if (i - start == strlen("--XFAIL--")   && memcmp(buf + start, "--XFAIL--",  strlen("--XFAIL--")) == 0) {
			section = &t->xfail;
		} else if (i - start == strlen("--TARGET--")  && memcmp(buf + start, "--TARGET--", strlen("--TARGET--")) == 0) {
			section = &t->target;
		} else {
			section = NULL;
			while (i < size && (buf[i] == '\r' || buf[i] == '\n')) i++;
		}
		if (section) {
			if (!last_section || last_section != &t->code) {
				while (start > 0 && (buf[start - 1] == '\r' || buf[start - 1] == '\n') && buf + start > *last_section) start--;
			}
			buf[start] = 0;
			if (*section) {
				free(t);
				return NULL;
			}
			if (section == &t->code) {
				if (i < size && buf[i] == '\r') i++;
				if (i < size && buf[i] == '\n') i++;
			} else {
				while (i < size && (buf[i] == '\r' || buf[i] == '\n')) i++;
			}
			*section = buf + i;
			last_section = section;
		}
	}
	if (!t->name || !t->code || !t->expect) {
		free(t);
		return NULL;
	}
	t->id = id;
	return t;
}

static int skip_test(test *t)
{
	if (target && (t->target && strcmp(t->target, target) != 0)) {
		return 1;
	}
	return 0;
}

static int same_text(const char *exp, const char *out)
{
	int i, j;

	while (*exp == '\r' || *exp == '\n') exp++;
	while (*out == '\r' || *out == '\n') out++;
	while (*exp != 0 && *out != 0) {
		i = j = 0;
		while (exp[i] != 0 && exp[i] != '\r' && exp[i] != '\n') i++;
		while (out[j] != 0 && out[j] != '\r' && out[j] != '\n') j++;
		if (i != j || memcmp(exp, out, i) != 0) {
			return 0;
		}
		exp += i;
		out += j;
		if (*exp == '\r') exp++;
		if (*exp == '\n') exp++;
		if (*out == '\r') out++;
		if (*out == '\n') out++;
	}
	while (*exp == '\r' || *exp == '\n') exp++;
	while (*out == '\r' || *out == '\n') out++;
	return *exp == 0 && *out == 0;
}

static char *read_file(const char *filename)
{
	struct stat stat_buf;
	size_t size;
	char *buf;
	int fd;

	if (stat(filename, &stat_buf) != 0) {
		return NULL;
	}
	size = stat_buf.st_size;
	buf = malloc(size + 1);
	if (!buf) {
		return NULL;
	}
	fd = open(filename, O_RDONLY | O_BINARY, 0);
	if (fd < 0) {
		free(buf);
		return NULL;
	}
	if ((size_t)read(fd, buf, size) != size) {
		free(buf);
		return NULL;
	}
	close(fd);
	buf[size] = 0;
	return buf;
}

static int write_file(const char *filename, const char *buf, size_t size)
{
	int fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return 0;
	}
	if ((size_t)write(fd, buf, size) != size) {
		return 0;
	}
	close(fd);
	return 1;
}

static char *replace_extension(const char *filename, size_t len, const char *ext, size_t ext_len)
{
	char *ret;

	if (test_extension) {
		ret = malloc(len - test_extension_len + ext_len + 1);
		memcpy(ret, filename, len - test_extension_len);
		memcpy(ret + len - test_extension_len, ext, ext_len + 1);
	} else {
		ret = malloc(len + ext_len + 1);
		memcpy(ret, filename, len);
		memcpy(ret + len, ext, ext_len + 1);
	}
	return ret;
}

static int run_test(const char *filename, test *t, int show_diff)
{
	size_t len;
	int ret;
	char cmd[4096];
	char *code_filename, *out_filename, *exp_filename, *diff_filename;

	len = strlen(filename);

	code_filename = replace_extension(filename, len, code_extension, code_extension_len);
	out_filename = replace_extension(filename, len, ".out", strlen(".out"));
	exp_filename = replace_extension(filename, len, ".exp", strlen(".exp"));
	diff_filename = replace_extension(filename, len, ".diff", strlen(".diff"));

	unlink(code_filename);
	unlink(out_filename);
	unlink(exp_filename);
	unlink(diff_filename);

	if (!write_file(code_filename, t->code, strlen(t->code))) {
		free(code_filename);
		free(out_filename);
		free(exp_filename);
		free(diff_filename);
		return 0;
	}

	if ((size_t)snprintf(cmd, sizeof(cmd), "%s %s %s %s > %s 2>&1",
			test_cmd, code_filename,
			t->args ? t->args : default_args, additional_args,
			out_filename) > sizeof(cmd)) {
		free(code_filename);
		free(out_filename);
		free(exp_filename);
		free(diff_filename);
		return 0;
	}

	ret = system(cmd);

	if (ret != -1) {
	    FILE *f;
	    char *out;

#ifdef _WIN32
		if (ret) {
			f = fopen(out_filename, "a+");
			fprintf(f, "\nexit code = %d\n", ret);
			fclose(f);
		}
#else
		if (WIFEXITED(ret)) {
			ret = WEXITSTATUS(ret);
		    if (ret > 128 && ret < 160) {
				f = fopen(out_filename, "a+");
				fprintf(f, "\ntermsig = %d\n", ret - 128);
				fclose(f);
			} else if (ret) {
				f = fopen(out_filename, "a+");
				fprintf(f, "\nexit code = %d\n", ret);
				fclose(f);
			}
		} else if (WIFSIGNALED(ret)) {
			ret = WTERMSIG(ret);
			f = fopen(out_filename, "a+");
			fprintf(f, "\ntermsig = %d\n", ret);
			fclose(f);
			if (ret == SIGINT || ret == SIGQUIT) {
				kill(getpid(), ret);
			}
		}
#endif

		out = read_file(out_filename);
		if (!out) {
			out = malloc(1);
			out[0] = 0;
		}
		ret = same_text(t->expect, out);
		free(out);
	} else {
		ret = 0;
	}

	if (ret) {
		unlink(code_filename);
		unlink(out_filename);
	} else {
		if (write_file(exp_filename, t->expect, strlen(t->expect))) {
			if ((size_t)snprintf(cmd, sizeof(cmd), "%s %s %s > %s",
					diff_cmd, exp_filename, out_filename, diff_filename) < sizeof(cmd)) {
				system(cmd);
				if (show_diff && !t->xfail) {
					char *diff = read_file(diff_filename);
					printf("\n");
					printf("%s", diff);
					free(diff);
				}
			}
		}
	}

	free(code_filename);
	free(out_filename);
	free(exp_filename);
	free(diff_filename);

	return ret;
}

static void add_file(char *name)
{
	if (files_count >= files_limit) {
		files_limit += 1024;
		files = realloc(files, sizeof(char*) * files_limit);
	}
	files[files_count++] = name;
}

static void find_files_in_dir(const char *dir_name, size_t dir_name_len)
{
	char *name;
	size_t len;
#ifdef _WIN32
	char buf[MAX_PATH];
	HANDLE dir;
	WIN32_FIND_DATA info;

	memcpy(buf, dir_name, dir_name_len);
	buf[dir_name_len] = '\\';
	buf[dir_name_len + 1] = '*';
	buf[dir_name_len + 2] = 0;

	dir = FindFirstFile(buf, &info);
	if (dir == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "ERROR: Cannot read directory [%s]\n", dir_name);
		return;
	}
	do {
		len = strlen(info.cFileName);
		if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if ((len == 1 && info.cFileName[0] == '.')
			 || (len == 2 && info.cFileName[0] == '.' && info.cFileName[1] == '.')) {
				/* skip */
			} else {
				name = malloc(dir_name_len + len + 2);
				memcpy(name, dir_name, dir_name_len);
				name[dir_name_len] = PATH_SEP;
				memcpy(name + dir_name_len + 1, info.cFileName, len + 1);
				find_files_in_dir(name, dir_name_len + len + 1);
				free(name);
			}
		} else /*if (info.dwFileAttributes & FILE_ATTRIBUTE_NORMAL)*/ {
			if (!test_extension
			 || stricmp(info.cFileName + len - test_extension_len, test_extension) == 0) {
				name = malloc(dir_name_len + len + 2);
				memcpy(name, dir_name, dir_name_len);
				name[dir_name_len] = PATH_SEP;
				memcpy(name + dir_name_len + 1, info.cFileName, len + 1);
				add_file(name);
			}
		}
	} while (FindNextFile(dir, &info));
	FindClose(dir);
#else
	DIR *dir = opendir(dir_name);
	struct dirent *info;

	if (!dir) {
		fprintf(stderr, "ERROR: Cannot read directory [%s]\n", dir_name);
		return;
	}
	while ((info = readdir(dir)) != 0) {
		len = strlen(info->d_name);
#if defined(DT_DIR)
		if (info->d_type == DT_DIR) {
#else
		char buf[PATH_MAX];
		struct stat s;
		snprintf(buf, sizeof(buf), "%s/%s", dir_name, info->d_name);
		stat(buf, &s);
		if (S_ISDIR(s.st_mode)) {
#endif
			if ((len == 1 && info->d_name[0] == '.')
			 || (len == 2 && info->d_name[0] == '.' && info->d_name[1] == '.')) {
				/* skip */
			} else {
				name = malloc(dir_name_len + len + 2);
				memcpy(name, dir_name, dir_name_len);
				name[dir_name_len] = PATH_SEP;
				memcpy(name + dir_name_len + 1, info->d_name, len + 1);
				find_files_in_dir(name, dir_name_len + len + 1);
				free(name);
			}
#if defined(DT_DIR)
		} else if (info->d_type == DT_REG) {
#else
		} else if (S_ISREG(s.st_mode)) {
#endif
			if (!test_extension
			 || strcasecmp(info->d_name + len - test_extension_len, test_extension) == 0) {
				name = malloc(dir_name_len + len + 2);
				memcpy(name, dir_name, dir_name_len);
				name[dir_name_len] = PATH_SEP;
				memcpy(name + dir_name_len + 1, info->d_name, len + 1);
				add_file(name);
			}
		}
	}
	closedir(dir);
#endif
}

static int cmp_files(const void *s1, const void *s2)
{
	return strcmp(*(const char**)s1, *(const char**)s2);
}

static void find_files(char **tests, int tests_count)
{
	int i;
	struct stat stat_buf;

	for (i = 0; i < tests_count; i++) {
		if (stat(tests[i], &stat_buf) != 0) {
			fprintf(stderr, "ERROR: Bad File or Folder [%s]\n", tests[i]);
		}
		if ((stat_buf.st_mode & S_IFMT) == S_IFDIR) {
			find_files_in_dir(tests[i], strlen(tests[i]));
		} else {
			add_file(strdup(tests[i]));
		}
	}
	qsort(files, files_count, sizeof(char*), cmp_files);
}

static void print_help(const char *exe_name)
{
	printf(
		"Run IR uint tests\n"
		"Usage:\n"
		"  %s --test-cmd <cmd> {options} <test folders or files...>\n"
	    "  Run the \"--CODE--\" section of specified test files using <cmd>\n"
	    "Options:\n"
	    "  --target <target>        - skip tests that specifies different --TARGET--\n"
	    "  --default-args <args>    - default <cmd> arguments (if --ARGS-- is missed)\n"
	    "  --additional-args <args> - additional <cmd> arguments (always added at the end)\n"
	    "  --diff-cmd <cmd>         - diff command\n"
	    "  --test-extension <ext>   - search test files with the given extension\n"
	    "  --code-extension <ext>   - produce code files with the given extension\n"
	    "  --show-diff              - show diff of the failed tests\n"
	    "  --no-color               - disable color output\n"
	    , exe_name);
}

static int check_arg(const char *name, const char **value, int argc, char **argv, int *pos, int *bad_opt)
{
	if (strcmp(argv[*pos], name) == 0) {
		if (*value) {
			*bad_opt = 1;
			fprintf(stderr, "ERROR: Duplicate %s\n", name);
		} else if (*pos + 1 == argc) {
			*bad_opt = 1;
			fprintf(stderr, "ERROR: Missing %s value\n", name);
		} else {
			*value = argv[++(*pos)];
		}
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int bad_opt = 0;     // unsupported option given
	int bad_files = 0;   // bad file or folder given
	int show_diff = 0;
	struct stat stat_buf;
#ifdef _WIN32
	char **tests = _alloca(sizeof(char*) * argc);
#else
	char **tests = alloca(sizeof(char*) * argc);
#endif
	int i, tests_count = 0;
	int skipped = 0;
	int passed = 0;
	int xfailed = 0, xfailed_limit = 0;
	int warned = 0, warned_limit = 0;
	int failed = 0, failed_limit = 0;
	int broken = 0, broken_limit = 0;
	test *t, **xfailed_tests = NULL, **warned_tests = NULL, **failed_tests = NULL;
	char **broken_tests = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_help(argv[0]);
			return 0;
		} else if (check_arg("--test-cmd",        &test_cmd,        argc, argv, &i, &bad_opt)) {
		} else if (check_arg("--target",          &target,          argc, argv, &i, &bad_opt)) {
		} else if (check_arg("--default-args",    &default_args,    argc, argv, &i, &bad_opt)) {
		} else if (check_arg("--additional-args", &additional_args, argc, argv, &i, &bad_opt)) {
		} else if (check_arg("--diff-cmd",        &diff_cmd,        argc, argv, &i, &bad_opt)) {
		} else if (check_arg("--test-extension",  &test_extension,  argc, argv, &i, &bad_opt)) {
		} else if (check_arg("--code-extension",  &code_extension,  argc, argv, &i, &bad_opt)) {
		} else if (strcmp(argv[i], "--show-diff") == 0) {
			show_diff = 1;
		} else if (strcmp(argv[i], "--no-color") == 0) {
			colorize = 0;
		} else if (argv[i][0] == '-') {
			bad_opt = 1;
			fprintf(stderr, "ERROR: Unsupported Option [%s]\n", argv[i]);
		} else {
			// User specified test folders/files
			if (stat(argv[i], &stat_buf) == 0
			 && ((stat_buf.st_mode & S_IFMT) == S_IFDIR
			  || (stat_buf.st_mode & S_IFMT) == S_IFREG)) {
				tests[tests_count++] = argv[i];
			} else {
				bad_files = 1;
				fprintf(stderr, "ERROR: Bad File or Folder [%s]\n", argv[i]);
			}
		}
	}

	if (bad_opt || bad_files || !tests_count || !test_cmd) {
		if (bad_opt || !tests_count || !test_cmd) {
			print_help(argv[0]);
		}
		return 1;
	}

	if (!default_args) {
		default_args = "";
	}
	if (!additional_args) {
		additional_args = "";
	}

	if (!code_extension) {
		code_extension = ".code";
	}
	code_extension_len = (int)strlen(code_extension);
	if (test_extension) {
		test_extension_len = (int)strlen(test_extension);
		if (strcmp(test_extension, code_extension) == 0) {
			fprintf(stderr, "ERROR: --test-extension and --code-extension can't be the same\n");
			return 1;
		}
	}

	if (!diff_cmd) {
		diff_cmd = DEFAULT_DIFF_CMD;
	}

	init_console();

	find_files(tests, tests_count);

	// Run each test
	for (i = 0; i < files_count; i++) {
		t = parse_file(files[i], i);
		if (!t) {
			printf("\r");
			print_color("BROK", RED);
			printf(": [%s]\n", files[i]);
			if (broken >= broken_limit) {
				broken_limit += 1024;
				broken_tests = realloc(broken_tests, sizeof(char*) * broken_limit);
			}
			broken_tests[broken++] = files[i];
			continue;
		}
		printf("TEST: %s [%s]", t->name, files[i]);
		fflush(stdout);
		if (skip_test(t)) {
			printf("\r");
			print_color("SKIP", YELLOW);
			printf(": %s [%s]\n", t->name, files[i]);
			skipped++;
			free(t);
		} else if (run_test(files[i], t, show_diff)) {
			printf("\r");
			passed++;
			if (t->xfail) {
				print_color("WARN", YELLOW);
				printf(": %s [%s] (warn: XFAIL section but test passes)\n", t->name, files[i]);
				if (warned >= warned_limit) {
					warned_limit += 1024;
					warned_tests = realloc(warned_tests, sizeof(test*) * warned_limit);
				}
				warned_tests[warned++] = t;
			} else {
				print_color("PASS", GREEN);
				printf(": %s [%s]\n", t->name, files[i]);
				free(t);
			}
		} else if (t->xfail) {
			printf("\r");
			print_color("XFAIL", RED);
			printf(": %s [%s]\n", t->name, files[i]);
			if (xfailed >= xfailed_limit) {
				xfailed_limit += 1024;
				xfailed_tests = realloc(xfailed_tests, sizeof(test*) * xfailed_limit);
			}
			xfailed_tests[xfailed++] = t;
		} else {
			printf("\r");
			print_color("FAIL", RED);
			printf(": %s [%s]\n", t->name, files[i]);
			if (failed >= failed_limit) {
				failed_limit += 1024;
				failed_tests = realloc(failed_tests, sizeof(test*) * failed_limit);
			}
			failed_tests[failed++] = t;
		}
	}

	// Produce the summary
	printf("-------------------------------\n");
	printf("Test Sumary\n");
	printf("-------------------------------\n");
	printf("Total: %d\n", files_count);
	printf("Passed: %d\n", passed);
	printf("Skipped: %d\n", skipped);
	printf("Expected fail: %d\n", xfailed);
	printf("Warned: %d\n", warned);
	printf("Failed: %d\n", failed);
	if (broken) {
		printf("Broken: %d\n", broken);
	}
	if (xfailed > 0) {
		printf("-------------------------------\n");
		printf("EXPECTED FAILED TESTS\n");
		printf("-------------------------------\n");
		for (i = 0; i < xfailed; i++) {
			t = xfailed_tests[i];
			printf("%s [%s] XFAIL REASON: %s\n", t->name, files[t->id], t->xfail);
			free(t);
		}
		free(xfailed_tests);
	}
	if (warned > 0) {
		printf("-------------------------------\n");
		printf("WARNED TESTS\n");
		printf("-------------------------------\n");
		for (i = 0; i < warned; i++) {
			t = warned_tests[i];
			printf("%s [%s] WARN: XFAIL reason \"%s\" but test passes\n", t->name, files[t->id], t->xfail);
			free(t);
		}
		free(warned_tests);
	}
	if (failed > 0) {
		printf("-------------------------------\n");
		printf("FAILED TESTS\n");
		printf("-------------------------------\n");
		for (i = 0; i < failed; i++) {
			t = failed_tests[i];
			printf("%s [%s]\n", t->name, files[t->id]);
			free(t);
		}
		free(failed_tests);
	}
	if (broken > 0) {
		printf("-------------------------------\n");
		printf("BROKEN TESTS\n");
		printf("-------------------------------\n");
		for (i = 0; i < broken; i++) {
			printf("%s\n", broken_tests[i]);
		}
		free(broken_tests);
	}
	printf("-------------------------------\n");

	for (i = 0; i < files_count; i++) {
		free(files[i]);
	}
	free(files);

	return failed > 0 ? 1 : 0;
}
