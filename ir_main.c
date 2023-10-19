/*
 * IR - Lightweight JIT Compilation Framework
 * (IR CLI driver)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h" // TODO: move this together with loader
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
#endif

static void help(const char *cmd)
{
	printf(
#if HAVE_LLVM
		"Usage: %s [options] [--llvm-bitcode|--llvm-asm] input-file...\n"
#else
		"Usage: %s [options] input-file...\n"
#endif
		"Options:\n"
		"  -O[012]                    - optimization level (default: 2)\n"
		"  -S                         - dump final target assembler code\n"
#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
		"  -mavx                      - use AVX instruction set\n"
#endif
		"  -muse-fp                   - use base frame pointer register\n"
		"  --emit-c [file-name]       - convert to C source\n"
		"  --emit-llvm [file-name]    - convert to LLVM\n"
		"  --save [file-name]         - save IR\n"
		"  --dot  [file-name]         - dump IR graph\n"
		"  --dump [file-name]         - dump IR table\n"
		"  --dump-after-load          - dump IR after load and local optimization\n"
		"  --dump-after-sccp          - dump IR after SCCP optimization pass\n"
		"  --dump-after-gcm           - dump IR after GCM optimization pass\n"
		"  --dump-after-schedule      - dump IR after SCHEDULE pass\n"
		"  --dump-after-live-ranges   - dump IR after live ranges identification\n"
		"  --dump-after-coalescing    - dump IR after live ranges coalescing\n"
		"  --dump-after-all           - dump IR after each pass\n"
		"  --dump-final               - dump IR after all pass\n"
		"  --dump-size                - dump generated code size\n"
		"  --dump-use-lists           - dump def->use lists\n"
		"  --dump-cfg                 - dump CFG (Control Flow Graph)\n"
		"  --dump-cfg-map             - dump CFG map (instruction to BB number)\n"
		"  --dump-live-ranges         - dump live ranges\n"
#ifdef IR_DEBUG
		"  --debug-sccp               - debug SCCP optimization pass\n"
		"  --debug-gcm                - debug GCM optimization pass\n"
		"  --debug-schedule           - debug SCHEDULE optimization pass\n"
		"  --debug-ra                 - debug register allocator\n"
		"  --debug-regset <bit-mask>  - restrict available register set\n"
#endif
		"  --target                   - print JIT target\n"
		"  --version\n"
		"  --help\n",
		cmd);
}

#define IR_DUMP_SAVE                (1<<0)
#define IR_DUMP_DUMP                (1<<1)
#define IR_DUMP_DOT                 (1<<2)
#define IR_DUMP_USE_LISTS           (1<<3)
#define IR_DUMP_CFG                 (1<<4)
#define IR_DUMP_CFG_MAP             (1<<5)
#define IR_DUMP_LIVE_RANGES         (1<<6)
#define IR_DUMP_CODEGEN             (1<<7)

#define IR_DUMP_NAME                (1<<8)

#define IR_DUMP_AFTER_LOAD          (1<<16)
#define IR_DUMP_AFTER_SCCP          (1<<17)
#define IR_DUMP_AFTER_GCM           (1<<18)
#define IR_DUMP_AFTER_SCHEDULE      (1<<19)
#define IR_DUMP_AFTER_LIVE_RANGES   (1<<20)
#define IR_DUMP_AFTER_COALESCING    (1<<21)

#define IR_DUMP_AFTER_ALL           (1<<29)
#define IR_DUMP_FINAL               (1<<30)

static int _save(ir_ctx *ctx, uint32_t dump, uint32_t pass, FILE *f, const char *func_name)
{
	char fn[4096];
	bool close = 0;

	if (!f) {
		if (dump & IR_DUMP_AFTER_ALL) {
			if (pass == IR_DUMP_AFTER_LOAD) {
				snprintf(fn, sizeof(fn)-1, "01-load-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_SCCP) {
				snprintf(fn, sizeof(fn)-1, "02-sccp-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_GCM) {
				snprintf(fn, sizeof(fn)-1, "03-gcm-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_SCHEDULE) {
				snprintf(fn, sizeof(fn)-1, "04-schedule-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_LIVE_RANGES) {
				snprintf(fn, sizeof(fn)-1, "05-live-ranges-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_COALESCING) {
				snprintf(fn, sizeof(fn)-1, "06-coalescing-%s.ir", func_name);
			} else if (pass == IR_DUMP_FINAL) {
				if (dump & IR_DUMP_CODEGEN) {
					snprintf(fn, sizeof(fn)-1, "07-codegen-%s.ir", func_name);
				} else {
					snprintf(fn, sizeof(fn)-1, "07-final-%s.ir", func_name);
				}
			} else {
				f = stderr; // TODO:
			}
		} else {
			snprintf(fn, sizeof(fn)-1, "%s.ir", func_name);
		}
		f = fopen(fn, "w+");
		if (!f) {
			fprintf(stderr, "ERROR: Cannot create file '%s'\n", fn);
			return 0;
		}
		close = 1;
	}
	if (pass == IR_DUMP_FINAL && (dump & IR_DUMP_CODEGEN)) {
		ir_dump_codegen(ctx, f);
	} else if (dump & IR_DUMP_SAVE) {
		ir_save(ctx, f);
	}
	if (dump & IR_DUMP_DUMP) {
		ir_dump(ctx, f);
	}
	if (dump & IR_DUMP_DOT) {
		ir_dump_dot(ctx, f);
	}
	if (dump & IR_DUMP_USE_LISTS) {
		ir_dump_use_lists(ctx, f);
	}
	if (dump & IR_DUMP_CFG) {
		ir_dump_cfg(ctx, f);
	}
	if (dump & IR_DUMP_CFG_MAP) {
		ir_dump_cfg_map(ctx, f);
	}
	if (dump & IR_DUMP_LIVE_RANGES) {
		ir_dump_live_ranges(ctx, f);
	}
	if (close) {
		fclose(f);
	}
	return 1;
}

int ir_compile_func(ir_ctx *ctx, int opt_level, uint32_t dump, FILE *dump_file, const char *func_name)
{
	if ((dump & (IR_DUMP_AFTER_LOAD|IR_DUMP_AFTER_ALL))
	 && !_save(ctx, dump, IR_DUMP_AFTER_LOAD, dump_file, func_name)) {
		return 0;
	}

	if (opt_level > 0 || (ctx->flags & (IR_GEN_NATIVE|IR_GEN_CODE))) {
		ir_build_def_use_lists(ctx);
	}

	ir_check(ctx);

	/* Global Optimization */
	if (opt_level > 1) {
		ir_sccp(ctx);
		if ((dump & (IR_DUMP_AFTER_SCCP|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, dump, IR_DUMP_AFTER_SCCP, dump_file, func_name)) {
			return 0;
		}
	}

	if (opt_level > 0 || (ctx->flags & (IR_GEN_NATIVE|IR_GEN_CODE))) {
		ir_build_cfg(ctx);
	}

	/* Schedule */
	if (opt_level > 0) {
		ir_build_dominators_tree(ctx);
		ir_find_loops(ctx);
		ir_gcm(ctx);
		if ((dump & (IR_DUMP_AFTER_GCM|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, dump, IR_DUMP_AFTER_GCM, dump_file, func_name)) {
			return 0;
		}
		ir_schedule(ctx);
		if ((dump & (IR_DUMP_AFTER_SCHEDULE|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, dump, IR_DUMP_AFTER_SCHEDULE, dump_file, func_name)) {
			return 0;
		}
	}

	if (ctx->flags & IR_GEN_NATIVE) {
		ir_match(ctx);
	}

	if (opt_level > 0) {
		ir_assign_virtual_registers(ctx);
		ir_compute_live_ranges(ctx);

		if ((dump & (IR_DUMP_AFTER_LIVE_RANGES|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, dump, IR_DUMP_AFTER_LIVE_RANGES, dump_file, func_name)) {
			return 0;
		}

		ir_coalesce(ctx);

		if ((dump & (IR_DUMP_AFTER_COALESCING|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, dump, IR_DUMP_AFTER_COALESCING, dump_file, func_name)) {
			return 0;
		}

		if (ctx->flags & IR_GEN_NATIVE) {
			ir_reg_alloc(ctx);
		}

		ir_schedule_blocks(ctx);
	} else if (ctx->flags & (IR_GEN_NATIVE|IR_GEN_CODE)) {
		ir_assign_virtual_registers(ctx);
		ir_compute_dessa_moves(ctx);
	}

	if ((dump & (IR_DUMP_FINAL|IR_DUMP_AFTER_ALL|IR_DUMP_CODEGEN))
	 && !_save(ctx, dump, IR_DUMP_FINAL, dump_file, func_name)) {
		return 0;
	}

	ir_check(ctx);

	return 1;
}

typedef struct _ir_sym {
	void *addr;
} ir_sym;

typedef struct _ir_main_loader {
	ir_loader  loader;
	int        opt_level;
	uint32_t   mflags;
	uint64_t   debug_regset;
	uint32_t   dump;
	bool       dump_asm;
	bool       dump_size;
	bool       run;
	size_t     size;
	void      *main;
	FILE      *dump_file;
	FILE      *c_file;
	FILE      *llvm_file;
	ir_strtab  symtab;
	ir_sym    *sym;
	ir_ref     sym_count;
	void      *data;
} ir_main_loader;

static bool ir_loader_add_sym(ir_main_loader *l, const char *name, void *addr)
{
	uint32_t len = (uint32_t)strlen(name);
	ir_ref val = ir_strtab_count(&l->symtab) + 1;
	ir_ref old_val = ir_strtab_lookup(&l->symtab, name, len, val);
	if (old_val != val) {
		return 0;
	}
	if (val >= l->sym_count) {
		l->sym_count += 16;
		l->sym = ir_mem_realloc(l->sym, sizeof(ir_sym) * l->sym_count);
	}
	l->sym[val].addr = addr;
	return 1;
}

static void* ir_loader_resolve_sym_name(ir_loader *loader, const char *name)
{
	ir_main_loader *l = (ir_main_loader*)loader;
	uint32_t len = (uint32_t)strlen(name);
	ir_ref val = ir_strtab_find(&l->symtab, name, len);
	void *addr;

	if (val) {
		return l->sym[val].addr;
	}
	addr = ir_resolve_sym_name(name);
	ir_loader_add_sym(l, name, addr); /* cache */
	return addr;
}

static bool ir_loader_external_sym_dcl(ir_loader *loader, const char *name, bool is_const)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		fprintf(l->dump_file, "; external %s %s:\n", is_const ? "const" : "var", name);
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *addr = ir_loader_resolve_sym_name(loader, name);

		if (!addr) {
			return 0;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)addr, sizeof(void*));
		}
	}
	return 1;
}

