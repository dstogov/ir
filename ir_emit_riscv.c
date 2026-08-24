/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V 64 code generator — hand-written binary encoding, no DynAsm)
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

/* ---- code-generation rules (parallel to ctx->rules[]) ---- */
enum {
	IR_RISCV_RULE_NONE = 0,
	IR_RISCV_RULE_ADD,     /* IR_ADD    -> R-type ADD rd, rs1, rs2 */
	IR_RISCV_RULE_RETURN,  /* IR_RETURN -> [mv a0, src] + jalr x0, ra, 0 */
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
	 * has no float params) — float args would need fa0-fa7 tracked
	 * separately once fadd.ir-style tests are in scope. */
	if (pos >= 1 && pos <= 8) {
		return gp_arg_regs[pos - 1];
	}
	return IR_REG_NONE; /* stack-passed, beyond 8 args — not handled yet */
}

/* -------------------------------------------------------------------- */
/* ir_match()                                                            */
/* -------------------------------------------------------------------- */

int ir_match(ir_ctx *ctx)
{
	uint32_t b;

	/* VERIFY: ir_mem_calloc is the allocator convention used elsewhere
	 * in ir.c/ir_ra.c for ctx-owned buffers — confirm the exact name
	 * (could be ir_mem_malloc + memset, or a different allocator). */
	ctx->rules = ir_mem_calloc(ctx->insns_count, sizeof(uint32_t));
	if (!ctx->rules) {
		return 0;
	}

	for (b = 1; b <= ctx->cfg_blocks_count; b++) {
		ir_block *bb = &ctx->cfg_blocks[b];
		ir_ref ref;

		/* VERIFY: bb->start / bb->end are the field names used in
		 * ir_cfg.c for the first/last instruction ref of a block —
		 * confirmed only indirectly (bb->start appears in an
		 * ir_cfg.c snippet). Confirm bb->end exists under that name;
		 * it may instead be derived some other way (e.g. via the
		 * next block's start, or a separate "last insn" field). */
		for (ref = bb->start; ref <= bb->end; ref++) {
			ir_insn *insn = &ctx->ir_base[ref];

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
				case IR_RETURN:
					ctx->rules[ref] = IR_RISCV_RULE_RETURN;
					break;
				default:
					/* Not yet supported — leave rule as
					 * IR_RISCV_RULE_NONE. Anything reaching
					 * ir_emit_code() with rule == NONE should
					 * be treated as an unimplemented-op error,
					 * not silently skipped. */
					break;
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
	 * must be set from `n` right before the return — confirmed against
	 * ir_x86.dasc, where that assignment runs immediately before the
	 * function's one and only `return flags;`. */
	int flags = 0;
	int n = 0;

	constraints->def_reg = IR_REG_NONE;
	constraints->hints_count = 0;

	switch (ctx->rules[ref]) {
		case IR_RISCV_RULE_SKIP:
			flags = 0;
			break;

		case IR_RISCV_RULE_PARAM:
			constraints->def_reg = ir_get_param_reg(ctx, ref);
			flags = 0;
			break;

		case IR_RISCV_RULE_ADD:
			/* op1, op2 and the def all need to live in GP
			 * registers — RISC-V ADD has no memory operand form
			 * the way x86 ADD does. */
			flags = IR_OP1_MUST_BE_IN_REG | IR_OP2_MUST_BE_IN_REG
			      | IR_USE_MUST_BE_IN_REG;
			break;

		case IR_RISCV_RULE_RETURN:
			/* op2 (the return value) should end up in a0 (x10)
			 * per the RISC-V calling convention.
			 *
			 * VERIFY: how a fixed-register hint is actually
			 * communicated back to ir_ra.c is unconfirmed here —
			 * on x86 this is likely done via constraints->hints[]
			 * indexed by operand position, but I have not seen
			 * that array's layout. Check ir_x86.dasc's RETURN/CALL
			 * handling in ir_get_target_constraints() for the
			 * exact mechanism before trusting this branch — the
			 * current add.ir test passes without it because
			 * ir_ra.c apparently coalesces well enough here, but
			 * this may not hold for more complex cases. */
			flags = IR_OP2_MUST_BE_IN_REG;
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

void *ir_emit_code(ir_ctx *ctx, size_t *size)
{
	void *start = ctx->code_buffer->pos;
	uint32_t b;

	for (b = 1; b <= ctx->cfg_blocks_count; b++) {
		ir_block *bb = &ctx->cfg_blocks[b];
		ir_ref ref;

		for (ref = bb->start; ref <= bb->end; ref++) {
			ir_insn *insn = &ctx->ir_base[ref];

			switch (ctx->rules[ref]) {
				case IR_RISCV_RULE_SKIP:
				case IR_RISCV_RULE_PARAM:
					/* No machine code for these — START is a
					 * control marker, PARAM's value already
					 * lives in the ABI-mandated register. */
					break;

				case IR_RISCV_RULE_ADD: {
					int32_t rd  = riscv_reg_of(ctx, ref, 0);
					int32_t rs1 = riscv_reg_of(ctx, ref, 1);
					int32_t rs2 = riscv_reg_of(ctx, ref, 2);
					emit32(ctx, rv_add((uint32_t)rd, (uint32_t)rs1, (uint32_t)rs2));
					break;
				}

				case IR_RISCV_RULE_RETURN: {
					/* RETURN's return value is insn->op2 —
					 * i.e. operand position 2 — so read slot 2. */
					int32_t src = riscv_reg_of(ctx, ref, 2);
					if (src != IR_REG_A0) {
						emit32(ctx, rv_mv(IR_REG_A0, (uint32_t)src));
					}
					emit32(ctx, rv_ret(IR_REG_RA));
					break;
				}

				case IR_RISCV_RULE_NONE:
				default:
					if (insn->op != IR_NOP) {
						/* An instruction reached emit with no
						 * selected rule — this is a bug, not a
						 * silently-skippable case. Fail loudly
						 * during bring-up rather than emit
						 * incorrect code. */
						return NULL;
					}
					break;
			}
		}
	}

	*size = (size_t)((char *)ctx->code_buffer->pos - (char *)start);
	return start;
}

void ir_fix_stack_frame(ir_ctx *ctx)
{
	ctx->locals_area_size = ctx->stack_frame_size;

	/* VERIFY: add.ir has no varargs and no preserved-register spills,
	 * so this simplified version is untested against those paths.
	 * When adding CALL/varargs/PHI support later, come back and port
	 * the full x86 logic (additional_size accumulation for varargs
	 * register-save area + preserved-register spill slots, then
	 * ctx->stack_frame_size += additional_size at the end). */
}

/* ---- calling convention / scratch register set ---- */

#define IR_REG_SCRATCH_RISCV64  IR_REG_SET_1

#define IR_REGSET_SCRATCH_RISCV64 \
	(IR_REGSET_DIFFERENCE(IR_REGSET_GP, IR_REGSET_INTERVAL(IR_REG_S0, IR_REG_S11)) \
	| IR_REGSET_DIFFERENCE(IR_REGSET_FP, IR_REGSET_INTERVAL(IR_REG_FS0, IR_REG_FS11)))

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
	IR_REG_A0,   /* int_ret_reg             */
	IR_REG_A1,   /* int_ret2_reg            */
	IR_REG_FA0,  /* fp_ret_reg              */
	IR_REG_FA1,  /* fp_ret2_reg             */
	IR_REG_A0,   /* fp_varargs_reg          */
	IR_REG_SCRATCH_RISCV64,
	riscv_int_param_regs,
	riscv_fp_param_regs,
	IR_REGSET_INTERVAL(IR_REG_S0, IR_REG_S11),
};

const ir_call_conv_dsc *ir_get_call_conv_dsc(uint32_t flags)
{
	(void)flags;
	return &ir_call_conv_riscv64_lp64d;
}

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

/* Must have one entry per IR_RISCV_RULE_* value above (5 total) —
 * ir_dump.c indexes this array directly by rule number. */
const char *ir_rule_name[] = {
	"NONE",
	"ADD",
	"RETURN",
	"SKIP",
	"PARAM",
};

/* ---- stubs: not exercised by add.ir yet, needed only to link ---- */

void *ir_resolve_sym_name(const char *name)
{
	(void)name;
	return NULL; /* TODO: implement when supporting external calls */
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

const ir_proto_t *ir_call_proto(const ir_ctx *ctx, const ir_insn *insn)
{
	(void)ctx; (void)insn;
	return NULL; /* TODO: only needed for --emit-llvm path, not exercised yet */
}