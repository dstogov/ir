#include "ir.h"

#include "ir_x86.h"

void ir_dump(ir_ctx *ctx, FILE *f)
{
	ir_ref i, j, n, ref, *p;
	ir_insn *insn;
	uint32_t flags;

	for (i = 1 - ctx->consts_count, insn = ctx->ir_base + i; i < IR_UNUSED; i++, insn++) {
		fprintf(f, "%05d %s %s(", i, ir_op_name[insn->op], ir_type_name[insn->type]);
		ir_print_const(ctx, insn, f);
		fprintf(f, ")\n");
	}

	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count; i++, insn++) {
		flags = ir_op_flags[insn->op];
		fprintf(f, "%05d %s", i, ir_op_name[insn->op]);
		if ((flags & IR_OP_FLAG_DATA) || ((flags & IR_OP_FLAG_MEM) && insn->type != IR_VOID)) {
			fprintf(f, " %s", ir_type_name[insn->type]);
		}
		n = ir_operands_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= 3; j++, p++) {
			ref = *p;
			if (ref) {
				fprintf(f, " %05d", ref);
			}
		}
		if (n > 3) {
			n -= 3;
			do {
				i++;
				insn++;
				fprintf(f, "\n%05d", i);
				for (j = 0; j < 4; j++, p++) {
					ref = *p;
					if (ref) {
						fprintf(f, " %05d", ref);
					}
				}
				n -= 4;
			} while (n > 0);
		}
		fprintf(f, "\n");
	}
}

void ir_dump_dot(ir_ctx *ctx, FILE *f)
{
	int DATA_WEIGHT    = 1;
	int CONTROL_WEIGHT = 2;
	int REF_WEIGHT     = 1;
	ir_ref i, j, n, ref, *p;
	ir_insn *insn;
	uint32_t flags;

	fprintf(f, "digraph ir {\n");
	fprintf(f, "\trankdir=TB;\n");
	for (i = 1 - ctx->consts_count, insn = ctx->ir_base + i; i < IR_UNUSED; i++, insn++) {
		fprintf(f, "\tc%d [label=\"C%d: CONST %s(", -i, -i, ir_type_name[insn->type]);
		ir_print_const(ctx, insn, f);
		fprintf(f, ")\",style=filled,fillcolor=yellow];\n");
	}

	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count;) {
		flags = ir_op_flags[insn->op];
		if (flags & IR_OP_FLAG_CONTROL) {
			if (insn->op == IR_START) {
				fprintf(f, "\t{rank=min; n%d [label=\"%d: %s\",shape=box,style=\"rounded,filled\",fillcolor=red,rank=min];}\n", i, i, ir_op_name[insn->op]);
			} else if (insn->op == IR_RETURN || insn->op == IR_UNREACHABLE) {
				fprintf(f, "\t{rank=max; n%d [label=\"%d: %s\",shape=box,style=\"rounded,filled\",fillcolor=red,rank=max];}\n", i, i, ir_op_name[insn->op]);
			} else if (flags & IR_OP_FLAG_MEM) {
				fprintf(f, "\tn%d [label=\"%d: %s\",shape=box,style=filled,fillcolor=pink];\n", i, i, ir_op_name[insn->op]);
			} else {
				fprintf(f, "\tn%d [label=\"%d: %s\",shape=box,style=filled,fillcolor=lightcoral];\n", i, i, ir_op_name[insn->op]);
			}
		} else if (flags & IR_OP_FLAG_DATA) {
			if (IR_OPND_KIND(flags, 1) == IR_OPND_DATA) {
				/* not a leaf */
				fprintf(f, "\tn%d [label=\"%d: %s %s\"", i, i, ir_op_name[insn->op], ir_type_name[insn->type]);
				fprintf(f, ",shape=diamond,style=filled,fillcolor=deepskyblue];\n");
			} else {
				if (insn->op == IR_PARAM) {
					fprintf(f, "\tn%d [label=\"%d: %s %s \\\"%s\\\"\",style=filled,fillcolor=lightblue];\n",
						i, i, ir_op_name[insn->op], ir_type_name[insn->type], ir_get_str(ctx, insn->op2));
				} else if (insn->op == IR_VAR) {
					fprintf(f, "\tn%d [label=\"%d: %s %s \\\"%s\\\"\"];\n", i, i, ir_op_name[insn->op], ir_type_name[insn->type], ir_get_str(ctx, insn->op2));
				} else {
					fprintf(f, "\tn%d [label=\"%d: %s %s\",style=filled,fillcolor=deepskyblue];\n", i, i, ir_op_name[insn->op], ir_type_name[insn->type]);
				}
			}
		}
		n = ir_operands_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			ref = *p;
			if (ref) {
				switch (IR_OPND_KIND(flags, j)) {
					case IR_OPND_DATA:
					case IR_OPND_VAR:
						if (IR_IS_CONST_REF(ref)) {
							fprintf(f, "\tc%d -> n%d [color=blue,weight=%d];\n", -ref, i, DATA_WEIGHT);
						} else {
							fprintf(f, "\tn%d -> n%d [color=blue,weight=%d];\n", ref, i, DATA_WEIGHT);
						}
						break;
					case IR_OPND_CONTROL:
						if (insn->op == IR_LOOP_BEGIN && ctx->ir_base[ref].op == IR_LOOP_END) {
							fprintf(f, "\tn%d -> n%d [style=bold,color=red,weight=%d];\n", ref, i, REF_WEIGHT);
						} else {
							fprintf(f, "\tn%d -> n%d [style=bold,color=red,weight=%d];\n", ref, i, CONTROL_WEIGHT);
						}
						break;
					case IR_OPND_CONTROL_DEP:
					case IR_OPND_CONTROL_REF:
						fprintf(f, "\tn%d -> n%d [style=dashed,weight=%d];\n", i, ref, REF_WEIGHT);
						break;
				}
			}
		}
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		i += n;
		insn += n;
	}
	fprintf(f, "}\n");
}

