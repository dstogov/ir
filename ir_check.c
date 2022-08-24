#include "ir.h"
#include "ir_private.h"

void ir_consistency_check(void)
{
	IR_ASSERT(IR_UNUSED == 0);
	IR_ASSERT(IR_NOP == 0);

	IR_ASSERT((int)IR_BOOL   == (int)IR_C_BOOL);
	IR_ASSERT((int)IR_U8     == (int)IR_C_U8);
	IR_ASSERT((int)IR_U16    == (int)IR_C_U16);
	IR_ASSERT((int)IR_U32    == (int)IR_C_U32);
	IR_ASSERT((int)IR_U64    == (int)IR_C_U64);
	IR_ASSERT((int)IR_ADDR   == (int)IR_C_ADDR);
	IR_ASSERT((int)IR_CHAR   == (int)IR_C_CHAR);
	IR_ASSERT((int)IR_I8     == (int)IR_C_I8);
	IR_ASSERT((int)IR_I16    == (int)IR_C_I16);
	IR_ASSERT((int)IR_I32    == (int)IR_C_I32);
	IR_ASSERT((int)IR_I64    == (int)IR_C_I64);
	IR_ASSERT((int)IR_DOUBLE == (int)IR_C_DOUBLE);
	IR_ASSERT((int)IR_FLOAT  == (int)IR_C_FLOAT);

	IR_ASSERT((IR_EQ ^ 1) == IR_NE);
	IR_ASSERT((IR_LT ^ 3) == IR_GT);
	IR_ASSERT((IR_GT ^ 3) == IR_LT);
	IR_ASSERT((IR_LE ^ 3) == IR_GE);
	IR_ASSERT((IR_GE ^ 3) == IR_LE);
	IR_ASSERT((IR_ULT ^ 3) == IR_UGT);
	IR_ASSERT((IR_UGT ^ 3) == IR_ULT);
	IR_ASSERT((IR_ULE ^ 3) == IR_UGE);
	IR_ASSERT((IR_UGE ^ 3) == IR_ULE);

	IR_ASSERT(IR_ADD + 1 == IR_SUB);
}

void ir_check(ir_ctx *ctx)
{
	//TODO:
	ir_ref i, j, n, *p, use;
	ir_insn *insn;
	uint32_t flags;
	bool ok = 1;

	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count;) {
		flags = ir_op_flags[insn->op];
		n = ir_input_edges_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			use = *p;
			if (use != IR_UNUSED) {
				if (IR_IS_CONST_REF(use)) {
					if (use >= ctx->consts_count) {
						fprintf(stderr, "ir_base[%d].ops[%d] constant reference (%d) is out of range\n", i, j, use);
						ok = 0;
					}
				} else {
					if (use >= ctx->insns_count) {
						fprintf(stderr, "ir_base[%d].ops[%d] insn reference (%d) is out of range\n", i, j, use);
						ok = 0;
					}
					switch (IR_OPND_KIND(flags, j)) {
						case IR_OPND_DATA:
							if (ctx->ir_base[use].op == IR_VAR
							 || !(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_DATA)) {
								if (!(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_MEM)
								 || ctx->ir_base[use].type == IR_VOID) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be DATA\n", i, j, use);
									ok = 0;
								}
							}
							if (use >= i
							 && !(insn->op == IR_PHI && j > 2 && ctx->ir_base[insn->op1].op == IR_LOOP_BEGIN)) {
								fprintf(stderr, "ir_base[%d].ops[%d] invalid forward reference (%d)\n", i, j, use);
								ok = 0;
							}
							break;
						case IR_OPND_CONTROL:
							if (flags & IR_OP_FLAG_BB_START) {
								if (!(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_BB_END)) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be BB_END\n", i, j, use);
									ok = 0;
								}
							} else {
								if (ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_BB_END) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must not be BB_END\n", i, j, use);
									ok = 0;
								}
							}
						case IR_OPND_CONTROL_DEP:
							if (use >= i
							 && !(insn->op == IR_LOOP_BEGIN && j > 1)) {
								fprintf(stderr, "ir_base[%d].ops[%d] invalid forward reference (%d)\n", i, j, use);
								ok = 0;
							}
						case IR_OPND_CONTROL_REF:
							if (!(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL)) {
								fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be CONTROL\n", i, j, use);
								ok = 0;
							}
							break;
						case IR_OPND_VAR:
							if (ctx->ir_base[use].op != IR_VAR) {
								fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be VAR\n", i, j, use);
								ok = 0;
							}
							if (use >= i) {
								fprintf(stderr, "ir_base[%d].ops[%d] invalid forward reference (%d)\n", i, j, use);
								ok = 0;
							}
							break;
						default:
							fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) of unsupported kind\n", i, j, use);
							ok = 0;
					}
				}
			} else if ((insn->op == IR_RETURN || insn->op == IR_UNREACHABLE) && j == 2) {
				/* pass (function returns void) */
			} else if (insn->op == IR_BEGIN && j == 1) {
				/* pass (start of unreachable basic block) */
			} else if (insn->op == IR_LOOP_BEGIN && j > 1) {
				/* TODO: something wrong ??? */
			} else if (IR_OPND_KIND(flags, j) != IR_OPND_CONTROL_REF) {
				fprintf(stderr, "ir_base[%d].ops[%d] missing reference (%d)\n", i, j, use);
				ok = 0;
			}
		}
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		i += n;
		insn += n;
	}

	IR_ASSERT(ok);
}
