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
#elif defined(__linux__) || defined(__sun)
# include <alloca.h>
#endif

#ifndef _WIN32
# include <sys/time.h>
static double ir_time(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}
#else
static double ir_time(void)
{
	FILETIME filetime;

	GetSystemTimeAsFileTime(&filetime);
	return (double)((((uint64_t)filetime.dwHighDateTime << 32) | (uint64_t)filetime.dwLowDateTime)/10) /
		1000000.0;
}
#endif

static double ir_atexit_start = 0.0;

static void ir_atexit(void)
{
	if (ir_atexit_start) {
		double t = ir_time();
		fprintf(stderr, "\nexecution time = %0.6f\n", t - ir_atexit_start);
		ir_atexit_start = 0.0;
	}
}

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
		"  -f[no-]inline              - disable/enable function inlining\n"
		"  -fno-mem2ssa               - disable mem2ssa\n"
		"  -S                         - dump final target assembler code\n"
		"  --run ...                  - run the main() function of generated code\n"
		"                               (the remaining arguments are passed to main)\n"
#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
		"  -mavx                      - use AVX instruction set\n"
		"  -mno-bmi1                  - disable BMI1 instruction set\n"
#endif
		"  -muse-fp                   - use base frame pointer register\n"
		"  --emit-c [file-name]       - convert to C source\n"
		"  --emit-llvm [file-name]    - convert to LLVM\n"
		"  --save [file-name]         - save IR\n"
		"  --save-cfg                 - save IR\n"
		"  --save-cfg-map             - save IR\n"
		"  --save-rules               - save IR\n"
		"  --save-regs                - save IR\n"
		"  --save-use_lists           - save IR\n"
		"  --dot  [file-name]         - dump IR graph\n"
		"  --dump [file-name]         - dump IR table\n"
		"  --dump-after-load          - dump IR after load and local optimization\n"
		"  --dump-after-use-lists     - dump IR after USE-LISTS construction\n"
		"  --dump-after-mem2ssa       - dump IR after MEM2SSA pass\n"
		"  --dump-after-sccp          - dump IR after SCCP optimization pass\n"
		"  --dump-after-cfg           - dump IR after CFG construction\n"
		"  --dump-after-dom           - dump IR after Dominators tree construction\n"
		"  --dump-after-loop          - dump IR after Loop detection\n"
		"  --dump-after-gcm           - dump IR after GCM optimization pass\n"
		"  --dump-after-schedule      - dump IR after SCHEDULE pass\n"
		"  --dump-after-live-ranges   - dump IR after live ranges identification\n"
		"  --dump-after-coalescing    - dump IR after live ranges coalescing\n"
		"  --dump-after-regalloc      - dump IR after register allocation\n"
		"  --dump-after-all           - dump IR after each pass\n"
		"  --dump-final               - dump IR after all pass\n"
		"  --dump-size                - dump generated code size\n"
		"  --dump-time                - dump compilation and execution time\n"
		"  --dump-use-lists           - dump def->use lists\n"
		"  --dump-cfg                 - dump CFG (Control Flow Graph)\n"
		"  --dump-cfg-map             - dump CFG map (instruction to BB number)\n"
		"  --dump-live-ranges         - dump live ranges\n"
#ifdef IR_DEBUG
		"  --debug-sccp               - debug SCCP optimization pass\n"
		"  --debug-gcm                - debug GCM optimization pass\n"
		"  --debug-gcm-split          - debug floating node splitting\n"
		"  --debug-schedule           - debug SCHEDULE optimization pass\n"
		"  --debug-ra                 - debug register allocator\n"
		"  --debug-regset <bit-mask>  - restrict available register set\n"
		"  --debug-bb-schedule        - debug BB PLCEMENT optimization pass\n"
#endif
		"  --disable-gdb              - disable JIT code registration in GDB\n"
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

#define IR_DUMP_AFTER_LOAD          (1<<16)
#define IR_DUMP_AFTER_USE_LISTS     (1<<17)
#define IR_DUMP_AFTER_MEM2SSA       (1<<18)
#define IR_DUMP_AFTER_SCCP          (1<<19)
#define IR_DUMP_AFTER_CFG           (1<<20)
#define IR_DUMP_AFTER_DOM           (1<<21)
#define IR_DUMP_AFTER_LOOP          (1<<22)
#define IR_DUMP_AFTER_GCM           (1<<23)
#define IR_DUMP_AFTER_SCHEDULE      (1<<24)
#define IR_DUMP_AFTER_LIVE_RANGES   (1<<25)
#define IR_DUMP_AFTER_COALESCING    (1<<26)
#define IR_DUMP_AFTER_REGALLOC      (1<<27)

#define IR_DUMP_AFTER_ALL           (1<<29)
#define IR_DUMP_FINAL               (1<<30)

#define IR_UNKNOWN_SIZE             1

