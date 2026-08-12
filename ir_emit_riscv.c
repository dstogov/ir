/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V 64 code generator --- hand-written binary encoding, no DynAsm)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 *          Meng Zhuo <mengzhuo@iscas.ac.cn>
 *
 * Scope so far: IR_ADD and IR_RETURN, integer registers only, plus the
 * IR_START/IR_PARAM control/data markers needed to get a leaf function
 * (two int params, one ADD, one RETURN) through end to end.
 *
 * Verified correct against riscv64-linux-gnu-objdump on 2026-07-21 for
 * `int add(int a, int b) { return a + b; }`:
 *   add  t0, a1, a0
 *   addi a0, t0, 0      (mv a0, t0)
 *   jalr zero, 0(ra)    (ret)
 *
 * Per Dmitry's guidance on PR #169: next step is expanding to
 * SUB/MUL and other simple ops before wider control flow.
 */

#include "ir.h"

#if defined(IR_TARGET_RISCV64)
# include "ir_riscv.h"
#else
# error "ir_emit_riscv.c is RISC-V specific"
#endif

#include "ir_private.h"
#include "ir_riscv_enc.h"
#include <limits.h>

#ifndef _WIN32
# include <dlfcn.h>
#endif

#define IR_SPILL_POS_TO_OFFSET(offset) \
	((offset) + ctx->call_stack_size)

typedef struct _rv_str_fixup {
	int32_t patch_offset;
	ir_ref  str_ref;
	bool    raw;   /* emit the constant's 8 raw bytes instead of a string */
	struct _rv_str_fixup *next;
} rv_str_fixup;

static rv_str_fixup *rv_str_fixups;

/* branch fixups: B-type/JAL emitted as 4-byte placeholders, patched once
 * every block's position is known (single pass, fixed branch size) */
typedef struct _rv_br_fixup {
	int32_t  patch_off;      /* function-relative offset of the branch */
	uint32_t target_block;
	uint8_t  f3;             /* B-type funct3: BNE=1, BEQ=0 */
	uint8_t  is_jal;
	int32_t  cond_reg;       /* rs1 for B-type branches */
	struct _rv_br_fixup *next;
} rv_br_fixup;

static rv_br_fixup *rv_br_fixups;

int32_t ir_get_spill_slot_offset(const ir_ctx *ctx, ir_ref ref)
{
	int32_t offset;

	IR_ASSERT(ref >= 0 && ctx->vregs[ref] && ctx->live_intervals[ctx->vregs[ref]]);
	offset = ctx->live_intervals[ctx->vregs[ref]]->stack_spill_pos;
	IR_ASSERT(offset != -1);
	return IR_SPILL_POS_TO_OFFSET(offset);
}

/* ---- code-generation rules (parallel to ctx->rules[]) ---- */
enum {
	IR_RISCV_RULE_NONE = 0,
	IR_RISCV_RULE_ADD,
	IR_RISCV_RULE_SUB,
	IR_RISCV_RULE_MUL,
	IR_RISCV_RULE_AND,
	IR_RISCV_RULE_OR,
	IR_RISCV_RULE_XOR,
	IR_RISCV_RULE_SHL,
	IR_RISCV_RULE_SHR,
	IR_RISCV_RULE_SAR,
	IR_RISCV_RULE_NEG,
	IR_RISCV_RULE_NOT,
	IR_RISCV_RULE_RETURN,
	IR_RISCV_RULE_CALL,
	IR_RISCV_RULE_LOAD,
	IR_RISCV_RULE_STORE,
	IR_RISCV_RULE_ZEXT,
	IR_RISCV_RULE_SEXT,
	IR_RISCV_RULE_TRUNC,
	IR_RISCV_RULE_DIV,
	IR_RISCV_RULE_MOD,
	IR_RISCV_RULE_MIN,
	IR_RISCV_RULE_MAX,
	IR_RISCV_RULE_ROL,
	IR_RISCV_RULE_ROR,
	IR_RISCV_RULE_CTLZ,
	IR_RISCV_RULE_CTTZ,
	IR_RISCV_RULE_CTPOP,
	IR_RISCV_RULE_COND,
	IR_RISCV_RULE_IF,
	IR_RISCV_RULE_BITCAST,
	IR_RISCV_RULE_EQ,
	IR_RISCV_RULE_NE,
	IR_RISCV_RULE_LT,
	IR_RISCV_RULE_GT,
	IR_RISCV_RULE_LE,
	IR_RISCV_RULE_GE,
	/* IR_ALLOCA keeps the IR opcode value: the generic RA's vars-list
	 * check compares the rule against IR_ALLOCA. */
	IR_RISCV_RULE_ALLOCA = IR_ALLOCA,
	IR_RISCV_RULE_INT2FP,  /* IR_INT2FP -> fcvt (GPR to FP) */
	IR_RISCV_RULE_FP2INT,  /* IR_FP2INT -> fcvt (FP to GPR) */
	IR_RISCV_RULE_GUARD,   /* IR_GUARD -> conditional branch to a deopt target */
	IR_RISCV_RULE_GUARD_NOT,
	IR_RISCV_RULE_SKIP,    /* IR_START and other pure control markers -> no code emitted */
	IR_RISCV_RULE_PARAM,   /* IR_PARAM  -> no code emitted; def_reg pinned to a0-a7 per ABI */
	IR_RISCV_RULE_VLOAD,   /* IR_VLOAD  -> load from a VAR stack slot */
	IR_RISCV_RULE_VSTORE,  /* IR_VSTORE -> store to a VAR stack slot */
	IR_RISCV_RULE_VADDR,   /* IR_VADDR  -> address of a VAR stack slot */
	IR_RISCV_RULE_SWITCH,  /* IR_SWITCH -> compare chain over CASE_VALs */
	IR_RISCV_RULE_IJMP,    /* IR_IJMP   -> unconditional jump to an address */
	IR_RISCV_RULE_VA_START,/* IR_VA_START -> initialize a riscv64 va_list */
	IR_RISCV_RULE_VA_ARG,  /* IR_VA_ARG -> fetch the next variadic argument */
	IR_RISCV_RULE_FP2FP,   /* IR_FP2FP -> fcvt between float and double */
	IR_RISCV_RULE_ARGVAL,  /* IR_ARGVAL -> fused into CALL: struct-by-value */
	IR_RISCV_RULE_VA_COPY, /* IR_VA_COPY -> copy a riscv64 va_list */
};

static ir_reg ir_get_param_reg(const ir_ctx *ctx, ir_ref ref)
{
	ir_insn *insn = &ctx->ir_base[ref];
	int32_t pos = insn->op3;

	if (pos >= 1 && pos <= 8) {
		if (IR_IS_TYPE_FP(insn->type)) {
			static const ir_reg fp_arg_regs[8] = {
				IR_REG_FA0, IR_REG_FA1, IR_REG_FA2, IR_REG_FA3,
				IR_REG_FA4, IR_REG_FA5, IR_REG_FA6, IR_REG_FA7,
			};

			return fp_arg_regs[pos - 1];
		} else {
			static const ir_reg gp_arg_regs[8] = {
				IR_REG_A0, IR_REG_A1, IR_REG_A2, IR_REG_A3,
				IR_REG_A4, IR_REG_A5, IR_REG_A6, IR_REG_A7,
			};

			return gp_arg_regs[pos - 1];
		}
	}
	return IR_REG_NONE; /* stack-passed, beyond 8 args --- not handled yet */
}

static bool ir_is_addr_const(const ir_ctx *ctx, ir_ref ref)
{
	return IR_IS_CONST_REF(ref)
		&& (ctx->ir_base[ref].op == IR_SYM || ctx->ir_base[ref].op == IR_FUNC
			|| ctx->ir_base[ref].op == IR_FUNC_ADDR || ctx->ir_base[ref].op == IR_STR);
}

const ir_proto_t *ir_call_proto(const ir_ctx *ctx, const ir_insn *insn)
{
	if (IR_IS_CONST_REF(insn->op2)) {
		const ir_insn *func = &ctx->ir_base[insn->op2];

		if (func->op == IR_FUNC || func->op == IR_FUNC_ADDR) {
			if (func->proto) {
				return (const ir_proto_t *)ir_get_str(ctx, func->proto);
			}
		}
	} else if (ctx->ir_base[insn->op2].op == IR_PROTO) {
		return (const ir_proto_t *)ir_get_str(ctx, ctx->ir_base[insn->op2].op2);
	}
	return NULL;
}

static int ir_get_args_regs(const ir_ctx *ctx, const ir_insn *insn, const ir_call_conv_dsc *cc, int8_t *regs)
{
	int j, n;
	ir_type type;
	int int_param = 0;
	int fp_param = 0;
	int count = 0;
	/* riscv64 psABI: unnamed (variadic) arguments are passed in general
	 * purpose registers regardless of their type. */
	int last_named;
	const ir_proto_t *proto = ir_call_proto(ctx, insn);

	if (proto && (proto->flags & IR_VARARG_FUNC)) {
		last_named = proto->params_count + 2;
	} else {
		last_named = INT_MAX;
	}

	n = insn->inputs_count;
	n = IR_MIN(n, IR_MAX_REG_ARGS + 2);
	for (j = 3; j <= n; j++) {
		ir_ref arg_ref = ir_insn_op(insn, j);
		ir_insn *arg = &ctx->ir_base[arg_ref];

		if (arg_ref == IR_UNUSED) {
			continue;
		}
		type = arg->type;
		if (IR_IS_TYPE_INT(type) || (j > last_named && IR_IS_TYPE_FP(type))) {
			if (arg->op == IR_ARGVAL) {
				/* struct-by-value: the callee gets the address of a stack copy */
				if (int_param < cc->int_param_regs_count) {
					regs[j] = cc->int_param_regs[int_param];
					count = j + 1;
				} else {
					regs[j] = IR_REG_NONE;
				}
				int_param++;
			} else if (int_param < cc->int_param_regs_count) {
				regs[j] = cc->int_param_regs[int_param];
				count = j + 1;
				int_param++;
			} else {
				regs[j] = IR_REG_NONE;
			}
		} else {
			if (!IR_IS_TYPE_FP(type)) return 0;
			if (fp_param < cc->fp_param_regs_count) {
				regs[j] = cc->fp_param_regs[fp_param];
				count = j + 1;
				fp_param++;
			} else {
				regs[j] = IR_REG_NONE;
			}
		}
	}
	return count;
}

static void *ir_call_addr(ir_ctx *ctx, ir_insn *insn, ir_insn *addr_insn)
{
	void *addr;

	IR_ASSERT(addr_insn->type == IR_ADDR);
	if (addr_insn->op == IR_FUNC) {
		addr = (ctx->loader && ctx->loader->resolve_sym_name) ?
			ctx->loader->resolve_sym_name(ctx->loader, ctx, addr_insn->val.name, IR_RESOLVE_SYM_ADD_THUNK) :
			ir_resolve_sym_name(ir_get_str(ctx, addr_insn->val.name));
		IR_ASSERT(addr);
	} else {
		IR_ASSERT(addr_insn->op == IR_ADDR || addr_insn->op == IR_FUNC_ADDR);
		addr = (void*)addr_insn->val.addr;
	}
	return addr;
}

/* -------------------------------------------------------------------- */
/* ir_match()                                                            */
/* -------------------------------------------------------------------- */

int ir_match(ir_ctx *ctx)
{
	uint32_t b;

	/* VERIFY: ir_mem_calloc is the allocator convention used elsewhere
	 * in ir.c/ir_ra.c for ctx-owned buffers --- confirm the exact name
	 * (could be ir_mem_malloc + memset, or a different allocator). */
	ctx->rules = ir_mem_calloc(ctx->insns_count, sizeof(uint32_t));
	if (!ctx->rules) {
		return 0;
	}

	for (b = 1; b <= ctx->cfg_blocks_count; b++) {
		ir_block *bb = &ctx->cfg_blocks[b];
		ir_ref ref;
		ir_insn *insn;

		/* VERIFY: bb->start / bb->end are the field names used in
		 * ir_cfg.c for the first/last instruction ref of a block ---
		 * confirmed only indirectly (bb->start appears in an
		 * ir_cfg.c snippet). Confirm bb->end exists under that name;
		 * it may instead be derived some other way (e.g. via the
		 * next block's start, or a separate "last insn" field). */
		for (ref = bb->start; ref <= bb->end; ref += ir_insn_len(insn)) {
			insn = &ctx->ir_base[ref];

			switch (insn->op) {
				case IR_START:
					ctx->rules[ref] = IR_RISCV_RULE_SKIP;
					break;
				case IR_PARAM:
					ctx->rules[ref] = IR_RISCV_RULE_PARAM;
					break;
				case IR_ALLOCA:
					if (IR_IS_CONST_REF(insn->op2)) {
						/* IR_SKIPPED makes the RA put the static alloca on
						 * the vars list so it gets a frame slot */
						ctx->rules[ref] = IR_RISCV_RULE_ALLOCA | IR_SKIPPED;
					} else {
						ctx->status = IR_ERROR_UNSUPPORTED_CODE_RULE;
						return 0;
					}
					break;
				case IR_VAR:
					/* static stack slot, same mechanism as static ALLOCA */
					ctx->rules[ref] = IR_RISCV_RULE_ALLOCA | IR_SKIPPED;
					break;
				case IR_VLOAD:
				case IR_VLOAD_v:
					ctx->rules[ref] = IR_RISCV_RULE_VLOAD;
					break;
				case IR_VSTORE:
				case IR_VSTORE_v:
					ctx->rules[ref] = IR_RISCV_RULE_VSTORE;
					break;
				case IR_VADDR:
					ctx->rules[ref] = IR_RISCV_RULE_VADDR;
					break;
				case IR_SWITCH:
					ctx->rules[ref] = IR_RISCV_RULE_SWITCH;
					break;
				case IR_ARGVAL:
					ctx->rules[ref] = IR_FUSED | IR_RISCV_RULE_ARGVAL;
					break;
				case IR_CASE_VAL:
				case IR_CASE_RANGE:
				case IR_CASE_DEFAULT:
					ctx->rules[ref] = IR_RISCV_RULE_SKIP;
					break;
				case IR_IJMP:
					ctx->rules[ref] = IR_RISCV_RULE_IJMP;
					break;
				case IR_VA_START:
					ctx->flags2 |= IR_HAS_VA_START;
					ctx->rules[ref] = IR_RISCV_RULE_VA_START;
					break;
				case IR_VA_ARG:
					ctx->flags2 |= IR_HAS_VA_ARG_GP;
					ctx->rules[ref] = IR_RISCV_RULE_VA_ARG;
					break;
				case IR_VA_END:
					ctx->rules[ref] = IR_RISCV_RULE_SKIP;
					break;
				case IR_VA_COPY:
					ctx->rules[ref] = IR_RISCV_RULE_VA_COPY;
					break;
				case IR_INT2FP:
					ctx->rules[ref] = IR_RISCV_RULE_INT2FP;
					break;
				case IR_FP2FP:
					ctx->rules[ref] = IR_RISCV_RULE_FP2FP;
					break;
				case IR_FP2INT:
					ctx->rules[ref] = IR_RISCV_RULE_FP2INT;
					break;
				case IR_GUARD:
				case IR_GUARD_NOT:
					/* the deopt fallback is an internal call; the function
					 * must save ra across it */
					ctx->flags2 |= IR_HAS_CALLS | IR_16B_FRAME_ALIGNMENT;
					ctx->rules[ref] = (insn->op == IR_GUARD)
						? IR_RISCV_RULE_GUARD : IR_RISCV_RULE_GUARD_NOT;
					break;
				case IR_ADD:
					ctx->rules[ref] = IR_RISCV_RULE_ADD;
					break;
				case IR_SUB:
					ctx->rules[ref] = IR_RISCV_RULE_SUB;
					break;
				case IR_MUL:
					ctx->rules[ref] = IR_RISCV_RULE_MUL;
					break;
				case IR_AND:
					ctx->rules[ref] = IR_RISCV_RULE_AND;
					break;
				case IR_OR:
					ctx->rules[ref] = IR_RISCV_RULE_OR;
					break;
				case IR_XOR:
					ctx->rules[ref] = IR_RISCV_RULE_XOR;
					break;
				case IR_SHL:
					ctx->rules[ref] = IR_RISCV_RULE_SHL;
					break;
				case IR_SHR:
					ctx->rules[ref] = IR_RISCV_RULE_SHR;
					break;
				case IR_SAR:
					ctx->rules[ref] = IR_RISCV_RULE_SAR;
					break;
				case IR_NEG:
					ctx->rules[ref] = IR_RISCV_RULE_NEG;
					break;
				case IR_NOT:
					ctx->rules[ref] = IR_RISCV_RULE_NOT;
					break;
				case IR_RETURN:
					ctx->rules[ref] = IR_RISCV_RULE_RETURN;
					break;
				case IR_CALL:
					ctx->flags2 |= IR_HAS_CALLS | IR_16B_FRAME_ALIGNMENT;
					ctx->rules[ref] = IR_RISCV_RULE_CALL;
					break;
				case IR_LOAD:
					ctx->rules[ref] = IR_RISCV_RULE_LOAD;
					break;
				case IR_STORE:
					ctx->rules[ref] = IR_RISCV_RULE_STORE;
					break;
				case IR_ZEXT:
					ctx->rules[ref] = IR_RISCV_RULE_ZEXT;
					break;
				case IR_SEXT:
					ctx->rules[ref] = IR_RISCV_RULE_SEXT;
					break;
				case IR_TRUNC:
					ctx->rules[ref] = IR_RISCV_RULE_TRUNC;
					break;
				case IR_DIV:
					ctx->rules[ref] = IR_RISCV_RULE_DIV;
					break;
				case IR_MOD:
					ctx->rules[ref] = IR_RISCV_RULE_MOD;
					break;
				case IR_MIN:
					ctx->rules[ref] = IR_RISCV_RULE_MIN;
					break;
				case IR_MAX:
					ctx->rules[ref] = IR_RISCV_RULE_MAX;
					break;
				case IR_ROL:
					ctx->rules[ref] = IR_RISCV_RULE_ROL;
					break;
				case IR_ROR:
					ctx->rules[ref] = IR_RISCV_RULE_ROR;
					break;
				case IR_CTLZ:
					ctx->rules[ref] = IR_RISCV_RULE_CTLZ;
					break;
				case IR_CTTZ:
					ctx->rules[ref] = IR_RISCV_RULE_CTTZ;
					break;
				case IR_CTPOP:
					ctx->rules[ref] = IR_RISCV_RULE_CTPOP;
					break;
				case IR_COND:
					ctx->rules[ref] = IR_RISCV_RULE_COND;
					break;
				case IR_IF:
					ctx->rules[ref] = IR_RISCV_RULE_IF;
					break;
				case IR_BITCAST:
					ctx->rules[ref] = IR_RISCV_RULE_BITCAST;
					break;
				case IR_IF_TRUE:
				case IR_IF_FALSE:
				case IR_MERGE:
				case IR_LOOP_BEGIN:
				case IR_LOOP_END:
				case IR_END:
				case IR_BEGIN:
				case IR_PHI:
					ctx->rules[ref] = IR_RISCV_RULE_SKIP;
					break;
				case IR_EQ:
					ctx->rules[ref] = IR_RISCV_RULE_EQ;
					break;
				case IR_NE:
					ctx->rules[ref] = IR_RISCV_RULE_NE;
					break;
				case IR_LT:
					ctx->rules[ref] = IR_RISCV_RULE_LT;
					break;
				case IR_GT:
					ctx->rules[ref] = IR_RISCV_RULE_GT;
					break;
				case IR_LE:
					ctx->rules[ref] = IR_RISCV_RULE_LE;
					break;
				case IR_GE:
					ctx->rules[ref] = IR_RISCV_RULE_GE;
					break;
				case IR_ULT:
					ctx->rules[ref] = IR_RISCV_RULE_LT;
					break;
				case IR_UGT:
					ctx->rules[ref] = IR_RISCV_RULE_GT;
					break;
				case IR_ULE:
					ctx->rules[ref] = IR_RISCV_RULE_LE;
					break;
				case IR_UGE:
					ctx->rules[ref] = IR_RISCV_RULE_GE;
					break;
				default:
					/* Not yet supported --- leave rule as
					 * IR_RISCV_RULE_NONE. Anything reaching
					 * ir_emit_code() with rule == NONE should
					 * be treated as an unimplemented-op error,
					 * not silently skipped. */
					break;
			}

			if (ctx->rules[ref] != IR_RISCV_RULE_NONE
			 && (ir_op_flags[insn->op] & IR_OP_FLAG_DATA)
			 && ctx->use_lists[ref].count == 0) {
				ctx->rules[ref] |= IR_SKIPPED;
			}
		}
	}

	return 1;
}

