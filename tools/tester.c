/*
 * IR - Lightweight JIT Compilation Framework
 * (Test runner implementation)
 * Copyright (C) 2023 by IR project.
 * Authors: Dmitry Stogov <dmitry@php.net>, Anatol Belski <anbelski@linux.microsoft.com>
 */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

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
	char *ext;
	char *run_args;
	int   run_args_len;
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

static test *parse_file(const char *filename)
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
		} else if (i - start == strlen("--EXT--")  && memcmp(buf + start, "--EXT--", strlen("--EXT--")) == 0) {
			section = &t->ext;
		} else {
			section = NULL;
			while (i < size && (buf[i] == '\r' || buf[i] == '\n')) i++;
		}
		if (section) {
			if (last_section && last_section != &t->code) {
				/* Trim terminating EOL(s) */
				size_t last_section_start = *last_section - buf;
				while (start > last_section_start && (buf[start - 1] == '\r' || buf[start - 1] == '\n')) start--;
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
	return t;
}

static test *parse_test(const char *filename, int id)
{
	test *t = parse_file(filename);

	if (!t) {
		return NULL;
	}
	if (!t->name || !t->code || !t->expect) {
		free(t);
		return NULL;
	}
	t->id = id;
	return t;
}

static test *parse_rc(const char *dir_name, int dir_len)
{
	char *filename;
	struct stat stat_buf;
	test *t;

#ifdef _WIN32
	filename = _alloca(dir_len + strlen("tester.rc") + 1);
#else
	filename = alloca(dir_len + strlen("tester.rc") + 1);
#endif

	if (dir_len) {
		memcpy(filename, dir_name, dir_len);
	}
	memcpy(filename + dir_len, "tester.rc", strlen("tester.rc") + 1);


	if (stat(filename, &stat_buf) != 0) {
		return NULL;
	}

	t = parse_file(filename);
	if (!t || t->name || t->code || t->expect || t->xfail) {
		print_color("BROK", RED);
		printf(": [%s]\n", filename);
		if (t) {
			free(t);
		}
		return NULL;
	}

	return t;
}

static test *dup_test(const test *t, const char *args, int args_len)
{
	test *copy;
	size_t size = strlen(t->name) + 1;
	char *buf;

	if (t->target) {
		size += strlen(t->target) + 1;
	}
	if (args) {
		size += args_len + 1;
	}
	if (t->code) {
		size += strlen(t->code) + 1;
	}
	if (t->expect) {
		size += strlen(t->expect) + 1;
	}
	if (t->ext) {
		size += strlen(t->ext) + 1;
	}
	if (t->xfail) {
		size += strlen(t->xfail) + 1;
	}

	copy = malloc(sizeof(test) + size);
	if (!copy) {
		return NULL;
	}
	buf = (char*)copy + sizeof(test);

	memset(copy, 0, sizeof(test));
	copy->id = t->id;

	copy->name = buf;
	size = strlen(t->name);
	memcpy(buf, t->name, size + 1);
	buf += size + 1;

	if (t->target) {
		copy->target = buf;
		size = strlen(t->target);
		memcpy(buf, t->target, size + 1);
		buf += size + 1;
	}

	if (args) {
		copy->args = copy->run_args = buf;
		copy->run_args_len = args_len;
		memcpy(buf, args, args_len);
		buf[args_len] = 0;
		buf += args_len + 1;
	}

	if (t->code) {
		copy->code = buf;
		size = strlen(t->code);
		memcpy(buf, t->code, size + 1);
		buf += size + 1;
	}

	if (t->expect) {
		copy->expect = buf;
		size = strlen(t->expect);
		memcpy(buf, t->expect, size + 1);
		buf += size + 1;
	}

	if (t->ext) {
		copy->ext = buf;
		size = strlen(t->ext);
		memcpy(buf, t->ext, size + 1);
		buf += size + 1;
	}

	if (t->xfail) {
		copy->xfail = buf;
		size = strlen(t->xfail);
		memcpy(buf, t->xfail, size + 1);
		buf += size + 1;
	}

	return copy;
}

static int match(const char *pattern, const char *str, int single)
{
	const char *start = str;
	char ch;

	while (1) {
		ch = *pattern++;
		if (ch == 0) {
			return *str == 0 || *str == '-';
		} else if (ch != *str) {
			if (ch == ' ') {
				if (*str == 0 || *str == '-') return 1;
				if (single) return 0;
				str = start;
				continue;
			} else if (ch == '*') {
				do {
					ch = *pattern++;
				} while (ch == '*');
				if (ch == 0 || *str == 0) return 1;
				do {
					if (match(pattern, str, single)) return 1;
					str++;
				} while (*str != 0);
				return 0;
			} else if (ch == '?') {
				if (*str == 0 || *str == '-') return 0;
			} else {
				return 0;
			}
		}
		str++;
	}
}

static int skip_test(test *t)
{
	if (target && t->target) {
		if (t->target[0] == '!') {
			return match(t->target + 1, target, 1);
		} else {
			return !match(t->target, target, 0);
		}
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

static int run_test(const char *filename, test *t, int show_diff, const char *args, int args_len)
{
	size_t len;
	int ret;
	char cmd[4096];
	char *code_filename, *out_filename, *exp_filename, *diff_filename;

	len = strlen(filename);

	if (t->ext) {
		code_filename = replace_extension(filename, len, t->ext, strlen(t->ext));
	} else {
		code_filename = replace_extension(filename, len, code_extension, code_extension_len);
	}
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

	if ((size_t)snprintf(cmd, sizeof(cmd), "%s %s %s %.*s > %s 2>&1",
			test_cmd, code_filename,
			additional_args,
			args_len, args,
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
				if (system(cmd) != -1 && show_diff && !t->xfail) {
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
	char *args, *next_args;
	const char *last_dir = NULL;
	int default_args_len, args_len, last_dir_len = -1;
	test *rc = NULL, **rcs = NULL;
	int rcs_count = 0, rcs_limit = 0;
	int may_free;

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
		default_args_len = 0;
	} else {
		default_args_len = strlen(default_args);
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
		const char *file = files[i];
		int dir_len;

		dir_len = strlen(file);
		while (dir_len > 0
#ifdef _WIN32
		 && file[dir_len-1] != PATH_SEP
#endif
		 && file[dir_len-1] != '/') {
			dir_len--;
		}
		if (dir_len != last_dir_len || strncmp(file, last_dir, dir_len)) {
			rc = parse_rc(file, dir_len);
			if (rc) {
				if (rcs_count >= rcs_limit) {
					rcs_limit += 32;
					rcs = realloc(rcs, sizeof(test*) * rcs_limit);
				}
				rcs[rcs_count++] = rc;
			}
			last_dir = file;
			last_dir_len = dir_len;
		}
		t = parse_test(file, i);
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

		if (rc) {
			if (rc->target && !t->target) {
				t->target = rc->target;
			}
			if (rc->args && !t->args) {
				t->args = rc->args;
			}
			if (rc->ext && !t->ext) {
				t->ext = rc->ext;
			}
		}

		if (skip_test(t)) {
			printf("\r");
			print_color("SKIP", YELLOW);
			printf(": %s [%s]\n", t->name, files[i]);
			skipped++;
			free(t);
			continue;
		}

		may_free = 1;
		next_args = NULL;
		if (t->args) {
			args = t->args;
try_args:
			next_args = strchr(args, '\n');
			if (next_args) {
				char *s = next_args;
				next_args++;
				while (*next_args == '\r' || *next_args == '\n' || *next_args == ' ' || *next_args == '\t') {
					next_args++;
				}
				if (*next_args == '\0') {
					next_args = NULL;
				}
				do {
					s--;
					if (s == args) {
						if (!next_args) continue;
						args = next_args;
						goto try_args;
					}
				} while (*s == '\r' || *s == ' ' || *s == '\t');
				args_len = s + 1 - args;
			} else {
				args_len = strlen(args);
			}
		} else {
			args = (char*)default_args;
			args_len = default_args_len;
		}

		if (args_len) {
			printf("TEST: %s [%s] (%.*s)", t->name, files[i], args_len, args);
		} else {
			printf("TEST: %s [%s]", t->name, files[i]);
		}
		fflush(stdout);

		if (run_test(files[i], t, show_diff, args, args_len)) {
			printf("\r");
			passed++;
			if (t->xfail) {
				print_color("WARN", YELLOW);
				if (args_len) {
					printf(": %s [%s] (%.*s) (warn: XFAIL section but test passes)\n", t->name, files[i], args_len, args);
				} else {
					printf(": %s [%s] (warn: XFAIL section but test passes)\n", t->name, files[i]);
				}
				if (warned >= warned_limit) {
					warned_limit += 1024;
					warned_tests = realloc(warned_tests, sizeof(test*) * warned_limit);
				}
				if (t->run_args) {
					t = dup_test(t, args, args_len);
				} else {
					t->run_args = args;
				}
				warned_tests[warned++] = t;
				may_free = 0;
			} else {
				print_color("PASS", GREEN);
				if (args_len) {
					printf(": %s [%s] (%.*s)\n", t->name, files[i], args_len, args);
				} else {
					printf(": %s [%s]\n", t->name, files[i]);
				}
			}
		} else if (t->xfail) {
			printf("\r");
			print_color("XFAIL", RED);
			if (args_len) {
				printf(": %s [%s] (%.*s)\n", t->name, files[i], args_len, args);
			} else {
				printf(": %s [%s]\n", t->name, files[i]);
			}
			if (xfailed >= xfailed_limit) {
				xfailed_limit += 1024;
				xfailed_tests = realloc(xfailed_tests, sizeof(test*) * xfailed_limit);
			}
			if (t->run_args) {
				t = dup_test(t, args, args_len);
			} else {
				t->run_args = args;
			}
			xfailed_tests[xfailed++] = t;
			may_free = 0;
		} else {
			printf("\r");
			print_color("FAIL", RED);
			if (args_len) {
				printf(": %s [%s] (%.*s)\n", t->name, files[i], args_len, args);
			} else {
				printf(": %s [%s]\n", t->name, files[i]);
			}
			if (failed >= failed_limit) {
				failed_limit += 1024;
				failed_tests = realloc(failed_tests, sizeof(test*) * failed_limit);
			}
			if (t->run_args) {
				t = dup_test(t, args, args_len);
			} else {
				t->run_args = args;
				t->run_args_len = args_len;
			}
			failed_tests[failed++] = t;
			may_free = 0;
		}

		if (next_args) {
			args = next_args;
			goto try_args;
		}
		if (may_free) {
			free(t);
		}
	}

	// Produce the summary
	printf("-------------------------------\n");
	printf("Test Sumary\n");
	printf("-------------------------------\n");
	printf("Test Files: %d\n", files_count);
	if (broken) {
		printf("Broken: %d\n", broken);
	}
	printf("Skipped: %d\n", skipped);
	printf("Test Runs: %d\n", passed + xfailed + failed);
	printf("Passed: %d\n", passed);
	printf("Expected fail: %d\n", xfailed);
	printf("Warned: %d\n", warned);
	printf("Failed: %d\n", failed);
	if (xfailed > 0) {
		printf("-------------------------------\n");
		printf("EXPECTED FAILED TESTS\n");
		printf("-------------------------------\n");
		for (i = 0; i < xfailed; i++) {
			t = xfailed_tests[i];
			if (t->run_args_len) {
				printf("%s [%s] (%.*s) XFAIL REASON: %s\n",
					t->name, files[t->id], t->run_args_len, t->run_args, t->xfail);
			} else {
				printf("%s [%s] XFAIL REASON: %s\n", t->name, files[t->id], t->xfail);
			}
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
			if (t->run_args_len) {
				printf("%s [%s] (%.*s) WARN: XFAIL reason \"%s\" but test passes\n",
					t->name, files[t->id], t->run_args_len, t->run_args, t->xfail);
			} else {
				printf("%s [%s] WARN: XFAIL reason \"%s\" but test passes\n", t->name, files[t->id], t->xfail);
			}
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
			if (t->run_args_len) {
				printf("%s [%s] (%.*s)\n", t->name, files[t->id], t->run_args_len, t->run_args);
			} else {
				printf("%s [%s]\n", t->name, files[t->id]);
			}
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

	if (rcs_count) {
		for (i = 0; i < rcs_count; i++) {
			free(rcs[i]);
		}
		free(rcs);
	}

	return failed > 0 ? 1 : 0;
}