void ir_dump_use_lists(ir_ctx *ctx, FILE *f)
{
	ir_ref i, j, n, *p;
	ir_use_list *list;

	if (ctx->use_lists) {
		fprintf(f, "{ # Use Lists\n");
		for (i = 1, list = &ctx->use_lists[1]; i < ctx->insns_count; i++, list++) {
			n = list->count;
			if (n > 0) {
				p = &ctx->use_edges[list->refs];
				fprintf(f, "%05d(%d): [%05d", i, n, *p);
				p++;
				for (j = 1; j < n; j++, p++) {
					fprintf(f, ", %05d", *p);
				}
				fprintf(f, "]\n");
			}
		}
		fprintf(f, "}\n");
	}
}

static int ir_dump_dessa_move(ir_ctx *ctx, uint8_t type, ir_ref from, ir_ref to)
{
	FILE *f = ctx->data;
	int8_t reg;

	if (IR_IS_CONST_REF(from)) {
		fprintf(f, "\tmov c_%d -> ", -from);
	} else if (from) {
		fprintf(f, "\tmov R%d", ctx->vregs[from]);
		if (ctx->live_intervals && ctx->live_intervals[ctx->vregs[from]]) {
			reg = ctx->live_intervals[ctx->vregs[from]]->reg;
			if (reg >= 0) {
				fprintf(f, " [%%%s]", ir_reg_name(reg, type));
			}
		}
		fprintf(f, " -> ");
	} else {
		fprintf(f, "\tmov TMP -> ");
	}

	if (to) {
		fprintf(f, "R%d", ctx->vregs[to]);
		if (ctx->live_intervals && ctx->live_intervals[ctx->vregs[to]]) {
			reg = ctx->live_intervals[ctx->vregs[to]]->reg;
			if (reg >= 0) {
				fprintf(f, " [%%%s]", ir_reg_name(reg, type));
			}
		}
		fprintf(f, "\n");
	} else {
		fprintf(f, "TMP\n");
	}
	return 1;
}