static int _save(ir_ctx *ctx, uint32_t save_flags, uint32_t dump, uint32_t pass, FILE *f, const char *func_name)
{
	char fn[4096];
	bool close = 0;

	if (!f) {
		if (dump & IR_DUMP_AFTER_ALL) {
			if (pass == IR_DUMP_AFTER_LOAD) {
				snprintf(fn, sizeof(fn)-1, "01-load-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_USE_LISTS) {
				snprintf(fn, sizeof(fn)-1, "02-use-lists-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_MEM2SSA) {
				snprintf(fn, sizeof(fn)-1, "03-mem2ssa-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_SCCP) {
				snprintf(fn, sizeof(fn)-1, "04-sccp-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_CFG) {
				snprintf(fn, sizeof(fn)-1, "05-cfg-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_DOM) {
				snprintf(fn, sizeof(fn)-1, "06-dom-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_LOOP) {
				snprintf(fn, sizeof(fn)-1, "07-loop-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_GCM) {
				snprintf(fn, sizeof(fn)-1, "08-gcm-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_SCHEDULE) {
				snprintf(fn, sizeof(fn)-1, "09-schedule-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_LIVE_RANGES) {
				snprintf(fn, sizeof(fn)-1, "10-live-ranges-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_COALESCING) {
				snprintf(fn, sizeof(fn)-1, "11-coalescing-%s.ir", func_name);
			} else if (pass == IR_DUMP_AFTER_REGALLOC) {
				snprintf(fn, sizeof(fn)-1, "12-regalloc-%s.ir", func_name);
			} else if (pass == IR_DUMP_FINAL) {
				if (dump & IR_DUMP_CODEGEN) {
					snprintf(fn, sizeof(fn)-1, "13-codegen-%s.ir", func_name);
				} else {
					snprintf(fn, sizeof(fn)-1, "13-final-%s.ir", func_name);
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
		ir_save(ctx, save_flags, f);
	}
	if (dump & IR_DUMP_DUMP) {
		ir_dump(ctx, f);
	}
	if (dump & IR_DUMP_DOT) {
		ir_dump_dot(ctx, func_name, f);
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

int ir_compile_func(ir_ctx *ctx, int opt_level, uint32_t save_flags, uint32_t dump, FILE *dump_file, const char *func_name)
{
	if ((dump & (IR_DUMP_AFTER_LOAD|IR_DUMP_AFTER_ALL))
	 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_LOAD, dump_file, func_name)) {
		return 0;
	}

	if (opt_level > 0 || (ctx->flags & (IR_GEN_NATIVE|IR_GEN_CODE))) {
		ir_build_def_use_lists(ctx);
		if ((dump & (IR_DUMP_AFTER_USE_LISTS|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_USE_LISTS, dump_file, func_name)) {
			return 0;
		}
	}

#ifdef IR_DEBUG
	ir_check(ctx);
#endif

	if (opt_level > 0 && (ctx->flags & IR_OPT_MEM2SSA)) {
		ir_build_cfg(ctx);
		if ((dump & (IR_DUMP_AFTER_CFG|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_CFG, dump_file, func_name)) {
			return 0;
		}
#ifdef IR_DEBUG
		ir_check(ctx);
#endif

		ir_build_dominators_tree(ctx);
		if ((dump & (IR_DUMP_AFTER_DOM|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_DOM, dump_file, func_name)) {
			return 0;
		}

		ir_mem2ssa(ctx);
		ir_reset_cfg(ctx);

		if ((dump & (IR_DUMP_AFTER_MEM2SSA|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_MEM2SSA, dump_file, func_name)) {
			return 0;
		}
#ifdef IR_DEBUG
		ir_check(ctx);
#endif
	}

	/* Global Optimization */
	if (opt_level > 1) {
		ir_sccp(ctx);
		if ((dump & (IR_DUMP_AFTER_SCCP|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_SCCP, dump_file, func_name)) {
			return 0;
		}
#ifdef IR_DEBUG
		ir_check(ctx);
#endif
	}

	if ((opt_level > 0 || (ctx->flags & (IR_GEN_NATIVE|IR_GEN_CODE))) && !ctx->cfg_blocks) {
		ir_build_cfg(ctx);
		if ((dump & (IR_DUMP_AFTER_CFG|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_CFG, dump_file, func_name)) {
			return 0;
		}
#ifdef IR_DEBUG
		ir_check(ctx);
#endif

		if (opt_level > 0) {
			ir_build_dominators_tree(ctx);
			if ((dump & (IR_DUMP_AFTER_DOM|IR_DUMP_AFTER_ALL))
			 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_DOM, dump_file, func_name)) {
				return 0;
			}
		}
	}

	/* Schedule */
	if (opt_level > 0) {
		ir_find_loops(ctx);
		if ((dump & (IR_DUMP_AFTER_LOOP|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_LOOP, dump_file, func_name)) {
			return 0;
		}

		ir_gcm(ctx);
		if ((dump & (IR_DUMP_AFTER_GCM|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_GCM, dump_file, func_name)) {
			return 0;
		}
#ifdef IR_DEBUG
		ir_check(ctx);
#endif

		ir_schedule(ctx);
		if ((dump & (IR_DUMP_AFTER_SCHEDULE|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_SCHEDULE, dump_file, func_name)) {
			return 0;
		}
#ifdef IR_DEBUG
		ir_check(ctx);
#endif
	}

	if (ctx->flags & IR_GEN_NATIVE) {
		ir_match(ctx);
	}

	if (opt_level > 0) {
		ir_assign_virtual_registers(ctx);
		ir_compute_live_ranges(ctx);

		if ((dump & (IR_DUMP_AFTER_LIVE_RANGES|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_LIVE_RANGES, dump_file, func_name)) {
			return 0;
		}

		ir_coalesce(ctx);

		if ((dump & (IR_DUMP_AFTER_COALESCING|IR_DUMP_AFTER_ALL))
		 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_COALESCING, dump_file, func_name)) {
			return 0;
		}

		if (ctx->flags & IR_GEN_NATIVE) {
			ir_reg_alloc(ctx);
			if ((dump & (IR_DUMP_AFTER_REGALLOC|IR_DUMP_AFTER_ALL))
			 && !_save(ctx, save_flags, dump, IR_DUMP_AFTER_REGALLOC, dump_file, func_name)) {
				return 0;
			}
		}

		ir_schedule_blocks(ctx);
	} else if (ctx->flags & (IR_GEN_NATIVE|IR_GEN_CODE)) {
		ir_assign_virtual_registers(ctx);
		ir_compute_dessa_moves(ctx);
	}

	if ((dump & (IR_DUMP_FINAL|IR_DUMP_AFTER_ALL|IR_DUMP_CODEGEN))
	 && !_save(ctx, save_flags, dump, IR_DUMP_FINAL, dump_file, func_name)) {
		return 0;
	}

#ifdef IR_DEBUG
	ir_check(ctx);
#endif

	return 1;
}

typedef struct _ir_sym {
	void *addr;
	void *thunk_addr;
} ir_sym;

typedef struct _ir_reloc {
	void   *addr;
	ir_ref  sym;
} ir_reloc;

typedef struct _ir_main_loader {
	ir_loader  loader;
	int        opt_level;
	uint32_t   mflags;
	uint64_t   debug_regset;
	uint32_t   save_flags;
	uint32_t   dump;
	bool       dump_asm;
	bool       dump_size;
	bool       run;
	bool       gdb;
	size_t     size;
	void      *main;
	FILE      *dump_file;
	FILE      *c_file;
	FILE      *llvm_file;
	ir_strtab  symtab;
	ir_sym    *sym;
	ir_reloc  *reloc;
	ir_ref     sym_count;
	ir_ref     reloc_count;
	void      *data_start;
	size_t     data_pos;
	ir_code_buffer code_buffer;
} ir_main_loader;

static void ir_loader_free_symbols(ir_main_loader *l)
{
	ir_strtab_free(&l->symtab);
	if (l->sym) {
		ir_mem_free(l->sym);
	}
	if (l->reloc) {
		ir_mem_free(l->reloc);
	}
}

static void ir_loader_add_reloc(ir_main_loader *l, const char *name, void *addr)
{
	ir_reloc *r;
	ir_ref val = ir_strtab_count(&l->symtab) + 1;
	ir_ref sym = ir_strtab_lookup(&l->symtab, name, strlen(name), val);

	if (sym == val) {
		if (val >= l->sym_count) {
			l->sym_count += 16;
			l->sym = ir_mem_realloc(l->sym, sizeof(ir_sym) * l->sym_count);
		}
		l->sym[val].addr = NULL;
		l->sym[val].thunk_addr = NULL;
	}

	l->reloc = ir_mem_realloc(l->reloc, sizeof(ir_reloc) * (l->reloc_count + 1));
	r = &l->reloc[l->reloc_count];
	r->addr = addr;
	r->sym = sym;
	l->reloc_count++;
}

static bool ir_loader_fix_relocs(ir_main_loader *l)
{
	bool ret = 1;
	ir_ref n = l->reloc_count;

	if (n > 0) {
		ir_reloc *r = l->reloc;
		ir_sym *s;

		ir_mem_unprotect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
		for (; n > 0; r++, n--) {
			IR_ASSERT(r->sym > 0 && r->sym < l->sym_count);
			s = &l->sym[r->sym];
			if (s->addr) {
				*(void**)r->addr = (void*)((uintptr_t)s->addr + *(uintptr_t*)r->addr);
#if 0
				uintptr_t addr;

				memcpy(&addr, r->addr, sizeof(void*));
				addr += (uintptr_t)s->addr;
				memcpy(r->addr, &addr, sizeof(void*));
#endif
			} else {
				fprintf(stderr, "Undefined symbol: %s\n", ir_strtab_str(&l->symtab, r->sym - 1));
				ret = 0;
				break;
			}
		}
		ir_mem_protect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
	}

	if (ret) {
		/* Check for unresolved external symbols */
		n = ir_strtab_count(&l->symtab);
		if (n > 0) {
			ir_sym *s = l->sym + 1;
			ir_ref j;

			for (j = 1; j < n; s++, j++) {
				if (!s->addr && s->thunk_addr) {
					fprintf(stderr, "Undefined symbol: %s\n", ir_strtab_str(&l->symtab, j - 1));
					ret = 0;
				}
			}
		}
	}

	return ret;
}

static bool ir_loader_add_sym(ir_loader *loader, const char *name, void *addr)
{
	ir_main_loader *l = (ir_main_loader*)loader;
	uint32_t len = (uint32_t)strlen(name);
	ir_ref val = ir_strtab_count(&l->symtab) + 1;
	ir_ref old_val = ir_strtab_lookup(&l->symtab, name, len, val);
	if (old_val != val) {
		if (addr && !l->sym[old_val].addr) {
			/* Update forward declaration */
			l->sym[old_val].addr = addr;
			if (l->sym[old_val].thunk_addr) {
				// TODO: Fix thunk or relocation ???
				if (l->code_buffer.start) {
					ir_mem_unprotect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
				}
				ir_fix_thunk(l->sym[old_val].thunk_addr, addr);
				if (l->code_buffer.start) {
					ir_mem_protect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
				}
			}
			return 1;
		}
		return 0;
	}
	if (val >= l->sym_count) {
		l->sym_count += 16;
		l->sym = ir_mem_realloc(l->sym, sizeof(ir_sym) * l->sym_count);
	}
	l->sym[val].addr = addr;
	l->sym[val].thunk_addr = NULL;
	return 1;
}

static bool ir_loader_has_sym(ir_loader *loader, const char *name)
{
	ir_main_loader *l = (ir_main_loader*)loader;
	uint32_t len = (uint32_t)strlen(name);
	ir_ref val = ir_strtab_find(&l->symtab, name, len);
	return val != 0;
}

static void* ir_loader_resolve_sym_name(ir_loader *loader, const char *name, uint32_t flags)
{
	ir_main_loader *l = (ir_main_loader*)loader;
	uint32_t len = (uint32_t)strlen(name);
	ir_ref val = ir_strtab_find(&l->symtab, name, len);
	void *addr;

	if (val) {
		if (l->sym[val].addr) {
			return l->sym[val].addr;
		}
		if (!l->sym[val].thunk_addr && (flags & IR_RESOLVE_SYM_ADD_THUNK)) {
			/* Undefined declaration */
			// TODO: Add thunk or relocation ???
			size_t size;

			l->sym[val].thunk_addr = ir_emit_thunk(&l->code_buffer, NULL, &size);
			ir_disasm_add_symbol(name, (uint64_t)(uintptr_t)l->sym[val].thunk_addr, size);
		}
		return l->sym[val].thunk_addr;
	}
	addr = ir_resolve_sym_name(name);
	if (addr) {
		ir_loader_add_sym(loader, name, addr); /* cache */
	}
	if (!addr && !(flags & IR_RESOLVE_SYM_SILENT)) {
		fprintf(stderr, "Undefined symbol: %s\n", name);
	}
	return addr;
}

static bool ir_loader_external_sym_dcl(ir_loader *loader, const char *name, uint32_t flags)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if (ir_loader_has_sym(loader, name)) {
		return 1;
	}

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		fprintf(l->dump_file, "extern %s %s;\n", (flags & IR_CONST) ? "const" : "var", name);
	}
	if (l->c_file) {
		ir_emit_c_sym_decl(name, flags | IR_EXTERN, l->c_file);
	}
	if (l->llvm_file) {
		ir_emit_llvm_sym_decl(name, flags | IR_EXTERN, l->llvm_file);
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *addr = ir_loader_resolve_sym_name(loader, name, IR_RESOLVE_SYM_SILENT);

		if (!addr) {
			return 0;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)addr, IR_UNKNOWN_SIZE);
		}
	} else {
		ir_loader_add_sym(loader, name, NULL);
	}
	return 1;
}

static void ir_dump_func_dcl(const char *name, uint32_t flags, ir_type ret_type, uint32_t params_count, const uint8_t *param_types, FILE *f)
{
	if (flags & IR_EXTERN) {
		fprintf(f, "extern ");
	} else if (flags & IR_STATIC) {
		fprintf(f, "static ");
	}
	fprintf(f, "func %s", name);
	ir_print_proto_ex(flags, ret_type, params_count, param_types, f);
	fprintf(f, ";\n");
}

static bool ir_loader_external_func_dcl(ir_loader *loader, const char *name, uint32_t flags,
                                        ir_type ret_type, uint32_t params_count, const uint8_t *param_types)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if (ir_loader_has_sym(loader, name)) {
		return 1;
	}

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		ir_dump_func_dcl(name, flags | IR_EXTERN, ret_type, params_count, param_types, l->dump_file);
	}
	if (l->c_file) {
		ir_emit_c_func_decl(name, flags | IR_EXTERN, ret_type,  params_count, param_types, l->c_file);
	}
	if (l->llvm_file) {
		ir_emit_llvm_func_decl(name, flags | IR_EXTERN, ret_type,  params_count, param_types, l->llvm_file);
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *addr = ir_loader_resolve_sym_name(loader, name, IR_RESOLVE_SYM_SILENT);

		if (!addr) {
			/* Unresolved external function */
			ir_loader_add_sym(loader, name, NULL);
			return 1;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)addr, IR_UNKNOWN_SIZE);
		}
	} else {
		ir_loader_add_sym(loader, name, NULL);
	}
	return 1;
}

static bool ir_loader_forward_func_dcl(ir_loader *loader, const char *name, uint32_t flags,
                                       ir_type ret_type, uint32_t params_count, const uint8_t *param_types)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if (ir_loader_has_sym(loader, name)) {
		return 1;
	}

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		ir_dump_func_dcl(name, flags, ret_type, params_count, param_types, l->dump_file);
	}
	if (l->c_file) {
		ir_emit_c_func_decl(name, flags, ret_type,  params_count, param_types, l->c_file);
	}

	ir_loader_add_sym(loader, name, NULL);
	return 1;
}

static bool ir_loader_sym_dcl(ir_loader *loader, const char *name, uint32_t flags, size_t size)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	l->data_start = NULL;
	l->data_pos = 0;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		if (flags & IR_STATIC) {
			fprintf(l->dump_file, "static ");
		}
		fprintf(l->dump_file, "%s %s [%" PRIuPTR "]%s",
			(flags & IR_CONST) ? "const" : "var", name, size,
			(flags & IR_INITIALIZED) ? ((flags & IR_CONST_STRING) ? " = " : " = {\n") : ";\n");
	}
	if (l->c_file) {
		ir_emit_c_sym_decl(name, flags, l->c_file);
	}
	if (l->llvm_file) {
		ir_emit_llvm_sym_decl(name, flags, l->llvm_file);
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *data;

		if (flags & IR_CONST) {
			data = l->code_buffer.pos;
			/* Data Alignment */
			if (size > 8) {
				data = (void*)IR_ALIGNED_SIZE(((size_t)(data)), 16);
			} else if (size == 8) {
				data = (void*)IR_ALIGNED_SIZE(((size_t)(data)), 8);
			} else if (size >= 4) {
				data = (void*)IR_ALIGNED_SIZE(((size_t)(data)), 4);
			} else if (size >= 2) {
				data = (void*)IR_ALIGNED_SIZE(((size_t)(data)), 2);
			}
			if (size > (size_t)((char*)l->code_buffer.end - (char*)data)) {
				return 0;
			}
			l->code_buffer.pos = (char*)data + size;
			ir_mem_unprotect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
		} else {
			data = ir_mem_malloc(size);
		}

		if (!ir_loader_add_sym(loader, name, data)) {
			ir_mem_free(data);
			return 0;
		}
		memset(data, 0, size);
		if (flags & IR_INITIALIZED) {
			l->data_start = data;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)data, size);
		}
#ifndef _WIN32
		if (l->gdb) {
			ir_gdb_register(name, data, size, 0, 0);
		}
#endif
	} else {
		ir_loader_add_sym(loader, name, NULL);
	}
	return 1;
}

static bool ir_loader_sym_data(ir_loader *loader, ir_type type, uint32_t count, const void *data)
{
	ir_main_loader *l = (ir_main_loader*) loader;
	size_t size = ir_type_size[type];

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		const void *p = data;
		uint32_t i;

		switch (size) {
			case 1:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, "\t%s 0x%02x,\n", ir_type_cname[type], (uint32_t)*(uint8_t*)p);
				}
				break;
			case 2:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, "\t%s 0x%04x,\n", ir_type_cname[type], (uint32_t)*(uint16_t*)p);
				}
				break;
			case 4:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, "\t%s 0x%08x,\n", ir_type_cname[type], *(uint32_t*)p);
				}
				break;
			case 8:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, "\t%s 0x%016" PRIx64 ",\n", ir_type_cname[type], *(uint64_t*)p);
				}
				break;
		}
	}
	if (l->c_file) {
		// TODO:
	}
	if (l->llvm_file) {
		// TODO:
	}
	if (l->dump_asm || l->dump_size || l->run) {
		IR_ASSERT(l->data_start);
		if (count == 1) {
			memcpy((char*)l->data_start + l->data_pos, data, size);
		} else {
			size_t pos = 0;
			uint32_t i;

			IR_ASSERT(count > 1);
			for (i = 0; i < count; i++) {
				memcpy((char*)l->data_start + l->data_pos + pos, data, size);
				pos += size;
			}
		}
	}
	l->data_pos += size * count;
	return 1;
}