static bool ir_loader_sym_dcl(ir_loader *loader, const char *name, bool is_const, bool is_static, size_t size, bool has_data)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		fprintf(l->dump_file, "; %s%s %s: [%lld]\n", is_static ? "static " : "", is_const ? "const" : "var", name, (long long int)size);
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *data = ir_mem_malloc(size);

		if (!ir_loader_add_sym((ir_main_loader*)loader, name, data)) {
			ir_mem_free(data);
			return 0;
		}
		memset(data, 0, size);
		if (has_data) {
			l->data = data;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)data, size);
		}
	}
	return 1;
}

static bool ir_loader_data(ir_loader *loader, ir_type type, uint32_t count, void *data)
{
	ir_main_loader *l = (ir_main_loader*) loader;
	size_t size;
	uint32_t i;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		switch (ir_type_size[type]) {
			case 1:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, ";   .db 0x%02x\n", (uint32_t)*(uint8_t*)data);
					data = (void*)((uintptr_t)data + 1);
				}
				break;
			case 2:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, ";   .dw 0x%04x\n", (uint32_t)*(uint16_t*)data);
					data = (void*)((uintptr_t)data + 1);
				}
				break;
			case 4:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, ";   .dd 0x%08x\n", *(uint32_t*)data);
					data = (void*)((uintptr_t)data + 4);
				}
				break;
			case 8:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, ";   .dq 0x%016lx\n", *(uint64_t*)data);
					data = (void*)((uintptr_t)data + 8);
				}
				break;
		}
	}
	if (l->dump_asm || l->dump_size || l->run) {
		if (!l->data) {
			return 0;
		}
		size = ir_type_size[type] * count;
		// TODO: alignement
		memcpy(l->data, data, size);
		l->data = (void*)((uintptr_t)l->data + size);
	}
	return 1;
}

