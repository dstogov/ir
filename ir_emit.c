/*
 * IR - Lightweight JIT Compilation Framework
 * (Native code generator based on DynAsm)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"

#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
# include "ir_x86.h"
#elif defined(IR_TARGET_AARCH64)
# include "ir_aarch64.h"
#else
# error "Unknown IR target"
#endif

#include "ir_private.h"
#include <dlfcn.h>

#define DASM_M_GROW(ctx, t, p, sz, need) \
  do { \
    size_t _sz = (sz), _need = (need); \
    if (_sz < _need) { \
      if (_sz < 16) _sz = 16; \
      while (_sz < _need) _sz += _sz; \
      (p) = (t *)ir_mem_realloc((p), _sz); \
      (sz) = _sz; \
    } \
  } while(0)

#define DASM_M_FREE(ctx, p, sz) ir_mem_free(p)

#if IR_DEBUG
# define DASM_CHECKS
#endif

typedef struct _ir_copy {
	ir_type type;
	ir_reg  from;
	ir_reg  to;
} ir_copy;

#if IR_REG_INT_ARGS
static const int8_t _ir_int_reg_params[IR_REG_INT_ARGS];
#else
static const int8_t *_ir_int_reg_params;
#endif
#if IR_REG_FP_ARGS
static const int8_t _ir_fp_reg_params[IR_REG_FP_ARGS];
#else
static const int8_t *_ir_fp_reg_params;
#endif

#ifdef IR_HAVE_FASTCALL
static const int8_t _ir_int_fc_reg_params[IR_REG_INT_FCARGS];
static const int8_t *_ir_fp_fc_reg_params;

static bool ir_is_fastcall(ir_ctx *ctx, ir_insn *insn)
{
	if (sizeof(void*) == 4) {
		if (IR_IS_CONST_REF(insn->op2)) {
			return (ctx->ir_base[insn->op2].const_flags & IR_CONST_FASTCALL_FUNC) != 0;
		} else if (ctx->ir_base[insn->op2].op == IR_BITCAST) {
			return (ctx->ir_base[insn->op2].op2 & IR_CONST_FASTCALL_FUNC) != 0;
		}
		return 0;
	}
	return 0;
}
#else
# define ir_is_fastcall(ctx, insn) 0
#endif

IR_ALWAYS_INLINE uint32_t ir_rule(ir_ctx *ctx, ir_ref ref)
{
	IR_ASSERT(!IR_IS_CONST_REF(ref));
	return ctx->rules[ref];
}

static ir_reg ir_get_param_reg(ir_ctx *ctx, ir_ref ref)
{
	ir_use_list *use_list = &ctx->use_lists[1];
	int i;
	ir_ref use, *p;
	ir_insn *insn;
	int int_param = 0;
	int fp_param = 0;
	int int_reg_params_count = IR_REG_INT_ARGS;
	int fp_reg_params_count = IR_REG_FP_ARGS;
	const int8_t *int_reg_params = _ir_int_reg_params;
	const int8_t *fp_reg_params = _ir_fp_reg_params;

#ifdef IR_HAVE_FASTCALL
	if (sizeof(void*) == 4 && (ctx->flags & IR_FASTCALL_FUNC)) {
		int_reg_params_count = IR_REG_INT_FCARGS;
		fp_reg_params_count = IR_REG_FP_FCARGS;
		int_reg_params = _ir_int_fc_reg_params;
		fp_reg_params = _ir_fp_fc_reg_params;
	}
#endif

	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < use_list->count; i++, p++) {
		use = *p;
		insn = &ctx->ir_base[use];
		if (insn->op == IR_PARAM) {
			if (IR_IS_TYPE_INT(insn->type)) {
				if (use == ref) {
					if (int_param < int_reg_params_count) {
						return int_reg_params[int_param];
					} else {
						return IR_REG_NONE;
					}
				}
				int_param++;
			} else if (IR_IS_TYPE_FP(insn->type)) {
				if (use == ref) {
					if (fp_param < fp_reg_params_count) {
						return fp_reg_params[fp_param];
					} else {
						return IR_REG_NONE;
					}
				}
				fp_param++;
			} else {
				IR_ASSERT(0);
			}
		}
	}
	return IR_REG_NONE;
}

static int ir_get_args_regs(ir_ctx *ctx, ir_insn *insn, int8_t *regs)
{
	int j, n;
	ir_type type;
	int int_param = 0;
	int fp_param = 0;
	int count = 0;
	int int_reg_params_count = IR_REG_INT_ARGS;
	int fp_reg_params_count = IR_REG_FP_ARGS;
	const int8_t *int_reg_params = _ir_int_reg_params;
	const int8_t *fp_reg_params = _ir_fp_reg_params;

#ifdef IR_HAVE_FASTCALL
	if (sizeof(void*) == 4 && ir_is_fastcall(ctx, insn)) {
		int_reg_params_count = IR_REG_INT_FCARGS;
		fp_reg_params_count = IR_REG_FP_FCARGS;
		int_reg_params = _ir_int_fc_reg_params;
		fp_reg_params = _ir_fp_fc_reg_params;
	}
#endif

	n = ir_input_edges_count(ctx, insn);
	n = IR_MIN(n, IR_MAX_REG_ARGS + 2);
	for (j = 3; j <= n; j++) {
		type = ctx->ir_base[ir_insn_op(insn, j)].type;
		if (IR_IS_TYPE_INT(type)) {
			if (int_param < int_reg_params_count) {
				regs[j] = int_reg_params[int_param];
				count = j + 1;
			} else {
				regs[j] = IR_REG_NONE;
			}
			int_param++;
		} else if (IR_IS_TYPE_FP(type)) {
			if (fp_param < fp_reg_params_count) {
				regs[j] = fp_reg_params[fp_param];
				count = j + 1;
			} else {
				regs[j] = IR_REG_NONE;
			}
			fp_param++;
		} else {
			IR_ASSERT(0);
		}
	}
	return count;
}

static bool ir_is_same_mem(ir_ctx *ctx, ir_ref r1, ir_ref r2)
{
	ir_live_interval *ival1, *ival2;
	int32_t o1, o2;

	if (IR_IS_CONST_REF(r1) || IR_IS_CONST_REF(r2)) {
		return 0;
	}

	IR_ASSERT(ctx->vregs[r1] && ctx->vregs[r2]);
	ival1 = ctx->live_intervals[ctx->vregs[r1]];
	ival2 = ctx->live_intervals[ctx->vregs[r2]];
	IR_ASSERT(ival1 && ival2);
	o1 = ival1->stack_spill_pos;
	o2 = ival2->stack_spill_pos;
	IR_ASSERT(o1 != -1 && o2 != -1);
	return o1 == o2;
}

static void *ir_resolve_sym_name(const char *name)
{
	void *handle = NULL;
	void *addr;

#ifdef RTLD_DEFAULT
	handle = RTLD_DEFAULT;
#endif
	addr = dlsym(handle, name);
	IR_ASSERT(addr != NULL);
	return addr;
}

#ifdef IR_SNAPSHOT_HANDLER_DCL
	IR_SNAPSHOT_HANDLER_DCL();
#endif

static void *ir_jmp_addr(ir_ctx *ctx, ir_insn *insn, ir_insn *addr_insn)
{
	void *addr;

	IR_ASSERT(addr_insn->type == IR_ADDR);
	if (addr_insn->op == IR_FUNC) {
		addr = ir_resolve_sym_name(ir_get_str(ctx, addr_insn->val.addr));
	} else {
		IR_ASSERT(addr_insn->op == IR_ADDR || addr_insn->op == IR_FUNC_ADDR);
		addr = (void*)addr_insn->val.addr;
	}
#ifdef IR_SNAPSHOT_HANDLER
	if (ctx->ir_base[insn->op1].op == IR_SNAPSHOT) {
		addr = IR_SNAPSHOT_HANDLER(ctx, insn->op1, &ctx->ir_base[insn->op1], addr);
	}
#endif
	return addr;
}

#if defined(__GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Warray-bounds"
# pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
# include "dynasm/dasm_proto.h"
# include "dynasm/dasm_x86.h"
#elif defined(IR_TARGET_AARCH64)
# include "dynasm/dasm_proto.h"
# include "dynasm/dasm_arm64.h"
#else
# error "Unknown IR target"
#endif

#if defined(__GNUC__)
# pragma GCC diagnostic pop
#endif

#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
# include "ir_emit_x86.h"
#elif defined(IR_TARGET_AARCH64)
# include "ir_emit_aarch64.h"
#else
# error "Unknown IR target"
#endif

int ir_match(ir_ctx *ctx)
{
	uint32_t b;
	ir_ref i, n, prev;
	ir_block *bb;
	ir_insn *insn;

	if (!ctx->prev_ref) {
		ctx->prev_ref = ir_mem_malloc(ctx->insns_count * sizeof(ir_ref));
		prev = 0;
		for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
			if (bb->flags & IR_BB_UNREACHABLE) {
				continue;
			}
			for (i = bb->start, insn = ctx->ir_base + i; i < bb->end;) {
				ctx->prev_ref[i] = prev;
				n = ir_operands_count(ctx, insn);
				n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
				prev = i;
				i += n;
				insn += n;
			}
			ctx->prev_ref[i] = prev;
		}
	}

	ctx->rules = ir_mem_calloc(ctx->insns_count, sizeof(uint32_t));
	for (b = ctx->cfg_blocks_count, bb = ctx->cfg_blocks + b; b > 0; b--, bb--) {
		if (bb->flags & IR_BB_UNREACHABLE) {
			continue;
		}
		for (i = bb->end; i > bb->start; i = ctx->prev_ref[i]) {
			insn = &ctx->ir_base[i];
			if (!ctx->rules[i]) {
				ctx->rules[i] = ir_match_insn(ctx, i, bb);
			}
		}
		ctx->rules[i] = IR_SKIP;
	}

	return 1;
}