static bool ir_loader_sym_data_str(ir_loader *loader, const char *str, size_t len)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		fprintf(l->dump_file, "\"");
		ir_print_escaped_str(str, len, l->dump_file);
		fprintf(l->dump_file, "\"");
	}
	if (l->c_file) {
		// TODO:
	}
	if (l->llvm_file) {
		// TODO:
	}
	if (l->dump_asm || l->dump_size || l->run) {
		IR_ASSERT(l->data_start);
		memcpy((char*)l->data_start + l->data_pos, str, len);
	}
	l->data_pos += len;
	return 1;
}

static bool ir_loader_sym_data_pad(ir_loader *loader, size_t offset)
{
	ir_main_loader *l = (ir_main_loader*) loader;
	size_t i;

	IR_ASSERT(offset >= l->data_pos);
	offset -= l->data_pos;
	if (offset) {
		if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
			for (i = 0; i < offset; i++) {
				fprintf(l->dump_file, "\tuint8_t 0x00,\n");
			}
		}
		if (l->c_file) {
			// TODO:
		}
		if (l->llvm_file) {
			// TODO:
		}
		if (l->dump_asm || l->dump_size || l->run) {
			IR_ASSERT(l->data_start);
			memset((char*)l->data_start + l->data_pos, 0, offset);
		}
		l->data_pos += offset;
	}
	return 1;
}