void ir_dump_cfg(ir_ctx *ctx, FILE *f)
{
	if (ctx->cfg_blocks) {
		uint32_t  b, i, bb_count = ctx->cfg_blocks_count;
		ir_block *bb = ctx->cfg_blocks + 1;

		fprintf(f, "{ # CFG\n");
		for (b = 1; b <= bb_count; b++, bb++) {
			fprintf(f, "BB%d:\n", b);
			fprintf(f, "\tstart=%d\n", bb->start);
			fprintf(f, "\tend=%d\n", bb->end);
			if (bb->successors_count) {
				fprintf(f, "\tsuccessors(%d) [BB%d", bb->successors_count, ctx->cfg_edges[bb->successors]);
				for (i = 1; i < bb->successors_count; i++) {
					fprintf(f, ", BB%d", ctx->cfg_edges[bb->successors + i]);
				}
				fprintf(f, "]\n");
			}
			if (bb->predecessors_count) {
				fprintf(f, "\tpredecessors(%d) [BB%d", bb->predecessors_count, ctx->cfg_edges[bb->predecessors]);
				for (i = 1; i < bb->predecessors_count; i++) {
					fprintf(f, ", BB%d", ctx->cfg_edges[bb->predecessors + i]);
				}
				fprintf(f, "]\n");
			}
			if (bb->dom_parent > 0) {
				fprintf(f, "\tdom_parent=BB%d\n", bb->dom_parent);
			}
			fprintf(f, "\tdom_depth=%d\n", bb->dom_depth);
			if (bb->dom_child > 0) {
				int child = bb->dom_child;
				fprintf(f, "\tdom_children [BB%d", child);
				child = ctx->cfg_blocks[child].dom_next_child;
				while (child > 0) {
					fprintf(f, ", BB%d", child);
					child = ctx->cfg_blocks[child].dom_next_child;
				}
				fprintf(f, "]\n");
			}
			if (bb->flags & IR_BB_LOOP_HEADER) {
				fprintf(f, "\tLOOP_HEADER\n");
			}
			if (bb->flags & IR_BB_IRREDUCIBLE_LOOP) {
				fprintf(stderr, "\tIRREDUCIBLE_LOOP\n");
			}
			if (bb->loop_header > 0) {
				fprintf(f, "\tloop_header=BB%d\n", bb->loop_header);
			}
			if (bb->loop_depth != 0) {
				fprintf(f, "\tloop_depth=%d\n", bb->loop_depth);
			}
			if (bb->flags & IR_BB_DESSA_MOVES) {
				ctx->data = f;
				ir_gen_dessa_moves(ctx, b, ir_dump_dessa_move);
			}
		}
		fprintf(f, "}\n");
	}
}

void ir_dump_gcm(ir_ctx *ctx, FILE *f)
{
	ir_ref i;
    ir_ref *_blocks = ctx->gcm_blocks;

    if (_blocks) {
		fprintf(f, "{ # GCM\n");
		for (i = IR_UNUSED + 1; i < ctx->insns_count; i++) {
			fprintf(f, "%d -> %d\n", i, _blocks[i]);
		}
		fprintf(f, "}\n");
	}
}

