/*
 * IR - Lightweight JIT Compilation Framework
 * (IR saver)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h"

void ir_save(const ir_ctx *ctx, FILE *f)
{
	ir_ref i, j, n, ref, *p;
	ir_insn *insn;
	uint32_t flags;
	bool first;
#ifdef IR_DEBUG
	bool verbose = (ctx->flags & IR_DEBUG_CODEGEN);
#endif

	fprintf(f, "{\n");
	for (i = IR_UNUSED + 1, insn = ctx->ir_base - i; i < ctx->consts_count; i++, insn--) {
		fprintf(f, "\t%s c_%d = ", ir_type_cname[insn->type], i);
		if (insn->op == IR_FUNC) {
			if (!insn->const_flags) {
				fprintf(f, "func(%s)", ir_get_str(ctx, insn->val.i32));
			} else {
				fprintf(f, "func(%s, %d)", ir_get_str(ctx, insn->val.i32), insn->const_flags);
			}
		} else if (insn->op == IR_FUNC_ADDR) {
			fprintf(f, "func_addr(");
			ir_print_const(ctx, insn, f, true);
			if (insn->const_flags) {
				fprintf(f, ", %d", insn->const_flags);
			}
			fprintf(f, ")");
		} else {
			ir_print_const(ctx, insn, f, true);
		}
		fprintf(f, ";\n");
	}

	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count;) {
		flags = ir_op_flags[insn->op];
		if (flags & IR_OP_FLAG_CONTROL) {
#ifdef IR_DEBUG
			if (verbose && ctx->cfg_map && (flags & IR_OP_FLAG_BB_START)) {
				fprintf(f, "#BB%d:\n", ctx->cfg_map[i]);
			}
#endif
			if (!(flags & IR_OP_FLAG_MEM) || insn->type == IR_VOID) {
				fprintf(f, "\tl_%d = ", i);
			} else {
				fprintf(f, "\t%s d_%d, l_%d = ", ir_type_cname[insn->type], i, i);
			}
		} else {
			fprintf(f, "\t");
			if (flags & IR_OP_FLAG_DATA) {
				fprintf(f, "%s d_%d", ir_type_cname[insn->type], i);
#ifdef IR_DEBUG
				if (verbose && ctx->vregs && ctx->vregs[i]) {
					fprintf(f, " {R%d}", ctx->vregs[i]);
				}
				if (verbose && ctx->regs) {
					int8_t reg = ctx->regs[i][0];
					if (reg != IR_REG_NONE) {
						fprintf(f, " {%%%s%s}", ir_reg_name(IR_REG_NUM(reg), ctx->ir_base[ref].type),
							(reg & (IR_REG_SPILL_STORE|IR_REG_SPILL_SPECIAL)) ? ":store" : "");
					}
				}
#endif
				fprintf(f, " = ");
			}
		}
		fprintf(f, "%s", ir_op_name[insn->op]);
		n = ir_operands_count(ctx, insn);
		if ((insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) && n != 2) {
			fprintf(f, "/%d", n);
		} else if ((insn->op == IR_CALL || insn->op == IR_TAILCALL) && n != 2) {
			fprintf(f, "/%d", n - 2);
		} else if (insn->op == IR_PHI && n != 3) {
			fprintf(f, "/%d", n - 1);
		} else if (insn->op == IR_SNAPSHOT) {
			fprintf(f, "/%d", n - 1);
		}
		first = 1;
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			uint32_t opnd_kind = IR_OPND_KIND(flags, j);

			ref = *p;
			if (ref) {
				switch (opnd_kind) {
					case IR_OPND_DATA:
						if (IR_IS_CONST_REF(ref)) {
							fprintf(f, "%sc_%d", first ? "(" : ", ", -ref);
						} else {
							fprintf(f, "%sd_%d", first ? "(" : ", ", ref);
						}
#ifdef IR_DEBUG
						if (verbose && ctx->vregs && ref > 0 && ctx->vregs[ref]) {
							fprintf(f, " {R%d}", ctx->vregs[ref]);
						}
						if (verbose && ctx->regs) {
							int8_t reg = ctx->regs[i][j];
							if (reg != IR_REG_NONE) {
								fprintf(f, " {%%%s%s}", ir_reg_name(IR_REG_NUM(reg), ctx->ir_base[ref].type),
									(reg & (IR_REG_SPILL_LOAD|IR_REG_SPILL_SPECIAL)) ? ":load" : "");
							}
						}
#endif
						first = 0;
						break;
					case IR_OPND_CONTROL:
					case IR_OPND_CONTROL_DEP:
					case IR_OPND_CONTROL_REF:
						fprintf(f, "%sl_%d", first ? "(" : ", ", ref);
						first = 0;
						break;
					case IR_OPND_STR:
						fprintf(f, "%s\"%s\"", first ? "(" : ", ", ir_get_str(ctx, ref));
						first = 0;
						break;
					case IR_OPND_PROB:
						if (ref == 0) {
							break;
						}
						IR_FALLTHROUGH;
					case IR_OPND_NUM:
						fprintf(f, "%s%d", first ? "(" : ", ", ref);
						first = 0;
						break;
				}
			} else if (opnd_kind == IR_OPND_NUM) {
				fprintf(f, "%s%d", first ? "(" : ", ", ref);
				first = 0;
			} else if (IR_IS_REF_OPND_KIND(opnd_kind) && j != n) {
				fprintf(f, "%snull", first ? "(" : ", ");
				first = 0;
			}
		}
		if (first) {
			fprintf(f, ";");
		} else {
			fprintf(f, ");");
		}
		if (((flags & IR_OP_FLAG_DATA) || ((flags & IR_OP_FLAG_MEM) && insn->type != IR_VOID)) && ctx->binding) {
			ir_ref var = ir_binding_find(ctx, i);
			if (var) {
				IR_ASSERT(var < 0);
				fprintf(f, " # BIND(0x%x);", -var);
			}
		}
#ifdef IR_DEBUG
		if (verbose && ctx->rules) {
			uint32_t rule = ctx->rules[i];
			uint32_t id = rule & ~(IR_FUSED|IR_SKIPPED|IR_SIMPLE);

			if (id < IR_LAST_OP) {
				fprintf(f, " # RULE(%s", ir_op_name[id]);
			} else {
				IR_ASSERT(id > IR_LAST_OP /*&& id < IR_LAST_RULE*/);
				fprintf(f, " # RULE(%s", ir_rule_name[id - IR_LAST_OP]);
			}
			if (rule & IR_FUSED) {
				fprintf(f, ":FUSED");
			}
			if (rule & IR_SKIPPED) {
				fprintf(f, ":SKIPPED");
			}
			if (rule & IR_SIMPLE) {
				fprintf(f, ":SIMPLE");
			}
			fprintf(f, ")");
		}