static bool ir_loader_sym_data_ref(ir_loader *loader, ir_op op, const char *ref, uintptr_t offset)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	IR_ASSERT(op == IR_FUNC || op == IR_SYM);
	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		if (!offset) {
			fprintf(l->dump_file, "\t%s %s(%s),\n", ir_type_cname[IR_ADDR], op == IR_FUNC ? "func" : "sym", ref);
		} else {
			fprintf(l->dump_file, "\t%s %s(%s)+0x%" PRIxPTR ",\n", ir_type_cname[IR_ADDR], op == IR_FUNC ? "func" : "sym", ref, offset);
		}
	}
	if (l->c_file) {
		// TODO:
	}
	if (l->llvm_file) {
		// TODO:
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *data = (char*)l->data_start + l->data_pos;
		void *addr = ir_loader_resolve_sym_name(loader, ref, IR_RESOLVE_SYM_SILENT);

		if (!addr) {
			ir_loader_add_reloc(l, ref, data);
		}
		IR_ASSERT(l->data_start);
		addr = (void*)((uintptr_t)(addr) + offset);
		memcpy(data, &addr, sizeof(void*));
	}
	l->data_pos += sizeof(void*);
	return 1;
}

static bool ir_loader_sym_data_end(ir_loader *loader, uint32_t flags)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		if (flags & IR_CONST_STRING) {
			fprintf(l->dump_file, ";\n");
		} else {
			fprintf(l->dump_file, "};\n");
		}
	}
	if (l->c_file) {
		// TODO:
	}
	if (l->llvm_file) {
		// TODO:
	}
	if (l->dump_asm || l->dump_size || l->run) {
		if ((char*)l->data_start >= (char*)l->code_buffer.start
		 && (char*)l->data_start < (char*)l->code_buffer.end) {
			ir_mem_protect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
		}
		// TODO:
	}
	return 1;
}