static bool ir_loader_external_func_dcl(ir_loader *loader, const char *name)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		fprintf(l->dump_file, "; external func %s:\n", name);
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *addr = ir_loader_resolve_sym_name(loader, name);

		if (!addr) {
			return 0;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)addr, sizeof(void*));
		}
	}
	return 1;
}

static bool ir_loader_init_func(ir_loader *loader, ir_ctx *ctx, const char *name)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	ctx->mflags = l->mflags;
	ctx->fixed_regset = ~l->debug_regset;
	ctx->loader = loader;
	return 1;
}

static bool ir_loader_process_func(ir_loader *loader, ir_ctx *ctx, const char *name)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	// TODO: remove this
	if (name == NULL) {
		name = (l->run) ? "main" : "test";
	}

	if ((l->dump & IR_DUMP_SAVE) && (l->dump & IR_DUMP_NAME) && l->dump_file) {
		fprintf(l->dump_file, "%s:\n", name);
	}

	if (!ir_compile_func(ctx, l->opt_level, l->dump, l->dump_file, name)) {
		return 0;
	}

	if (l->c_file) {
		if (!ir_emit_c(ctx, name, l->c_file)) {
			fprintf(stderr, "\nERROR: %d\n", ctx->status);
			return 0;
		}
	}

	if (l->llvm_file) {
		if (!ir_emit_llvm(ctx, name, l->llvm_file)) {
			fprintf(stderr, "\nERROR: %d\n", ctx->status);
			return 0;
		}
	}

	if (l->dump_asm || l->dump_size || l->run) {
		size_t size;
		void *entry = ir_emit_code(ctx, &size);

		l->size += size;
		if (entry) {
			if (!ir_loader_add_sym(l, name, entry)) {
				fprintf(stderr, "\nERROR: Symbol redefinition: %s\n", name);
				return 0;
			}
			if (l->dump_asm) {
				ir_ref i;
				ir_insn *insn;

				ir_disasm_add_symbol(name, (uintptr_t)entry, size);

				for (i = IR_UNUSED + 1, insn = ctx->ir_base - i; i < ctx->consts_count; i++, insn--) {
					if (insn->op == IR_FUNC) {
						const char *name = ir_get_str(ctx, insn->val.i32);
						void *addr = ir_loader_resolve_sym_name(loader, name);

						ir_disasm_add_symbol(name, (uintptr_t)addr, sizeof(void*));
//TODO:					} else if (insn->op == IR_SYM) {
					}
				}

				ir_disasm(name, entry, size, 0, ctx, stderr);
			}
			if (l->run) {
#ifndef _WIN32
				ir_perf_map_register(name, entry, size);
				ir_perf_jitdump_open();
				ir_perf_jitdump_register(name, entry, size);

				ir_mem_unprotect(entry, 4096);
				ir_gdb_register(name, entry, size, sizeof(void*), 0);
				ir_mem_protect(entry, 4096);
#endif
				if (strcmp(name, "main") == 0) {
					l->main = entry;
				}
			}
		} else {
			fprintf(stderr, "\nERROR: %d\n", ctx->status);
			return 0;
		}
	}
	return 1;
}

