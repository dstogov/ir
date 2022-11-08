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

IR_ALWAYS_INLINE uint32_t ir_rule(ir_ctx *ctx, ir_ref ref)
{
	IR_ASSERT(!IR_IS_CONST_REF(ref));
	return ctx->rules[ref];
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
# pragma GCC push_options
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
# pragma GCC pop_options
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
	ir_ref i, n;
	ir_block *bb;
	ir_insn *insn;

	if (!ctx->prev_insn_len) {
		ctx->prev_insn_len = ir_mem_malloc(ctx->insns_count * sizeof(uint32_t));
		n = 1;
		for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
			if (bb->flags & IR_BB_UNREACHABLE) {
				continue;
			}
			for (i = bb->start, insn = ctx->ir_base + i; i <= bb->end;) {
				ctx->prev_insn_len[i] = n;
				n = ir_operands_count(ctx, insn);
				n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
				i += n;
				insn += n;
			}
		}
	}

	ctx->rules = ir_mem_calloc(ctx->insns_count, sizeof(uint32_t));
	for (b = ctx->cfg_blocks_count, bb = ctx->cfg_blocks + b; b > 0; b--, bb--) {
		if (bb->flags & IR_BB_UNREACHABLE) {
			continue;
		}
		for (i = bb->end; i >= bb->start; i -= ctx->prev_insn_len[i]) {
			insn = &ctx->ir_base[i];
			if (!ctx->rules[i]) {
				ctx->rules[i] = ir_match_insn(ctx, i, bb);
			}
		}
	}

	return 1;
}
