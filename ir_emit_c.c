#include "ir.h"
#include "ir_private.h"

static int ir_emit_dessa_move(ir_ctx *ctx, uint8_t type, ir_ref from, ir_ref to)
{
	FILE *f = ctx->data;

	if (to) {
		fprintf(f, "\td_%d = ", ctx->vregs[to]);
	} else {
		fprintf(f, "\ttmp = ");
	}
	if (IR_IS_CONST_REF(from)) {
		ir_print_const(ctx, &ctx->ir_base[from], f);
		fprintf(f, ";\n");
	} else if (from) {
		fprintf(f, "d_%d;\n", ctx->vregs[from]);
	} else {
		fprintf(f, "tmp;\n");
	}
	return 1;
}

static void ir_emit_ref(ir_ctx *ctx, FILE *f, ir_ref ref)
{
	if (IR_IS_CONST_REF(ref)) {
		ir_print_const(ctx, &ctx->ir_base[ref], f);
	} else {
		ir_insn *insn = &ctx->ir_base[ref];
		if (insn->op == IR_VLOAD) {
			ir_insn *var = &ctx->ir_base[insn->op2];

			IR_ASSERT(var->op == IR_VAR/* || var->op == IR_PARAM*/);
			fprintf(f, "%s", ir_get_str(ctx, var->op2));
			return;
		}
		fprintf(f, "d_%d", ctx->vregs[ref]);
	}
}

static void ir_emit_def_ref(ir_ctx *ctx, FILE *f, ir_ref def)
{
	ir_use_list *use_list = &ctx->use_lists[def];
	if (use_list->count == 1) {
		ir_ref use = ctx->use_edges[use_list->refs];
		ir_insn *insn = &ctx->ir_base[use];

		if (insn->op == IR_VSTORE) {
			ir_insn *var = &ctx->ir_base[insn->op2];

			IR_ASSERT(var->op == IR_VAR/* || var->op == IR_PARAM*/);
			fprintf(f, "\t%s = ", ir_get_str(ctx, var->op2));
			return;
		}
	}
	fprintf(f, "\td_%d = ", ctx->vregs[def]);
}

static void ir_emit_copy(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ";\n");
}

static void ir_emit_unary_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "%s", op);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ";\n");
}

static void ir_emit_binary_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op)
{
	ir_emit_def_ref(ctx, f, def);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " %s ", op);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ";\n");
}

static void ir_emit_signed_cast(FILE *f, ir_type type)
{
	if (!IR_IS_TYPE_SIGNED(type)) {
		switch (ir_type_size[type]) {
			case 1:
				fprintf(f, "(int8_t)");
				break;
			case 2:
				fprintf(f, "(int16_t)");
				break;
			case 4:
				fprintf(f, "(int32_t)");
				break;
			case 8:
				fprintf(f, "(int64_t)");
				break;
			default:
				IR_ASSERT(0);
		}
	}
}

static void ir_emit_unsigned_cast(FILE *f, ir_type type)
{
	if (!IR_IS_TYPE_UNSIGNED(type)) {
		switch (ir_type_size[type]) {
			case 1:
				fprintf(f, "(uint8_t)");
				break;
			case 2:
				fprintf(f, "(uint16_t)");
				break;
			case 4:
				fprintf(f, "(uint32_t)");
				break;
			case 8:
				fprintf(f, "(uint64_t)");
				break;
			default:
				IR_ASSERT(0);
		}
	}
}

static void ir_emit_signed_binary_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op)
{
	uint8_t t1 = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	ir_emit_signed_cast(f, t1);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " %s ", op);
	ir_emit_signed_cast(f, t1);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ";\n");
}

static void ir_emit_unsigned_binary_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op)
{
	uint8_t t1 = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	ir_emit_unsigned_cast(f, t1);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " %s ", op);
	ir_emit_unsigned_cast(f, t1);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ";\n");
}