void ir_dump_live_ranges(ir_ctx *ctx, FILE *f)
{
    uint32_t i;
    ir_ref j;

	if (!ctx->live_intervals) {
		return;
	}
	fprintf(f, "{ # LIVE-RANGES (vregs_count=%d)\n", ctx->vregs_count);
	for (i = 0; i <= ctx->vregs_count; i++) {
		ir_live_interval *ival = ctx->live_intervals[i];

		if (ival) {
			ir_live_range *p;
			ir_use_pos *use_pos;

			if (i == 0) {
				fprintf(f, "TMP");
			} else {
				for (j = 1; j < ctx->insns_count; j++) {
					if (ctx->vregs[j] == i) {
						break;
					}
				}
				fprintf(f, "R%d (d_%d", i, j);
				for (j++; j < ctx->insns_count; j++) {
					if (ctx->vregs[j] == i) {
						fprintf(f, ", d_%d", j);
					}
				}
				fprintf(f, ")");
				if (ival->stack_spill_pos != -1) {
					fprintf(f, " [SPILL=0x%x]", ival->stack_spill_pos);
				}
			}
			if (ival->next) {
				fprintf(f, "\n\t");
			} else if (ival->reg >= 0) {
				fprintf(f, " ");
			}
			do {
				if (ival->reg >= 0) {
					fprintf(f, "[%%%s]", ir_reg_name(ival->reg, ival->type));
				}
				p = &ival->range;
				fprintf(f, ": [%d.%d-%d.%d)",
					IR_LIVE_POS_TO_REF(p->start), IR_LIVE_POS_TO_SUB_REF(p->start),
					IR_LIVE_POS_TO_REF(p->end), IR_LIVE_POS_TO_SUB_REF(p->end));
				p = p->next;
				while (p) {
					fprintf(f, ", [%d.%d-%d.%d)",
						IR_LIVE_POS_TO_REF(p->start), IR_LIVE_POS_TO_SUB_REF(p->start),
						IR_LIVE_POS_TO_REF(p->end), IR_LIVE_POS_TO_SUB_REF(p->end));

					p = p->next;
				}
				use_pos = ival->use_pos;
				while (use_pos) {
					if (use_pos->flags & IR_PHI_USE) {
						fprintf(f, ", PHI_USE(%d.%d, phi=d_%d/%d)",
							IR_LIVE_POS_TO_REF(use_pos->pos), IR_LIVE_POS_TO_SUB_REF(use_pos->pos),
							use_pos->hint_ref, use_pos->op_num);
					} else {
						if (!use_pos->op_num) {
							fprintf(f, ", DEF(%d.%d",
								IR_LIVE_POS_TO_REF(use_pos->pos), IR_LIVE_POS_TO_SUB_REF(use_pos->pos));
						} else {
							fprintf(f, ", USE(%d.%d/%d",
								IR_LIVE_POS_TO_REF(use_pos->pos), IR_LIVE_POS_TO_SUB_REF(use_pos->pos),
								use_pos->op_num);
						}
						if (use_pos->hint >= 0) {
							fprintf(f, ", hint=%%%s", ir_reg_name(use_pos->hint, ival->type));
						}
						if (use_pos->hint_ref) {
							fprintf(f, ", hint=R%d", ctx->vregs[use_pos->hint_ref]);
						}
						fprintf(f, ")");
						if (use_pos->flags & IR_USE_MUST_BE_IN_REG) {
							fprintf(f, "!");
						}
					}
					use_pos = use_pos->next;
				}
				if (ival->next) {
					fprintf(f, "\n\t");
				}
				ival = ival->next;
			} while (ival);
			fprintf(f, "\n");
		}
	}
#if 1
	for (i = ctx->vregs_count + 1; i <= ctx->vregs_count + IR_REG_NUM; i++) {
		ir_live_interval *ival = ctx->live_intervals[i];

		if (ival) {
			ir_live_range *p = &ival->range;
			fprintf(f, "[%%%s] : [%d.%d-%d.%d)",
				ir_reg_name(ival->reg, IR_ADDR),
				IR_LIVE_POS_TO_REF(p->start), IR_LIVE_POS_TO_SUB_REF(p->start),
				IR_LIVE_POS_TO_REF(p->end), IR_LIVE_POS_TO_SUB_REF(p->end));
			p = p->next;
			while (p) {
				fprintf(f, ", [%d.%d-%d.%d)",
					IR_LIVE_POS_TO_REF(p->start), IR_LIVE_POS_TO_SUB_REF(p->start),
					IR_LIVE_POS_TO_REF(p->end), IR_LIVE_POS_TO_SUB_REF(p->end));
				p = p->next;
			}
			fprintf(f, "\n");
		}
	}
#endif
	fprintf(f, "}\n");
}