int main(int argc, char **argv)
{
	int i;
	char *input = NULL;
	char *dump_file = NULL, *c_file = NULL, *llvm_file = 0;
	FILE *f;
	bool emit_c = 0, emit_llvm = 0, dump_size = 0, dump_asm = 0, run = 0;
	uint32_t dump = 0;
	int opt_level = 2;
	uint32_t flags = 0;
	uint32_t mflags = 0;
	uint64_t debug_regset = 0xffffffffffffffff;
#ifdef _WIN32
	bool abort_fault = 1;
#endif
#if HAVE_LLVM
	bool load_llvm_bitcode = 0;
	bool load_llvm_asm = 0;
#endif
	ir_main_loader loader;

	ir_consistency_check();

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0
		 || strcmp(argv[i], "--help") == 0) {
			help(argv[0]);
			return 0;
		} else if (strcmp(argv[i], "--version") == 0) {
			printf("IR %s\n", IR_VERSION);
			return 0;
		} else if (strcmp(argv[i], "--target") == 0) {
			printf("%s\n", IR_TARGET);
			return 0;
		} else if (argv[i][0] == '-' && argv[i][1] == 'O' && strlen(argv[i]) == 3) {
			if (argv[i][2] == '0') {
				opt_level = 0;
			} else if (argv[i][2] == '1') {
				opt_level = 1;
			} else if (argv[i][2] == '2') {
				opt_level = 2;
			} else {
				fprintf(stderr, "ERROR: Invalid usage' (use --help)\n");
				return 1;
			}
		} else if (strcmp(argv[i], "--emit-c") == 0) {
			emit_c = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				c_file = argv[i + 1];
				i++;
			}
		} else if (strcmp(argv[i], "--emit-llvm") == 0) {
			emit_llvm = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				llvm_file = argv[i + 1];
				i++;
			}
		} else if (strcmp(argv[i], "--save") == 0) {
			// TODO: check save/dot/dump/... conflicts
			dump |= IR_DUMP_SAVE;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				dump_file = argv[i + 1];
				i++;
			}
		} else if (strcmp(argv[i], "--dot") == 0) {
			dump |= IR_DUMP_DOT;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				dump_file = argv[i + 1];
				i++;
			}
		} else if (strcmp(argv[i], "--dump") == 0) {
			dump |= IR_DUMP_DUMP;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				dump_file = argv[i + 1];
				i++;
			}
		} else if (strcmp(argv[i], "--dump-use-lists") == 0) {
			dump |= IR_DUMP_USE_LISTS;
		} else if (strcmp(argv[i], "--dump-cfg") == 0) {
			dump |= IR_DUMP_CFG;
		} else if (strcmp(argv[i], "--dump-cfg-map") == 0) {
			dump |= IR_DUMP_CFG_MAP;
		} else if (strcmp(argv[i], "--dump-live-ranges") == 0) {
			dump |= IR_DUMP_LIVE_RANGES;
		} else if (strcmp(argv[i], "--dump-codegen") == 0) {
			dump |= IR_DUMP_CODEGEN;
		} else if (strcmp(argv[i], "--dump-after-load") == 0) {
			dump |= IR_DUMP_AFTER_LOAD;
		} else if (strcmp(argv[i], "--dump-after-sccp") == 0) {
			dump |= IR_DUMP_AFTER_SCCP;
		} else if (strcmp(argv[i], "--dump-after-gcm") == 0) {
			dump |= IR_DUMP_AFTER_GCM;
		} else if (strcmp(argv[i], "--dump-after-schedule") == 0) {
			dump |= IR_DUMP_AFTER_SCHEDULE;
		} else if (strcmp(argv[i], "--dump-after-live-ranges") == 0) {
			dump |= IR_DUMP_AFTER_LIVE_RANGES;
		} else if (strcmp(argv[i], "--dump-after-coalescing") == 0) {
			dump |= IR_DUMP_AFTER_COALESCING;
		} else if (strcmp(argv[i], "--dump-after-all") == 0) {
			dump |= IR_DUMP_AFTER_ALL;
		} else if (strcmp(argv[i], "--dump-final") == 0) {
			dump |= IR_DUMP_FINAL;
		} else if (strcmp(argv[i], "--dump-size") == 0) {
			dump_size = 1;
		} else if (strcmp(argv[i], "-S") == 0) {
			dump_asm = 1;
		} else if (strcmp(argv[i], "--run") == 0) {
			run = 1;
#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
		} else if (strcmp(argv[i], "-mavx") == 0) {
			mflags |= IR_X86_AVX;
#endif
		} else if (strcmp(argv[i], "-muse-fp") == 0) {
			flags |= IR_USE_FRAME_POINTER;
		} else if (strcmp(argv[i], "-mfastcall") == 0) {
			flags |= IR_FASTCALL_FUNC;
#ifdef IR_DEBUG
		} else if (strcmp(argv[i], "--debug-sccp") == 0) {
			flags |= IR_DEBUG_SCCP;
		} else if (strcmp(argv[i], "--debug-gcm") == 0) {
			flags |= IR_DEBUG_GCM;
		} else if (strcmp(argv[i], "--debug-schedule") == 0) {
			flags |= IR_DEBUG_SCHEDULE;
		} else if (strcmp(argv[i], "--debug-ra") == 0) {
			flags |= IR_DEBUG_RA;
#endif
		} else if (strcmp(argv[i], "--debug-regset") == 0) {
			if (i + 1 == argc || argv[i + 1][0] == '-') {
				fprintf(stderr, "ERROR: Invalid usage' (use --help)\n");
				return 1;
			}
			debug_regset = strtoull(argv[i + 1], NULL, 0);
			i++;
#ifdef _WIN32
		} else if (strcmp(argv[i], "--no-abort-fault") == 0) {
			abort_fault = 0;
#endif
#if HAVE_LLVM
		} else if (strcmp(argv[i], "--llvm-bitcode") == 0) {
			if (input || i + 1 == argc || argv[i + 1][0] == '-') {
				fprintf(stderr, "ERROR: Invalid usage' (use --help)\n");
				return 1;
			}
			load_llvm_bitcode = 1;
			input = argv[++i];
		} else if (strcmp(argv[i], "--llvm-asm") == 0) {
			if (input || i + 1 == argc || argv[i + 1][0] == '-') {
				fprintf(stderr, "ERROR: Invalid usage' (use --help)\n");
				return 1;
			}
			load_llvm_asm = 1;
			input = argv[++i];
#endif
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "ERROR: Unknown option '%s' (use --help)\n", argv[i]);
			return 1;
		} else {
			if (input) {
				fprintf(stderr, "ERROR: Invalid usage' (use --help)\n");
				return 1;
			}
			input = argv[i];
		}
	}

	if (dump && !(dump & (IR_DUMP_AFTER_LOAD|IR_DUMP_AFTER_SCCP|
		IR_DUMP_AFTER_GCM|IR_DUMP_AFTER_SCHEDULE|
		IR_DUMP_AFTER_LIVE_RANGES|IR_DUMP_AFTER_COALESCING|IR_DUMP_FINAL))) {
		dump |= IR_DUMP_FINAL;
	}

	if (!input) {
		fprintf(stderr, "ERROR: no input file\n");
		return 1;
	}