static bool ir_loader_func_init(ir_loader *loader, ir_ctx *ctx, const char *name)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	ir_init(ctx, loader->default_func_flags, 256, 1024);
	ctx->mflags = l->mflags;
	ctx->fixed_regset = ~l->debug_regset;
	ctx->loader = loader;
	return 1;
}

static bool ir_loader_func_process(ir_loader *loader, ir_ctx *ctx, const char *name)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	// TODO: remove this
	if (ctx->ret_type == (ir_type)-1) {
		ir_ref ref = ctx->ir_base[1].op1;
		while (ref) {
			ir_insn *insn = &ctx->ir_base[ref];
			if (insn->op == IR_RETURN) {
				ctx->ret_type = insn->op2 ? ctx->ir_base[insn->op2].type : IR_VOID;
				break;
			} else if (insn->op == IR_UNREACHABLE && ctx->ir_base[insn->op1].op == IR_TAILCALL) {
				ctx->ret_type = ctx->ir_base[insn->op1].type;
				break;
			}
			ref = ctx->ir_base[ref].op3;
		}
	}

	if (name == NULL) {
		name = (l->run) ? "main" : "test";
	} else if ((l->dump & IR_DUMP_SAVE) && l->dump_file) {
		if (ctx->flags & IR_STATIC) {
			fprintf(l->dump_file, "static ");
		}
		fprintf(l->dump_file, "func %s(", name);
		if (ctx->ir_base[2].op == IR_PARAM) {
			ir_insn *insn = &ctx->ir_base[2];

			fprintf(l->dump_file, "%s", ir_type_cname[insn->type]);
			insn++;
			while (insn->op == IR_PARAM) {
				fprintf(l->dump_file, ", %s", ir_type_cname[insn->type]);
				insn++;;
			}
			if (ctx->flags & IR_VARARG_FUNC) {
				fprintf(l->dump_file, ", ...");
			}
		} else if (ctx->flags & IR_VARARG_FUNC) {
			fprintf(l->dump_file, "...");
		}
		fprintf(l->dump_file, "): %s", ir_type_cname[ctx->ret_type != (ir_type)-1 ? ctx->ret_type : IR_VOID]);
		if (ctx->flags & IR_FASTCALL_FUNC) {
			fprintf(l->dump_file, " __fastcall");
		}
		if (ctx->flags & IR_CONST_FUNC) {
			fprintf(l->dump_file, " __const");
		} else if (ctx->flags & IR_PURE_FUNC) {
			fprintf(l->dump_file, " __pure");
		}
		fprintf(l->dump_file, "\n");
	}

	if (!ir_compile_func(ctx, l->opt_level, l->save_flags, l->dump, l->dump_file, name)) {
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
		void *entry;

		if (l->code_buffer.start) {
			ctx->code_buffer = &l->code_buffer;
			ir_mem_unprotect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
		}
		entry = ir_emit_code(ctx, &size);
#ifndef _WIN32
		if (l->gdb) {
			if (!l->code_buffer.start) {
				ir_mem_unprotect(entry, size);
			}
			ir_gdb_register(name, entry, size, sizeof(void*), 0);
			if (!l->code_buffer.start) {
				ir_mem_protect(entry, size);
			}
		}
#endif
		if (l->code_buffer.start) {
			ir_mem_protect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
		}
		if (entry) {
			if (!l->code_buffer.start) {
				l->size += size;
				l->size = IR_ALIGNED_SIZE(l->size, 16);
			}
			if (!ir_loader_add_sym(loader, name, entry)) {
				fprintf(stderr, "\nERROR: Symbol redefinition: %s\n", name);
				return 0;
			}
			if (l->dump_asm) {
				ir_ref i;
				ir_insn *insn;

				ir_disasm_add_symbol(name, (uintptr_t)entry, size);

				for (i = IR_UNUSED + 1, insn = ctx->ir_base - i; i < ctx->consts_count; i++, insn--) {
					if (insn->op == IR_FUNC) {
						const char *name = ir_get_str(ctx, insn->val.name);
						void *addr = ir_loader_resolve_sym_name(loader, name, 0);

						IR_ASSERT(addr);
						ir_disasm_add_symbol(name, (uintptr_t)addr, IR_UNKNOWN_SIZE);
//TODO:					} else if (insn->op == IR_SYM) {
					}
				}

				ir_disasm(name, entry, size, 0, ctx, stderr);
			}
			if (l->run) {
#ifndef _WIN32
				ir_perf_map_register(name, entry, size);
				ir_perf_jitdump_register(name, entry, size);
#endif
				if (strcmp(name, "main") == 0) {
					l->main = entry;
				}
			}
		} else {
			fprintf(stderr, "\nERROR: %d\n", ctx->status);
			return 0;
		}
	} else {
		ir_loader_add_sym(loader, name, NULL);
	}
	return 1;
}