#endif
		fprintf(f, "\n");
#ifdef IR_DEBUG
		if (verbose && ctx->cfg_map && (flags & IR_OP_FLAG_BB_END)) {
			uint32_t b = ctx->cfg_map[i];
			ir_block *bb = &ctx->cfg_blocks[b];

			if (bb->flags & IR_BB_DESSA_MOVES) {
				uint32_t succ;
				ir_block *succ_bb;
				ir_use_list *use_list;
				ir_ref k, i, *p, use_ref, input;
				ir_insn *use_insn;

				IR_ASSERT(bb->successors_count == 1);
				succ = ctx->cfg_edges[bb->successors];
				succ_bb = &ctx->cfg_blocks[succ];
				IR_ASSERT(succ_bb->predecessors_count > 1);
				use_list = &ctx->use_lists[succ_bb->start];
				k = ir_phi_input_number(ctx, succ_bb, b);

				for (i = 0, p = &ctx->use_edges[use_list->refs]; i < use_list->count; i++, p++) {
					use_ref = *p;
					use_insn = &ctx->ir_base[use_ref];
					if (use_insn->op == IR_PHI) {
						input = ir_insn_op(use_insn, k);
						if (IR_IS_CONST_REF(input)) {
							fprintf(f, "\t# DESSA MOV c_%d", -input);
						} else if (ctx->vregs[input] != ctx->vregs[use_ref]) {
							fprintf(f, "\t# DESSA MOV d_%d {R%d}", input, ctx->vregs[input]);
						} else {
							continue;
						}
						if (ctx->regs) {
							int8_t reg = ctx->regs[use_ref][k];
							if (reg != IR_REG_NONE) {
								fprintf(f, " {%%%s%s}", ir_reg_name(IR_REG_NUM(reg), ctx->ir_base[ref].type),
									(reg & (IR_REG_SPILL_LOAD|IR_REG_SPILL_SPECIAL)) ? ":load" : "");
							}
						}
						fprintf(f, " -> d_%d {R%d}", use_ref, ctx->vregs[use_ref]);
						if (ctx->regs) {
							int8_t reg = ctx->regs[use_ref][0];
							if (reg != IR_REG_NONE) {
								fprintf(f, " {%%%s%s}", ir_reg_name(IR_REG_NUM(reg), ctx->ir_base[ref].type),
									(reg & (IR_REG_SPILL_STORE|IR_REG_SPILL_SPECIAL)) ? ":store" : "");
							}
						}
						fprintf(f, "\n");
					}
				}
			}
		}
#endif
		n = ir_insn_inputs_to_len(n);
		i += n;
		insn += n;
	}
	fprintf(f, "}\n");
}
