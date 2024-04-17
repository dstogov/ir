/*
 * IR - Lightweight JIT Compilation Framework
 * (C code generator)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h"

#define IR_TYPE_TNAME(name, type, field, flags) #field,

static const char *ir_type_tname[IR_LAST_TYPE] = {
	NULL,
	IR_TYPES(IR_TYPE_TNAME)
};

static int ir_add_tmp_type(ir_ctx *ctx, uint8_t type, ir_ref from, ir_ref to)
{
	if (from == 0) {
		ir_bitset tmp_types = ctx->data;

		ir_bitset_incl(tmp_types, type);
	}
	return 1;
}

static int ir_emit_dessa_move(ir_ctx *ctx, uint8_t type, ir_ref from, ir_ref to)
{
	FILE *f = ctx->data;

	if (to) {
		fprintf(f, "\td_%d = ", ctx->vregs[to]);
	} else {
		fprintf(f, "\ttmp_%s = ", ir_type_tname[type]);
	}
	if (IR_IS_CONST_REF(from)) {
		ir_print_const(ctx, &ctx->ir_base[from], f, true);
		fprintf(f, ";\n");
	} else if (from) {
		fprintf(f, "d_%d;\n", ctx->vregs[from]);
	} else {
		fprintf(f, "tmp_%s;\n", ir_type_tname[type]);
	}
	return 1;
}

static void ir_emit_ref(ir_ctx *ctx, FILE *f, ir_ref ref)
{
	if (IR_IS_CONST_REF(ref)) {
		ir_print_const(ctx, &ctx->ir_base[ref], f, true);
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
	IR_ASSERT(ctx->vregs[def]);
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
			default:
				IR_ASSERT(0);
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
		}
	}
}

static void ir_emit_unsigned_cast(FILE *f, ir_type type)
{
	if (!IR_IS_TYPE_UNSIGNED(type)) {
		switch (ir_type_size[type]) {
			default:
				IR_ASSERT(0);
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
		default:
			IR_ASSERT(0);
		case 4:
			fprintf(f, "__builtin_bswap32(");
			break;
		case 8:
			fprintf(f, "__builtin_bswap64(");
			break;
	}

	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ");\n");
}

static void ir_emit_count(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *name)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "__builtin_%s%s(", name, ir_type_size[insn->type] == 8 ? "ll" : "");
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ");\n");
}

static void ir_emit_sext(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	IR_ASSERT(IR_IS_TYPE_INT(insn->type));
	IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
	IR_ASSERT(ir_type_size[insn->type] > ir_type_size[ctx->ir_base[insn->op1].type]);
	ir_emit_def_ref(ctx, f, def);
	switch (ir_type_size[insn->type]) {
		default:
			IR_ASSERT(0);
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
	}
	switch (ir_type_size[ctx->ir_base[insn->op1].type]) {
		default:
			IR_ASSERT(0);
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
	}
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ";\n");
}

static void ir_emit_zext(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	IR_ASSERT(IR_IS_TYPE_INT(insn->type));
	IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
	IR_ASSERT(ir_type_size[insn->type] > ir_type_size[ctx->ir_base[insn->op1].type]);
	ir_emit_def_ref(ctx, f, def);
	switch (ir_type_size[insn->type]) {
		default:
			IR_ASSERT(0);
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
	}
	switch (ir_type_size[ctx->ir_base[insn->op1].type]) {
		default:
			IR_ASSERT(0);
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
	}
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ";\n");
}

static void ir_emit_trunc(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	IR_ASSERT(IR_IS_TYPE_INT(insn->type));
	IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
	IR_ASSERT(ir_type_size[insn->type] < ir_type_size[ctx->ir_base[insn->op1].type]);
	ir_emit_def_ref(ctx, f, def);
	switch (ir_type_size[insn->type]) {
		default:
			IR_ASSERT(0);
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
	}
	switch (ir_type_size[ctx->ir_base[insn->op1].type]) {
		default:
			IR_ASSERT(0);
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
	}
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ";\n");
}

static void ir_emit_bitcast(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	IR_ASSERT(ir_type_size[insn->type] == ir_type_size[ctx->ir_base[insn->op1].type]);
	if (IR_IS_TYPE_INT(insn->type)) {
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
		} else {
			IR_ASSERT(ctx->ir_base[insn->op1].type == IR_FLOAT);
			fprintf(f, "\t{union {float f; uint32_t bits;} _u; _u.f = ");
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, "; ");
			ir_emit_ref(ctx, f, def);
			fprintf(f, " = _u.bits;}\n");
		}
	} else {
		IR_ASSERT(IR_IS_TYPE_FP(insn->type));
		IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
		if (insn->type == IR_DOUBLE) {
			fprintf(f, "\t{union {double d; uint64_t bits;} _u; _u.bits = ");
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, "; ");
			ir_emit_ref(ctx, f, def);
			fprintf(f, " = _u.d;}\n");
		} else {
			IR_ASSERT(insn->type == IR_FLOAT);
			fprintf(f, "\t{union {float f; uint32_t bits;} _u; _u.bits = ");
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, "; ");
			ir_emit_ref(ctx, f, def);
			fprintf(f, " = _u.f;}\n");
		}
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
	} else {
		IR_ASSERT(insn->op == IR_MAX);
		fprintf(f, " > ");
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

static void ir_emit_overflow_math(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *func, const char *op)
{
	ir_type type = insn->type;
	ir_use_list *use_list = &ctx->use_lists[def];
	ir_ref i, n, *p, overflow = IR_UNUSED;

	n = use_list->count;
	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
		ir_insn *use_insn = &ctx->ir_base[*p];
		if (use_insn->op == IR_OVERFLOW) {
			overflow = *p;
			break;
		}
	}
	IR_ASSERT(overflow != IR_UNUSED);

	if (ir_type_size[type] == 4 || ir_type_size[type] == 8) {
		fprintf(f, "\tint overflow_%d;\n", overflow);
		ir_emit_def_ref(ctx, f, def);
		fprintf(f, "__builtin_%s%s%s_overflow(",
			IR_IS_TYPE_SIGNED(type) ? "s" : "u",
			func,
			ir_type_size[type] == 8 ? "ll" : "");
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", ");
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", &overflow_%d);\n", overflow);
		ir_emit_def_ref(ctx, f, def);
		fprintf(f, "overflow_%d;\n", overflow);
	} else {
		ir_emit_binary_op(ctx, f, def, insn, op);
		ir_emit_def_ref(ctx, f, overflow);
		if (IR_IS_TYPE_SIGNED(type)) {
			fprintf(f, "(int32_t)");
			ir_emit_ref(ctx, f, def);
			fprintf(f, " != (int32_t)");
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, " %s (int32_t)", op);
			ir_emit_ref(ctx, f, insn->op2);
			fprintf(f, ";\n");
		} else {
			fprintf(f, "(uint32_t)");
			ir_emit_ref(ctx, f, def);
			fprintf(f, " != (uint32_t)");
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, " %s (uint32_t)", op);
			ir_emit_ref(ctx, f, insn->op2);
			fprintf(f, ";\n");
		}
	}
}

static void ir_emit_if(ir_ctx *ctx, FILE *f, uint32_t b, ir_ref def, ir_insn *insn, uint32_t next_block)
{
	uint32_t true_block = 0, false_block = 0;
	bool short_true = 0, short_false = 0;

	ir_get_true_false_blocks(ctx, b, &true_block, &false_block);
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

static void ir_emit_switch(ir_ctx *ctx, FILE *f, uint32_t b, ir_ref def, ir_insn *insn)
{
	ir_block *bb;
	uint32_t n, *p, use_block;
	ir_insn *use_insn;

	fprintf(f, "\tswitch (");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ") {\n");

	bb = &ctx->cfg_blocks[b];
	p = &ctx->cfg_edges[bb->successors];
	for (n = bb->successors_count; n != 0; p++, n--) {
		use_block = *p;
		use_insn = &ctx->ir_base[ctx->cfg_blocks[use_block].start];
		if (use_insn->op == IR_CASE_VAL) {
			fprintf(f, "\t\tcase ");
			ir_emit_ref(ctx, f, use_insn->op2);
			fprintf(f, ": goto bb%d;\n", ir_skip_empty_target_blocks(ctx, use_block));
		} else {
			IR_ASSERT(use_insn->op == IR_CASE_DEFAULT);
			fprintf(f, "\t\tdefault: goto bb%d;\n", ir_skip_empty_target_blocks(ctx, use_block));
		}
	}
	fprintf(f, "\t}\n");
}

static void ir_emit_call(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	int j, n;

	if (insn->type != IR_VOID && ctx->vregs[def] != IR_UNUSED) {
		ir_emit_def_ref(ctx, f, def);
	} else {
		fprintf(f, "\t");
	}
	if (IR_IS_CONST_REF(insn->op2)) {
		IR_ASSERT(ctx->ir_base[insn->op2].op == IR_FUNC);
		fprintf(f, "%s", ir_get_str(ctx, ctx->ir_base[insn->op2].val.name));
	} else {
		ir_emit_ref(ctx, f, insn->op2);
	}
	fprintf(f, "(");
	n = insn->inputs_count;
	for (j = 3; j <= n; j++) {
		if (j != 3) {
			fprintf(f, ", ");
		}
		ir_emit_ref(ctx, f, ir_insn_op(insn, j));
	}
	fprintf(f, ");\n");
}

static void ir_emit_tailcall(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	int j, n;

	if (insn->type != IR_VOID) {
		fprintf(f, "\treturn ");
	} else {
		fprintf(f, "\t");
	}
	if (IR_IS_CONST_REF(insn->op2)) {
		IR_ASSERT(ctx->ir_base[insn->op2].op == IR_FUNC);
		fprintf(f, "%s", ir_get_str(ctx, ctx->ir_base[insn->op2].val.name));
	} else {
		ir_emit_ref(ctx, f, insn->op2);
	}
	fprintf(f, "(");
	n = insn->inputs_count;
	for (j = 3; j <= n; j++) {
		if (j != 3) {
			fprintf(f, ", ");
		}
		ir_emit_ref(ctx, f, ir_insn_op(insn, j));
	}
	fprintf(f, ");\n");
	if (insn->type == IR_VOID) {
		fprintf(f, "\treturn;\n");
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
	fprintf(f, "(uintptr_t)alloca(");
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
	fprintf(f, "*((%s*)", ir_type_cname[insn->type]);
	if (IR_IS_CONST_REF(insn->op2)) {
		ir_insn *const_insn = &ctx->ir_base[insn->op2];

		if (const_insn->op == IR_SYM) {
			fprintf(f, "&");
		}
		ir_print_const(ctx, const_insn, f, true);
	} else {
		fprintf(f, "d_%d", ctx->vregs[insn->op2]);
	}
	fprintf(f, ");\n");
}

static void ir_emit_store(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op3].type;

	fprintf(f, "\t*((%s*)", ir_type_cname[type]);
	if (IR_IS_CONST_REF(insn->op2)) {
		if (insn->op == IR_SYM) {
			fprintf(f, "&");
		}
		ir_print_const(ctx, &ctx->ir_base[insn->op2], f, true);
	} else {
		fprintf(f, "d_%d", ctx->vregs[insn->op2]);
	}
	fprintf(f, ") = ");
	ir_emit_ref(ctx, f, insn->op3);
	fprintf(f, ";\n");
}

static int ir_emit_func(ir_ctx *ctx, const char *name, FILE *f)
{
	ir_ref i, n, *p;
	ir_insn *insn;
	ir_use_list *use_list;
	bool first;
	ir_bitset vars, tmp_types;
	uint32_t _b, b, target, prev = 0;
	ir_block *bb;

	/* Emit function prototype */
	if (ctx->flags & IR_STATIC) {
		fprintf(f, "static ");
	}
	fprintf(f, "%s %s%s(", ir_type_cname[ctx->ret_type != (ir_type)-1 ? ctx->ret_type : IR_VOID],
		(ctx->flags & IR_FASTCALL_FUNC) ? "__fastcall " : "", name);
	use_list = &ctx->use_lists[1];
	n = use_list->count;
	first = 1;
	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
		insn = &ctx->ir_base[*p];
		if (insn->op == IR_PARAM) {
			if (first) {
				first = 0;
			} else {
				fprintf(f, ", ");
			}
			fprintf(f, "%s %s", ir_type_cname[insn->type], ir_get_str(ctx, insn->op2));
		}
	}
	if (ctx->flags & IR_VARARG_FUNC) {
		if (first) {
			first = 0;
		} else {
			fprintf(f, ", ");
		}
		fprintf(f, "...");
	}
	if (first) {
		fprintf(f, "void");
	}
	fprintf(f, ")\n{\n");

	if (!ctx->prev_ref) {
		ir_build_prev_refs(ctx);
	}

	/* Emit declarations for local variables */
	tmp_types = ir_bitset_malloc(IR_LAST_TYPE);
	vars = ir_bitset_malloc(ctx->vregs_count + 1);
	for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		if (ctx->prev_ref[bb->end] == bb->start
		 && bb->successors_count == 1
		 && (ctx->ir_base[bb->end].op == IR_END || ctx->ir_base[bb->end].op == IR_LOOP_END)
		 && !(bb->flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_DESSA_MOVES))) {
			bb->flags |= IR_BB_EMPTY;
		}
		if (bb->flags & IR_BB_DESSA_MOVES) {
			ctx->data = tmp_types;
			ir_gen_dessa_moves(ctx, b, ir_add_tmp_type);
		}
		for (i = bb->start, insn = ctx->ir_base + i; i <= bb->end;) {
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
			n = ir_insn_len(insn);
			i += n;
			insn += n;
		}
	}
	ir_mem_free(vars);

	IR_BITSET_FOREACH(tmp_types, ir_bitset_len(IR_LAST_TYPE), i) {
		fprintf(f, "\t%s tmp_%s;\n", ir_type_cname[i], ir_type_tname[i]);
	} IR_BITSET_FOREACH_END();
	ir_mem_free(tmp_types);

	for (_b = 1; _b <= ctx->cfg_blocks_count; _b++) {
		if (ctx->cfg_schedule) {
			b = ctx->cfg_schedule[_b];
		} else {
			b = _b;
		}
		bb = &ctx->cfg_blocks[b];
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		if ((bb->flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_EMPTY)) == IR_BB_EMPTY) {
			continue;
		}
		if (bb->predecessors_count > 1
		 || (bb->predecessors_count == 1 && ctx->cfg_edges[bb->predecessors] != prev)
		 || ctx->ir_base[bb->start].op == IR_CASE_VAL
		 || ctx->ir_base[bb->start].op == IR_CASE_DEFAULT) {
			fprintf(f, "bb%d:\n", b);
		}
		prev = b;
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
				case IR_CTPOP:
					ir_emit_count(ctx, f, i, insn, "popcount");
					break;
				case IR_CTLZ:
					ir_emit_count(ctx, f, i, insn, "clz");
					break;
				case IR_CTTZ:
					ir_emit_count(ctx, f, i, insn, "ctz");
					break;
				case IR_SEXT:
					ir_emit_sext(ctx, f, i, insn);
					break;
				case IR_ZEXT:
					ir_emit_zext(ctx, f, i, insn);
					break;
				case IR_TRUNC:
					ir_emit_trunc(ctx, f, i, insn);
					break;
				case IR_BITCAST:
				case IR_PROTO:
					ir_emit_bitcast(ctx, f, i, insn);
					break;
				case IR_INT2FP:
				case IR_FP2INT:
				case IR_FP2FP:
					ir_emit_conv(ctx, f, i, insn);
					break;
				case IR_ADD_OV:
					ir_emit_overflow_math(ctx, f, i, insn, "add", "+");
					break;
				case IR_SUB_OV:
					ir_emit_overflow_math(ctx, f, i, insn, "sub", "-");
					break;
				case IR_MUL_OV:
					ir_emit_overflow_math(ctx, f, i, insn, "mul", "*");
					break;
				case IR_OVERFLOW:
					break;
				case IR_COPY:
					ir_emit_copy(ctx, f, i, insn);
					break;
				case IR_RETURN:
					IR_ASSERT(bb->successors_count == 0);
					fprintf(f, "\treturn");
					if (!insn->op2) {
						fprintf(f, ";\n");
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
					target = ir_skip_empty_target_blocks(ctx, ctx->cfg_edges[bb->successors]);
					if (target != ir_next_block(ctx, _b)) {
						fprintf(f, "\tgoto bb%d;\n", target);
					}
					break;
				case IR_IF:
					ir_emit_if(ctx, f, b, i, insn, ir_next_block(ctx, _b));
					break;
				case IR_SWITCH:
					ir_emit_switch(ctx, f, b, i, insn);
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
				case IR_BLOCK_BEGIN:
					fprintf(f, "{\n");
					break;
				case IR_BLOCK_END:
					fprintf(f, "}\n");
					break;
				case IR_FRAME_ADDR:
					ir_emit_def_ref(ctx, f, i);
					fprintf(f, "__builtin_frame_address(0);");
					break;
				case IR_VA_START:
					fprintf(f, "\tva_start(");
					ir_emit_ref(ctx, f, insn->op2);
					fprintf(f, ");\n");
					break;
				case IR_VA_END:
					fprintf(f, "\tva_end(");
					ir_emit_ref(ctx, f, insn->op2);
					fprintf(f, ");\n");
					break;
				case IR_VA_COPY:
					fprintf(f, "\tva_copy(");
					ir_emit_ref(ctx, f, insn->op2);
					fprintf(f, ", ");
					ir_emit_ref(ctx, f, insn->op3);
					fprintf(f, ");\n");
					break;
				case IR_VA_ARG:
					ir_emit_def_ref(ctx, f, i);
					fprintf(f, "va_arg(");
					ir_emit_ref(ctx, f, insn->op2);
					fprintf(f, ", %s);\n", ir_type_cname[insn->type]);
					break;
				case IR_TRAP:
					fprintf(f, "\t__builtin_debugtrap();\n");
					break;
				default:
					IR_ASSERT(0 && "NIY instruction");
					ctx->status = IR_ERROR_UNSUPPORTED_CODE_RULE;
					return 0;
			}
			n = ir_insn_len(insn);
			i += n;
			insn += n;
		}
	}

	fprintf(f, "}\n");

	return 1;
}

