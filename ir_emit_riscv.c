/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V 64 code generator --- hand-written binary encoding, no DynAsm)
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

#ifndef _WIN32
# include <dlfcn.h>
#endif

#define IR_SPILL_POS_TO_OFFSET(offset) \
	((offset) + ctx->call_stack_size)

typedef struct _rv_str_fixup {
	int32_t patch_offset;
	ir_ref  str_ref;
	struct _rv_str_fixup *next;
} rv_str_fixup;

static rv_str_fixup *rv_str_fixups;

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
	IR_RISCV_RULE_SKIP,    /* IR_START and other pure control markers -> no code emitted */
	IR_RISCV_RULE_PARAM,   /* IR_PARAM  -> no code emitted; def_reg pinned to a0-a7 per ABI */
};

static ir_reg ir_get_param_reg(const ir_ctx *ctx, ir_ref ref)
{
	ir_insn *insn = &ctx->ir_base[ref];
	/* insn->op3 = argument position (see PARAM's "num" operand in ir.h) */
	int32_t pos = insn->op3;
	static const ir_reg gp_arg_regs[8] = {
		IR_REG_A0, IR_REG_A1, IR_REG_A2, IR_REG_A3,
		IR_REG_A4, IR_REG_A5, IR_REG_A6, IR_REG_A7,
	};
	/* VERIFY: assumes int/pointer args only use gp_arg_regs and a
	 * separate fp counter isn't needed for this first pass (add.ir
	 * has no float params) --- float args would need fa0-fa7 tracked
	 * separately once fadd.ir-style tests are in scope. */
	if (pos >= 1 && pos <= 8) {
		return gp_arg_regs[pos - 1];
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

	n = insn->inputs_count;
	n = IR_MIN(n, IR_MAX_REG_ARGS + 2);
	for (j = 3; j <= n; j++) {
		ir_insn *arg = &ctx->ir_base[ir_insn_op(insn, j)];

		type = arg->type;
		if (IR_IS_TYPE_INT(type)) {
			if (int_param < cc->int_param_regs_count && arg->op != IR_ARGVAL) {
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
			ctx->loader->resolve_sym_name(ctx->loader, ctx, addr_insn->val.name, 0) :
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
				flags |= IR_OP1_MUST_BE_IN_REG;
			} else if (ir_is_addr_const(ctx, ctx->ir_base[ref].op1)) {
				constraints->tmp_regs[n] = IR_TMP_REG(1, IR_ADDR, IR_LOAD_SUB_REF, IR_DEF_SUB_REF);
				n++;
			}
			if (!IR_IS_CONST_REF(ctx->ir_base[ref].op2)) {
				flags |= IR_OP2_MUST_BE_IN_REG;
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
				constraints->hints[2] = IR_REG_NONE;
				constraints->hints_count = ir_get_args_regs(ctx, insn, cc, constraints->hints);
			}
			flags = IR_USE_SHOULD_BE_IN_REG | IR_OP2_SHOULD_BE_IN_REG | IR_OP3_SHOULD_BE_IN_REG;
			break;
		}

		case IR_RISCV_RULE_LOAD:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP2_MUST_BE_IN_REG
			      | IR_DEF_CONFLICTS_WITH_INPUT_REGS;
			break;

		case IR_RISCV_RULE_STORE:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP2_MUST_BE_IN_REG
			      | IR_OP3_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_ZEXT:
		case IR_RISCV_RULE_SEXT:
		case IR_RISCV_RULE_TRUNC:
			flags = IR_USE_MUST_BE_IN_REG | IR_OP1_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_RETURN:
			flags = IR_OP2_MUST_BE_IN_REG;
			if (!IR_IS_CONST_REF(ctx->ir_base[ref].op2)) {
				constraints->hints[2] =
					IR_IS_TYPE_INT(ctx->ir_base[ctx->ir_base[ref].op2].type)
						? IR_REG_A0 : IR_REG_FA0;
				constraints->hints_count = 3;
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
	}
}

static void rv_emit_epilogue(ir_ctx *ctx)
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
		}
		emit32(ctx, rv_addi(IR_REG_SP, IR_REG_SP, frame));
	}
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

static void rv_emit_store_def(ir_ctx *ctx, ir_ref ref, ir_type type, int32_t reg)
{
	int32_t off = ir_get_spill_slot_offset(ctx, ref);

	IR_ASSERT(off >= -2048 && off <= 2047);
	emit32(ctx, rv_store(rv_store_f3(type), (uint32_t)reg, IR_REG_SP, off));
}

static bool rv_emit_get(ir_ctx *ctx, int32_t reg, ir_ref val_ref)
{
	int32_t home;

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
		emit32(ctx, rv_mv((uint32_t)reg, (uint32_t)home));
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

	if (rd == IR_REG_NONE) {
		return false;
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
		emit32(ctx, rv_alu(w, dsc->r_f7, dsc->r_f3,
		                   (uint32_t)rdtmp, (uint32_t)rs1, (uint32_t)rs2));
	}
	if (rd_spilled) {
		rv_emit_store_def(ctx, ref, insn->type, rdtmp);
	}
	return true;
}

