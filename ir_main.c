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
	void *thunk_addr;
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
	void      *data_start;
	void      *data;
	ir_code_buffer code_buffer;
} ir_main_loader;

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

static void* ir_loader_resolve_sym_name(ir_loader *loader, const char *name)
{
	ir_main_loader *l = (ir_main_loader*)loader;
	uint32_t len = (uint32_t)strlen(name);
	ir_ref val = ir_strtab_find(&l->symtab, name, len);
	void *addr;

	if (val) {
		if (l->sym[val].addr) {
			return l->sym[val].addr;
		}
		if (!l->sym[val].thunk_addr) {
			/* Undefined declaration */
			// TODO: Add thunk or relocation ???
			l->sym[val].thunk_addr = (void*)(intptr_t)sizeof(void*);
		}
		return l->sym[val].thunk_addr;
	}
	addr = ir_resolve_sym_name(name);
	ir_loader_add_sym(loader, name, addr); /* cache */
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
		ir_emit_c_sym_decl(name, flags | IR_EXTERN, 0, l->c_file);
	}
	if (l->llvm_file) {
		ir_emit_llvm_sym_decl(name, flags | IR_EXTERN, 0, l->llvm_file);
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *addr = ir_loader_resolve_sym_name(loader, name);

		if (!addr) {
			return 0;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)addr, sizeof(void*));
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
	fprintf(f, "func %s(", name);
	if (params_count) {
		const uint8_t *p = param_types;

		fprintf(f, "%s", ir_type_cname[*p]);
		p++;
		while (--params_count) {
			fprintf(f, ", %s", ir_type_cname[*p]);
			p++;
		}
		if (flags & IR_VARARG_FUNC) {
			fprintf(f, ", ...");
		}
	} else if (flags & IR_VARARG_FUNC) {
		fprintf(f, "...");
	}
	fprintf(f, "): %s", ir_type_cname[ret_type]);
	if (flags & IR_FASTCALL_FUNC) {
		fprintf(f, " __fastcall");
	} else if (flags & IR_BUILTIN_FUNC) {
		fprintf(f, " __builtin");
	}
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
		void *addr = ir_loader_resolve_sym_name(loader, name);

		if (!addr) {
			return 0;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)addr, sizeof(void*));
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

static bool ir_loader_sym_dcl(ir_loader *loader, const char *name, uint32_t flags, size_t size, bool has_data)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		if (flags & IR_STATIC) {
			fprintf(l->dump_file, "static ");
		}
		fprintf(l->dump_file, "%s %s [%ld]%s\n", (flags & IR_CONST) ? "const" : "var", name, size, has_data ? " = {" : ";");
	}
	if (l->c_file) {
		ir_emit_c_sym_decl(name, flags, has_data, l->c_file);
	}
	if (l->llvm_file) {
		ir_emit_llvm_sym_decl(name, flags, has_data, l->llvm_file);
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *data = ir_mem_malloc(size);

		if (!ir_loader_add_sym(loader, name, data)) {
			ir_mem_free(data);
			return 0;
		}
		memset(data, 0, size);
		if (has_data) {
			l->data_start = l->data = data;
		}
		if (l->dump_asm) {
			ir_disasm_add_symbol(name, (uintptr_t)data, size);
		}
	} else {
		ir_loader_add_sym(loader, name, NULL);
	}
	return 1;
}

static bool ir_loader_sym_data(ir_loader *loader, ir_type type, uint32_t count, const void *data)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		const void *p = data;
		uint32_t i;

		switch (ir_type_size[type]) {
			case 1:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, "\t%s 0x%02x,\n", ir_type_cname[type], (uint32_t)*(uint8_t*)p);
					p = (void*)((uintptr_t)p + 1);
				}
				break;
			case 2:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, "\t%s 0x%04x,\n", ir_type_cname[type], (uint32_t)*(uint16_t*)p);
					p = (void*)((uintptr_t)p + 1);
				}
				break;
			case 4:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, "\t%s 0x%08x,\n", ir_type_cname[type], *(uint32_t*)p);
					p = (void*)((uintptr_t)p + 4);
				}
				break;
			case 8:
				for (i = 0; i < count; i++) {
					fprintf(l->dump_file, "\t%s 0x%016lx,\n", ir_type_cname[type], *(uint64_t*)p);
					p = (void*)((uintptr_t)p + 8);
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
		size_t size = ir_type_size[type] * count;

		if (!l->data) {
			return 0;
		}
		memcpy(l->data, data, size);
		l->data = (void*)((uintptr_t)l->data + size);
	}
	return 1;
}