#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
	uint32_t cpuinfo = ir_cpuinfo();

	if (!(cpuinfo & IR_X86_SSE2)) {
		fprintf(stderr, "ERROR: incompatible CPU (SSE2 is not supported)\n");
		return 1;
	}

	if ((mflags & IR_X86_AVX) && !(cpuinfo & IR_X86_AVX)) {
		fprintf(stderr, "ERROR: -mavx is not compatible with CPU (AVX is not supported)\n");
		return 1;
	}
#endif

#ifdef _WIN32
	if (!abort_fault) {
		_set_abort_behavior(0, _WRITE_ABORT_MSG|_CALL_REPORTFAULT);
		SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
	}
#endif

	flags |= IR_FUNCTION;

	if (opt_level > 0) {
		flags |= IR_OPT_FOLDING | IR_OPT_CFG | IR_OPT_CODEGEN;
	}
	if (emit_c || emit_llvm) {
		flags |= IR_GEN_CODE;
	}
	if (dump_asm || dump_size || run) {
		flags |= IR_GEN_NATIVE;
		if (emit_c) {
			fprintf(stderr, "ERROR: --emit-c is incompatible with native code generator (-S, --dump-size, --run)\n");
			return 1;
		}
	}

	loader.loader.default_func_flags = flags;
	loader.loader.init_module        = NULL;
	loader.loader.external_sym_dcl   = ir_loader_external_sym_dcl;
	loader.loader.sym_dcl            = ir_loader_sym_dcl;
	loader.loader.data               = ir_loader_data;
	loader.loader.external_func_dcl  = ir_loader_external_func_dcl;
	loader.loader.forward_func_dcl   = NULL;
	loader.loader.init_func          = ir_loader_init_func;
	loader.loader.process_func       = ir_loader_process_func;
	loader.loader.resolve_sym_name   = ir_loader_resolve_sym_name;

	loader.opt_level = opt_level;
	loader.mflags = mflags;
	loader.debug_regset = debug_regset;
	loader.dump = dump;
	loader.dump_asm = dump_asm;
	loader.dump_size = dump_size;
	loader.run = run;

	loader.size = 0;
	loader.main = NULL;

	loader.dump_file = NULL;
	loader.c_file = NULL;
	loader.llvm_file = NULL;

	ir_strtab_init(&loader.symtab, 16, 4096);
	loader.sym = NULL;
	loader.sym_count = 0;

	if (dump_file) {
		loader.dump_file = fopen(dump_file, "w+");
		if (!loader.dump_file) {
			fprintf(stderr, "ERROR: Cannot create file '%s'\n", dump_file);
			return 0;
		}
	} else {
		loader.dump_file = stderr;
	}
	if (emit_c) {
		if (c_file) {
			loader.c_file = fopen(c_file, "w+");
			if (!loader.c_file) {
				fprintf(stderr, "ERROR: Cannot create file '%s'\n", c_file);
				return 0;
			}
		} else {
			loader.c_file = stderr;
		}
	}
	if (emit_llvm) {
		if (llvm_file) {
			loader.llvm_file = fopen(llvm_file, "w+");
			if (!loader.llvm_file) {
				fprintf(stderr, "ERROR: Cannot create file '%s'\n", llvm_file);
				return 0;
			}
		} else {
			loader.llvm_file = stderr;
		}
	}