/* -------------------------------------------------------------------- */
/* ir_get_target_constraints()                                          */
/* -------------------------------------------------------------------- */

int ir_get_target_constraints(ir_ctx *ctx, ir_ref ref, ir_target_constraints *constraints)
{
	/* Single shared exit point, matching ir_x86.dasc's structure:
	 * every case sets `flags` (and `n` if it uses tmp_regs[]) and
	 * breaks, rather than returning directly. constraints->tmps_count
	 * must be set from `n` right before the return --- confirmed against
	 * ir_x86.dasc, where that assignment runs immediately before the
	 * function's one and only `return flags;`. */
	int flags = 0;
	int n = 0;
	ir_insn *insn;

	constraints->def_reg = IR_REG_NONE;
	constraints->hints_count = 0;

	switch (ctx->rules[ref]) {
		case IR_RISCV_RULE_SKIP:
			flags = 0;
			break;

		case IR_RISCV_RULE_PARAM:
			constraints->def_reg = ir_get_param_reg(ctx, ref);
			flags = (constraints->def_reg != IR_REG_NONE) ? IR_USE_SHOULD_BE_IN_REG : 0;
			break;

		case IR_RISCV_RULE_ADD:
		case IR_RISCV_RULE_SUB:
		case IR_RISCV_RULE_MUL:
		case IR_RISCV_RULE_AND:
		case IR_RISCV_RULE_OR:
		case IR_RISCV_RULE_XOR:
		case IR_RISCV_RULE_SHL:
		case IR_RISCV_RULE_SHR:
		case IR_RISCV_RULE_SAR:
			flags = IR_USE_MUST_BE_IN_REG;
			if (!IR_IS_CONST_REF(ctx->ir_base[ref].op1)) {
				if (ctx->ir_base[ctx->ir_base[ref].op1].op == IR_ALLOCA) {
					constraints->tmp_regs[n] = IR_TMP_REG(1, IR_ADDR,
					                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
					n++;
				} else {
					flags |= IR_OP1_MUST_BE_IN_REG;
				}
			} else if (IR_IS_TYPE_FP(ctx->ir_base[ref].type)) {
				constraints->tmp_regs[n] = IR_TMP_REG(1, ctx->ir_base[ref].type,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			} else if (ir_is_addr_const(ctx, ctx->ir_base[ref].op1)) {
				constraints->tmp_regs[n] = IR_TMP_REG(1, IR_ADDR, IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			if (!IR_IS_CONST_REF(ctx->ir_base[ref].op2)) {
				if (ctx->ir_base[ctx->ir_base[ref].op2].op == IR_ALLOCA) {
					constraints->tmp_regs[n] = IR_TMP_REG(2, IR_ADDR,
					                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
					n++;
				} else {
					flags |= IR_OP2_MUST_BE_IN_REG;
				}
			} else if (IR_IS_TYPE_FP(ctx->ir_base[ref].type)) {
				constraints->tmp_regs[n] = IR_TMP_REG(2, ctx->ir_base[ref].type,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			} else if (ir_is_addr_const(ctx, ctx->ir_base[ref].op2)) {
				constraints->tmp_regs[n] = IR_TMP_REG(2, IR_ADDR, IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			break;

		case IR_RISCV_RULE_NEG:
		case IR_RISCV_RULE_NOT:
			flags = IR_OP1_MUST_BE_IN_REG | IR_USE_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_CALL: {
			const ir_proto_t *proto;
			const ir_call_conv_dsc *cc;

			insn = &ctx->ir_base[ref];
			proto = ir_call_proto(ctx, insn);
			cc = ir_get_call_conv_dsc(proto ? proto->flags : IR_CC_DEFAULT);
			if (insn->type != IR_VOID) {
				constraints->def_reg = IR_IS_TYPE_INT(insn->type)
					? cc->int_ret_reg : cc->fp_ret_reg;
			}
			constraints->tmp_regs[0] = IR_SCRATCH_REG(cc->scratch_reg, IR_USE_SUB_REF, IR_DEF_SUB_REF);
			n = 1;
			if (insn->inputs_count > 2) {
				int a;

				for (a = 3; a <= insn->inputs_count; a++) {
					ir_ref arg = ir_insn_op(insn, a);

					if (IR_IS_CONST_REF(arg) && IR_IS_TYPE_FP(ctx->ir_base[arg].type)) {
						constraints->tmp_regs[n] = IR_SCRATCH_REG(IR_REG_T5,
						                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
						n++;
						break;
					}
				}
				constraints->hints[2] = IR_REG_NONE;
				constraints->hints_count = ir_get_args_regs(ctx, insn, cc, constraints->hints);
				{
					/* Variadic FP args are passed in GPRs, but an FP-typed
					 * interval can only be allocated an FP register --- leave
					 * them unhinted and convert at the call site. */
					const ir_proto_t *proto = ir_call_proto(ctx, insn);
					int last_named = (proto && (proto->flags & IR_VARARG_FUNC))
						? proto->params_count + 2 : INT_MAX;
					for (a = 3; a <= insn->inputs_count; a++) {
						ir_ref arg = ir_insn_op(insn, a);

						if (a > last_named
						 && IR_IS_TYPE_FP(ctx->ir_base[arg].type)) {
							constraints->hints[a] = IR_REG_NONE;
						}
					}
				}
			}
			flags = IR_USE_SHOULD_BE_IN_REG | IR_OP2_SHOULD_BE_IN_REG | IR_OP3_SHOULD_BE_IN_REG;
			break;
		}

		case IR_RISCV_RULE_LOAD:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP2_MUST_BE_IN_REG
			      | IR_DEF_CONFLICTS_WITH_INPUT_REGS;
			insn = &ctx->ir_base[ref];
			if (ctx->ir_base[insn->op2].op == IR_ALLOCA) {
				/* the static alloca address is materialized into a temp */
				constraints->tmp_regs[n] = IR_TMP_REG(2, IR_ADDR,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			break;

		case IR_RISCV_RULE_STORE:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP2_MUST_BE_IN_REG;
			insn = &ctx->ir_base[ref];
			if (ctx->ir_base[insn->op2].op == IR_ALLOCA) {
				constraints->tmp_regs[n] = IR_TMP_REG(2, IR_ADDR,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			if (IR_IS_CONST_REF(insn->op3)) {
				constraints->tmp_regs[n] = IR_TMP_REG(3, ctx->ir_base[insn->op3].type,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			break;

		case IR_RISCV_RULE_ZEXT:
		case IR_RISCV_RULE_SEXT:
		case IR_RISCV_RULE_TRUNC:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_DIV:
		case IR_RISCV_RULE_MOD:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG;
			constraints->tmp_regs[0] = IR_TMP_REG(3, ctx->ir_base[ref].type,
			                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
			n = 1;
			break;

		case IR_RISCV_RULE_MIN:
		case IR_RISCV_RULE_MAX:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG
			      | IR_OP2_MUST_BE_IN_REG;
			constraints->tmp_regs[0] = IR_TMP_REG(3, ctx->ir_base[ref].type,
			                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
			n = 1;
			if (IR_IS_CONST_REF(ctx->ir_base[ref].op2)) {
				constraints->tmp_regs[n] = IR_SCRATCH_REG(IR_REG_T5,
				                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			break;
		case IR_RISCV_RULE_ROL:
		case IR_RISCV_RULE_ROR:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG
			      | IR_OP2_MUST_BE_IN_REG;
			constraints->tmp_regs[0] = IR_TMP_REG(3, ctx->ir_base[ref].type,
			                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
			n = 1;
			break;

		case IR_RISCV_RULE_CTLZ:
		case IR_RISCV_RULE_CTTZ:
		case IR_RISCV_RULE_CTPOP:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG;
			/* software implementations (RVA20U64 has no Zbb) need two
			 * scratch registers for masks and intermediates */
			constraints->tmp_regs[0] = IR_SCRATCH_REG(IR_REG_T5,
			                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
			constraints->tmp_regs[1] = IR_SCRATCH_REG(IR_REG_T6,
			                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
			n = 2;
			break;

		case IR_RISCV_RULE_IF:
			flags = IR_OP2_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_VLOAD:
			flags = IR_USE_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_VSTORE:
			flags = IR_OP3_MUST_BE_IN_REG;
			insn = &ctx->ir_base[ref];
			if (IR_IS_CONST_REF(insn->op3)) {
				constraints->tmp_regs[0] = IR_TMP_REG(3, ctx->ir_base[insn->op3].type,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n = 1;
			}
			break;

		case IR_RISCV_RULE_VADDR:
			flags = IR_USE_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_SWITCH:
			flags = IR_OP2_MUST_BE_IN_REG;
			constraints->tmp_regs[0] = IR_TMP_REG(3, IR_ADDR,
			                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
			n = 1;
			break;

		case IR_RISCV_RULE_IJMP:
			flags = 0;
			break;

		case IR_RISCV_RULE_ARGVAL:
			flags = 0;
			break;

		case IR_RISCV_RULE_VA_COPY:
			flags = IR_OP2_MUST_BE_IN_REG | IR_OP3_MUST_BE_IN_REG;
			insn = &ctx->ir_base[ref];
			if (ctx->ir_base[insn->op2].op == IR_ALLOCA) {
				constraints->tmp_regs[n] = IR_TMP_REG(2, IR_ADDR,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			if (ctx->ir_base[insn->op3].op == IR_ALLOCA) {
				constraints->tmp_regs[n] = IR_TMP_REG(3, IR_ADDR,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			break;

		case IR_RISCV_RULE_VA_START:
		case IR_RISCV_RULE_VA_ARG:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP2_MUST_BE_IN_REG;
			insn = &ctx->ir_base[ref];
			if (ctx->ir_base[insn->op2].op == IR_ALLOCA) {
				constraints->tmp_regs[0] = IR_TMP_REG(2, IR_ADDR,
				                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n = 1;
			} else {
				constraints->tmp_regs[0] = IR_SCRATCH_REG(IR_REG_T5,
				                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				constraints->tmp_regs[1] = IR_SCRATCH_REG(IR_REG_T6,
				                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n = 2;
			}
			break;

		case IR_RISCV_RULE_ALLOCA:
			flags = IR_USE_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_INT2FP:
		case IR_RISCV_RULE_FP2FP:
		case IR_RISCV_RULE_FP2INT:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_GUARD:
		case IR_RISCV_RULE_GUARD_NOT:
			flags = IR_OP2_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_BITCAST:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_COND:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG
			      | IR_OP2_MUST_BE_IN_REG | IR_OP3_MUST_BE_IN_REG;
			constraints->tmp_regs[0] = IR_SCRATCH_REG(IR_REG_T5,
			                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
			n = 1;
			break;

		case IR_RISCV_RULE_EQ:
		case IR_RISCV_RULE_NE:
		case IR_RISCV_RULE_LT:
		case IR_RISCV_RULE_GT:
		case IR_RISCV_RULE_LE:
		case IR_RISCV_RULE_GE:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG
			      | IR_OP2_MUST_BE_IN_REG;
			insn = &ctx->ir_base[ref];
			if (IR_IS_CONST_REF(insn->op1) || IR_IS_CONST_REF(insn->op2)) {
				if (IR_IS_TYPE_FP(ctx->ir_base[insn->op1].type)) {
					/* FP const operands materialize into allocated FP tmps */
					if (IR_IS_CONST_REF(insn->op1)) {
						constraints->tmp_regs[n] = IR_TMP_REG(1, ctx->ir_base[insn->op1].type,
						                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
						n++;
					}
					if (IR_IS_CONST_REF(insn->op2)) {
						constraints->tmp_regs[n] = IR_TMP_REG(2, ctx->ir_base[insn->op2].type,
						                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
						n++;
					}
				} else {
					/* INT const operands are materialized into pinned GPR scratches */
					constraints->tmp_regs[n] = IR_SCRATCH_REG(IR_REG_T5,
					                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
					n++;
					constraints->tmp_regs[n] = IR_SCRATCH_REG(IR_REG_T6,
					                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
					n++;
				}
			} else if (IR_IS_TYPE_FP(ctx->ir_base[insn->op1].type)) {
				/* NaN checks for unordered FP compares use a pinned GPR */
				constraints->tmp_regs[0] = IR_SCRATCH_REG(IR_REG_T5,
				                                          IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n = 1;
			}
			break;

		case IR_RISCV_RULE_RETURN:
			flags = IR_OP2_MUST_BE_IN_REG;
			if (!IR_IS_CONST_REF(ctx->ir_base[ref].op2)) {
				constraints->hints[2] =
					IR_IS_TYPE_INT(ctx->ir_base[ctx->ir_base[ref].op2].type)
						? IR_REG_A0 : IR_REG_FA0;
				constraints->hints_count = 3;
				if (IR_IS_TYPE_FP(ctx->ir_base[ctx->ir_base[ref].op2].type)) {
					constraints->tmp_regs[0] = IR_TMP_REG(3, IR_U64,
					                                      IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
					n = 1;
				}
			}
			break;

		default:
			flags = 0;
			break;
	}

	constraints->tmps_count = n;
	return flags;
}

/* -------------------------------------------------------------------- */
/* ir_emit_code()                                                       */
/* -------------------------------------------------------------------- */

/* Confirmed against ir_x86.dasc (ir_get_alocated_reg and the ubiquitous
 * ctx->regs[def][0] idiom): slot 0 = this instruction's own def
 * register, slot 1 = op1's register, slot 2 = op2's register, slot 3 =
 * op3's register. */
static int32_t riscv_reg_of(ir_ctx *ctx, ir_ref ref, int slot)
{
	IR_ASSERT(slot >= 0 && slot <= 3);
	return ctx->regs[ref][slot];
}

static void emit32(ir_ctx *ctx, uint32_t word)
{
	*(uint32_t *)ctx->code_buffer->pos = word;
	ctx->code_buffer->pos = (char *)ctx->code_buffer->pos + 4;
}

static void rv_emit_li64(ir_ctx *ctx, uint32_t rd, int64_t v)
{
	int32_t lo;
	int64_t hi;

	if (v >= -2048 && v <= 2047) {
		emit32(ctx, rv_addi(rd, 0, (int32_t)v));
		return;
	}
	lo = (int32_t)((v << 52) >> 52);
	hi = (v - lo) >> 12;
	if (v == (int32_t)v) {
		emit32(ctx, rv_enc_u((int32_t)(hi << 12), rd, RV_OP_LUI));
		if (lo) {
			emit32(ctx, rv_alui(1, RV_F3_ADDI, rd, rd, lo));
		}
		return;
	}
	rv_emit_li64(ctx, rd, hi);
	emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, rd, rd, 12));
	if (lo) {
		emit32(ctx, rv_addi(rd, rd, lo));
	}
}

/* Fixed 8-instruction constant form; used when a later patch must rewrite
 * the value in place (string addresses) without changing the length. */
static int rv_li64_fixed_words(uint32_t rd, int64_t v, uint32_t *out)
{
	int64_t u = v;
	int32_t g[3];
	int n = 0, i;

	for (i = 0; i < 3; i++) {
		g[i] = (int32_t)((u << 52) >> 52);
		u = (u - g[i]) >> 12;
	}
	{
		int64_t hi = (u + 0x800) >> 12;
		int32_t lo = (int32_t)(u - (hi << 12));

		out[n++] = rv_enc_u((int32_t)(hi << 12), rd, RV_OP_LUI);
		out[n++] = rv_alui(1, RV_F3_ADDI, rd, rd, lo);
	}
	for (i = 2; i >= 0; i--) {
		out[n++] = rv_shifti(0, RV_F3_SLLI, 0, rd, rd, 12);
		out[n++] = rv_addi(rd, rd, g[i]);
	}
	return n;
}

static void rv_emit_prologue(ir_ctx *ctx)
{
	int32_t frame = ctx->stack_frame_size;
	ir_regset used;
	ir_reg reg;
	int32_t off;

	if (!frame) {
		return;
	}
	IR_ASSERT(frame <= 2047);
	emit32(ctx, rv_addi(IR_REG_SP, IR_REG_SP, -frame));
	off = ctx->locals_area_size;
	used = IR_REGSET_INTERSECTION((ir_regset)ctx->used_preserved_regs, IR_REGSET_GP);
	IR_REGSET_FOREACH(used, reg) {
		emit32(ctx, rv_enc_s(off, reg, IR_REG_SP, 3 /* SD */, RV_OP_STORE));
		off += sizeof(void*);
	} IR_REGSET_FOREACH_END();
	if (ctx->flags2 & IR_HAS_CALLS) {
		emit32(ctx, rv_enc_s(off, IR_REG_RA, IR_REG_SP, 3 /* SD */, RV_OP_STORE));
		off += sizeof(void*);
	}
	/* save the used FP callee-saved registers (their numbers are
	 * 32-63, but the instruction encodes 0-31) */
	used = IR_REGSET_INTERSECTION((ir_regset)ctx->used_preserved_regs, IR_REGSET_FP);
	IR_REGSET_FOREACH(used, reg) {
		emit32(ctx, rv_enc_s(off, (uint32_t)(reg - IR_REG_FP_FIRST),
		                     IR_REG_SP, 3 /* SD */, RV_OP_STORE));
		off += sizeof(void*);
	} IR_REGSET_FOREACH_END();
	if (ctx->flags2 & IR_HAS_VA_START) {
		/* snapshot incoming GPR arguments; VA_START points reg_save_area here */
		int i;

		for (i = 0; i < 8; i++) {
			emit32(ctx, rv_enc_s(off, (uint32_t)(IR_REG_A0 + i),
			                     IR_REG_SP, 3 /* SD */, RV_OP_STORE));
			off += sizeof(void*);
		}
	}
}

static void rv_emit_restore_regs(ir_ctx *ctx)
{
	int32_t frame = ctx->stack_frame_size;
	ir_regset used;
	ir_reg reg;
	int32_t off;

	if (frame) {
		off = ctx->locals_area_size;
		used = IR_REGSET_INTERSECTION((ir_regset)ctx->used_preserved_regs, IR_REGSET_GP);
		IR_REGSET_FOREACH(used, reg) {
			emit32(ctx, rv_enc_i(off, IR_REG_SP, 3 /* LD */, reg, RV_OP_LOAD));
			off += sizeof(void*);
		} IR_REGSET_FOREACH_END();
		if (ctx->flags2 & IR_HAS_CALLS) {
			emit32(ctx, rv_enc_i(off, IR_REG_SP, 3 /* LD */, IR_REG_RA, RV_OP_LOAD));
			off += sizeof(void*);
		}
		used = IR_REGSET_INTERSECTION((ir_regset)ctx->used_preserved_regs, IR_REGSET_FP);
		IR_REGSET_FOREACH(used, reg) {
			emit32(ctx, rv_enc_i(off, IR_REG_SP, 3 /* LD */,
			                     (uint32_t)(reg - IR_REG_FP_FIRST), RV_OP_LOAD));
			off += sizeof(void*);
		} IR_REGSET_FOREACH_END();
		emit32(ctx, rv_addi(IR_REG_SP, IR_REG_SP, frame));
	}
}

static void rv_emit_epilogue(ir_ctx *ctx)
{
	rv_emit_restore_regs(ctx);
	emit32(ctx, rv_ret(IR_REG_RA));
}

typedef struct _rv_alu_dsc {
	uint32_t r_f3;
	uint32_t r_f7;
	uint32_t i_f3;    /* OP-IMM funct3; 0xFF = no immediate form */
	int      i_neg;   /* immediate form uses the negated value (SUB -> addi -v) */
	int      i_shift; /* 0 = not a shift, 1 = logical, 2 = arithmetic */
	int      commutative;
} rv_alu_dsc;

static const rv_alu_dsc rv_alu_add = { RV_F3_ADD, RV_F7_ADD,  RV_F3_ADDI, 0, 0, 1 };
static const rv_alu_dsc rv_alu_sub = { RV_F3_SUB, RV_F7_SUB,  RV_F3_ADDI, 1, 0, 0 };
static const rv_alu_dsc rv_alu_mul = { RV_F3_MUL, RV_F7_MUL,  0xFF,       0, 0, 1 };
static const rv_alu_dsc rv_alu_and = { RV_F3_AND, RV_F7_BASE, RV_F3_ANDI, 0, 0, 1 };
static const rv_alu_dsc rv_alu_or  = { RV_F3_OR,  RV_F7_BASE, RV_F3_ORI,  0, 0, 1 };
static const rv_alu_dsc rv_alu_xor = { RV_F3_XOR, RV_F7_BASE, RV_F3_XORI, 0, 0, 1 };
static const rv_alu_dsc rv_alu_shl = { RV_F3_SLL, RV_F7_BASE, RV_F3_SLLI, 0, 1, 0 };
static const rv_alu_dsc rv_alu_shr = { RV_F3_SR,  RV_F7_SRL,  RV_F3_SRI,  0, 1, 0 };
static const rv_alu_dsc rv_alu_sar = { RV_F3_SR,  RV_F7_SRA,  RV_F3_SRI,  0, 2, 0 };

static uint32_t rv_load_f3(ir_type type)
{
	switch (ir_type_size[type]) {
		case 1: return IR_IS_TYPE_SIGNED(type) ? RV_F3_LB : RV_F3_LBU;
		case 2: return IR_IS_TYPE_SIGNED(type) ? RV_F3_LH : RV_F3_LHU;
		case 4: return IR_IS_TYPE_SIGNED(type) ? RV_F3_LW : RV_F3_LWU;
		default: return RV_F3_LD;
	}
}

static uint32_t rv_store_f3(ir_type type)
{
	switch (ir_type_size[type]) {
		case 1: return RV_F3_SB;
		case 2: return RV_F3_SH;
		case 4: return RV_F3_SW;
		default: return RV_F3_SD;
	}
}

/* Materialize an FP value into FP reg `dst` (fp reg number 0-31).
 * gpr: a scratch GPR for address/constant handling. */
static bool rv_emit_get_fp(ir_ctx *ctx, int32_t dst, ir_ref val_ref, int32_t gpr)
{
	IR_ASSERT(dst >= 0 && dst < 32);
	int32_t home;
	int d = 1; /* doubles (the IR keeps floats as doubles in regs) */

	if (IR_IS_CONST_REF(val_ref)) {
		rv_str_fixup *fx = ir_mem_malloc(sizeof(rv_str_fixup));
		uint32_t words[8];
		int n = rv_li64_fixed_words((uint32_t)gpr, 0, words);
		int i;
		ir_type t = ctx->ir_base[val_ref].type;

		if (gpr == IR_REG_NONE) return false;
		fx->patch_offset = (int32_t)((char*)ctx->code_buffer->pos - (char*)ctx->code_buffer->start);
		fx->str_ref = val_ref;
		fx->raw = true;
		fx->next = rv_str_fixups;
		rv_str_fixups = fx;
		for (i = 0; i < n; i++) {
			emit32(ctx, words[i]);
		}
		/* float constants must be loaded with FLW (32-bit): an 8-byte
		 * load zero-fills the upper half, which on some targets makes a
		 * subsequent fadd.s misbehave. */
		emit32(ctx, rv_enc_i(0, (uint32_t)gpr,
		                     ir_type_size[t] <= 4 ? 2 /* FLW */ : 3 /* LD */,
		                     (uint32_t)dst, 0x07 /* LOAD-FP */));
		return true;
	}
	home = ctx->regs[val_ref][0];
	if (home == IR_REG_NONE) {
		return false;
	}
	if (IR_REG_SPILLED(home)) {
		int32_t off = ir_get_spill_slot_offset(ctx, val_ref);

		if (off < -2048 || off > 2047) return false;
		emit32(ctx, rv_load(3 /* LD double */, (uint32_t)dst, IR_REG_SP, off));
		return true;
	}
	if (home != IR_REG_FP_FIRST + dst) {
		emit32(ctx, rv_fop(d, RV_F7_FSGNJ, 0,
		                   (uint32_t)dst, (uint32_t)(home - IR_REG_FP_FIRST),
		                   (uint32_t)(home - IR_REG_FP_FIRST)));
	}
	return true;
}

static void rv_emit_store_def(ir_ctx *ctx, ir_ref ref, ir_type type, int32_t reg)
{
	int32_t off = ir_get_spill_slot_offset(ctx, ref);

	IR_ASSERT(off >= -2048 && off <= 2047);
	emit32(ctx, rv_store(rv_store_f3(type), (uint32_t)reg, IR_REG_SP, off));
}

/* RVA20U64 popcount: no Zbb, so use the classic divide-and-conquer
 * sequence (rd may alias rs; T5/T6 are scratch). */
static void rv_emit_soft_cpop(ir_ctx *ctx, int32_t rd, int32_t rs)
{
	/* consume rs up front: T5/T6 are reused as mask/intermediate below */
	if (rs != rd) {
		emit32(ctx, rv_mv((uint32_t)rd, (uint32_t)rs));
	}
	rv_emit_li64(ctx, IR_REG_T5, 0x5555555555555555LL);
	emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rd, 1));
	emit32(ctx, rv_alu(0, 0, RV_F3_AND, (uint32_t)IR_REG_T6, (uint32_t)IR_REG_T6, (uint32_t)IR_REG_T5));
	emit32(ctx, rv_alu(0, RV_F7_SUB, RV_F3_SUB, (uint32_t)rd, (uint32_t)rd, (uint32_t)IR_REG_T6));
	rv_emit_li64(ctx, IR_REG_T5, 0x3333333333333333LL);
	emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rd, 2));
	emit32(ctx, rv_alu(0, 0, RV_F3_AND, (uint32_t)IR_REG_T6, (uint32_t)IR_REG_T6, (uint32_t)IR_REG_T5));
	emit32(ctx, rv_alu(0, 0, RV_F3_AND, (uint32_t)rd, (uint32_t)rd, (uint32_t)IR_REG_T5));
	emit32(ctx, rv_alu(0, 0, RV_F3_ADD, (uint32_t)rd, (uint32_t)rd, (uint32_t)IR_REG_T6));
	rv_emit_li64(ctx, IR_REG_T5, 0x0f0f0f0f0f0f0f0fLL);
	emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rd, 4));
	emit32(ctx, rv_alu(0, 0, RV_F3_ADD, (uint32_t)IR_REG_T6, (uint32_t)rd, (uint32_t)IR_REG_T6));
	emit32(ctx, rv_alu(0, 0, RV_F3_AND, (uint32_t)rd, (uint32_t)IR_REG_T6, (uint32_t)IR_REG_T5));
	emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rd, 8));
	emit32(ctx, rv_alu(0, 0, RV_F3_ADD, (uint32_t)rd, (uint32_t)rd, (uint32_t)IR_REG_T6));
	emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rd, 16));
	emit32(ctx, rv_alu(0, 0, RV_F3_ADD, (uint32_t)rd, (uint32_t)rd, (uint32_t)IR_REG_T6));
	emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rd, 32));
	emit32(ctx, rv_alu(0, 0, RV_F3_ADD, (uint32_t)rd, (uint32_t)rd, (uint32_t)IR_REG_T6));
	emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rd, (uint32_t)rd, 0x7f));
}

static bool rv_emit_get(ir_ctx *ctx, int32_t reg, ir_ref val_ref)
{
	int32_t home;

	if (ctx->ir_base[val_ref].op == IR_ALLOCA) {
		ir_insn *ai = &ctx->ir_base[val_ref];
		int32_t off = IR_SPILL_POS_TO_OFFSET(ai->op3);

		if (off < -2048 || off > 2047) return false;
		emit32(ctx, rv_addi((uint32_t)reg, IR_REG_SP, off));
		return true;
	}
	if (IR_IS_CONST_REF(val_ref)) {
		const ir_insn *ai = &ctx->ir_base[val_ref];
		int64_t v;

		if (!IR_IS_TYPE_INT(ai->type)) {
			return false;
		}
		if (ai->op == IR_SYM) {
			v = (int64_t)(uintptr_t)((ctx->loader && ctx->loader->resolve_sym_name)
				? ctx->loader->resolve_sym_name(ctx->loader, ctx, ai->val.name, IR_RESOLVE_SYM_SILENT)
				: ir_resolve_sym_name(ir_get_str(ctx, ai->val.name)));
			if (!v) return false;
		} else if (ai->op == IR_STR) {
			/* string bytes are appended to the code buffer after the
			 * function; emit a fixed-length placeholder now and patch
			 * the absolute address once the string position is known */
			rv_str_fixup *fx = ir_mem_malloc(sizeof(rv_str_fixup));
			uint32_t words[8];
			int n = rv_li64_fixed_words((uint32_t)reg, 0, words);
			int i;

			fx->raw = false;
			fx->patch_offset = (int32_t)((char*)ctx->code_buffer->pos - (char*)ctx->code_buffer->start);
			fx->str_ref = val_ref;
			fx->next = rv_str_fixups;
			rv_str_fixups = fx;
			for (i = 0; i < n; i++) {
				emit32(ctx, words[i]);
			}
			return true;
		} else {
			v = ai->val.i64;
		}
		rv_emit_li64(ctx, (uint32_t)reg, v);
		return true;
	}
	home = ctx->regs[val_ref][0];
	if (home == IR_REG_NONE) {
		return false;
	}
	if (IR_REG_SPILLED(home)) {
		int32_t off = ir_get_spill_slot_offset(ctx, val_ref);
		ir_type type = ctx->ir_base[val_ref].type;

		if (off < -2048 || off > 2047) return false;
		emit32(ctx, rv_load(rv_load_f3(type), (uint32_t)reg, IR_REG_SP, off));
		return true;
	}
	if (home != reg) {
		if (IR_IS_TYPE_FP(ctx->ir_base[val_ref].type)) {
			emit32(ctx, rv_fop(1, RV_F7_FSGNJ, 0,
			                   (uint32_t)(reg - IR_REG_FP_FIRST),
			                   (uint32_t)(home - IR_REG_FP_FIRST),
			                   (uint32_t)(home - IR_REG_FP_FIRST)));
		} else {
			emit32(ctx, rv_mv((uint32_t)reg, (uint32_t)home));
		}
	}
	return true;
}

static bool rv_emit_binop(ir_ctx *ctx, ir_ref ref, ir_insn *insn, const rv_alu_dsc *dsc)
{
	int32_t rd  = riscv_reg_of(ctx, ref, 0);
	int32_t rs1 = riscv_reg_of(ctx, ref, 1);
	int32_t rs2 = riscv_reg_of(ctx, ref, 2);
	ir_ref  op1 = insn->op1;
	ir_ref  op2 = insn->op2;
	bool rd_spilled = IR_REG_SPILLED(rd);
	int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
	int w = ir_type_size[insn->type] == 4;
	bool is_shr = (dsc == &rv_alu_shr);
	int sz = ir_type_size[insn->type];

	if (rd == IR_REG_NONE) {
		return false;
	}
	if (IR_IS_CONST_REF(op1) && IR_IS_CONST_REF(op2)) {
		/* constant-fold a binary op that survived optimization (e.g. -O0) */
		int64_t a = ctx->ir_base[op1].val.i64;
		int64_t b = ctx->ir_base[op2].val.i64;
		int64_t r;

		if (dsc == &rv_alu_add) r = a + b;
		else if (dsc == &rv_alu_sub) r = a - b;
		else if (dsc == &rv_alu_mul) r = a * b;
		else if (dsc == &rv_alu_and) r = a & b;
		else if (dsc == &rv_alu_or) r = a | b;
		else if (dsc == &rv_alu_xor) r = a ^ b;
		else if (dsc == &rv_alu_shl) r = a << b;
		else if (dsc == &rv_alu_shr) r = (int64_t)((uint64_t)a >> b);
		else if (dsc == &rv_alu_sar) r = a >> b;
		else return false;
		rv_emit_li64(ctx, (uint32_t)rdtmp, r);
		if (rd_spilled) {
			rv_emit_store_def(ctx, ref, insn->type, rdtmp);
		}
		return true;
	}
	if (IR_IS_TYPE_FP(insn->type)) {
		uint32_t f7;

		switch (ctx->rules[ref] & IR_RULE_MASK) {
			case IR_RISCV_RULE_ADD: f7 = RV_F7_FADD; break;
			case IR_RISCV_RULE_SUB: f7 = RV_F7_FSUB; break;
			case IR_RISCV_RULE_MUL: f7 = RV_F7_FMUL; break;
			default: return false;
		}
		rs1 = IR_REG_NUM(rs1);
		if (rs1 < IR_REG_FP_FIRST || !rv_emit_get_fp(ctx, rs1 - IR_REG_FP_FIRST, op1, IR_REG_T5)) {
			return false;
		}
		if (IR_IS_CONST_REF(op2)) {
			if (rs2 == IR_REG_NONE) return false;
			rs2 = IR_REG_NUM(rs2);
			if (!rv_emit_get_fp(ctx, rs2 - IR_REG_FP_FIRST, op2, IR_REG_T5)) return false;
		} else {
			rs2 = IR_REG_NUM(rs2);
			if (rs2 < IR_REG_FP_FIRST || !rv_emit_get_fp(ctx, rs2 - IR_REG_FP_FIRST, op2, IR_REG_T5)) return false;
		}
		emit32(ctx, rv_fop(insn->type == IR_DOUBLE, f7, 0,
		                   (uint32_t)(rdtmp - IR_REG_FP_FIRST),
		                   (uint32_t)(rs1 - IR_REG_FP_FIRST),
		                   (uint32_t)(rs2 - IR_REG_FP_FIRST)));
		if (rd_spilled) {
			rv_emit_store_def(ctx, ref, insn->type, rdtmp);
		}
		return true;
	}
	if (IR_IS_CONST_REF(op1)) {
		if (!dsc->commutative || IR_IS_CONST_REF(op2)) {
			return false;
		}
		op1 = insn->op2;
		op2 = insn->op1;
	}
	if (IR_IS_CONST_REF(op2)) {
		const ir_insn *ai = &ctx->ir_base[op2];

		if (ai->op == IR_SYM || ai->op == IR_FUNC || ai->op == IR_FUNC_ADDR) {
			/* Address constants are materialized into the tmp reg the RA
			 * reserved (slot2, marked IR_REG_SPILL_LOAD for consts). The tmp's
			 * live range ends at this insn's DEF sub-ref, so it may legally
			 * reuse rs1 --- a shared reg is a real hazard, fail loudly rather
			 * than emit a self-clobbering add. */
			if (rs2 == IR_REG_NONE) {
				return false;
			}
			rs2 = IR_REG_NUM(rs2);
			if (rs2 == rs1) {
				return false;
			}
			if (!rv_emit_get(ctx, rs2, op2)) {
				return false;
			}
			if (!rv_emit_get(ctx, rs1, op1)) {
				return false;
			}
			emit32(ctx, rv_alu(w, dsc->r_f7, dsc->r_f3,
			                   (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
			return true;
		}
		int64_t v = ai->val.i64;

		if (dsc->i_f3 == 0xFF || rs1 == IR_REG_NONE) {
			return false;
		}
		if (!rv_emit_get(ctx, rs1, op1)) {
			return false;
		}
		if (is_shr && sz < 4) {
			/* logical shift of a narrow value: zero-extend first so that a
			 * sign-extended i8/i16 operand (e.g. -1) shifts like 0xFF/0xFFFF */
			if (sz == 1) {
				emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rdtmp, (uint32_t)rs1, 0xFF));
			} else {
				emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rdtmp, (uint32_t)rs1, 48));
				emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
			}
			rs1 = rdtmp;
		}
		if (dsc->i_shift) {
			if (v < 0 || v > (w ? 31 : 63)) {
				return false;
			}
			emit32(ctx, rv_shifti(w, dsc->i_f3, dsc->i_shift == 2,
			                      (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)v));
		} else {
			if (dsc->i_neg) {
				v = -v;
			}
			if (v < -2048 || v > 2047) {
				return false; /* needs lui+addiw materialization --- later slice */
			}
			emit32(ctx, rv_alui(dsc->i_f3 == RV_F3_ADDI ? w : 0, dsc->i_f3,
			                    (uint32_t)rdtmp, (uint32_t)rs1, (int32_t)v));
		}
	} else {
		int32_t h1 = ctx->regs[op1][0];
		int32_t h2 = ctx->regs[op2][0];

		if (rs1 == IR_REG_NONE || rs2 == IR_REG_NONE) {
			return false;
		}
		if (rs1 == h2 && rs2 == h1) {
			return false; /* register swap needs a scratch --- later slice */
		}
		if (rs1 == h2) {
			if (!rv_emit_get(ctx, rs2, op2) || !rv_emit_get(ctx, rs1, op1)) {
				return false;
			}
		} else {
			if (!rv_emit_get(ctx, rs1, op1) || !rv_emit_get(ctx, rs2, op2)) {
				return false;
			}
		}
		if (is_shr && sz < 4) {
			if (sz == 1) {
				emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rdtmp, (uint32_t)rs1, 0xFF));
			} else {
				emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rdtmp, (uint32_t)rs1, 48));
				emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
			}
			rs1 = rdtmp;
		}
		emit32(ctx, rv_alu(w, dsc->r_f7, dsc->r_f3,
		                   (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
	}
	if (rd_spilled) {
		rv_emit_store_def(ctx, ref, insn->type, rdtmp);
	}
	return true;
}

/* de-ssa move callback for ir_gen_dessa_moves: emit the value of `from`
 * into the PHI register of `to`; to==0 / from==0 break parallel-move
 * cycles through the T5 scratch. */
static int rv_emit_dessa_copy(ir_ctx *ctx, uint8_t type, ir_ref from, ir_ref to, void *data)
{
	(void)data;
	if (to == 0) {
		if (IR_IS_TYPE_FP((ir_type)type)) {
			int32_t fr = ctx->regs[from][0];

			if (fr == IR_REG_NONE || IR_REG_SPILLED(fr) || fr < IR_REG_FP_FIRST) return 0;
			emit32(ctx, rv_fmv_x_d((uint32_t)IR_REG_T5, (uint32_t)(fr - IR_REG_FP_FIRST)));
		} else {
			if (!rv_emit_get(ctx, IR_REG_T5, from)) return 0;
		}
		return 0;
	}
	if (from == 0) {
		int32_t to_reg = ctx->regs[to][0];

		if (to_reg == IR_REG_NONE || IR_REG_SPILLED(to_reg)) return 0;
		if (IR_IS_TYPE_FP((ir_type)type)) {
			emit32(ctx, rv_fmv_d_x((uint32_t)(to_reg - IR_REG_FP_FIRST), (uint32_t)IR_REG_T5));
		} else {
			emit32(ctx, rv_mv((uint32_t)to_reg, (uint32_t)IR_REG_T5));
		}
		return 0;
	}
	if (IR_IS_TYPE_FP((ir_type)type)) {
		int32_t to_reg = ctx->regs[to][0];

		if (to_reg == IR_REG_NONE || IR_REG_SPILLED(to_reg) || to_reg < IR_REG_FP_FIRST) return 0;
		rv_emit_get_fp(ctx, to_reg - IR_REG_FP_FIRST, from, IR_REG_T5);
		return 0;
	}
	if (!rv_emit_get(ctx, ctx->regs[to][0], from)) return 0;
	return 0;
}

void *ir_emit_code(ir_ctx *ctx, size_t *size)
{
	IR_ASSERT(ctx && size);
	void *start;
	uint32_t _b, b;
	ir_insn *insn = NULL;
	ir_ref ref = 0;
	uint32_t rule = 0;
	int32_t *block_off = NULL;
	rv_br_fixup *fx;

	if (!ctx->code_buffer) {
		ctx->status = IR_ERROR_CODE_MEM_OVERFLOW;
		return NULL;
	}
	/* Pre-resolve forward function references so their thunks land before
	 * the function entry instead of mid-code at the call site. */
	{
		ir_ref i;

		for (i = IR_UNUSED + 1; i < ctx->consts_count; i++) {
			const ir_insn *ci = &ctx->ir_base[-i];

			if (ci->op == IR_FUNC && ctx->loader && ctx->loader->resolve_sym_name) {
				ctx->loader->resolve_sym_name(ctx->loader, ctx, ci->val.name,
					IR_RESOLVE_SYM_ADD_THUNK);
			}
		}
	}
	/* Instruction fetch needs 4-byte alignment on RV64 (no RVC); use 16
	 * to match the x86 backend's entry alignment contract. */
	start = (void*)IR_ALIGNED_SIZE((size_t)ctx->code_buffer->pos, 16);
	if ((size_t)((char*)ctx->code_buffer->end - (char*)start)
	    < 64 + (size_t)ctx->insns_count * 64) {
		ctx->status = IR_ERROR_CODE_MEM_OVERFLOW;
		return NULL;
	}
	ctx->code_buffer->pos = start;

	if (!(ctx->flags & IR_SKIP_PROLOGUE)) {
		rv_emit_prologue(ctx);
	}
	rv_br_fixups = NULL;

	/* Mark single-END blocks as empty, like the C emitter does --- the
	 * branch fixups need this to skip blocks that emit no code. */
	if (!ctx->prev_ref) {
		ir_build_prev_refs(ctx);
	}
	for (_b = 1; _b <= ctx->cfg_blocks_count; _b++) {
		ir_block *bb = &ctx->cfg_blocks[_b];

		if (ctx->prev_ref[bb->end] == bb->start
		 && bb->successors_count == 1
		 && (ctx->ir_base[bb->end].op == IR_END || ctx->ir_base[bb->end].op == IR_LOOP_END)
		 && !(bb->flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_DESSA_MOVES))) {
			bb->flags |= IR_BB_EMPTY;
		}
	}

	if (!ctx->cfg_schedule) {
		uint32_t *list = ctx->cfg_schedule =
			ir_mem_malloc(sizeof(uint32_t) * (ctx->cfg_blocks_count + 2));
		uint32_t i;

		for (i = 0; i <= ctx->cfg_blocks_count; i++) {
			list[i] = i;
		}
		list[ctx->cfg_blocks_count + 1] = 0;
	}
	block_off = ir_mem_malloc((ctx->cfg_blocks_count + 1) * sizeof(int32_t));

	for (_b = 1; _b <= ctx->cfg_blocks_count; _b++) {
		ir_block *bb;

		b = ctx->cfg_schedule[_b];
		bb = &ctx->cfg_blocks[b];
		block_off[b] = (int32_t)((char*)ctx->code_buffer->pos - (char*)start);
		if ((bb->flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_EMPTY)) == IR_BB_EMPTY
		 || (bb->flags & IR_BB_UNREACHABLE)) {
			continue;
		}

		for (ref = bb->start; ref <= bb->end; ) {
			insn = &ctx->ir_base[ref];
			rule = ctx->rules[ref];

			if (rule & IR_SKIPPED) {
				ref += ir_insn_len(insn);
				continue;
			}

			switch (rule & IR_RULE_MASK) {
				case IR_RISCV_RULE_SKIP:
					/* No machine code --- START is a control marker. */
					break;

				case IR_RISCV_RULE_PARAM: {
					/* The caller left the value in the ABI register; if the
					 * register allocator placed it elsewhere (e.g. a
					 * callee-saved reg because its live range crosses a
					 * call), copy it here at function entry. */
					int32_t abi = ir_get_param_reg(ctx, ref);
					int32_t dst = ctx->regs[ref][0];

					if (dst != IR_REG_NONE && dst != abi) {
						if (IR_REG_SPILLED(dst)) {
							emit32(ctx, rv_store(rv_store_f3(insn->type),
							                     (uint32_t)abi, IR_REG_SP,
							                     ir_get_spill_slot_offset(ctx, ref)));
						} else if (IR_IS_TYPE_FP(insn->type)) {
							emit32(ctx, rv_fop(1, RV_F7_FSGNJ, 0,
							                   (uint32_t)(dst - IR_REG_FP_FIRST),
							                   (uint32_t)(abi - IR_REG_FP_FIRST),
							                   (uint32_t)(abi - IR_REG_FP_FIRST)));
						} else {
							emit32(ctx, rv_mv((uint32_t)dst, (uint32_t)abi));
						}
					}
					break;
				}

			case IR_RISCV_RULE_ADD:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_add)) goto fail;
				break;

			case IR_RISCV_RULE_SUB:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_sub)) goto fail;
				break;

			case IR_RISCV_RULE_MUL:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_mul)) goto fail;
				break;

			case IR_RISCV_RULE_AND:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_and)) goto fail;
				break;

			case IR_RISCV_RULE_OR:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_or)) goto fail;
				break;

			case IR_RISCV_RULE_XOR:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_xor)) goto fail;
				break;

			case IR_RISCV_RULE_SHL:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_shl)) goto fail;
				break;

			case IR_RISCV_RULE_SHR:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_shr)) goto fail;
				break;

			case IR_RISCV_RULE_SAR:
				if (!rv_emit_binop(ctx, ref, insn, &rv_alu_sar)) goto fail;
				break;

			case IR_RISCV_RULE_NEG: {
				int32_t rd  = riscv_reg_of(ctx, ref, 0);
				int32_t rs1 = riscv_reg_of(ctx, ref, 1);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				int w = ir_type_size[insn->type] == 4;

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				emit32(ctx, rv_alu(w, RV_F7_SUB, RV_F3_SUB,
				                   (uint32_t)rdtmp, 0 /* x0 */, (uint32_t)rs1));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_NOT: {
				int32_t rd  = riscv_reg_of(ctx, ref, 0);
				int32_t rs1 = riscv_reg_of(ctx, ref, 1);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				emit32(ctx, rv_alui(0, RV_F3_XORI, (uint32_t)rdtmp, (uint32_t)rs1, -1));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_CALL: {
				const ir_proto_t *proto = ir_call_proto(ctx, insn);
				const ir_call_conv_dsc *cc =
					ir_get_call_conv_dsc(proto ? proto->flags : IR_CC_DEFAULT);
				int8_t want[IR_MAX_REG_ARGS + 3];
				int n_args = insn->inputs_count;
				int j, k;
				int stack_size = 0;

				if (!IR_IS_CONST_REF(insn->op2)) goto fail;
				memset(want, IR_REG_NONE, sizeof(want));
				ir_get_args_regs(ctx, insn, cc, want);

				/* Stack args: riscv64 psABI passes arguments beyond the
				 * register window on the caller's stack, in argument order.
				 * ARGVAL (struct-by-value) is a stack copy of the struct,
				 * and the address of that copy is passed as a normal arg. */
				{
					int32_t copy_offs[IR_MAX_REG_ARGS + 3];
					int32_t param_size = 0, copy_off;

					for (j = 3; j <= n_args; j++) {
						ir_ref arg = ir_insn_op(insn, j);

						if (arg != IR_UNUSED && want[j] == IR_REG_NONE) {
							param_size += 8;
						}
					}
					param_size = IR_ALIGNED_SIZE(param_size, 16);
					copy_off = param_size;
					for (j = 3; j <= n_args; j++) {
						ir_ref arg = ir_insn_op(insn, j);

						if (arg != IR_UNUSED && ctx->ir_base[arg].op == IR_ARGVAL) {
							copy_off = IR_ALIGNED_SIZE(copy_off, 8);
							copy_offs[j] = copy_off;
							copy_off += ctx->ir_base[arg].op2;
						}
					}
					stack_size = IR_ALIGNED_SIZE(copy_off, 16);
					if (stack_size) {
						int32_t off;

						for (j = 3; j <= n_args; j++) {
							ir_ref arg = ir_insn_op(insn, j);

							if (arg == IR_UNUSED) continue;
							if (ctx->ir_base[arg].op == IR_ARGVAL) {
								ir_insn *av = &ctx->ir_base[arg];
								int32_t i;

								emit32(ctx, rv_addi(IR_REG_T6, IR_REG_SP, copy_offs[j] - stack_size));
								if (!rv_emit_get(ctx, IR_REG_T5, av->op1)) goto fail;
								for (i = 0; i + 8 <= (int32_t)av->op2; i += 8) {
									emit32(ctx, rv_load(3, IR_REG_T4, IR_REG_T5, i));
									emit32(ctx, rv_store(3, IR_REG_T4, IR_REG_T6, i));
								}
								for (; i < (int32_t)av->op2; i++) {
									emit32(ctx, rv_load(1, IR_REG_T4, IR_REG_T5, i));
									emit32(ctx, rv_store(1, IR_REG_T4, IR_REG_T6, i));
								}
							}
						}
						emit32(ctx, rv_addi(IR_REG_SP, IR_REG_SP, -stack_size));
						off = 0;
						for (j = 3; j <= n_args; j++) {
							ir_ref arg = ir_insn_op(insn, j);

							if (want[j] == IR_REG_NONE && arg != IR_UNUSED) {
								if (ctx->ir_base[arg].op == IR_ARGVAL) {
									emit32(ctx, rv_addi(IR_REG_T5, IR_REG_SP, copy_offs[j]));
								} else if (IR_IS_TYPE_FP(ctx->ir_base[arg].type)) {
									if (IR_IS_CONST_REF(arg)) {
										rv_str_fixup *fx = ir_mem_malloc(sizeof(rv_str_fixup));
										uint32_t words[8];
										int n = rv_li64_fixed_words(IR_REG_T5, 0, words);
										int i;

										fx->patch_offset = (int32_t)((char*)ctx->code_buffer->pos - (char*)ctx->code_buffer->start);
										fx->str_ref = arg;
										fx->raw = true;
										fx->next = rv_str_fixups;
										rv_str_fixups = fx;
										for (i = 0; i < n; i++) {
											emit32(ctx, words[i]);
										}
										emit32(ctx, rv_load(3 /* LD */, IR_REG_T5, IR_REG_T5, 0));
									} else {
										int32_t fr = ctx->regs[arg][0];

										if (fr == IR_REG_NONE || IR_REG_SPILLED(fr)
										 || fr < IR_REG_FP_FIRST) goto fail;
										emit32(ctx, rv_fmv_x_d((uint32_t)IR_REG_T5,
										                       (uint32_t)(fr - IR_REG_FP_FIRST)));
									}
								} else {
									if (!rv_emit_get(ctx, IR_REG_T5, arg)) goto fail;
								}
								emit32(ctx, rv_store(3, IR_REG_T5, IR_REG_SP, off));
								off += 8;
							}
						}
					}
					/* register-passed ARGVAL: address of the stack copy */
					for (j = 3; j <= n_args; j++) {
						ir_ref arg = ir_insn_op(insn, j);

						if (arg != IR_UNUSED && want[j] != IR_REG_NONE
						 && ctx->ir_base[arg].op == IR_ARGVAL) {
							emit32(ctx, rv_addi((uint32_t)want[j], IR_REG_SP, copy_offs[j]));
						}
					}
				}

				/* Pass 1: register args. Pass 2: constant args --- a constant
				 * materialization into aX must not clobber a live value
				 * that a later register-arg move still needs. */
				for (k = 0; k < 2; k++) {
					for (j = 3; j <= n_args; j++) {
						ir_ref arg = ir_insn_op(insn, j);
						int32_t dst = want[j];
						int32_t src;
						bool is_const = IR_IS_CONST_REF(arg);

						if (arg == IR_UNUSED) continue;
						if (ctx->ir_base[arg].op == IR_ARGVAL) continue; /* lea'd above */
						if (dst == IR_REG_NONE) continue; /* handled as stack arg */
						if (is_const != (k == 1)) continue;
						if (is_const) {
							if (IR_IS_TYPE_FP(ctx->ir_base[arg].type)) {
								if (dst >= IR_REG_FP_FIRST && dst <= IR_REG_FP_LAST) {
									int32_t fa = dst - IR_REG_FP_FIRST;

									if (fa < 0) goto fail;
									if (!rv_emit_get_fp(ctx, fa, arg, IR_REG_T5)) goto fail;
								} else {
									/* variadic arg: double bits go to a GPR */
									rv_str_fixup *fx = ir_mem_malloc(sizeof(rv_str_fixup));
									uint32_t words[8];
									int n = rv_li64_fixed_words(IR_REG_T5, 0, words);
									int i;

									fx->patch_offset = (int32_t)((char*)ctx->code_buffer->pos - (char*)ctx->code_buffer->start);
									fx->str_ref = arg;
									fx->raw = true;
									fx->next = rv_str_fixups;
									rv_str_fixups = fx;
									for (i = 0; i < n; i++) {
										emit32(ctx, words[i]);
									}
									emit32(ctx, rv_load(3 /* LD */, (uint32_t)dst, IR_REG_T5, 0));
								}
							} else {
								if (!rv_emit_get(ctx, dst, arg)) goto fail;
							}
							continue;
						}
						if (ctx->ir_base[arg].op == IR_ALLOCA) {
							if (!rv_emit_get(ctx, dst, arg)) goto fail;
							continue;
						}
						src = ctx->regs[arg][0];
						if (src == IR_REG_NONE) {
							if (!rv_emit_get(ctx, dst, arg)) goto fail;
							continue;
						}
						if (IR_REG_SPILLED(src)) {
							int32_t off = ir_get_spill_slot_offset(ctx, arg);

							if (off < -2048 || off > 2047) goto fail;
							emit32(ctx, rv_load(rv_load_f3(ctx->ir_base[arg].type),
							                     (uint32_t)dst, IR_REG_SP, off));
							continue;
						}
						if (src != dst) {
							int m;

							for (m = 3; m <= n_args; m++) {
								ir_ref other = ir_insn_op(insn, m);

								if (m != j && !IR_IS_CONST_REF(other)
								 && ctx->regs[other][0] == dst) {
									goto fail; /* move cycle: later slice */
								}
							}
							if (IR_IS_TYPE_FP(ctx->ir_base[arg].type)) {
								if (dst >= IR_REG_FP_FIRST && dst <= IR_REG_FP_LAST) {
									emit32(ctx, rv_fop(1, RV_F7_FSGNJ, 0,
									                   (uint32_t)dst,
									                   (uint32_t)(src - IR_REG_FP_FIRST),
									                   (uint32_t)(src - IR_REG_FP_FIRST)));
								} else {
									emit32(ctx, rv_fmv_x_d((uint32_t)dst,
									                       (uint32_t)(src - IR_REG_FP_FIRST)));
								}
							} else {
								emit32(ctx, rv_mv((uint32_t)dst, (uint32_t)src));
							}
						}
					}
				}

				rv_emit_li64(ctx, IR_REG_T6,
					(int64_t)(uintptr_t)ir_call_addr(ctx, insn, &ctx->ir_base[insn->op2]));
				emit32(ctx, rv_jalr(IR_REG_RA, IR_REG_T6, 0));
				if (stack_size) {
					emit32(ctx, rv_addi(IR_REG_SP, IR_REG_SP, stack_size));
				}

				if (insn->type != IR_VOID) {
					int32_t def_reg;
					bool def_spilled;
					int32_t deftmp;

					def_reg = ctx->regs[ref][0];
					if (def_reg == IR_REG_NONE) {
						break; /* unused result --- value stays in a0/fa0, nobody reads it */
					}
					def_spilled = IR_REG_SPILLED(def_reg);
					deftmp = def_spilled ? IR_REG_NUM(def_reg) : def_reg;
					if (IR_IS_TYPE_FP(insn->type)) {
						if (deftmp < IR_REG_FP_FIRST) goto fail;
						if (deftmp != cc->fp_ret_reg) {
							emit32(ctx, rv_fop(1, RV_F7_FSGNJ, 0,
							                   (uint32_t)(deftmp - IR_REG_FP_FIRST),
							                   (uint32_t)(cc->fp_ret_reg - IR_REG_FP_FIRST),
							                   (uint32_t)(cc->fp_ret_reg - IR_REG_FP_FIRST)));
						}
					} else {
						if (deftmp != cc->int_ret_reg) {
							emit32(ctx, rv_mv((uint32_t)deftmp, (uint32_t)cc->int_ret_reg));
						}
					}
					if (def_spilled) {
						rv_emit_store_def(ctx, ref, insn->type, deftmp);
					}
				}
				break;
			}

			case IR_RISCV_RULE_LOAD: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t base = riscv_reg_of(ctx, ref, 2);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;

				if (rd == IR_REG_NONE || base == IR_REG_NONE) goto fail;
				base = IR_REG_NUM(base);
				if (!rv_emit_get(ctx, base, insn->op2)) goto fail;
				emit32(ctx, rv_load(rv_load_f3(insn->type),
				                     (uint32_t)rdtmp, (uint32_t)base, 0));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_STORE: {
				int32_t base = riscv_reg_of(ctx, ref, 2);
				int32_t val  = riscv_reg_of(ctx, ref, 3);

				if (base == IR_REG_NONE || val == IR_REG_NONE) goto fail;
				base = IR_REG_NUM(base);
				val = IR_REG_NUM(val);
				if (!rv_emit_get(ctx, base, insn->op2)) goto fail;
				if (IR_IS_TYPE_FP(insn->type)) {
					if (val < IR_REG_FP_FIRST) goto fail;
					if (!rv_emit_get_fp(ctx, val - IR_REG_FP_FIRST, insn->op3, IR_REG_T5)) goto fail;
					emit32(ctx, rv_store(rv_store_f3(insn->type),
					                      (uint32_t)(val - IR_REG_FP_FIRST),
					                      (uint32_t)base, 0));
				} else {
					if (!rv_emit_get(ctx, val, insn->op3)) goto fail;
					emit32(ctx, rv_store(rv_store_f3(insn->type),
					                      (uint32_t)val, (uint32_t)base, 0));
				}
				break;
			}

			case IR_RISCV_RULE_VLOAD: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				ir_insn *var_insn = &ctx->ir_base[insn->op2];
				int32_t off;

				if (rd == IR_REG_NONE) goto fail;
				off = IR_SPILL_POS_TO_OFFSET(var_insn->op3);
				if (off < -2048 || off > 2047) goto fail;
				emit32(ctx, rv_load(rv_load_f3(insn->type),
				                     (uint32_t)rdtmp, IR_REG_SP, off));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_VSTORE: {
				int32_t val  = riscv_reg_of(ctx, ref, 3);
				ir_insn *var_insn = &ctx->ir_base[insn->op2];
				int32_t off;

				if (val == IR_REG_NONE) goto fail;
				off = IR_SPILL_POS_TO_OFFSET(var_insn->op3);
				if (off < -2048 || off > 2047) goto fail;
				if (IR_IS_TYPE_FP(insn->type)) {
					val = IR_REG_NUM(val);
					if (val < IR_REG_FP_FIRST) goto fail;
					if (!rv_emit_get_fp(ctx, val - IR_REG_FP_FIRST, insn->op3, IR_REG_T5)) goto fail;
					emit32(ctx, rv_store(rv_store_f3(insn->type),
					                      (uint32_t)(val - IR_REG_FP_FIRST),
					                      IR_REG_SP, off));
				} else {
					if (!rv_emit_get(ctx, val, insn->op3)) goto fail;
					emit32(ctx, rv_store(rv_store_f3(insn->type),
					                      (uint32_t)val, IR_REG_SP, off));
				}
				break;
			}

			case IR_RISCV_RULE_VADDR: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				ir_insn *var_insn = &ctx->ir_base[insn->op1];
				int32_t off;

				if (rd == IR_REG_NONE) goto fail;
				off = IR_SPILL_POS_TO_OFFSET(var_insn->op3);
				if (off < -2048 || off > 2047) goto fail;
				emit32(ctx, rv_addi((uint32_t)rdtmp, IR_REG_SP, off));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_ZEXT:
			case IR_RISCV_RULE_SEXT:
			case IR_RISCV_RULE_TRUNC: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				int rule = ctx->rules[ref] & IR_RULE_MASK;
				int src_sz = IR_IS_CONST_REF(insn->op1) ? 0
					: ir_type_size[ctx->ir_base[insn->op1].type];
				int dst_sz = ir_type_size[insn->type];

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				if (rule == IR_RISCV_RULE_ZEXT || rule == IR_RISCV_RULE_SEXT) {
					if (src_sz == 4) {
						emit32(ctx, rv_alui(1, RV_F3_ADDI, (uint32_t)rdtmp, (uint32_t)rs1, 0));
					} else if (src_sz == 1) {
						if (rule == IR_RISCV_RULE_ZEXT) {
							emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rdtmp, (uint32_t)rs1, 0xFF));
						} else {
							emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rdtmp, (uint32_t)rs1, 56));
							emit32(ctx, rv_shifti(0, RV_F3_SRI, 1, (uint32_t)rdtmp, (uint32_t)rdtmp, 56));
						}
					} else if (src_sz == 2) {
						/* 0xFFFF does not fit the 12-bit andi immediate */
						emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rdtmp, (uint32_t)rs1, 48));
						emit32(ctx, rv_shifti(0, RV_F3_SRI,
						                    rule == IR_RISCV_RULE_SEXT,
						                    (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
					} else if (rdtmp != rs1) {
						emit32(ctx, rv_mv((uint32_t)rdtmp, (uint32_t)rs1));
					}
				} else {
					if (dst_sz == 4) {
						emit32(ctx, rv_alui(1, RV_F3_ADDI, (uint32_t)rdtmp, (uint32_t)rs1, 0));
					} else if (dst_sz == 1) {
						emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rdtmp, (uint32_t)rs1, 0xFF));
					} else if (dst_sz == 2) {
						emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rdtmp, (uint32_t)rs1, 48));
						emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
					} else if (rdtmp != rs1) {
						emit32(ctx, rv_mv((uint32_t)rdtmp, (uint32_t)rs1));
					}
				}
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_DIV:
			case IR_RISCV_RULE_MOD: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				int32_t rs2  = riscv_reg_of(ctx, ref, 2);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				bool uns = !IR_IS_TYPE_SIGNED(insn->type);
				bool w = ir_type_size[insn->type] == 4;

				int32_t tmp = riscv_reg_of(ctx, ref, 3);

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (IR_IS_TYPE_FP(insn->type)) {
					rs1 = IR_REG_NUM(rs1);
					if (rs1 < IR_REG_FP_FIRST || !rv_emit_get_fp(ctx, rs1 - IR_REG_FP_FIRST, insn->op1, IR_REG_T5)) goto fail;
					if (IR_IS_CONST_REF(insn->op2)) {
						if (tmp == IR_REG_NONE) goto fail;
						tmp = IR_REG_NUM(tmp);
						if (!rv_emit_get_fp(ctx, tmp - IR_REG_FP_FIRST, insn->op2, IR_REG_T5)) goto fail;
						rs2 = tmp;
					} else {
						rs2 = IR_REG_NUM(rs2);
						if (rs2 < IR_REG_FP_FIRST || !rv_emit_get_fp(ctx, rs2 - IR_REG_FP_FIRST, insn->op2, IR_REG_T5)) goto fail;
					}
					emit32(ctx, rv_fop(1, RV_F7_FDIV, 0,
					                   (uint32_t)(rdtmp - IR_REG_FP_FIRST),
					                   (uint32_t)(rs1 - IR_REG_FP_FIRST),
					                   (uint32_t)(rs2 - IR_REG_FP_FIRST)));
					if (rd_spilled) {
						rv_emit_store_def(ctx, ref, insn->type, rdtmp);
					}
					break;
				}
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				if (IR_IS_CONST_REF(insn->op2)) {
					if (tmp == IR_REG_NONE || !rv_emit_get(ctx, tmp, insn->op2)) goto fail;
					rs2 = tmp;
				} else {
					if (rs2 == IR_REG_NONE || !rv_emit_get(ctx, rs2, insn->op2)) goto fail;
				}
				/* funct3: div=4, divu=5, rem=6, remu=7 */
				emit32(ctx, rv_alu(w, 0x01,
				                   ctx->rules[ref] == IR_RISCV_RULE_DIV
					                   ? (uns ? 0x05 : 0x04)
					                   : (uns ? 0x07 : 0x06),
				                   (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_MIN:
			case IR_RISCV_RULE_MAX: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				int32_t rs2  = riscv_reg_of(ctx, ref, 2);
				int32_t tmp  = riscv_reg_of(ctx, ref, 3);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE || tmp == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				if (IR_IS_CONST_REF(insn->op2)) {
					if (rs2 == IR_REG_NONE) {
						if (!rv_emit_get(ctx, IR_REG_T5, insn->op2)) goto fail;
						rs2 = IR_REG_T5;
					} else {
						if (!rv_emit_get(ctx, rs2, insn->op2)) goto fail;
					}
				} else {
					if (rs2 == IR_REG_NONE || !rv_emit_get(ctx, rs2, insn->op2)) goto fail;
				}
				/* min(a,b) = a - ((a-b) & ((a-b)>>63)) */
				emit32(ctx, rv_alu(0, RV_F7_SUB, RV_F3_SUB, (uint32_t)tmp, (uint32_t)rs1, (uint32_t)rs2)); /* tmp = a-b */
				emit32(ctx, rv_shifti(0, RV_F3_SRI, 1, (uint32_t)rdtmp, (uint32_t)tmp, 63));      /* rdtmp = (a-b)>>63 */
				emit32(ctx, rv_alu(0, 0, RV_F3_AND, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)tmp));
				if (rdtmp != rs2) {
					/* min = b + (d & (d>>63)) / max = a - (d & (d>>63)) */
					if (ctx->rules[ref] == IR_RISCV_RULE_MIN) {
						emit32(ctx, rv_alu(0, 0, RV_F3_ADD, (uint32_t)rdtmp, (uint32_t)rs2, (uint32_t)rdtmp));
					} else {
						emit32(ctx, rv_alu(0, RV_F7_SUB, RV_F3_SUB, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rdtmp));
					}
				} else {
					/* rdtmp aliases rs2 (b), which is clobbered by the
					 * masked computation: use the forms reading only rs1 (a)
					 *   min = a - (d & ~(d>>63)), max = a - (d & (d>>63)) */
					if (ctx->rules[ref] == IR_RISCV_RULE_MAX) {
						emit32(ctx, rv_alu(0, 0, RV_F3_AND, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)tmp));
						emit32(ctx, rv_alu(0, RV_F7_SUB, RV_F3_SUB, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rdtmp));
					} else {
						emit32(ctx, rv_alui(0, RV_F3_XORI, (uint32_t)rdtmp, (uint32_t)rdtmp, -1));
						emit32(ctx, rv_alu(0, 0, RV_F3_AND, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)tmp));
						emit32(ctx, rv_alu(0, RV_F7_SUB, RV_F3_SUB, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rdtmp));
					}
				}
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_ROL:
			case IR_RISCV_RULE_ROR: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				int32_t rs2  = riscv_reg_of(ctx, ref, 2);
				int32_t tmp  = riscv_reg_of(ctx, ref, 3);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				int sz = ir_type_size[insn->type];
				int bits = sz * 8;
				int w = sz == 4;
				bool ror = ctx->rules[ref] == IR_RISCV_RULE_ROR;
				int64_t sh;
				int32_t s2;   /* second scratch: rs1 unless it aliases rdtmp */

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				if (!IR_IS_CONST_REF(insn->op2)) {
					/* variable shift: need a distinct scratch for -s */
					int32_t s2v;

					if (tmp == IR_REG_NONE || tmp == rs1) goto fail;
					if (!rv_emit_get(ctx, rs2, insn->op2)) goto fail;
					if (sz == 1) {
						emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rs1, (uint32_t)rs1, 0xFF));
					} else if (sz == 2) {
						emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rs1, (uint32_t)rs1, 48));
						emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rs1, (uint32_t)rs1, 48));
					} else if (w) {
						/* 64-bit shifts: shamt 32 does not fit the 5-bit W-form field */
						emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rs1, (uint32_t)rs1, 32));
						emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rs1, (uint32_t)rs1, 32));
					}
					if (sz < 4) {
						/* narrow rotate: normalize the shift amount and later
						 * mask the result; the complement shift must be
						 * (bits - s), not the 64-bit (-s). */
						emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rs2, (uint32_t)rs2, bits - 1));
					}
					s2v = (rdtmp == rs1) ? tmp : rs1;
					if (s2v == IR_REG_NONE || s2v == rdtmp) goto fail;
					if (rdtmp == rs1) {
						/* def aliases the source: part2 into tmp first (reads x),
						 * then part1 into rdtmp (reads x before writing it). */
						emit32(ctx, rv_alu(w, RV_F7_SUB, RV_F3_SUB, (uint32_t)tmp, 0, (uint32_t)rs2));
						if (sz < 4) {
							emit32(ctx, rv_alui(0, RV_F3_ADDI, (uint32_t)tmp, (uint32_t)tmp, bits));
						}
						emit32(ctx, rv_alu(w, 0, ror ? RV_F3_SLL : RV_F3_SRL,
						                    (uint32_t)tmp, (uint32_t)rs1, (uint32_t)tmp));
						emit32(ctx, rv_alu(w, 0, ror ? RV_F3_SRL : RV_F3_SLL,
						                    (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
						emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)tmp));
						if (sz == 1) {
							emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rdtmp, (uint32_t)rdtmp, 0xFF));
						} else if (sz == 2) {
							emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
							emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
						}
					} else if (tmp == rdtmp) {
						/* tmp aliases the def: put -s in rdtmp, shift part2
						 * back into rdtmp (read-before-write), then part1 into
						 * rs1 which still holds x. */
						emit32(ctx, rv_alu(w, RV_F7_SUB, RV_F3_SUB, (uint32_t)rdtmp, 0, (uint32_t)rs2));
						if (sz < 4) {
							emit32(ctx, rv_alui(0, RV_F3_ADDI, (uint32_t)rdtmp, (uint32_t)rdtmp, bits));
						}
						emit32(ctx, rv_alu(w, 0, ror ? RV_F3_SLL : RV_F3_SRL,
						                    (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rdtmp));
						emit32(ctx, rv_alu(w, 0, ror ? RV_F3_SRL : RV_F3_SLL,
						                    (uint32_t)rs1, (uint32_t)rs1, (uint32_t)rs2));
						emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rdtmp));
						if (sz == 1) {
							emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rdtmp, (uint32_t)rdtmp, 0xFF));
						} else if (sz == 2) {
							emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
							emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
						}
					} else {
						emit32(ctx, rv_alu(w, RV_F7_SUB, RV_F3_SUB, (uint32_t)tmp, 0, (uint32_t)rs2)); /* tmp = -s */
						if (sz < 4) {
							emit32(ctx, rv_alui(0, RV_F3_ADDI, (uint32_t)tmp, (uint32_t)tmp, bits));
						}
						if (ror) {
							emit32(ctx, rv_alu(w, 0, RV_F3_SRL, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
							emit32(ctx, rv_alu(w, 0, RV_F3_SLL, (uint32_t)s2v, (uint32_t)rs1, (uint32_t)tmp));
						} else {
							emit32(ctx, rv_alu(w, 0, RV_F3_SLL, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
							emit32(ctx, rv_alu(w, 0, RV_F3_SRL, (uint32_t)s2v, (uint32_t)rs1, (uint32_t)tmp));
						}
						emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)s2v));
						if (sz == 1) {
							emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rdtmp, (uint32_t)rdtmp, 0xFF));
						} else if (sz == 2) {
							emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
							emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rdtmp, (uint32_t)rdtmp, 48));
						}
					}
					if (w) {
						emit32(ctx, rv_alui(1, RV_F3_ADDI, (uint32_t)rdtmp, (uint32_t)rdtmp, 0));
					}
					if (rd_spilled) {
						rv_emit_store_def(ctx, ref, insn->type, rdtmp);
					}
					break;
				}
				sh = ctx->ir_base[insn->op2].val.i64;
				if (sh < 0 || sh >= bits) goto fail;
				s2 = (rdtmp == rs1) ? tmp : rs1;
				if (s2 == IR_REG_NONE || s2 == rdtmp) goto fail;
				if (sz == 1) {
					emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rs1, (uint32_t)rs1, 0xFF));
				} else if (sz == 2) {
					emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rs1, (uint32_t)rs1, 48));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rs1, (uint32_t)rs1, 48));
				} else if (w) {
					/* 64-bit shifts: shamt 32 does not fit the 5-bit W-form field */
					emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rs1, (uint32_t)rs1, 32));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rs1, (uint32_t)rs1, 32));
				}
				/* part1 into rdtmp (reads x), then part2 into s2, then or.
				 * If rdtmp aliases rs1, part1 would clobber x, so compute
				 * part2 into s2 first in that case. */
				if (rdtmp == rs1) {
					emit32(ctx, rv_shifti(w, ror ? RV_F3_SLLI : RV_F3_SRI, 0,
					                    (uint32_t)s2, (uint32_t)rs1, (uint32_t)(bits - sh)));
					emit32(ctx, rv_shifti(w, ror ? RV_F3_SRI : RV_F3_SLLI, 0,
					                    (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)sh));
				} else {
					emit32(ctx, rv_shifti(w, ror ? RV_F3_SRI : RV_F3_SLLI, 0,
					                    (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)sh));
					emit32(ctx, rv_shifti(w, ror ? RV_F3_SLLI : RV_F3_SRI, 0,
					                    (uint32_t)s2, (uint32_t)rs1, (uint32_t)(bits - sh)));
				}
				/* OR has no 32-bit W-form (only ADD/SUB/SHIFTS/MUL/DIV/REM do);
				 * combine with the 64-bit OR and canonicalize for 32-bit. */
				emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)s2));
				if (w) {
					emit32(ctx, rv_alui(1, RV_F3_ADDI, (uint32_t)rdtmp, (uint32_t)rdtmp, 0));
				}
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_CTLZ:
			case IR_RISCV_RULE_CTTZ:
			case IR_RISCV_RULE_CTPOP: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				int sz = ir_type_size[insn->type];
				int rule = ctx->rules[ref] & IR_RULE_MASK;

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				/* sub-64-bit types are held sign-extended; zero the bits above
				 * the type width so clz/cpop do not count them (ctz is
				 * unaffected --- trailing zeros live in the low bits) */
				if (sz == 4) {
					emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rs1, (uint32_t)rs1, 32));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rs1, (uint32_t)rs1, 32));
				} else if (sz == 2) {
					emit32(ctx, rv_shifti(0, RV_F3_SLLI, 0, (uint32_t)rs1, (uint32_t)rs1, 48));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)rs1, (uint32_t)rs1, 48));
				} else if (sz == 1) {
					emit32(ctx, rv_alui(0, RV_F3_ANDI, (uint32_t)rs1, (uint32_t)rs1, 0xFF));
				}
				if (rule == IR_RISCV_RULE_CTLZ) {
					/* y = x | x>>1 | x>>2 | x>>4 | x>>8 | x>>16 | x>>32 */
					emit32(ctx, rv_mv((uint32_t)rdtmp, (uint32_t)rs1));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rdtmp, 1));
					emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)IR_REG_T6));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rdtmp, 2));
					emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)IR_REG_T6));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rdtmp, 4));
					emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)IR_REG_T6));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rdtmp, 8));
					emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)IR_REG_T6));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rdtmp, 16));
					emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)IR_REG_T6));
					emit32(ctx, rv_shifti(0, RV_F3_SRI, 0, (uint32_t)IR_REG_T6, (uint32_t)rdtmp, 32));
					emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)IR_REG_T6));
					rv_emit_soft_cpop(ctx, rdtmp, rdtmp);
					/* clz = 64 - popcount */
					emit32(ctx, rv_alu(0, RV_F7_SUB, RV_F3_SUB, (uint32_t)rdtmp, 0, (uint32_t)rdtmp));
					emit32(ctx, rv_alui(0, RV_F3_ADDI, (uint32_t)rdtmp, (uint32_t)rdtmp, 64));
					if (sz < 8) {
						emit32(ctx, rv_alui(0, RV_F3_ADDI, (uint32_t)rdtmp, (uint32_t)rdtmp,
						                    (sz == 4) ? -32 : ((sz == 2) ? -48 : -56)));
					}
				} else if (rule == IR_RISCV_RULE_CTTZ) {
					/* ctz(x) = cpop((x & -x) - 1); x == 0 -> cpop(-1) = 64 */
					emit32(ctx, rv_alu(0, RV_F7_SUB, RV_F3_SUB, (uint32_t)IR_REG_T6, 0, (uint32_t)rs1));
					emit32(ctx, rv_alu(0, 0, RV_F3_AND, (uint32_t)IR_REG_T6, (uint32_t)rs1, (uint32_t)IR_REG_T6));
					emit32(ctx, rv_alui(0, RV_F3_ADDI, (uint32_t)IR_REG_T6, (uint32_t)IR_REG_T6, -1));
					rv_emit_soft_cpop(ctx, rdtmp, IR_REG_T6);
				} else {
					rv_emit_soft_cpop(ctx, rdtmp, rs1);
				}
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_COND: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				int32_t rs2  = riscv_reg_of(ctx, ref, 2);
				int32_t rs3  = riscv_reg_of(ctx, ref, 3);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				bool fp = IR_IS_TYPE_FP(ctx->ir_base[insn->op2].type);
				int32_t t5 = IR_REG_T5;
				int32_t t6 = IR_REG_T6;
				int32_t a, b;
				uint32_t bne, j_skip;

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				if (IR_IS_CONST_REF(insn->op2)) {
					if (!rv_emit_get(ctx, t5, insn->op2)) goto fail;
					a = t5;
				} else {
					if (rs2 == IR_REG_NONE || !rv_emit_get(ctx, rs2, insn->op2)) goto fail;
					a = rs2;
				}
				if (IR_IS_CONST_REF(insn->op3)) {
					if (!rv_emit_get(ctx, t6, insn->op3)) goto fail;
					b = t6;
				} else {
					if (rs3 == IR_REG_NONE || !rv_emit_get(ctx, rs3, insn->op3)) goto fail;
					b = rs3;
				}
				/* branch-based select: cond ? a : b
				 *   bnez cond, +8; mv rd, b; j +4; mv rd, a */
				/* bne target = +12: skip mv-b and the jal, land on mv-a */
				bne = rv_enc_b(12, rs1, 0 /* x0 */, RV_F3_BNE, RV_OP_BRANCH);
				emit32(ctx, bne);
				if (fp) {
					emit32(ctx, rv_fsgnj(1, 0, (uint32_t)(rdtmp - IR_REG_FP_FIRST),
					                      (uint32_t)(b - IR_REG_FP_FIRST),
					                      (uint32_t)(b - IR_REG_FP_FIRST)));
				} else {
					emit32(ctx, rv_mv((uint32_t)rdtmp, (uint32_t)b));
				}
				/* jal target = +8: skip mv-a */
				j_skip = rv_enc_j(8, 0, RV_OP_JAL);
				emit32(ctx, j_skip);
				if (fp) {
					emit32(ctx, rv_fsgnj(1, 0, (uint32_t)(rdtmp - IR_REG_FP_FIRST),
					                      (uint32_t)(a - IR_REG_FP_FIRST),
					                      (uint32_t)(a - IR_REG_FP_FIRST)));
				} else {
					emit32(ctx, rv_mv((uint32_t)rdtmp, (uint32_t)a));
				}
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_IF: {
				uint32_t true_block, false_block;
				uint32_t nb = 0;
				int32_t cond;
				uint32_t _b2;

				ir_get_true_false_blocks(ctx, b, &true_block, &false_block);
				/* find the next scheduled (non-empty) block for fall-through */
				for (_b2 = _b + 1; _b2 <= ctx->cfg_blocks_count; _b2++) {
					ir_block *nbb = &ctx->cfg_blocks[ctx->cfg_schedule[_b2]];

					if (!((nbb->flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_EMPTY)) == IR_BB_EMPTY)
					 && !(nbb->flags & IR_BB_UNREACHABLE)) {
						nb = ctx->cfg_schedule[_b2];
						break;
					}
				}
				if (IR_IS_CONST_REF(insn->op2)) {
					/* constant condition: single unconditional branch */
					rv_br_fixup *fx = ir_mem_malloc(sizeof(rv_br_fixup));
					const ir_insn *ci = &ctx->ir_base[insn->op2];

					if (ir_const_is_true(ci)) {
						if (true_block == nb) break;
						fx->target_block = true_block;
					} else {
						if (false_block == nb) break;
						fx->target_block = false_block;
					}
					fx->is_jal = 1;
					fx->f3 = 0;
					fx->cond_reg = 0;
					fx->patch_off = (int32_t)((char*)ctx->code_buffer->pos - (char*)start);
					fx->next = rv_br_fixups;
					rv_br_fixups = fx;
					emit32(ctx, 0); /* jal placeholder */
					break;
				}
				cond = riscv_reg_of(ctx, ref, 2);
				if (cond == IR_REG_NONE || !rv_emit_get(ctx, cond, insn->op2)) goto fail;
				/* Fixed 3-instruction sequence so branch targets never exceed
				 * the 12-bit beq/bne range, however far the scheduler places
				 * the true/false blocks (mirrors how large functions reorder
				 * cold blocks to the end, e.g. fcmp_003 main).
				 *   bnez cond, +8      ; cond != 0 -> skip jal-false
				 *   jal  false_block   ; cond == 0
				 *   jal  true_block    ; cond != 0
				 */
				emit32(ctx, rv_enc_b(8, 0, (uint32_t)cond, RV_F3_BNE, RV_OP_BRANCH));
				{
					rv_br_fixup *fx = ir_mem_malloc(sizeof(rv_br_fixup));

					fx->is_jal = 1;
					fx->f3 = 0;
					fx->cond_reg = 0;
					fx->target_block = false_block;
					fx->patch_off = (int32_t)((char*)ctx->code_buffer->pos - (char*)start);
					fx->next = rv_br_fixups;
					rv_br_fixups = fx;
					emit32(ctx, 0); /* jal false_block */
				}
				{
					rv_br_fixup *fx = ir_mem_malloc(sizeof(rv_br_fixup));

					fx->is_jal = 1;
					fx->f3 = 0;
					fx->cond_reg = 0;
					fx->target_block = true_block;
					fx->patch_off = (int32_t)((char*)ctx->code_buffer->pos - (char*)start);
					fx->next = rv_br_fixups;
					rv_br_fixups = fx;
					emit32(ctx, 0); /* jal true_block */
				}
				break;
			}

			case IR_RISCV_RULE_BITCAST: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				bool to_fp = IR_IS_TYPE_FP(insn->type);

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				if (to_fp) {
					emit32(ctx, rv_fmv_d_x((uint32_t)(rdtmp - IR_REG_FP_FIRST), (uint32_t)rs1));
				} else {
					emit32(ctx, rv_fmv_x_d((uint32_t)rdtmp, (uint32_t)(rs1 - IR_REG_FP_FIRST)));
				}
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_EQ:
			case IR_RISCV_RULE_NE:
			case IR_RISCV_RULE_LT:
			case IR_RISCV_RULE_GT:
			case IR_RISCV_RULE_LE:
			case IR_RISCV_RULE_GE: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				int32_t rs2  = riscv_reg_of(ctx, ref, 2);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				int rule = ctx->rules[ref] & IR_RULE_MASK;
				bool fp = IR_IS_TYPE_FP(ctx->ir_base[insn->op1].type);

				if (rd == IR_REG_NONE) goto fail;
				if (IR_IS_CONST_REF(insn->op1)) {
					if (fp) {
						if (rs1 == IR_REG_NONE) goto fail;
						rs1 = IR_REG_NUM(rs1);
						if (!rv_emit_get_fp(ctx, rs1 - IR_REG_FP_FIRST, insn->op1, IR_REG_T5)) goto fail;
					} else {
						if (!rv_emit_get(ctx, IR_REG_T5, insn->op1)) goto fail;
						rs1 = IR_REG_T5;
					}
				} else if (fp) {
					rs1 = IR_REG_NUM(rs1);
					if (rs1 < IR_REG_FP_FIRST || !rv_emit_get_fp(ctx, rs1 - IR_REG_FP_FIRST, insn->op1, IR_REG_T5)) goto fail;
				} else {
					if (rs1 == IR_REG_NONE || !rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				}
				if (IR_IS_CONST_REF(insn->op2)) {
					if (fp) {
						if (rs2 == IR_REG_NONE) goto fail;
						rs2 = IR_REG_NUM(rs2);
						if (!rv_emit_get_fp(ctx, rs2 - IR_REG_FP_FIRST, insn->op2, IR_REG_T5)) goto fail;
					} else {
						if (!rv_emit_get(ctx, IR_REG_T6, insn->op2)) goto fail;
						rs2 = IR_REG_T6;
					}
				} else if (fp) {
					rs2 = IR_REG_NUM(rs2);
					if (rs2 < IR_REG_FP_FIRST || !rv_emit_get_fp(ctx, rs2 - IR_REG_FP_FIRST, insn->op2, IR_REG_T5)) goto fail;
				} else {
					if (rs2 == IR_REG_NONE || !rv_emit_get(ctx, rs2, insn->op2)) goto fail;
				}
				if (fp) {
					int f1 = rs1 - IR_REG_FP_FIRST;
					int f2 = rs2 - IR_REG_FP_FIRST;
					/* pinned scratch GPR (IR_SCRATCH_REG in constraints) */
					int32_t tmp = IR_REG_T5;
					bool unord = (insn->op == IR_ULT || insn->op == IR_UGT
					           || insn->op == IR_ULE || insn->op == IR_UGE);

					if (rs1 < IR_REG_FP_FIRST || rs2 < IR_REG_FP_FIRST) goto fail;
					switch (rule) {
						case IR_RISCV_RULE_EQ:
							emit32(ctx, rv_fop(1, RV_F7_FEQ, RV_F3_FEQ, (uint32_t)rdtmp, (uint32_t)f1, (uint32_t)f2));
							break;
						case IR_RISCV_RULE_NE:
							emit32(ctx, rv_fop(1, RV_F7_FEQ, RV_F3_FEQ, (uint32_t)rdtmp, (uint32_t)f1, (uint32_t)f2));
							emit32(ctx, rv_alui(0, RV_F3_XORI, (uint32_t)rdtmp, (uint32_t)rdtmp, 1));
							break;
						case IR_RISCV_RULE_LT:
							emit32(ctx, rv_fop(1, RV_F7_FLT, RV_F3_FLT, (uint32_t)rdtmp, (uint32_t)f1, (uint32_t)f2));
							break;
						case IR_RISCV_RULE_GT:
							emit32(ctx, rv_fop(1, RV_F7_FLT, RV_F3_FLT, (uint32_t)rdtmp, (uint32_t)f2, (uint32_t)f1));
							break;
						case IR_RISCV_RULE_LE:
							emit32(ctx, rv_fop(1, RV_F7_FLE, RV_F3_FLE, (uint32_t)rdtmp, (uint32_t)f1, (uint32_t)f2));
							break;
						case IR_RISCV_RULE_GE:
							emit32(ctx, rv_fop(1, RV_F7_FLE, RV_F3_FLE, (uint32_t)rdtmp, (uint32_t)f2, (uint32_t)f1));
							break;
					}
					if (unord) {
						/* unordered compare: true when either operand is NaN */
						int i;

						for (i = 0; i < 2; i++) {
							emit32(ctx, rv_fop(1, RV_F7_FEQ, RV_F3_FEQ,
							                   (uint32_t)tmp,
							                   (uint32_t)(i ? f2 : f1),
							                   (uint32_t)(i ? f2 : f1)));
							emit32(ctx, rv_alui(0, RV_F3_SLTIU, (uint32_t)tmp, (uint32_t)tmp, 1));
							emit32(ctx, rv_alu(0, 0, RV_F3_OR, (uint32_t)rdtmp, (uint32_t)rdtmp, (uint32_t)tmp));
						}
					}
				} else {
					bool uns = !IR_IS_TYPE_SIGNED(ctx->ir_base[insn->op1].type);
					uint32_t f3 = uns ? RV_F3_SLTU : RV_F3_SLT;

					if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
					if (!rv_emit_get(ctx, rs2, insn->op2)) goto fail;
					switch (rule) {
						case IR_RISCV_RULE_EQ:
							emit32(ctx, rv_alu(0, 0, RV_F3_XOR, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
							emit32(ctx, rv_alui(0, RV_F3_SLTIU, (uint32_t)rdtmp, (uint32_t)rdtmp, 1));
							break;
						case IR_RISCV_RULE_NE:
							emit32(ctx, rv_alu(0, 0, RV_F3_XOR, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
							emit32(ctx, rv_alu(0, 0, RV_F3_SLTU, (uint32_t)rdtmp, (uint32_t)rdtmp, 0 /* x0 */));
							break;
						case IR_RISCV_RULE_LT:
							emit32(ctx, rv_alu(0, 0, f3, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
							break;
						case IR_RISCV_RULE_GT:
							emit32(ctx, rv_alu(0, 0, f3, (uint32_t)rdtmp, (uint32_t)rs2, (uint32_t)rs1));
							break;
						case IR_RISCV_RULE_LE:
							emit32(ctx, rv_alu(0, 0, f3, (uint32_t)rdtmp, (uint32_t)rs2, (uint32_t)rs1));
							emit32(ctx, rv_alui(0, RV_F3_XORI, (uint32_t)rdtmp, (uint32_t)rdtmp, 1));
							break;
						case IR_RISCV_RULE_GE:
							emit32(ctx, rv_alu(0, 0, f3, (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
							emit32(ctx, rv_alui(0, RV_F3_XORI, (uint32_t)rdtmp, (uint32_t)rdtmp, 1));
							break;
					}
				}
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_ALLOCA:
				/* the address is materialized at each use (rv_emit_get) */
				break;

			case IR_RISCV_RULE_GUARD:
			case IR_RISCV_RULE_GUARD_NOT: {
				int32_t cond;
				uint32_t words[8];
				uint32_t *cond_pos;
				int n, i;
				int32_t skip;
				void *addr;

				cond = riscv_reg_of(ctx, ref, 2);
				if (cond == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, cond, insn->op2)) goto fail;
				if (!IR_IS_CONST_REF(insn->op3)) goto fail;
				{
					const ir_insn *ai = &ctx->ir_base[insn->op3];

					if (ai->op != IR_FUNC && ai->op != IR_FUNC_ADDR && ai->op != IR_ADDR) goto fail;
					if (ai->op == IR_FUNC) {
						addr = (ctx->loader && ctx->loader->resolve_sym_name)
							? ctx->loader->resolve_sym_name(ctx->loader, ctx, ai->val.name, 0)
							: ir_resolve_sym_name(ir_get_str(ctx, ai->val.name));
					} else {
						addr = (void*)(uintptr_t)ai->val.addr;
					}
				}
				if (!addr) goto fail;
				/* Guard: when the condition fails, restore the callee-saved
				 * state and tail-jump to the deopt fallback (op3). Its return
				 * goes straight back to our caller (like x86's `jmp addr`),
				 * carrying the guard result. The skipped region length is
				 * known right after emission, so the branch is patched in
				 * place without a fixup. */
				cond_pos = (uint32_t*)ctx->code_buffer->pos;
				emit32(ctx, 0);
				rv_emit_restore_regs(ctx);
				n = rv_li64_fixed_words(IR_REG_T6, (int64_t)(uintptr_t)addr, words);
				for (i = 0; i < n; i++) {
					emit32(ctx, words[i]);
				}
				emit32(ctx, rv_jalr(0 /* x0 */, IR_REG_T6, 0));
				skip = (int32_t)((char*)ctx->code_buffer->pos - (char*)cond_pos);
				*cond_pos = rv_enc_b(skip, 0 /* x0 */, (uint32_t)cond,
				                     (ctx->rules[ref] & IR_RULE_MASK) == IR_RISCV_RULE_GUARD
					                     ? RV_F3_BNE : RV_F3_BEQ,
				                     RV_OP_BRANCH);
				break;
			}

			case IR_RISCV_RULE_FP2FP: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				int32_t rs1  = riscv_reg_of(ctx, ref, 1);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				bool to_double = (insn->type == IR_DOUBLE);

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (rs1 < IR_REG_FP_FIRST || rdtmp < IR_REG_FP_FIRST) goto fail;
				if (!rv_emit_get_fp(ctx, rs1 - IR_REG_FP_FIRST, insn->op1, IR_REG_T5)) goto fail;
				emit32(ctx, rv_fcvt(to_double, to_double ? 0 : 1,
				                    (uint32_t)(rdtmp - IR_REG_FP_FIRST),
				                    (uint32_t)(rs1 - IR_REG_FP_FIRST)));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_INT2FP: {
				int32_t rd  = riscv_reg_of(ctx, ref, 0);
				int32_t rs1 = riscv_reg_of(ctx, ref, 1);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				bool w = ir_type_size[ctx->ir_base[insn->op1].type] > 4;
				bool uns = !IR_IS_TYPE_SIGNED(ctx->ir_base[insn->op1].type);

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, rs1, insn->op1)) goto fail;
				emit32(ctx, rv_fcvt_from_x(w, uns, (uint32_t)(rdtmp - IR_REG_FP_FIRST),
				                           (uint32_t)rs1));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_FP2INT: {
				int32_t rd  = riscv_reg_of(ctx, ref, 0);
				int32_t rs1 = riscv_reg_of(ctx, ref, 1);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				bool w = ir_type_size[insn->type] > 4;
				bool uns = !IR_IS_TYPE_SIGNED(insn->type);

				if (rd == IR_REG_NONE || rs1 == IR_REG_NONE) goto fail;
				if (!rv_emit_get_fp(ctx, rs1 - IR_REG_FP_FIRST, insn->op1, IR_REG_T5)) goto fail;
				emit32(ctx, rv_fcvt_x(w, uns, (uint32_t)rdtmp,
				                      (uint32_t)(rs1 - IR_REG_FP_FIRST)));
				if (rd_spilled) {
					rv_emit_store_def(ctx, ref, insn->type, rdtmp);
				}
				break;
			}

			case IR_RISCV_RULE_SWITCH: {
				int32_t cond = riscv_reg_of(ctx, ref, 2);
				int32_t tmp  = riscv_reg_of(ctx, ref, 3);
				ir_block *bb = &ctx->cfg_blocks[b];
				uint32_t *p = &ctx->cfg_edges[bb->successors];
				uint32_t n;

				if (cond == IR_REG_NONE || !rv_emit_get(ctx, cond, insn->op2)) goto fail;
				for (n = bb->successors_count; n != 0; p++, n--) {
					uint32_t succ = *p;
					ir_insn *sinsn = &ctx->ir_base[ctx->cfg_blocks[succ].start];

					if (sinsn->op == IR_CASE_VAL) {
						int64_t case_val = ctx->ir_base[sinsn->op2].val.i64;

						if (tmp == IR_REG_NONE) goto fail;
						rv_emit_li64(ctx, (uint32_t)tmp, case_val);
						/* bne cond, tmp, +8; jal succ */
						emit32(ctx, rv_enc_b(8, 0, (uint32_t)cond, RV_F3_BNE, RV_OP_BRANCH));
						{
							rv_br_fixup *fx = ir_mem_malloc(sizeof(rv_br_fixup));

							fx->is_jal = 1;
							fx->f3 = 0;
							fx->cond_reg = 0;
							fx->target_block = succ;
							fx->patch_off = (int32_t)((char*)ctx->code_buffer->pos - (char*)start);
							fx->next = rv_br_fixups;
							rv_br_fixups = fx;
							emit32(ctx, 0); /* jal case block */
						}
					} else if (sinsn->op == IR_CASE_DEFAULT || sinsn->op == IR_CASE_RANGE) {
						rv_br_fixup *fx = ir_mem_malloc(sizeof(rv_br_fixup));

						fx->is_jal = 1;
						fx->f3 = 0;
						fx->cond_reg = 0;
						fx->target_block = succ;
						fx->patch_off = (int32_t)((char*)ctx->code_buffer->pos - (char*)start);
						fx->next = rv_br_fixups;
						rv_br_fixups = fx;
						emit32(ctx, 0); /* jal default/range block */
					}
				}
				break;
			}

			case IR_RISCV_RULE_IJMP: {
				uint32_t words[8];
				void *addr;
				int n, i;

				if (!IR_IS_CONST_REF(insn->op2)) goto fail;
				{
					const ir_insn *ai = &ctx->ir_base[insn->op2];

					if (ai->op == IR_FUNC) {
						const char *name = ir_get_str(ctx, ai->val.name);

						addr = (ctx->loader && ctx->loader->resolve_sym_name)
							? ctx->loader->resolve_sym_name(ctx->loader, name, 0)
							: ir_resolve_sym_name(name);
					} else if (ai->op == IR_ADDR || ai->op == IR_FUNC_ADDR) {
						addr = (void*)(uintptr_t)ai->val.addr;
					} else {
						goto fail;
					}
				}
				if (!addr) goto fail;
				n = rv_li64_fixed_words(IR_REG_T6, (int64_t)(uintptr_t)addr, words);
				for (i = 0; i < n; i++) {
					emit32(ctx, words[i]);
				}
				emit32(ctx, rv_jalr(0 /* x0 */, IR_REG_T6, 0));
				break;
			}

			case IR_RISCV_RULE_ARGVAL:
				/* emitted as part of the containing CALL */
				break;

			case IR_RISCV_RULE_VA_COPY: {
				int32_t dst = riscv_reg_of(ctx, ref, 2);
				int32_t src = riscv_reg_of(ctx, ref, 3);
				int32_t i;

				if (dst == IR_REG_NONE || src == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, src, insn->op3)) goto fail;
				if (!rv_emit_get(ctx, dst, insn->op2)) goto fail;
				for (i = 0; i < 24; i += 8) {
					emit32(ctx, rv_load(3, IR_REG_T4, (uint32_t)src, i));
					emit32(ctx, rv_store(3, IR_REG_T4, (uint32_t)dst, i));
				}
				break;
			}

			case IR_RISCV_RULE_VA_START: {
				int32_t ap = riscv_reg_of(ctx, ref, 2);
				int32_t frame = ctx->stack_frame_size;
				int32_t save_off = ctx->locals_area_size;
				int named_gprs = 0;
				ir_ref i;
				ir_regset used;
				ir_reg reg;

				if (ap == IR_REG_NONE || !rv_emit_get(ctx, ap, insn->op2)) goto fail;
				for (i = 1; i < ctx->insns_count; i++) {
					if (ctx->ir_base[i].op == IR_PARAM
					 && IR_IS_TYPE_INT(ctx->ir_base[i].type)) {
						named_gprs++;
					}
				}
				/* the GPR snapshot starts after the saved preserved regs,
				 * ra and FP preserved regs (see rv_emit_prologue) */
				used = IR_REGSET_INTERSECTION((ir_regset)ctx->used_preserved_regs, IR_REGSET_GP);
				IR_REGSET_FOREACH(used, reg) {
					save_off += sizeof(void*);
				} IR_REGSET_FOREACH_END();
				if (ctx->flags2 & IR_HAS_CALLS) {
					save_off += sizeof(void*);
				}
				used = IR_REGSET_INTERSECTION((ir_regset)ctx->used_preserved_regs, IR_REGSET_FP);
				IR_REGSET_FOREACH(used, reg) {
					save_off += sizeof(void*);
				} IR_REGSET_FOREACH_END();
				/* riscv64 psABI va_list: gp_offset@0, fp_offset@4,
				 * overflow_arg_area@8, reg_save_area@16 */
				if (save_off < -2048 || save_off > 2047) goto fail;
				emit32(ctx, rv_addi(IR_REG_T6, IR_REG_SP, save_off));
				emit32(ctx, rv_store(3, IR_REG_T6, (uint32_t)ap, 16));       /* reg_save_area */
				emit32(ctx, rv_addi(IR_REG_T6, 0, named_gprs * 8));
				emit32(ctx, rv_store(2, IR_REG_T6, (uint32_t)ap, 0));        /* gp_offset (sw) */
				emit32(ctx, rv_store(2, 0, (uint32_t)ap, 4));                /* fp_offset = 0 */
				/* psABI: va_list points after the last named argument on the
				 * stack; named GPR args beyond the 8 a0-a7 slots land on the
				 * stack before any variadic args, so skip them. */
				if (named_gprs > 8) frame += (named_gprs - 8) * 8;
				if (frame < -2048 || frame > 2047) goto fail;
				emit32(ctx, rv_addi(IR_REG_T6, IR_REG_SP, frame));
				emit32(ctx, rv_store(3, IR_REG_T6, (uint32_t)ap, 8));        /* overflow_arg_area */
				break;
			}

			case IR_RISCV_RULE_VA_ARG: {
				int32_t rd   = riscv_reg_of(ctx, ref, 0);
				bool rd_spilled = IR_REG_SPILLED(rd);
				int32_t rdtmp = rd_spilled ? IR_REG_NUM(rd) : rd;
				int32_t ap = riscv_reg_of(ctx, ref, 2);
				bool fp = IR_IS_TYPE_FP(insn->type);

				if (ap != IR_REG_NONE) ap = IR_REG_NUM(ap);
				if (rd == IR_REG_NONE || ap == IR_REG_NONE) goto fail;
				if (!rv_emit_get(ctx, ap, insn->op2)) goto fail;
				/* t6 = gp_offset; t6 >= 64 means the GPR window is exhausted */
				emit32(ctx, rv_load(2, IR_REG_T6, (uint32_t)ap, 0));         /* lw t6, gp_offset */
				emit32(ctx, rv_addi(IR_REG_T5, 0, 64));
				emit32(ctx, rv_enc_b(28, (uint32_t)IR_REG_T5, (uint32_t)IR_REG_T6, RV_F3_BGEU, RV_OP_BRANCH));
				/* GPR path: value at reg_save_area + gp_offset */
				emit32(ctx, rv_load(3, IR_REG_T5, (uint32_t)ap, 16));        /* ld t5, reg_save_area */
				emit32(ctx, rv_alu(0, 0, RV_F3_ADD, IR_REG_T5, IR_REG_T5, IR_REG_T6));
				emit32(ctx, rv_alui(0, RV_F3_ADDI, IR_REG_T6, IR_REG_T6, 8));
				emit32(ctx, rv_store(2, IR_REG_T6, (uint32_t)ap, 0));        /* sw t6, gp_offset */
				emit32(ctx, rv_load(3, IR_REG_T5, IR_REG_T5, 0));            /* ld t5, value */
				emit32(ctx, rv_enc_j(24, 0, RV_OP_JAL));
				/* overflow path: value at overflow_arg_area, then advance it */
				emit32(ctx, rv_load(3, IR_REG_T5, (uint32_t)ap, 8));         /* ld t5, overflow_arg_area */
				emit32(ctx, rv_load(3, IR_REG_T6, IR_REG_T5, 0));            /* ld t6, value */
				emit32(ctx, rv_alui(0, RV_F3_ADDI, IR_REG_T5, IR_REG_T5, 8));
				emit32(ctx, rv_store(3, IR_REG_T5, (uint32_t)ap, 8));        /* sd t5, overflow_arg_area */
				emit32(ctx, rv_mv(IR_REG_T5, IR_REG_T6));
				if (fp) {
					/* variadic FP args arrive in GPRs */
					if (rdtmp < IR_REG_FP_FIRST) goto fail;
					if (insn->type == IR_FLOAT) {
						emit32(ctx, rv_fmv_w_x((uint32_t)(rdtmp - IR_REG_FP_FIRST), (uint32_t)IR_REG_T5));
					} else {
						emit32(ctx, rv_fmv_d_x((uint32_t)(rdtmp - IR_REG_FP_FIRST), (uint32_t)IR_REG_T5));
					}
				} else {
					emit32(ctx, rv_mv((uint32_t)rdtmp, (uint32_t)IR_REG_T5));
				}
				if (rd_spilled) {
					if (fp) {
						emit32(ctx, rv_store(rv_store_f3(insn->type),
						                     (uint32_t)(rdtmp - IR_REG_FP_FIRST),
						                     IR_REG_SP, ir_get_spill_slot_offset(ctx, ref)));
					} else {
						rv_emit_store_def(ctx, ref, insn->type, rdtmp);
					}
				}
				break;
			}

			case IR_RISCV_RULE_RETURN: {
				if (insn->op2 != 0
				 && ir_type_size[ctx->ir_base[insn->op2].type] != 0) {
					if (IR_IS_TYPE_FP(ctx->ir_base[insn->op2].type)) {
						int32_t fa = riscv_reg_of(ctx, ref, 2) - IR_REG_FP_FIRST;
						int32_t tmp = riscv_reg_of(ctx, ref, 3);

						if (tmp == IR_REG_NONE || !rv_emit_get_fp(ctx, fa, insn->op2, tmp)) goto fail;
					} else {
						if (!rv_emit_get(ctx, IR_REG_A0, insn->op2)) goto fail;
					}
				}
				rv_emit_epilogue(ctx);
				break;
			}

				case IR_RISCV_RULE_NONE:
				default:
					if (insn->op != IR_NOP) {
						goto fail;
					}
					break;
			}
			ref += ir_insn_len(insn);
		}
		if (bb->flags & IR_BB_DESSA_MOVES) {
			ir_gen_dessa_moves(ctx, b, rv_emit_dessa_copy, NULL);
		}
		if (bb->successors_count == 1) {
			/* END/LOOP_END-terminated blocks fall through to their single
			 * successor only when the scheduler placed that successor next;
			 * otherwise emit a jump to it (mirrors ir_x86.dasc IR_END). */
			ir_insn *e = &ctx->ir_base[bb->end];

			if (e->op == IR_END || e->op == IR_LOOP_END) {
				ir_ref succ = ctx->cfg_edges[bb->successors];
				ir_ref target = ir_skip_empty_target_blocks(ctx, succ);

				if (target != ir_next_block(ctx, _b)) {
					rv_br_fixup *fx = ir_mem_malloc(sizeof(rv_br_fixup));

					fx->is_jal = 1;
					fx->f3 = 0;
					fx->cond_reg = 0;
					fx->target_block = target;
					fx->patch_off = (int32_t)((char*)ctx->code_buffer->pos - (char*)start);
					fx->next = rv_br_fixups;
					rv_br_fixups = fx;
					emit32(ctx, 0);
				}
			}
		}
	}

	/* Patch branch fixups now that every block position is known. */
	for (fx = rv_br_fixups; fx; fx = fx->next) {
		uint32_t *patch = (uint32_t*)((char*)start + fx->patch_off);
		int32_t off = block_off[fx->target_block] - fx->patch_off;
		uint32_t word;

		if ((ctx->cfg_blocks[fx->target_block].flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_EMPTY)) == IR_BB_EMPTY) {
			/* An empty target block emits no code and shares the offset of
			 * the block that follows it in the schedule; jump past that
			 * block instead of into its first instruction. */
			uint32_t _b;
			int32_t t = block_off[fx->target_block];

			for (_b = 1; _b <= ctx->cfg_blocks_count; _b++) {
				int32_t o = block_off[ctx->cfg_schedule[_b]];

				if (o > t) {
					off = o - fx->patch_off;
					break;
				}
			}
		}
		if (fx->is_jal) {
			word = rv_enc_j(off, 0 /* x0 */, RV_OP_JAL);
		} else {
			word = rv_enc_b(off, 0 /* x0 */, (uint32_t)fx->cond_reg, fx->f3, RV_OP_BRANCH);
		}
		*patch = word;
	}
	rv_br_fixups = NULL;
	ir_mem_free(block_off);

	/* Append inline string data and patch the placeholder constants. */
	if (rv_str_fixups) {
		rv_str_fixup *fx;

		ctx->code_buffer->pos = (char*)IR_ALIGNED_SIZE((size_t)ctx->code_buffer->pos, 8);
		for (fx = rv_str_fixups; fx; fx = fx->next) {
			const ir_insn *ai = &ctx->ir_base[fx->str_ref];
			uint32_t words[8];
			char *str_pos;
			void *addr;
			int n, i;

			if (fx->raw) {
				if ((char*)ctx->code_buffer->end - (char*)ctx->code_buffer->pos < 16) {
					ctx->code_buffer->pos = start;
					ctx->status = IR_ERROR_CODE_MEM_OVERFLOW;
					return NULL;
				}
				str_pos = ctx->code_buffer->pos;
				memcpy(str_pos, &ai->val.u64, sizeof(uint64_t));
				ctx->code_buffer->pos = str_pos + 8;
				ctx->code_buffer->pos = (char*)IR_ALIGNED_SIZE((size_t)ctx->code_buffer->pos, 8);
			} else {
				size_t len;
				const char *str = ir_get_strl(ctx, ai->val.str, &len);

				if ((char*)ctx->code_buffer->end - (char*)ctx->code_buffer->pos
				    < (ptrdiff_t)(len + 1 + 8)) {
					ctx->code_buffer->pos = start;
					ctx->status = IR_ERROR_CODE_MEM_OVERFLOW;
					return NULL;
				}
				str_pos = ctx->code_buffer->pos;
				memcpy(str_pos, str, len);
				str_pos[len] = '\0';
				ctx->code_buffer->pos = str_pos + len + 1;
				ctx->code_buffer->pos = (char*)IR_ALIGNED_SIZE((size_t)ctx->code_buffer->pos, 8);
			}
			addr = (char*)start + (str_pos - (char*)start);
			/* reuse the placeholder's destination register (rd field of the first word) */
			{
				uint32_t *patch = (uint32_t*)((char*)ctx->code_buffer->start + fx->patch_offset);
				uint32_t rd = (patch[0] >> 7) & 0x1F;

				n = rv_li64_fixed_words(rd, (int64_t)(uintptr_t)addr, words);
				for (i = 0; i < n; i++) {
					patch[i] = words[i];
				}
			}
		}
		rv_str_fixups = NULL;
	}

	*size = (size_t)((char *)ctx->code_buffer->pos - (char *)start);
	return start;

fail:
	ctx->code_buffer->pos = start;
	ctx->status = IR_ERROR_UNSUPPORTED_CODE_RULE;
#ifdef IR_DEBUG_MESSAGES
	fprintf(stderr, "RVFAIL ref=%d op=%s rule=0x%x\n", ref, insn ? ir_op_name[insn->op] : "?", rule);
#endif
	return NULL;
}

void ir_fix_stack_frame(ir_ctx *ctx)
{
	uint32_t additional_size = 0;

	ctx->locals_area_size = ctx->stack_frame_size;

	if (ctx->used_preserved_regs) {
		ir_regset used_preserved_regs = (ir_regset)ctx->used_preserved_regs;
		ir_reg reg;
		(void)reg;

		IR_REGSET_FOREACH(used_preserved_regs, reg) {
			additional_size += sizeof(void*);
		} IR_REGSET_FOREACH_END();
	}
	if (ctx->flags2 & IR_HAS_CALLS) {
		additional_size += sizeof(void*); /* ra save slot */
	}
	if (ctx->flags2 & IR_HAS_VA_START) {
		/* 8 GPR slots to snapshot the incoming register arguments for va_list */
		additional_size += 8 * sizeof(void*);
	}

	ctx->stack_frame_size = IR_ALIGNED_SIZE(ctx->stack_frame_size, sizeof(void*));
	ctx->stack_frame_size += additional_size;
	if (ctx->flags2 & IR_HAS_CALLS) {
		ctx->stack_frame_size = IR_ALIGNED_SIZE(ctx->stack_frame_size, 16);
	}
	ctx->call_stack_size = 0;
}

/* ---- calling convention / scratch register set ---- */

#define IR_REG_SCRATCH_RISCV64  IR_REG_SET_1

/* s0/s1 and s2-s11 are not contiguous in riscv register numbering
 * (a0-a7 sit between s1 and s2), so the preserved set is two pieces. */
#define IR_REGSET_PRESERVED_GP_RISCV64 \
	(IR_REGSET(IR_REG_S0) | IR_REGSET(IR_REG_S1) | IR_REGSET_INTERVAL(IR_REG_S2, IR_REG_S11))
#define IR_REGSET_PRESERVED_FP_RISCV64 \
	(IR_REGSET(IR_REG_FS0) | IR_REGSET(IR_REG_FS1) | IR_REGSET_INTERVAL(IR_REG_FS2, IR_REG_FS11))

#define IR_REGSET_SCRATCH_RISCV64 \
	(IR_REGSET_DIFFERENCE(IR_REGSET_GP, IR_REGSET_PRESERVED_GP_RISCV64) \
	| IR_REGSET_DIFFERENCE(IR_REGSET_FP, IR_REGSET_PRESERVED_FP_RISCV64))

const ir_regset ir_scratch_regset[] = {
	IR_REGSET_INTERVAL(IR_REG_GP_FIRST, IR_REG_FP_LAST), /* index 0: IR_REG_ALL fallback */
	IR_REGSET_SCRATCH_RISCV64,                            /* index 1: IR_REG_SET_1 */
};

static const int8_t riscv_int_param_regs[8] = {
	IR_REG_A0, IR_REG_A1, IR_REG_A2, IR_REG_A3,
	IR_REG_A4, IR_REG_A5, IR_REG_A6, IR_REG_A7,
};
static const int8_t riscv_fp_param_regs[8] = {
	IR_REG_FA0, IR_REG_FA1, IR_REG_FA2, IR_REG_FA3,
	IR_REG_FA4, IR_REG_FA5, IR_REG_FA6, IR_REG_FA7,
};

const ir_call_conv_dsc ir_call_conv_riscv64_lp64d = {
	0,           /* cleanup_stack_by_callee */
	1,           /* pass_struct_by_val      */
	1,           /* sysv_varargs            */
	0,           /* shadow_param_regs       */
	0,           /* shadow_store_size       */
	8,           /* int_param_regs_count    */
	8,           /* fp_param_regs_count     */
	0,           /* vector_param_regs_count */
	IR_REG_A0,   /* int_ret_reg             */
	IR_REG_NONE, /* int_ret2_reg            */
	IR_REG_FA0,  /* fp_ret_reg              */
	IR_REG_NONE, /* fp_ret2_reg             */
	IR_REG_NONE, /* vector_ret_reg          */
	IR_REG_NONE, /* vector_ret2_reg         */
	IR_REG_NONE, /* fp_varargs_reg          */
	IR_REG_SCRATCH_RISCV64,
	riscv_int_param_regs,
	riscv_fp_param_regs,
	NULL,        /* vector_param_regs       */
	IR_REGSET_PRESERVED_GP_RISCV64 | IR_REGSET_PRESERVED_FP_RISCV64,
};

const ir_call_conv_dsc *ir_get_call_conv_dsc(uint32_t flags)
{
	(void)flags;
	return &ir_call_conv_riscv64_lp64d;
}

const ir_proto_t *ir_call_proto(const ir_ctx *ctx, const ir_insn *insn);

/* ---- debug / dump string helpers ---- */

const char *ir_reg_name(int8_t reg, ir_type type)
{
	(void)type;
	static const char *gp_names[] = {
#define IR_RISCV_GP_NAME(code, name) #name,
		IR_GP_REGS(IR_RISCV_GP_NAME)
#undef IR_RISCV_GP_NAME
	};
	static const char *fp_names[] = {
#define IR_RISCV_FP_NAME(code, name) #name,
		IR_FP_REGS(IR_RISCV_FP_NAME)
#undef IR_RISCV_FP_NAME
	};
	if (reg == IR_REG_NONE) {
		return "none";
	} else if (reg >= IR_REG_GP_FIRST && reg <= IR_REG_GP_LAST) {
		return gp_names[reg - IR_REG_GP_FIRST];
	} else if (reg >= IR_REG_FP_FIRST && reg <= IR_REG_FP_LAST) {
		return fp_names[reg - IR_REG_FP_FIRST];
	}
	return "?";
}

void ir_dump_reg(const ir_ctx *ctx, int8_t reg, ir_ref ref, bool store, FILE *f)
{
	(void)ctx; (void)ref; (void)store;
	fprintf(f, "%s", ir_reg_name(reg, IR_VOID));
}

/* Must have one entry per IR_RISCV_RULE_* value above (5 total) ---
 * ir_dump.c indexes this array directly by rule number. */
const char *ir_rule_name[] = {
	"NONE",
	"ADD",
	"SUB",
	"MUL",
	"AND",
	"OR",
	"XOR",
	"SHL",
	"SHR",
	"SAR",
	"NEG",
	"NOT",
	"RETURN",
	"CALL",
	"LOAD",
	"STORE",
	"ZEXT",
	"SEXT",
	"TRUNC",
	"DIV",
	"MOD",
	"MIN",
	"MAX",
	"ROL",
	"ROR",
	"CTLZ",
	"CTTZ",
	"CTPOP",
	"COND",
	"IF",
	"BITCAST",
	"ALLOCA",
	"INT2FP",
	"FP2INT",
	"GUARD",
	"GUARD_NOT",
	"EQ",
	"NE",
	"LT",
	"GT",
	"LE",
	"GE",
	"SKIP",
	"PARAM",
};

void *ir_resolve_sym_name(const char *name)
{
#ifndef _WIN32
# ifdef RTLD_DEFAULT
	return dlsym(RTLD_DEFAULT, name);
# else
	return dlsym(NULL, name);
# endif
#else
	(void)name;
	return NULL;
#endif
}

bool ir_needs_thunk(const ir_code_buffer *code_buffer, void *addr)
{
	(void)code_buffer; (void)addr;
	return false; /* TODO: implement when supporting far calls / trampolines */
}

void *ir_emit_thunk(ir_code_buffer *code_buffer, void *addr, size_t *size_ptr)
{
	uint32_t *p = (uint32_t*)IR_ALIGNED_SIZE((size_t)code_buffer->pos, 4);

	/* auipc t6,0; ld t6,12(t6); jalr x0,t6,0; .quad addr */
	p[0] = (IR_REG_T6 << 7) | 0x17;
	p[1] = rv_load(3, (uint32_t)IR_REG_T6, (uint32_t)IR_REG_T6, 12);
	p[2] = rv_jalr(0, (uint32_t)IR_REG_T6, 0);
	*(uint64_t*)(p + 3) = (uint64_t)(uintptr_t)addr;
	code_buffer->pos = (char*)p + 16;
	*size_ptr = 16;
	return (void*)p;
}

void ir_fix_thunk(void *thunk_entry, void *addr)
{
	*(uint64_t*)((char*)thunk_entry + 12) = (uint64_t)(uintptr_t)addr;
}