static void ir_emit_unsigned_comparison_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op, const char *fop)
{
	uint8_t t1 = ctx->ir_base[insn->op1].type;

	IR_ASSERT(t1 == ctx->ir_base[insn->op1].type);
	ir_emit_def_ref(ctx, f, def);
	if (t1 == IR_FLOAT || t1 == IR_DOUBLE) {
		fprintf(f, "!(");
	} else {
		ir_emit_unsigned_cast(f, t1);
	}
	ir_emit_ref(ctx, f, insn->op1);
	if (t1 == IR_FLOAT || t1 == IR_DOUBLE) {
		fprintf(f, " %s ", fop);
	} else {
		fprintf(f, " %s ", op);
		ir_emit_unsigned_cast(f, t1);
	}
	ir_emit_ref(ctx, f, insn->op2);
	if (t1 == IR_FLOAT || t1 == IR_DOUBLE) {
		fprintf(f, ")");
	}
	fprintf(f, ";\n");
}

static void ir_emit_rol_ror(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op1, const char *op2)
{
	uint8_t t1 = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "(");
	ir_emit_unsigned_cast(f, t1);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " %s ", op1);
	ir_emit_unsigned_cast(f, t1);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ") | (");
	ir_emit_unsigned_cast(f, t1);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " %s (%d", op2, ir_type_size[t1] * 8);
	fprintf(f, " - ");
	ir_emit_unsigned_cast(f, t1);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, "));\n");
}

static void ir_emit_bswap(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);

	switch (ir_type_size[insn->type]) {
		case 4:
			fprintf(f, "__builtin_bswap32(");
			break;
		case 8:
			fprintf(f, "__builtin_bswap64(");
			break;
		default:
			IR_ASSERT(0);
	}

	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ");\n");
}

static void ir_emit_sext(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	IR_ASSERT(IR_IS_TYPE_INT(insn->type));
	ir_emit_def_ref(ctx, f, def);
	switch (ir_type_size[insn->type]) {
		case 1:
			fprintf(f, "(int8_t)");
			break;
		case 2:
			fprintf(f, "(int16_t)");
			break;
		case 4:
			fprintf(f, "(int32_t)");
			break;
		case 8:
			fprintf(f, "(int64_t)");
			break;
		default:
			IR_ASSERT(0);
	}
	IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
	switch (ir_type_size[ctx->ir_base[insn->op1].type]) {
		case 1:
			fprintf(f, "(int8_t)");
			break;
		case 2:
			fprintf(f, "(int16_t)");
			break;
		case 4:
			fprintf(f, "(int32_t)");
			break;
		case 8:
			fprintf(f, "(int64_t)");
			break;
		default:
			IR_ASSERT(0);
	}
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ";\n");
}

static void ir_emit_zext(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	IR_ASSERT(IR_IS_TYPE_INT(insn->type));
	ir_emit_def_ref(ctx, f, def);
	switch (ir_type_size[insn->type]) {
		case 1:
			fprintf(f, "(uint8_t)");
			break;
		case 2:
			fprintf(f, "(uint16_t)");
			break;
		case 4:
			fprintf(f, "(uint32_t)");
			break;
		case 8:
			fprintf(f, "(uint64_t)");
			break;
		default:
			IR_ASSERT(0);
	}
	IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
	switch (ir_type_size[ctx->ir_base[insn->op1].type]) {
		case 1:
			fprintf(f, "(uint8_t)");
			break;
		case 2:
			fprintf(f, "(uint16_t)");
			break;
		case 4:
			fprintf(f, "(uint32_t)");
			break;
		case 8:
			fprintf(f, "(uint64_t)");
			break;
		default:
			IR_ASSERT(0);
	}
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ";\n");
}