int main(int argc, char **argv)
{
	int i, run_args = 0;
	char *input = NULL;
	char *dump_file = NULL, *c_file = NULL, *llvm_file = 0;
	FILE *f;
	bool emit_c = 0, emit_llvm = 0, dump_size = 0, dump_time = 0, dump_asm = 0, run = 0, gdb = 1;
	bool disable_inline = 0;
	bool force_inline = 0;
	bool disable_mem2ssa = 0;
	uint32_t save_flags = 0;
	uint32_t dump = 0;
	int opt_level = 2;
	uint32_t flags = 0;
	uint32_t mflags = 0;
#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
	uint32_t mflags_disabled = 0;
#endif
	uint64_t debug_regset = 0xffffffffffffffff;
#ifdef _WIN32
	bool abort_fault = 1;
#endif
#if HAVE_LLVM
	bool load_llvm_bitcode = 0;
	bool load_llvm_asm = 0;
#endif
	ir_main_loader loader;
	int ret = 0;
	double start;

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
		} else if (strcmp(argv[i], "--save-cfg") == 0) {
			dump |= IR_DUMP_SAVE;
			save_flags |= IR_SAVE_CFG;
		} else if (strcmp(argv[i], "--save-cfg-map") == 0) {
			dump |= IR_DUMP_SAVE;
			save_flags |= IR_SAVE_CFG_MAP;
		} else if (strcmp(argv[i], "--save-rules") == 0) {
			dump |= IR_DUMP_SAVE;
			save_flags |= IR_SAVE_RULES;
		} else if (strcmp(argv[i], "--save-regs") == 0) {
			dump |= IR_DUMP_SAVE;
			save_flags |= IR_SAVE_REGS;
		} else if (strcmp(argv[i], "--save-use-lists") == 0) {
			dump |= IR_DUMP_SAVE;
			save_flags |= IR_SAVE_USE_LISTS;
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
		} else if (strcmp(argv[i], "--dump-after-use-lists") == 0) {
			dump |= IR_DUMP_AFTER_USE_LISTS;
		} else if (strcmp(argv[i], "--dump-after-mem2ssa") == 0) {
			dump |= IR_DUMP_AFTER_MEM2SSA;
		} else if (strcmp(argv[i], "--dump-after-sccp") == 0) {
			dump |= IR_DUMP_AFTER_SCCP;
		} else if (strcmp(argv[i], "--dump-after-cfg") == 0) {
			dump |= IR_DUMP_AFTER_CFG;
		} else if (strcmp(argv[i], "--dump-after-dom") == 0) {
			dump |= IR_DUMP_AFTER_DOM;
		} else if (strcmp(argv[i], "--dump-after-loop") == 0) {
			dump |= IR_DUMP_AFTER_LOOP;
		} else if (strcmp(argv[i], "--dump-after-gcm") == 0) {
			dump |= IR_DUMP_AFTER_GCM;
		} else if (strcmp(argv[i], "--dump-after-schedule") == 0) {
			dump |= IR_DUMP_AFTER_SCHEDULE;
		} else if (strcmp(argv[i], "--dump-after-live-ranges") == 0) {
			dump |= IR_DUMP_AFTER_LIVE_RANGES;
		} else if (strcmp(argv[i], "--dump-after-coalescing") == 0) {
			dump |= IR_DUMP_AFTER_COALESCING;
		} else if (strcmp(argv[i], "--dump-after-regalloc") == 0) {
			dump |= IR_DUMP_AFTER_REGALLOC;
		} else if (strcmp(argv[i], "--dump-after-all") == 0) {
			dump |= IR_DUMP_AFTER_ALL;
		} else if (strcmp(argv[i], "--dump-final") == 0) {
			dump |= IR_DUMP_FINAL;
		} else if (strcmp(argv[i], "--dump-size") == 0) {
			dump_size = 1;
		} else if (strcmp(argv[i], "--dump-time") == 0) {
			dump_time = 1;
		} else if (strcmp(argv[i], "-S") == 0) {
			dump_asm = 1;
		} else if (strcmp(argv[i], "--run") == 0) {
			run = 1;
			if (i + 1 < argc) {
				run_args = i + 1;
			}
			break;
#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
		} else if (strcmp(argv[i], "-mavx") == 0) {
			mflags |= IR_X86_AVX;
		} else if (strcmp(argv[i], "-mno-bmi1") == 0) {
			mflags_disabled |= IR_X86_BMI1;
#endif
		} else if (strcmp(argv[i], "-muse-fp") == 0) {
			flags |= IR_USE_FRAME_POINTER;
		} else if (strcmp(argv[i], "-mfastcall") == 0) {
			flags |= IR_FASTCALL_FUNC;
		} else if (strcmp(argv[i], "-fno-inline") == 0) {
			disable_inline = 1;
		} else if (strcmp(argv[i], "-finline") == 0) {
			force_inline = 1;
		} else if (strcmp(argv[i], "-fno-mem2ssa") == 0) {
			disable_mem2ssa = 1;
#ifdef IR_DEBUG
		} else if (strcmp(argv[i], "--debug-sccp") == 0) {
			flags |= IR_DEBUG_SCCP;
		} else if (strcmp(argv[i], "--debug-gcm") == 0) {
			flags |= IR_DEBUG_GCM;
		} else if (strcmp(argv[i], "--debug-gcm-split") == 0) {
			flags |= IR_DEBUG_GCM_SPLIT;
		} else if (strcmp(argv[i], "--debug-schedule") == 0) {
			flags |= IR_DEBUG_SCHEDULE;
		} else if (strcmp(argv[i], "--debug-ra") == 0) {
			flags |= IR_DEBUG_RA;
		} else if (strcmp(argv[i], "--debug-bb-schedule") == 0) {
			flags |= IR_DEBUG_BB_SCHEDULE;
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
		} else if (strcmp(argv[i], "--disable-gdb") == 0) {
			gdb = 0;
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

	if (dump && !(dump & (IR_DUMP_AFTER_LOAD|IR_DUMP_AFTER_USE_LISTS|
		IR_DUMP_AFTER_MEM2SSA|IR_DUMP_AFTER_SCCP|
		IR_DUMP_AFTER_CFG|IR_DUMP_AFTER_DOM|IR_DUMP_AFTER_LOOP|
		IR_DUMP_AFTER_GCM|IR_DUMP_AFTER_SCHEDULE|
		IR_DUMP_AFTER_LIVE_RANGES|IR_DUMP_AFTER_COALESCING|IR_DUMP_AFTER_REGALLOC|
		IR_DUMP_FINAL))) {
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
	if ((cpuinfo & IR_X86_BMI1) && !(mflags_disabled & IR_X86_BMI1)) {
		mflags |= IR_X86_BMI1;
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
	if ((opt_level > 1 && !disable_inline) || force_inline) {
		flags |= IR_OPT_INLINE;
	}
	if (opt_level > 0 && !disable_mem2ssa) {
		flags |= IR_OPT_MEM2SSA;
	}
	if (emit_c || emit_llvm) {
		flags |= IR_GEN_CODE;
	}
	if (dump_asm || dump_size || run) {
		flags |= IR_GEN_NATIVE;
		if (emit_c || emit_llvm) {
			fprintf(stderr, "ERROR: --emit-c and --emit-llvm are incompatible with native code generator (-S, --dump-size, --run)\n");
			return 1;
		}
	}

	memset(&loader, 0, sizeof(loader));
	loader.loader.default_func_flags = flags;
	loader.loader.init_module        = NULL;
	loader.loader.external_sym_dcl   = ir_loader_external_sym_dcl;
	loader.loader.external_func_dcl  = ir_loader_external_func_dcl;
	loader.loader.forward_func_dcl   = ir_loader_forward_func_dcl;
	loader.loader.sym_dcl            = ir_loader_sym_dcl;
	loader.loader.sym_data           = ir_loader_sym_data;
	loader.loader.sym_data_str       = ir_loader_sym_data_str;
	loader.loader.sym_data_pad       = ir_loader_sym_data_pad;
	loader.loader.sym_data_ref       = ir_loader_sym_data_ref;
	loader.loader.sym_data_end       = ir_loader_sym_data_end;
	loader.loader.func_init          = ir_loader_func_init;
	loader.loader.func_process       = ir_loader_func_process;
	loader.loader.resolve_sym_name   = ir_loader_resolve_sym_name;
	loader.loader.has_sym            = ir_loader_has_sym;
	loader.loader.add_sym            = ir_loader_add_sym;

	loader.opt_level = opt_level;
	loader.mflags = mflags;
	loader.debug_regset = debug_regset;
	loader.save_flags = save_flags;
	loader.dump = dump;
	loader.dump_asm = dump_asm;
	loader.dump_size = dump_size;
	loader.run = run;
	loader.gdb = run && gdb;

	ir_strtab_init(&loader.symtab, 16, 4096);
	loader.sym = NULL;
	loader.reloc = NULL;
	loader.sym_count = 0;
	loader.reloc_count = 0;

	if (dump_file) {
		loader.dump_file = fopen(dump_file, "w+");
		if (!loader.dump_file) {
			fprintf(stderr, "ERROR: Cannot create file '%s'\n", dump_file);
			ret = 1;
			goto exit;
		}
	} else {
		loader.dump_file = stderr;
	}
	if (emit_c) {
		if (c_file) {
			loader.c_file = fopen(c_file, "w+");
			if (!loader.c_file) {
				fprintf(stderr, "ERROR: Cannot create file '%s'\n", c_file);
				ret = 1;
				goto exit;
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
				ret = 1;
				goto exit;
			}
		} else {
			loader.llvm_file = stderr;
		}
	}

	if (dump_asm || dump_size || run) {
		/* Preallocate 2MB JIT code buffer. It may be necessary to generate veneers and thunks. */
		size_t size = 2 * 1024 * 1024;
		void *entry;

		loader.code_buffer.start = ir_mem_mmap(size);
		if (!loader.code_buffer.start) {
			fprintf(stderr, "ERROR: Cannot allocate JIT code buffer\n");
			ret = 1;
			goto exit;
		}
		loader.code_buffer.pos = loader.code_buffer.start;
		loader.code_buffer.end = (char*)loader.code_buffer.start + size;

		if (ir_needs_thunk(&loader.code_buffer, printf)) {
			ir_mem_unprotect(loader.code_buffer.start, (char*)loader.code_buffer.end - (char*)loader.code_buffer.start);
			entry = ir_emit_thunk(&loader.code_buffer, printf, &size);
			ir_loader_add_sym(&loader.loader, (void*)"printf", entry);
			ir_mem_protect(loader.code_buffer.start, (char*)loader.code_buffer.end - (char*)loader.code_buffer.start);
		} else {
			ir_loader_add_sym(&loader.loader, (void*)"printf", printf);
		}
		if (ir_needs_thunk(&loader.code_buffer, putchar)) {
			ir_mem_unprotect(loader.code_buffer.start, (char*)loader.code_buffer.end - (char*)loader.code_buffer.start);
			entry = ir_emit_thunk(&loader.code_buffer, putchar, &size);
			ir_loader_add_sym(&loader.loader, (void*)"putchar", entry);
			ir_mem_protect(loader.code_buffer.start, (char*)loader.code_buffer.end - (char*)loader.code_buffer.start);
		} else {
			ir_loader_add_sym(&loader.loader, (void*)"putchar", putchar);
		}
	}

#ifndef _WIN32
	if (run) {
		ir_perf_jitdump_open();
	}
#endif

	if (dump_time) {
		start = ir_time();
	}

#if HAVE_LLVM
	if (load_llvm_bitcode) {
		if (!ir_load_llvm_bitcode(&loader.loader, input)) {
			fprintf(stderr, "ERROR: Cannot load LLVM file '%s'\n", input);
			ret = 1;
			goto exit;
		}
		goto finish;
	} else if (load_llvm_asm) {
		if (!ir_load_llvm_asm(&loader.loader, input)) {
			fprintf(stderr, "ERROR: Cannot load LLVM file '%s'\n", input);
			ret = 1;
			goto exit;
		}
		goto finish;
	}
#endif

	f = fopen(input, "rb");
	if (!f) {
		fprintf(stderr, "ERROR: Cannot open input file '%s'\n", input);
		ret = 1;
		goto exit;
	}

	ir_loader_init();

	if (!ir_load(&loader.loader, f)) {
		fclose(f);
		ir_loader_free();
		fprintf(stderr, "ERROR: Cannot load input file '%s'\n", input);
		ret = 1;
		goto exit;
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

	if (!ir_loader_fix_relocs(&loader)) {
		if (run && loader.main) {
			fprintf(stderr, "ERROR: Cannot run program with undefined symbols\n");
			ret = 1;
			goto exit;
		}
	}

	if (dump_size) {
		if (loader.code_buffer.start) {
			loader.size = (char*)loader.code_buffer.pos - (char*)loader.code_buffer.start;
		}
		fprintf(stderr, "\ncode size = %lld\n", (long long int)loader.size);
	}

	if (dump_time) {
		double t = ir_time();
		fprintf(stderr, "\ncompilation time = %0.6f\n", t - start);
		start = t;
	}

	if (run && loader.main) {
		int jit_argc = 1;
		char **jit_argv;
		int (*func)(int, char**) = loader.main;

		if (dump_time) {
			ir_atexit_start = start;
			atexit(ir_atexit);
		}

		if (run_args && argc > run_args) {
			jit_argc = argc - run_args + 1;
		}
		jit_argv = alloca(sizeof(char*) * jit_argc);
		jit_argv[0] = "jit code";
		for (i = 1; i < jit_argc; i++) {
			jit_argv[i] = argv[run_args + i - 1];
		}
		ret = func(jit_argc, jit_argv);

		if (dump_time) {
			double t = ir_time();
			fprintf(stderr, "\nexecution time = %0.6f\n", t - start);
			ir_atexit_start = 0.0;
		}

#ifndef _WIN32
		ir_perf_jitdump_close();
#endif
	}

exit:
	ir_loader_free_symbols(&loader);
	return ret;
}