int ir_emit_c(ir_ctx *ctx, const char *name, FILE *f)
{
	return ir_emit_func(ctx, name, f);
}

void ir_emit_c_func_decl(const char *name, uint32_t flags, ir_type ret_type, uint32_t params_count, const uint8_t *param_types, FILE *f)
{
	if (flags & IR_EXTERN) {
		fprintf(f, "extern ");
	} else if (flags & IR_STATIC) {
		fprintf(f, "static ");
	}
	fprintf(f, "%s ", ir_type_cname[ret_type]);
	if (flags & IR_FASTCALL_FUNC) {
		fprintf(f, "__fastcall ");
	}
	fprintf(f, "%s(", name);
	if (params_count) {
		const uint8_t *p = param_types;

		fprintf(f, "%s", ir_type_cname[*p]);
		p++;
		while (--params_count) {
			fprintf(f, ", %s", ir_type_cname[*p]);
			p++;
		}
		if (flags & IR_VARARG_FUNC) {
			fprintf(f, ", ...");
		}
	} else if (flags & IR_VARARG_FUNC) {
		fprintf(f, "...");
	} else {
		fprintf(f, "void");
	}
	fprintf(f, ");\n");
}

void ir_emit_c_sym_decl(const char *name, uint32_t flags, FILE *f)
{
	if (flags & IR_EXTERN) {
		fprintf(f, "extern ");
	} else if (flags & IR_STATIC) {
		fprintf(f, "static ");
	}
	if (flags & IR_CONST) {
		fprintf(f, "static ");
	}
	// TODO: type
	fprintf(f, "uintptr_t %s", name);
	if (flags & IR_INITIALIZED) {
		fprintf(f, " =\n");
	} else {
		fprintf(f, ";\n");
	}
}