static void ir_emit_bits(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	IR_ASSERT(IR_IS_TYPE_INT(insn->type));
	if (IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type)) {
		ir_emit_def_ref(ctx, f, def);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ";\n");
	} else if (ctx->ir_base[insn->op1].type == IR_DOUBLE) {
		fprintf(f, "\t{union {double d; uint64_t bits;} _u; _u.d = ");
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, "; ");
		ir_emit_ref(ctx, f, def);
		fprintf(f, " = _u.bits;}\n");
	} else if (ctx->ir_base[insn->op1].type == IR_FLOAT) {
		fprintf(f, "\t{union {float f; uint32_t bits;} _u; _u.f = ");
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, "; ");
		ir_emit_ref(ctx, f, def);
		fprintf(f, " = _u.bits;}\n");
	} else {
		IR_ASSERT(0);
	}
}

static void ir_emit_conv(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ";\n");
}

static void ir_emit_minmax_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
//	fprintf(f, "\td_%d = ", ctx->vregs[def]);
	ir_emit_def_ref(ctx, f, def);
	ir_emit_ref(ctx, f, insn->op1);
	if (insn->op == IR_MIN) {
		fprintf(f, " < ");
	} else if (insn->op == IR_MAX) {
		fprintf(f, " > ");
	} else {
		IR_ASSERT(0);
	}
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, " ? ");
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " : ");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ";\n");
}

static void ir_emit_conditional_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " ? ");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, " : ");
	ir_emit_ref(ctx, f, insn->op3);
	fprintf(f, ";\n");
}

static void ir_emit_abs(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	if (IR_IS_TYPE_FP(type)) {
		if (type == IR_DOUBLE) {
			fprintf(f, "fabs(");
		} else {
			fprintf(f, "fabsf(");
		}
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ")\n");
	} else {
		if (IR_IS_TYPE_SIGNED(type)) {
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, " < 0 ? -");
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, " : ");
			ir_emit_ref(ctx, f, insn->op1);
		} else {
			ir_emit_ref(ctx, f, insn->op1);
		}
		fprintf(f, ";\n");
	}
}

static int ir_skip_empty_blocks(ir_ctx *ctx, int b)
{
	while (ctx->cfg_blocks[b].flags & IR_BB_MAY_SKIP) {
		b++;
	}
	return b;
}

static bool ir_needs_block_label(ir_ctx *ctx, int b)
{
	while (1) {
		if (ctx->cfg_blocks[b].predecessors_count == 0) {
			return 0;
		} else if (ctx->cfg_blocks[b].predecessors_count == 1
		 && ctx->cfg_edges[ctx->cfg_blocks[b].predecessors] == b - 1
		 && ctx->ir_base[ctx->cfg_blocks[b - 1].end].op != IR_SWITCH) {
			b--;
			if (!(ctx->cfg_blocks[b].flags & IR_BB_MAY_SKIP)) {
				return 0;
			}
		} else {
			return 1;
		}
	}
	return 1;
}

static void ir_emit_if(ir_ctx *ctx, FILE *f, int b, ir_ref def, ir_insn *insn)
{
	ir_use_list *use_list;
	ir_insn *use_insn;
	ir_ref i, *p, use, n;
	int true_block = 0, false_block = 0, next_block;
	bool short_true = 0, short_false = 0;

	use_list = &ctx->use_lists[def];
	n = use_list->count;
	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
		use = *p;
		use_insn = &ctx->ir_base[use];
		if (use_insn->op == IR_IF_TRUE) {
			true_block = ir_skip_empty_blocks(ctx, ctx->bb_num[use]);
		} else if (use_insn->op == IR_IF_FALSE) {
			false_block = ir_skip_empty_blocks(ctx, ctx->bb_num[use]);
		} else {
			IR_ASSERT(0);
		}
	}
	IR_ASSERT(true_block && false_block);

	next_block = ir_skip_empty_blocks(ctx, b + 1);
	if (true_block == next_block) {
		short_false = 1;
	} else if (false_block == next_block) {
		short_true = 1;
	}

	fprintf(f, "\tif (");
	if (short_false) {
		fprintf(f, "!");
	}
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ")");

	if (short_true) {
		fprintf(f, " goto bb%d;\n", true_block);
	} else if (short_false) {
		fprintf(f, " goto bb%d;\n", false_block);
	} else {
		fprintf(f, " goto bb%d; else goto bb%d;\n", true_block, false_block);
	}
}

