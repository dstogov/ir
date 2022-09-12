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

bool ir_check(ir_ctx *ctx)
{
	//TODO:
	ir_ref i, j, n, *p, use;
	ir_insn *insn, *use_insn;
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
					use_insn = &ctx->ir_base[use];
					switch (IR_OPND_KIND(flags, j)) {
						case IR_OPND_DATA:
							if (use_insn->op == IR_VAR
							 || !(ir_op_flags[use_insn->op] & IR_OP_FLAG_DATA)) {
								if (!(ir_op_flags[use_insn->op] & IR_OP_FLAG_MEM)
								 || use_insn->type == IR_VOID) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be DATA\n", i, j, use);
									ok = 0;
								}
							}
							if (use >= i
							 && !(insn->op == IR_PHI && j > 2 && ctx->ir_base[insn->op1].op == IR_LOOP_BEGIN)) {
								fprintf(stderr, "ir_base[%d].ops[%d] invalid forward reference (%d)\n", i, j, use);
								ok = 0;
							}
							if (flags & IR_OP_FLAG_DATA) {
								switch (insn->op) {
									case IR_COND:
										if (j == 1) {
											break;
										}
									case IR_ADD:
									case IR_SUB:
									case IR_MUL:
									case IR_DIV:
									case IR_MOD:
									case IR_NEG:
									case IR_ABS:
									case IR_ADD_OV:
									case IR_SUB_OV:
									case IR_MUL_OV:
									case IR_NOT:
									case IR_OR:
									case IR_AND:
									case IR_XOR:
									case IR_SHL:
									case IR_SHR:
									case IR_SAR:
									case IR_ROL:
									case IR_ROR:
									case IR_BSWAP:
									case IR_MIN:
									case IR_MAX:
									case IR_PHI:
									case IR_COPY:
									case IR_PI:
										if (insn->type != use_insn->type) {
											if (j == 2
											 && (insn->op == IR_SHL
											  || insn->op == IR_SHR
											  || insn->op == IR_SAR
											  || insn->op == IR_ROL
											  || insn->op == IR_ROR)
											 && ir_type_size[use_insn->type] < ir_type_size[insn->type]) {
												/* second argument of SHIFT may be incompatible with result */
												break;
											}
											fprintf(stderr, "ir_base[%d].ops[%d] (%d) type is incompatible with result type (%d != %d)\n",
												i, j, use, use_insn->type, insn->type);
											ok = 0;
										}
										break;
								}
							}
							break;
						case IR_OPND_CONTROL:
							if (flags & IR_OP_FLAG_BB_START) {
								if (!(ir_op_flags[use_insn->op] & IR_OP_FLAG_BB_END)) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be BB_END\n", i, j, use);
									ok = 0;
								}
							} else {
								if (ir_op_flags[use_insn->op] & IR_OP_FLAG_BB_END) {
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
							if (!(ir_op_flags[use_insn->op] & IR_OP_FLAG_CONTROL)) {
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
		if (ctx->use_lists) {
			if ((flags & IR_OP_FLAG_CONTROL) && !(flags & IR_OP_FLAG_MEM)) {
				ir_use_list *use_list = &ctx->use_lists[i];
				ir_ref count;

				switch (insn->op) {
					case IR_SWITCH:
						/* may have many successors */
						break;
					case IR_IF:
						if (use_list->count != 2) {
							fprintf(stderr, "ir_base[%d].op (IF) must have 2 succesors (%d)\n", i, use_list->count);
							ok = 0;
						}
						break;
					case IR_UNREACHABLE:
					case IR_IJMP:
						if (use_list->count == 1) {
							/* UNREACHABLE and IJMP may be used in MERGE with the following ENTRY */
							break;
						}
					case IR_RETURN:
						if (use_list->count != 0) {
							fprintf(stderr, "ir_base[%d].op (%s) must not have successors (%d)\n",
								i, ir_op_name[insn->op], use_list->count);
							ok = 0;
						}
						break;
					default:
						/* skip data references */
						count = use_list->count;
						for (j = 0, p = &ctx->use_edges[use_list->refs]; j < use_list->count; j++, p++) {
							use = *p;
							if (!(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL)) {
								count--;
							}
						}
						if (count != 1) {
							if (insn->op == IR_CALL && count == 2) {
								/* result of CALL may be used as data in control instruction */
								break;
							}
							fprintf(stderr, "ir_base[%d].op (%s) must have 1 succesor (%d)\n",
								i, ir_op_name[insn->op], count);
							ok = 0;
						}
						break;
				}
			}
		}
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		i += n;
		insn += n;
	}

	IR_ASSERT(ok);
	return ok;
}