#if HAVE_LLVM
	if (load_llvm_bitcode) {
		loader.dump |= IR_DUMP_NAME;
		if (!ir_load_llvm_bitcode(&loader.loader, input)) {
			fprintf(stderr, "ERROR: Cannot load LLVM file '%s'\n", input);
			return 1;
		}
		goto finish;
	} else if (load_llvm_asm) {
		loader.dump |= IR_DUMP_NAME;
		if (!ir_load_llvm_asm(&loader.loader, input)) {
			fprintf(stderr, "ERROR: Cannot load LLVM file '%s'\n", input);
			return 1;
		}
		goto finish;
	}
#endif

	f = fopen(input, "rb");
	if (!f) {
		fprintf(stderr, "ERROR: Cannot open input file '%s'\n", input);
		return 1;
	}

	ir_loader_init();

	if (!ir_load(&loader.loader, f)) {
		fprintf(stderr, "ERROR: Cannot load input file '%s'\n", input);
	}

	fclose(f);

	
	ir_loader_free();

#if HAVE_LLVM
finish:
#endif

	if (loader.dump_file && loader.dump_file != stderr) {
		fclose(loader.dump_file);
	}
	if (loader.c_file && loader.c_file != stderr) {
		fclose(loader.c_file);
	}
	if (loader.llvm_file && loader.llvm_file != stderr) {
		fclose(loader.llvm_file);
	}

	if (dump_size) {
		fprintf(stderr, "\ncode size = %lld\n", (long long int)loader.size);
	}

	ir_strtab_free(&loader.symtab);
	if (loader.sym) {
		ir_mem_free(loader.sym);
	}

	if (run && loader.main) {
		int (*func)(void) = loader.main;
		int ret = func();
		fflush(stdout);
		fprintf(stderr, "\nexit code = %d\n", ret);
	}

	return 0;
}