static void ir_emit_switch(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_use_list *use_list;
	ir_insn *use_insn;
	ir_ref i, *p, use, n;

	fprintf(f, "\tswitch (");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ") {\n");

	use_list = &ctx->use_lists[def];
	n = use_list->count;
	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
		use = *p;
		use_insn = &ctx->ir_base[use];
		if (use_insn->op == IR_CASE_VAL) {
			fprintf(f, "\t\tcase ");
			ir_emit_ref(ctx, f, use_insn->op2);
			fprintf(f, ": goto bb%d;\n", ir_skip_empty_blocks(ctx, ctx->bb_num[use]));
		} else if (use_insn->op == IR_CASE_DEFAULT) {
			fprintf(f, "\t\tdefault: goto bb%d;\n", ir_skip_empty_blocks(ctx, ctx->bb_num[use]));
		} else {
			IR_ASSERT(0);
		}
	}

	fprintf(f, "\t}\n");
}

static void ir_emit_call(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	int j, n;

	if (insn->type != IR_VOID) {
		ir_emit_def_ref(ctx, f, def);
	}
	if (IR_IS_CONST_REF(insn->op2)) {
		fprintf(f, "%s", ir_get_str(ctx, ctx->ir_base[insn->op2].val.addr));
	} else {
		ir_emit_ref(ctx, f, insn->op2);
	}
	fprintf(f, "(");
	n = ir_input_edges_count(ctx, insn);
	for (j = 3; j <= n; j++) {
		if (j != 3) {
			fprintf(f, ", ");
		}
		ir_emit_ref(ctx, f, insn->ops[j]);
	}
	fprintf(f, ");\n");
}

static void ir_emit_tailcall(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	int j, n;

	if (insn->type != IR_VOID) {
		fprintf(f, "\treturn ");
	}
	if (IR_IS_CONST_REF(insn->op2)) {
		fprintf(f, "%s", ir_get_str(ctx, ctx->ir_base[insn->op2].val.addr));
	} else {
		ir_emit_ref(ctx, f, insn->op2);
	}
	fprintf(f, "(");
	n = ir_input_edges_count(ctx, insn);
	for (j = 3; j <= n; j++) {
		if (j != 3) {
			fprintf(f, ", ");
		}
		ir_emit_ref(ctx, f, insn->ops[j]);
	}
	fprintf(f, ");\n");
	if (insn->type == IR_VOID) {
		fprintf(f, "\treturn;");
	}
}

static void ir_emit_ijmp(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	fprintf(f, "\tgoto *(void**)(");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ");\n");
}

static void ir_emit_alloca(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "alloca(");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ");\n");
}

static void ir_emit_vaddr(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_insn *var = &ctx->ir_base[insn->op1];

	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "&");
	IR_ASSERT(var->op == IR_VAR/* || var->op == IR_PARAM*/);
	fprintf(f, "%s", ir_get_str(ctx, var->op2));
	fprintf(f, ";\n");
}

static void ir_emit_vstore(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	if (ctx->use_lists[insn->op3].count != 1) {
		ir_insn *var;

		IR_ASSERT(insn->op2 > 0);
		var = &ctx->ir_base[insn->op2];
		IR_ASSERT(var->op == IR_VAR/* || var->op == IR_PARAM*/);
		fprintf(f, "\t%s = ", ir_get_str(ctx, var->op2));
		ir_emit_ref(ctx, f, insn->op3);
		fprintf(f, ";\n");
	}
}

static void ir_emit_load(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "*((%s*)d_%d);\n", ir_type_cname[insn->type], ctx->vregs[insn->op2]);
}