void *ir_emit_code(ir_ctx *ctx, size_t *size)
{
	void *start;
	uint32_t b;
	ir_insn *insn = NULL;
	ir_ref ref = 0;
	uint32_t rule = 0;

	if (!ctx->code_buffer) {
		ctx->status = IR_ERROR_CODE_MEM_OVERFLOW;
		return NULL;
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

	for (b = 1; b <= ctx->cfg_blocks_count; b++) {
		ir_block *bb = &ctx->cfg_blocks[b];

		for (ref = bb->start; ref <= bb->end; ) {
			insn = &ctx->ir_base[ref];
			rule = ctx->rules[ref];

			if (rule & IR_SKIPPED) {
				ref += ir_insn_len(insn);
				continue;
			}

			switch (rule & IR_RULE_MASK) {
				case IR_RISCV_RULE_SKIP:
				case IR_RISCV_RULE_PARAM:
					/* No machine code for these --- START is a
					 * control marker, PARAM's value already
					 * lives in the ABI-mandated register. */
					break;

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

				if (!IR_IS_CONST_REF(insn->op2)) goto fail;
				memset(want, IR_REG_NONE, sizeof(want));
				ir_get_args_regs(ctx, insn, cc, want);

				/* Pass 1: register args. Pass 2: constant args --- a constant
				 * materialization into aX must not clobber a live value
				 * that a later register-arg move still needs. */
				for (k = 0; k < 2; k++) {
					for (j = 3; j <= n_args; j++) {
						ir_ref arg = ir_insn_op(insn, j);
						int32_t dst = want[j];
						int32_t src;
						bool is_const = IR_IS_CONST_REF(arg);

						if (dst == IR_REG_NONE) goto fail; /* stack args: later slice */
						if (is_const != (k == 1)) continue;
						if (is_const) {
							if (!rv_emit_get(ctx, dst, arg)) goto fail;
							continue;
						}
						src = ctx->regs[arg][0];
						if (src == IR_REG_NONE || IR_REG_SPILLED(src)) goto fail;
						if (src != dst) {
							int m;

							for (m = 3; m <= n_args; m++) {
								ir_ref other = ir_insn_op(insn, m);

								if (m != j && !IR_IS_CONST_REF(other)
								 && ctx->regs[other][0] == dst) {
									goto fail; /* move cycle: later slice */
								}
							}
							emit32(ctx, rv_mv((uint32_t)dst, (uint32_t)src));
						}
					}
				}

				rv_emit_li64(ctx, IR_REG_T6,
					(int64_t)(uintptr_t)ir_call_addr(ctx, insn, &ctx->ir_base[insn->op2]));
				emit32(ctx, rv_jalr(IR_REG_RA, IR_REG_T6, 0));

				if (insn->type != IR_VOID) {
					int32_t def_reg;
					bool def_spilled;
					int32_t deftmp;

					if (!IR_IS_TYPE_INT(insn->type)) goto fail;
					def_reg = ctx->regs[ref][0];
					if (def_reg == IR_REG_NONE) {
						break; /* unused result --- value stays in a0, nobody reads it */
					}
					def_spilled = IR_REG_SPILLED(def_reg);
					deftmp = def_spilled ? IR_REG_NUM(def_reg) : def_reg;
					if (deftmp != cc->int_ret_reg) {
						emit32(ctx, rv_mv((uint32_t)deftmp, (uint32_t)cc->int_ret_reg));
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
				if (!rv_emit_get(ctx, base, insn->op2)) goto fail;
				if (!rv_emit_get(ctx, val, insn->op3)) goto fail;
				emit32(ctx, rv_store(rv_store_f3(insn->type),
				                      (uint32_t)val, (uint32_t)base, 0));
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

			case IR_RISCV_RULE_RETURN: {
				if (!rv_emit_get(ctx, IR_REG_A0, insn->op2)) goto fail;
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
	}

	/* Append inline string data and patch the placeholder constants. */
	if (rv_str_fixups) {
		rv_str_fixup *fx;

		ctx->code_buffer->pos = (char*)IR_ALIGNED_SIZE((size_t)ctx->code_buffer->pos, 8);
		for (fx = rv_str_fixups; fx; fx = fx->next) {
			const ir_insn *ai = &ctx->ir_base[fx->str_ref];
			size_t len;
			const char *str = ir_get_strl(ctx, ai->val.str, &len);
			uint32_t words[8];
			char *str_pos;
			void *addr;
			int n, i;

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
	(void)code_buffer; (void)addr;
	*size_ptr = 0;
	return NULL; /* TODO: implement when needed */
}

void ir_fix_thunk(void *thunk_entry, void *addr)
{
	(void)thunk_entry; (void)addr;
	/* TODO: implement when needed */
}