static bool ir_loader_sym_data_pad(ir_loader *loader, size_t offset)
{
	ir_main_loader *l = (ir_main_loader*) loader;
	size_t i;

	IR_ASSERT(offset >= (size_t)((char*)l->data - (char*)l->data_start));
	offset -= (char*)l->data - (char*)l->data_start;
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
			memset(l->data, 0, offset);
			l->data = (void*)((uintptr_t)l->data + offset);
		}
	}
	return 1;
}

static bool ir_loader_sym_data_ref(ir_loader *loader, ir_op op, const char *ref)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	IR_ASSERT(op == IR_FUNC || op == IR_SYM);
	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		fprintf(l->dump_file, "\t%s %s(%s),\n", ir_type_cname[IR_ADDR], op == IR_FUNC ? "func" : "sym", ref);
	}
	if (l->c_file) {
		// TODO:
	}
	if (l->llvm_file) {
		// TODO:
	}
	if (l->dump_asm || l->dump_size || l->run) {
		void *addr = ir_loader_resolve_sym_name(loader, ref);

		IR_ASSERT(addr);
		memcpy(l->data, &addr, sizeof(void*));
		l->data = (void*)((uintptr_t)l->data + sizeof(void*));
	}
	return 1;
}

static bool ir_loader_sym_data_end(ir_loader *loader)
{
	ir_main_loader *l = (ir_main_loader*) loader;

	if ((l->dump & IR_DUMP_SAVE) && (l->dump_file)) {
		fprintf(l->dump_file, "};\n");
	}
	if (l->c_file) {
		// TODO:
	}
	if (l->llvm_file) {
		// TODO:
	}
	if (l->dump_asm || l->dump_size || l->run) {
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
		fprintf(l->dump_file, "\n");
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
		void *entry;

		if (l->code_buffer.start) {
			ctx->code_buffer = &l->code_buffer;
			ir_mem_unprotect(l->code_buffer.start, (char*)l->code_buffer.end - (char*)l->code_buffer.start);
		}
		entry = ir_emit_code(ctx, &size);
#ifndef _WIN32
		if (l->run) {
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
	int i, run_args = 0;
	char *input = NULL;
	char *dump_file = NULL, *c_file = NULL, *llvm_file = 0;
	FILE *f;
	bool emit_c = 0, emit_llvm = 0, dump_size = 0, dump_asm = 0, run = 0;
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

	memset(&loader, 0, sizeof(loader));
	loader.loader.default_func_flags = flags;
	loader.loader.init_module        = NULL;
	loader.loader.external_sym_dcl   = ir_loader_external_sym_dcl;
	loader.loader.external_func_dcl  = ir_loader_external_func_dcl;
	loader.loader.forward_func_dcl   = ir_loader_forward_func_dcl;
	loader.loader.sym_dcl            = ir_loader_sym_dcl;
	loader.loader.sym_data           = ir_loader_sym_data;
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
	loader.dump = dump;
	loader.dump_asm = dump_asm;
	loader.dump_size = dump_size;
	loader.run = run;

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

	if (dump_asm || dump_size || run) {
		/* Preallocate 2MB JIT code buffer. It may be necessary to generate veneers and thunks. */
		size_t size = 2 * 1024 * 1024;
		void *entry;

		loader.code_buffer.start = ir_mem_mmap(size);
		if (!loader.code_buffer.start) {
			fprintf(stderr, "ERROR: Cannot allocate JIT code buffer\n");
			return 0;
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

#if HAVE_LLVM
	if (load_llvm_bitcode) {
		if (!ir_load_llvm_bitcode(&loader.loader, input)) {
			fprintf(stderr, "ERROR: Cannot load LLVM file '%s'\n", input);
			return 1;
		}
		goto finish;
	} else if (load_llvm_asm) {
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
		if (loader.code_buffer.start) {
			loader.size = (char*)loader.code_buffer.pos - (char*)loader.code_buffer.start;
		}
		fprintf(stderr, "\ncode size = %lld\n", (long long int)loader.size);
	}

	ir_strtab_free(&loader.symtab);
	if (loader.sym) {
		ir_mem_free(loader.sym);
	}

	if (run && loader.main) {
		int jit_argc = 1;
		char **jit_argv;
		int (*func)(int, char**) = loader.main;

		if (run_args && argc > run_args) {
			jit_argc = argc - run_args + 1;
		}
		jit_argv = alloca(sizeof(char*) * jit_argc);
		jit_argv[0] = "jit code";
		for (i = 1; i < jit_argc; i++) {
			jit_argv[i] = argv[run_args + i - 1];
		}
		return func(jit_argc, jit_argv);
	}

	return 0;
}