static void ir_emit_store(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op3].type;

	fprintf(f, "\t*((%s*)d_%d) = ", ir_type_cname[type], ctx->vregs[insn->op2]);
	ir_emit_ref(ctx, f, insn->op3);
	fprintf(f, ";\n");
}

static int ir_emit_func(ir_ctx *ctx, FILE *f)
{
	ir_ref ref, i, n, *p;
	ir_insn *insn;
	ir_use_list *use_list;
	uint8_t ret_type = 255;
	bool has_params = 0;
	ir_bitset vars;
	int b, target;
	ir_block *bb;
	uint32_t flags;

	/* Check all RETURN nodes */
	ref = ctx->ir_base[1].op1;
	while (ref) {
		insn = &ctx->ir_base[ref];
		if (insn->op == IR_RETURN) {
			if (ret_type == 255) {
				if (insn->op2) {
					ret_type = ctx->ir_base[insn->op2].type;
				} else {
					ret_type = IR_VOID;
				}
			} else if (insn->op2) {
				if (ret_type != ctx->ir_base[insn->op2].type) {
					IR_ASSERT(0 && "conflicting return types");
					return 0;
				}
			} else {
				if (ret_type != IR_VOID) {
					IR_ASSERT(0 && "conflicting return types");
					return 0;
				}
			}
		}
		ref = ctx->ir_base[ref].op3;
	}

	use_list = &ctx->use_lists[1];
	n = use_list->count;
	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
		insn = &ctx->ir_base[*p];
		if (insn->op == IR_PARAM) {
			has_params = 1;
			break;
		}
	}

	/* Emit function prototype */
	if (ret_type != 255 || has_params) {
		if (ret_type == 255) {
			ret_type = IR_VOID;
		}
		fprintf(f, "%s", ir_type_cname[ret_type]);
		fprintf(f, " test(");
		if (has_params) {
			use_list = &ctx->use_lists[1];
			n = use_list->count;
			for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
				insn = &ctx->ir_base[*p];
				if (insn->op == IR_PARAM) {
					if (has_params) {
						has_params = 0;
					} else {
						fprintf(f, ", ");
					}
					fprintf(f, "%s %s", ir_type_cname[insn->type], ir_get_str(ctx, insn->op2));
				}
			}
		}
		fprintf(f, ")\n");
	}

	fprintf(f, "{\n");

	/* Emit declarations for local variables */
	vars = ir_bitset_malloc(ctx->vregs_count + 1);
	for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
		bb->flags &= ~IR_BB_MAY_SKIP;
		flags = IR_BB_MAY_SKIP;
		if (bb->successors_count != 1
		 || ctx->cfg_edges[bb->successors] != b + 1
		 || (bb->flags & IR_BB_DESSA_MOVES)) {
			flags = 0;
		}
		for (i = bb->start, insn = ctx->ir_base + i; i <= bb->end;) {
			switch (insn->op) {
				case IR_START:
				case IR_BEGIN:
				case IR_END:
				case IR_IF_TRUE:
				case IR_IF_FALSE:
				case IR_CASE_VAL:
				case IR_CASE_DEFAULT:
				case IR_MERGE:
				case IR_LOOP_BEGIN:
				case IR_LOOP_END:
				case IR_LOOP_EXIT:
				case IR_PARAM:
				case IR_VAR:
				case IR_PHI:
				case IR_PI:
					/* skip */
					break;
				default:
					flags = 0;
			}
			if (ctx->vregs[i]) {
				if (!ir_bitset_in(vars, ctx->vregs[i])) {
					ir_bitset_incl(vars, ctx->vregs[i]);
					if (insn->op == IR_PARAM) {
						fprintf(f, "\t%s d_%d = %s;\n", ir_type_cname[insn->type], ctx->vregs[i], ir_get_str(ctx, insn->op2));
					} else {
						ir_use_list *use_list = &ctx->use_lists[i];

						if (insn->op == IR_VAR) {
							if (use_list->count > 0) {
								fprintf(f, "\t%s %s;\n", ir_type_cname[insn->type], ir_get_str(ctx, insn->op2));
							} else {
								/* skip */
							}
						} else if ((insn->op == IR_VLOAD)
						 || (use_list->count == 1
						  && ctx->ir_base[ctx->use_edges[use_list->refs]].op == IR_VSTORE)) {
							/* skip, we use variable name instead */
						} else {
							fprintf(f, "\t%s d_%d;\n", ir_type_cname[insn->type], ctx->vregs[i]);
						}
					}
				} else if (insn->op == IR_PARAM) {
					IR_ASSERT(0 && "unexpected PARAM");
					return 0;
				}
			}
			n = ir_operands_count(ctx, insn);
			n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
			i += n;
			insn += n;
		}
		bb->flags |= flags;
	}
	ir_mem_free(vars);

	for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
		if (bb->flags & IR_BB_MAY_SKIP) {
			continue;
		}
		if (ir_needs_block_label(ctx, b)) {
			fprintf(f, "bb%d:\n", b);
		}
		for (i = bb->start, insn = ctx->ir_base + i; i <= bb->end;) {
			switch (insn->op) {
				case IR_START:
				case IR_BEGIN:
				case IR_IF_TRUE:
				case IR_IF_FALSE:
				case IR_CASE_VAL:
				case IR_CASE_DEFAULT:
				case IR_MERGE:
				case IR_LOOP_BEGIN:
				case IR_LOOP_EXIT:
				case IR_UNREACHABLE:
				case IR_PARAM:
				case IR_VAR:
				case IR_PHI:
				case IR_PI:
				case IR_VLOAD:
					/* skip */
					break;
				case IR_EQ:
					ir_emit_binary_op(ctx, f, i, insn, "==");
					break;
				case IR_NE:
					ir_emit_binary_op(ctx, f, i, insn, "!=");
					break;
				case IR_LT:
					ir_emit_binary_op(ctx, f, i, insn, "<");
					break;
				case IR_GE:
					ir_emit_binary_op(ctx, f, i, insn, ">=");
					break;
				case IR_LE:
					ir_emit_binary_op(ctx, f, i, insn, "<=");
					break;
				case IR_GT:
					ir_emit_binary_op(ctx, f, i, insn, ">");
					break;
				case IR_ULT:
					ir_emit_unsigned_comparison_op(ctx, f, i, insn, "<", ">=");
					break;
				case IR_UGE:
					ir_emit_unsigned_comparison_op(ctx, f, i, insn, ">=", "<");
					break;
				case IR_ULE:
					ir_emit_unsigned_comparison_op(ctx, f, i, insn, "<=", ">");
					break;
				case IR_UGT:
					ir_emit_unsigned_comparison_op(ctx, f, i, insn, ">", "<=");
					break;
				case IR_ADD:
					ir_emit_binary_op(ctx, f, i, insn, "+");
					break;
				case IR_SUB:
					ir_emit_binary_op(ctx, f, i, insn, "-");
					break;
				case IR_MUL:
					ir_emit_binary_op(ctx, f, i, insn, "*");
					break;
				case IR_DIV:
					ir_emit_binary_op(ctx, f, i, insn, "/");
					break;
				case IR_MOD:
					ir_emit_binary_op(ctx, f, i, insn, "%");
					break;
				case IR_NEG:
					ir_emit_unary_op(ctx, f, i, insn, "-");
					break;
				case IR_NOT:
					ir_emit_unary_op(ctx, f, i, insn, insn->type == IR_BOOL ? "!" : "~");
					break;
				case IR_OR:
					ir_emit_binary_op(ctx, f, i, insn, insn->type == IR_BOOL ? "||" : "|");
					break;
				case IR_AND:
					ir_emit_binary_op(ctx, f, i, insn, insn->type == IR_BOOL ? "&&" : "&");
					break;
				case IR_XOR:
					ir_emit_binary_op(ctx, f, i, insn, "^");
					break;
				case IR_MIN:
				case IR_MAX:
					ir_emit_minmax_op(ctx, f, i, insn);
					break;
				case IR_COND:
					ir_emit_conditional_op(ctx, f, i, insn);
					break;
				case IR_ABS:
					ir_emit_abs(ctx, f, i, insn);
					break;
				case IR_SHL:
					ir_emit_binary_op(ctx, f, i, insn, "<<");
					break;
				case IR_SHR:
					ir_emit_unsigned_binary_op(ctx, f, i, insn, ">>");
					break;
				case IR_SAR:
					ir_emit_signed_binary_op(ctx, f, i, insn, ">>");
					break;
				case IR_ROL:
					ir_emit_rol_ror(ctx, f, i, insn, "<<", ">>");
					break;
				case IR_ROR:
					ir_emit_rol_ror(ctx, f, i, insn, ">>", "<<");
					break;
				case IR_BSWAP:
					ir_emit_bswap(ctx, f, i, insn);
					break;
				case IR_SEXT:
					ir_emit_sext(ctx, f, i, insn);
					break;
				case IR_ZEXT:
					ir_emit_zext(ctx, f, i, insn);
					break;
				case IR_BITS:
					ir_emit_bits(ctx, f, i, insn);
					break;
				case IR_INT2FP:
				case IR_FP2INT:
					ir_emit_conv(ctx, f, i, insn);
					break;
				case IR_COPY:
					ir_emit_copy(ctx, f, i, insn);
					break;
				case IR_RETURN:
					IR_ASSERT(bb->successors_count == 0);
					fprintf(f, "\treturn");
					if (!insn->op2) {
						fprintf(f, ";");
					} else {
						fprintf(f, " ");
						ir_emit_ref(ctx, f, insn->op2);
						fprintf(f, ";\n");
					}
					break;
				case IR_END:
				case IR_LOOP_END:
					IR_ASSERT(bb->successors_count == 1);
					if (bb->flags & IR_BB_DESSA_MOVES) {
						ctx->data = f;
						ir_gen_dessa_moves(ctx, b, ir_emit_dessa_move);
					}
					target = ir_skip_empty_blocks(ctx, ctx->cfg_edges[bb->successors]);
					if (b == ctx->cfg_blocks_count || target != ir_skip_empty_blocks(ctx, b + 1)) {
						fprintf(f, "\tgoto bb%d;\n", target);
					}
					break;
				case IR_IF:
					ir_emit_if(ctx, f, b, i, insn);
					break;
				case IR_SWITCH:
					ir_emit_switch(ctx, f, i, insn);
					break;
				case IR_CALL:
					ir_emit_call(ctx, f, i, insn);
					break;
				case IR_TAILCALL:
					ir_emit_tailcall(ctx, f, insn);
					break;
				case IR_IJMP:
					ir_emit_ijmp(ctx, f, insn);
					break;
				case IR_ALLOCA:
					ir_emit_alloca(ctx, f, i, insn);
					break;
				case IR_VADDR:
					ir_emit_vaddr(ctx, f, i, insn);
					break;
				case IR_VSTORE:
					ir_emit_vstore(ctx, f, insn);
					break;
				case IR_LOAD:
					ir_emit_load(ctx, f, i, insn);
					break;
				case IR_STORE:
					ir_emit_store(ctx, f, insn);
					break;
				default:
					IR_ASSERT(0 && "NIY instruction");
			}
			n = ir_operands_count(ctx, insn);
			n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
			i += n;
			insn += n;
		}
	}

	fprintf(f, "}\n");

	return 1;
}

int ir_emit_c(ir_ctx *ctx, FILE *f)
{
	return ir_emit_func(ctx, f);
}
