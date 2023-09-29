/*
 * IR - Lightweight JIT Compilation Framework
 * (LLVM code generator)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h"

#include <math.h>

static const char *ir_type_llvm_name[IR_LAST_TYPE] = {
	"void",   // IR_VOID
	"i1",     // IR_BOOL
	"i8",     // IR_U8
	"i16",    // IR_U16
	"i32",    // IR_U32
	"i64",    // IR_U63
	"ptr",    // IR_ADDR
	"i8",     // IR_CHAR
	"i8",     // IR_I8
	"i16",    // IR_I16
	"i32",    // IR_I32
	"i64",    // IR_I64
	"double", // IR_DOUBLE
	"float",  // IR_FLOAT
};

static void ir_emit_ref(ir_ctx *ctx, FILE *f, ir_ref ref)
{
	if (IR_IS_CONST_REF(ref)) {
		ir_insn *insn = &ctx->ir_base[ref];
		if (insn->op == IR_FUNC) {
			fprintf(f, "@%s", ir_get_str(ctx, insn->val.i32));
		} else if (insn->op == IR_STR) {
			fprintf(f, "@.str%d", -ref);
		} else if (IR_IS_TYPE_FP(insn->type)) {
			if (insn->type == IR_DOUBLE) {
				if (isnan(insn->val.d)) {
					fprintf(f, "nan");
				} else if (insn->val.d == 0.0) {
					fprintf(f, "0.0");
				} else {
					fprintf(f, "%g", insn->val.d);
				}
			} else {
				IR_ASSERT(insn->type == IR_FLOAT);
				if (isnan(insn->val.f)) {
					fprintf(f, "nan");
				} else if (insn->val.f == 0.0) {
					fprintf(f, "0.0");
				} else {
					fprintf(f, "%e", insn->val.f);
				}
			}
		} else {
			ir_print_const(ctx, &ctx->ir_base[ref], f, true);
		}
	} else {
		fprintf(f, "%%d%d", ref);
	}
}

static void ir_emit_def_ref(ir_ctx *ctx, FILE *f, ir_ref def)
{
	fprintf(f, "\t%%d%d = ", def);
}

static void ir_emit_phi(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, ir_block *bb)
{
	bool first = 1;
	uint32_t j, n, *p;

	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "phi %s ", ir_type_llvm_name[insn->type]);
	n = insn->inputs_count;
	p = ctx->cfg_edges + bb->predecessors;
	for (j = 2; j <= n; j++) {
		if (first) {
			first = 0;
		} else {
			fprintf(f, ", ");
		}
		fprintf(f, "[");
		ir_emit_ref(ctx, f, ir_insn_op(insn, j));
		fprintf(f, ", %%l%d", *p);
		p++;
		fprintf(f, "]");
	}
	fprintf(f, "\n");
}

static void ir_emit_unary_neg(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = insn->type;

	ir_emit_def_ref(ctx, f, def);
	if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "fneg %s ", ir_type_llvm_name[type]);
	} else {
		fprintf(f, "sub %s 0, ", ir_type_llvm_name[type]);
	}
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, "\n");
}

static void ir_emit_unary_not(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	if (insn->type == IR_BOOL) {
		if (IR_IS_TYPE_FP(type)) {
			fprintf(f, "fcmp oeq %s ", ir_type_llvm_name[type]);
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, ", 0.0\n");
		} else {
			fprintf(f, "icmp eq %s ", ir_type_llvm_name[type]);
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, ", 0\n");
		}
	} else {
		IR_ASSERT(IR_IS_TYPE_INT(type) && type == insn->type);
		fprintf(f, "xor %s ", ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", -1\n");
	}
}

static void ir_emit_binary_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op, const char *uop, const char *fop)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	if (fop && IR_IS_TYPE_FP(type)) {
		fprintf(f, "%s ", fop);
	} else {
		IR_ASSERT(IR_IS_TYPE_INT(type));
		if (uop && IR_IS_TYPE_UNSIGNED(type)) {
			fprintf(f, "%s ", uop);
		} else {
			fprintf(f, "%s ", op);
		}
	}
	fprintf(f, "%s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", ");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, "\n");
}

static void ir_emit_binary_overflow_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op, const char *uop)
{
	ir_type type = insn->type;

	IR_ASSERT(IR_IS_TYPE_INT(type));
	fprintf(f, "\t%%t%d = call {%s, i1} @llvm.%s.with.overflow.%s(%s ", def, ir_type_llvm_name[type],
		IR_IS_TYPE_UNSIGNED(type) ? uop : op,
		ir_type_llvm_name[type], ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ")\n");

	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "extractvalue {%s, i1} %%t%d, 0\n", ir_type_llvm_name[type], def);
}

static void ir_emit_rol_ror(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "call %s @llvm.%s.%s(%s ",
		ir_type_llvm_name[type], op, ir_type_llvm_name[type], ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ")\n");
}

static void ir_emit_bswap(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = insn->type;

	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "call %s @llvm.bswap.%s(%s ",
		ir_type_llvm_name[type], ir_type_llvm_name[type], ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ")\n");
}

static void ir_emit_conv(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "%s %s ", op, ir_type_llvm_name[ctx->ir_base[insn->op1].type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " to %s\n", ir_type_llvm_name[insn->type]);
}

static void ir_emit_minmax_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op, const char *uop, const char *fop)
{
	ir_type type = insn->type;

	ir_emit_def_ref(ctx, f, def);
	if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "call %s @llvm.%s.%s(%s ", ir_type_llvm_name[type],
			fop, type == IR_DOUBLE ? "f64" : "f32",
			ir_type_llvm_name[type]);
	} else {
		IR_ASSERT(IR_IS_TYPE_INT(type));
		if (IR_IS_TYPE_UNSIGNED(type)) {
			fprintf(f, "call %s @llvm.%s.%s(%s ", ir_type_llvm_name[type],
				uop, ir_type_llvm_name[type], ir_type_llvm_name[type]);
		} else {
			fprintf(f, "call %s @llvm.%s.%s(%s ", ir_type_llvm_name[type],
				op, ir_type_llvm_name[type], ir_type_llvm_name[type]);
		}
	}
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ")\n");
}

static void ir_emit_conditional_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	if (type == IR_BOOL) {
		fprintf(f, "select i1 ");
		ir_emit_ref(ctx, f, insn->op1);
	} else if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "\t%%t%d = fcmp une %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", 0.0\n");
		fprintf(f, "select i1 %%t%d", def);
	} else {
		fprintf(f, "\t%%t%d = icmp ne %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", 0\n");
		fprintf(f, "select i1 %%t%d", def);
	}

	fprintf(f, ", %s ", ir_type_llvm_name[insn->type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ", %s ", ir_type_llvm_name[insn->type]);
	ir_emit_ref(ctx, f, insn->op3);
	fprintf(f, "\n");
}

static void ir_emit_abs(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "call %s @llvm.fabs.%s(%s ",
			ir_type_llvm_name[type], type == IR_DOUBLE ? "f64" : "f32", ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ")\n");
	} else {
		fprintf(f, "call %s @llvm.abs.%s(%s ",
			ir_type_llvm_name[type], ir_type_llvm_name[type], ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", i1 0)\n");
	}
}

static void ir_emit_if(ir_ctx *ctx, FILE *f, uint32_t b, ir_ref def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op2].type;
	uint32_t true_block = 0, false_block = 0, next_block;

	ir_get_true_false_blocks(ctx, b, &true_block, &false_block, &next_block);

	// TODO: i1 @llvm.expect.i1(i1 <val>, i1 <expected_val>) ???

	if (type == IR_BOOL) {
		fprintf(f, "\tbr i1 ");
		ir_emit_ref(ctx, f, insn->op2);
	} else if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "\t%%t%d = fcmp une %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", 0.0\n");
		fprintf(f, "\tbr i1 %%t%d", def);
	} else {
		fprintf(f, "\t%%t%d = icmp ne %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", 0\n");
		fprintf(f, "\tbr i1 %%t%d", def);
	}
	fprintf(f, ", label %%l%d, label %%l%d\n", true_block, false_block);
}

static void ir_emit_guard(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op2].type;

	// TODO: i1 @llvm.expect.i1(i1 <val>, i1 <expected_val>) ???
	if (type == IR_BOOL) {
		fprintf(f, "\tbr i1 ");
		ir_emit_ref(ctx, f, insn->op2);
	} else if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "\t%%t%d = fcmp une %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", 0.0\n");
		fprintf(f, "\tbr i1 %%t%d", def);
	} else {
		fprintf(f, "\t%%t%d = icmp ne %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", 0\n");
		fprintf(f, "\tbr i1 %%t%d", def);
	}
	fprintf(f, ", label %%l%d_true, label %%l%d_false\n", def, def);
	fprintf(f, "l%d_%s:\n", def, insn->op == IR_GUARD ? "false" : "true");
	fprintf(f, "\tindirectbr ptr ");
	if (IR_IS_CONST_REF(insn->op3) && ctx->ir_base[insn->op3].op == IR_ADDR) {
		fprintf(f, "inttoptr(i64 u0x%" PRIxPTR " to ptr)", ctx->ir_base[insn->op3].val.addr);
	} else {
		ir_emit_ref(ctx, f, insn->op3);
	}
	fprintf(f, ", []\n");
	fprintf(f, "l%d_%s:\n", def, insn->op == IR_GUARD ? "true" : "false");
}

static void ir_emit_switch(ir_ctx *ctx, FILE *f, uint32_t b, ir_ref def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op2].type;
	ir_block *bb;
	uint32_t n, *p, use_block;
	ir_insn *use_insn;

	// TODO: i1 @llvm.expect.i1(i1 <val>, i1 <expected_val>) ???

	fprintf(f, "\tswitch %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op2);

	bb = &ctx->cfg_blocks[b];

	p = &ctx->cfg_edges[bb->successors];
	for (n = bb->successors_count; n != 0; p++, n--) {
		use_block = *p;
		use_insn = &ctx->ir_base[ctx->cfg_blocks[use_block].start];
		if (use_insn->op == IR_CASE_DEFAULT) {
			fprintf(f, ", label %%l%d", ir_skip_empty_target_blocks(ctx, use_block));
			break;
		} else {
			IR_ASSERT(use_insn->op == IR_CASE_VAL);
		}
	}
	fprintf(f, " [\n");
	p = &ctx->cfg_edges[bb->successors];
	for (n = bb->successors_count; n != 0; p++, n--) {
		use_block = *p;
		use_insn = &ctx->ir_base[ctx->cfg_blocks[use_block].start];
		if (use_insn->op == IR_CASE_VAL) {
			fprintf(f, "\t\t%s ", ir_type_llvm_name[type]);
			ir_emit_ref(ctx, f, use_insn->op2);
			fprintf(f, ", label %%l%d\n", ir_skip_empty_target_blocks(ctx, use_block));
		} else {
			IR_ASSERT(use_insn->op == IR_CASE_DEFAULT);
		}
	}
	fprintf(f, "\t]\n");
}

static void ir_emit_call(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	int j, k, n;

	if (insn->type != IR_VOID) {
		ir_emit_def_ref(ctx, f, def);
	} else {
		fprintf(f, "\t");
	}
	if (insn->op == IR_TAILCALL) {
		fprintf(f, "tail ");
	}
	fprintf(f, "call ");
	if (ir_is_fastcall(ctx, insn)) {
		fprintf(f, "x86_fastcallcc ");
	}
	fprintf(f, "%s ", ir_type_llvm_name[insn->type]);
	if (ir_is_vararg(ctx, insn)) {
		fprintf(f, "(...) ");
	}

	// TODO: function prototype ???

	if (IR_IS_CONST_REF(insn->op2)) {
		fprintf(f, "@%s", ir_get_str(ctx, ctx->ir_base[insn->op2].val.i32));
	} else {
		ir_emit_ref(ctx, f, insn->op2);
	}
	fprintf(f, "(");
	n = insn->inputs_count;
	for (j = 3; j <= n; j++) {
		if (j != 3) {
			fprintf(f, ", ");
		}
		k = ir_insn_op(insn, j);
		fprintf(f, "%s ", ir_type_llvm_name[ctx->ir_base[k].type]);
		ir_emit_ref(ctx, f, k);
	}
	fprintf(f, ")\n");
	if (insn->op == IR_TAILCALL) {
		fprintf(f, "\tret %s", ir_type_llvm_name[insn->type]);
		if (insn->type != IR_VOID) {
			fprintf(f, " ");
			ir_emit_ref(ctx, f, def);
		}
		fprintf(f, "\n");
	}
}

static void ir_emit_ijmp(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	fprintf(f, "\tindirectbr ptr ");
	if (IR_IS_CONST_REF(insn->op2) && ctx->ir_base[insn->op2].op == IR_ADDR) {
		fprintf(f, "inttoptr(i64 u0x%" PRIxPTR " to ptr)", ctx->ir_base[insn->op2].val.addr);
	} else {
		ir_emit_ref(ctx, f, insn->op2);
	}
	fprintf(f, ", []\n");
}

static void ir_emit_alloca(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "alloca i8, %s ", ir_type_llvm_name[ctx->ir_base[insn->op2].type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ", align 16\n");
}

static void ir_emit_vaddr(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "bitcast ptr ");
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " to ptr\n");
}

static void ir_emit_load(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "load %s, ptr ", ir_type_llvm_name[insn->type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, "\n");
}

static void ir_emit_store(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op3].type;

	fprintf(f, "\tstore %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op3);
	fprintf(f, ", ptr ");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, "\n");
}

static bool may_be_used_by_phi(ir_ctx *ctx, ir_block *bb)
{
	if (bb->successors_count == 1) {
		bb = &ctx->cfg_blocks[ctx->cfg_edges[bb->successors]];
		if (bb->predecessors_count > 1) {
			ir_use_list *use_list = &ctx->use_lists[bb->start];
			ir_ref i, n, *p;

			n = use_list->count;
			if (n > 1) {
				for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
					if (ctx->ir_base[*p].op == IR_PHI) {
						return 1;
					}
				}
			}
		}
	}
	return 0;
}

static int ir_emit_func(ir_ctx *ctx, const char *name, FILE *f)
{
	ir_ref i, n, *p, use;
	ir_insn *insn;
	ir_use_list *use_list;
	uint8_t ret_type;
	bool first;
	uint32_t b, target;
	ir_block *bb;

	/* Emit function prototype */
	ret_type = ir_get_return_type(ctx);
	fprintf(f, "define %s", ir_type_llvm_name[ret_type]);
	fprintf(f, " @%s(", name);
	use_list = &ctx->use_lists[1];
	n = use_list->count;
	first = 1;
	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
		use = *p;
		insn = &ctx->ir_base[use];
		if (insn->op == IR_PARAM) {
			if (first) {
				first = 0;
			} else {
				fprintf(f, ", ");
			}
			fprintf(f, "%s %%d%d", ir_type_llvm_name[insn->type], use);
		}
	}
	fprintf(f, ")\n{\n");

	for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		if ((bb->flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_EMPTY)) == IR_BB_EMPTY) {
			continue;
		}
		if (bb->predecessors_count > 0 || may_be_used_by_phi(ctx, bb)) {
			fprintf(f, "l%d:\n", b);
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
				case IR_UNREACHABLE:
				case IR_PARAM:
					/* skip */
					break;
				case IR_PHI:
					ir_emit_phi(ctx, f, i, insn, bb);
					break;
				case IR_VAR:
					use_list = &ctx->use_lists[i];
					if (use_list->count > 0) {
						fprintf(f, "\t%%d%d = alloca %s\n", i, ir_type_llvm_name[insn->type]);
					}
					break;
				case IR_EQ:
					ir_emit_binary_op(ctx, f, i, insn, "icmp eq", NULL, "fcmp oeq");
					break;
				case IR_NE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp ne", NULL, "fcmp une");
					break;
				case IR_LT:
					ir_emit_binary_op(ctx, f, i, insn, "icmp slt", "icmp ult", "fcmp olt");
					break;
				case IR_GE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp sge", "icmp uge", "fcmp oge");
					break;
				case IR_LE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp sle", "icmp ule", "fcmp ole");
					break;
				case IR_GT:
					ir_emit_binary_op(ctx, f, i, insn, "icmp sgt", "icmp ugt", "fcmp ogt");
					break;
				case IR_ULT:
					ir_emit_binary_op(ctx, f, i, insn, "icmp ult", NULL, "fcmp ult");
					break;
				case IR_UGE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp uge", NULL, "fcmp uge");
					break;
				case IR_ULE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp ule", NULL, "fcmp ule");
					break;
				case IR_UGT:
					ir_emit_binary_op(ctx, f, i, insn, "icmp ugt", NULL, "fcmp ugt");
					break;
				case IR_ADD:
					ir_emit_binary_op(ctx, f, i, insn, "add", NULL, "fadd");
					break;
				case IR_SUB:
					ir_emit_binary_op(ctx, f, i, insn, "sub", NULL, "fsub");
					break;
				case IR_MUL:
					ir_emit_binary_op(ctx, f, i, insn, "mul", NULL, "fmul");
					break;
				case IR_DIV:
					ir_emit_binary_op(ctx, f, i, insn, "sdiv", "udiv", "fdiv");
					break;
				case IR_MOD:
					ir_emit_binary_op(ctx, f, i, insn, "srem", "urem", NULL);
					break;
				case IR_NEG:
					ir_emit_unary_neg(ctx, f, i, insn);
					break;
				case IR_NOT:
					ir_emit_unary_not(ctx, f, i, insn);
					break;
				case IR_OR:
					ir_emit_binary_op(ctx, f, i, insn, "or", NULL, NULL);
					break;
				case IR_AND:
					ir_emit_binary_op(ctx, f, i, insn, "and", NULL, NULL);
					break;
				case IR_XOR:
					ir_emit_binary_op(ctx, f, i, insn, "xor", NULL, NULL);
					break;
				case IR_MIN:
					ir_emit_minmax_op(ctx, f, i, insn, "smin", "umin", "minnum");
					break;
				case IR_MAX:
					ir_emit_minmax_op(ctx, f, i, insn, "smax", "umax", "maxnum");
					break;
				case IR_COND:
					ir_emit_conditional_op(ctx, f, i, insn);
					break;
				case IR_ABS:
					ir_emit_abs(ctx, f, i, insn);
					break;
				case IR_SHL:
					ir_emit_binary_op(ctx, f, i, insn, "shl", NULL, NULL);
					break;
				case IR_SHR:
					ir_emit_binary_op(ctx, f, i, insn, "lshr", NULL, NULL);
					break;
				case IR_SAR:
					ir_emit_binary_op(ctx, f, i, insn, "ashr", NULL, NULL);
					break;
				case IR_ROL:
					ir_emit_rol_ror(ctx, f, i, insn, "fshl");
					break;
				case IR_ROR:
					ir_emit_rol_ror(ctx, f, i, insn, "fshr");
					break;
				case IR_BSWAP:
					ir_emit_bswap(ctx, f, i, insn);
					break;
				case IR_SEXT:
					IR_ASSERT(IR_IS_TYPE_INT(insn->type));
					IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
					IR_ASSERT(ir_type_size[insn->type] > ir_type_size[ctx->ir_base[insn->op1].type]);
					ir_emit_conv(ctx, f, i, insn, "sext");
					break;
				case IR_ZEXT:
					IR_ASSERT(IR_IS_TYPE_INT(insn->type));
					IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
					IR_ASSERT(ir_type_size[insn->type] > ir_type_size[ctx->ir_base[insn->op1].type]);
					ir_emit_conv(ctx, f, i, insn, "zext");
					break;
				case IR_TRUNC:
					IR_ASSERT(IR_IS_TYPE_INT(insn->type));
					IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
					IR_ASSERT(ir_type_size[insn->type] < ir_type_size[ctx->ir_base[insn->op1].type]);
					ir_emit_conv(ctx, f, i, insn, "trunc");
					break;
				case IR_BITCAST:
					if (insn->type == IR_ADDR
					 && IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type)
					 && ctx->ir_base[insn->op1].type != IR_ADDR) {
						ir_emit_conv(ctx, f, i, insn, "inttoptr");
					} else if (ctx->ir_base[insn->op1].type == IR_ADDR
					 && IR_IS_TYPE_INT(insn->type)
					 && insn->type != IR_ADDR) {
						ir_emit_conv(ctx, f, i, insn, "ptrtoint");
					} else {
						ir_emit_conv(ctx, f, i, insn, "bitcast");
					}
					break;
				case IR_INT2FP:
					IR_ASSERT(IR_IS_TYPE_FP(insn->type));
					IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
					ir_emit_conv(ctx, f, i, insn, IR_IS_TYPE_UNSIGNED(ctx->ir_base[insn->op1].type) ? "uitofp" : "sitofp");
					break;
				case IR_FP2INT:
					IR_ASSERT(IR_IS_TYPE_INT(insn->type));
					IR_ASSERT(IR_IS_TYPE_FP(ctx->ir_base[insn->op1].type));
					ir_emit_conv(ctx, f, i, insn, IR_IS_TYPE_UNSIGNED(insn->type) ? "fptoui" : "fptosi");
					break;
				case IR_FP2FP:
					IR_ASSERT(IR_IS_TYPE_FP(insn->type));
					IR_ASSERT(IR_IS_TYPE_FP(ctx->ir_base[insn->op1].type));
					ir_emit_conv(ctx, f, i, insn, insn->type == IR_DOUBLE ? "fpext" : "fptrunc");
					break;
				case IR_COPY:
					ir_emit_conv(ctx, f, i, insn, "bitcast");
					break;
				case IR_ADD_OV:
					ir_emit_binary_overflow_op(ctx, f, i, insn, "sadd", "uadd");
					break;
				case IR_SUB_OV:
					ir_emit_binary_overflow_op(ctx, f, i, insn, "ssub", "usub");
					break;
				case IR_MUL_OV:
					ir_emit_binary_overflow_op(ctx, f, i, insn, "smul", "umul");
					break;
				case IR_OVERFLOW:
					ir_emit_def_ref(ctx, f, i);
					fprintf(f, "extractvalue {%s, i1} %%t%d, 1\n", ir_type_llvm_name[ctx->ir_base[insn->op1].type], insn->op1);
					break;
				case IR_RETURN:
					IR_ASSERT(bb->successors_count == 0);
					if (!insn->op2) {
						fprintf(f, "\tret void\n");
					} else {
						fprintf(f, "\tret %s ", ir_type_llvm_name[ctx->ir_base[insn->op2].type]);
						ir_emit_ref(ctx, f, insn->op2);
						fprintf(f, "\n");
					}
					break;
				case IR_END:
				case IR_LOOP_END:
					IR_ASSERT(bb->successors_count == 1);
					target = ir_skip_empty_target_blocks(ctx, ctx->cfg_edges[bb->successors]);
					fprintf(f, "\tbr label %%l%d\n", target);
					break;
				case IR_IF:
					ir_emit_if(ctx, f, b, i, insn);
					break;
				case IR_SWITCH:
					ir_emit_switch(ctx, f, b, i, insn);
					break;
				case IR_CALL:
				case IR_TAILCALL:
					ir_emit_call(ctx, f, i, insn);
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
				case IR_LOAD:
				case IR_VLOAD:
					ir_emit_load(ctx, f, i, insn);
					break;
				case IR_STORE:
				case IR_VSTORE:
					ir_emit_store(ctx, f, insn);
					break;
				case IR_TRAP:
					fprintf(f, "\tcall void @llvm.debugtrap()\n");
					break;
				case IR_GUARD:
				case IR_GUARD_NOT:
					ir_emit_guard(ctx, f, i, insn);
					break;
				case IR_RLOAD:
				case IR_RSTORE:
				case IR_TLS:
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

	for (i = IR_UNUSED + 1, insn = ctx->ir_base - i; i < ctx->consts_count; i++, insn--) {
		if (insn->op == IR_FUNC) {
			// TODO: function prototype ???
			fprintf(f, "declare void @%s()\n", ir_get_str(ctx, insn->val.i32));
		} else if (insn->op == IR_STR) {
			const char *str = ir_get_str(ctx, insn->val.i32);
			// TODO: strlen != size ???
			int len = strlen(str);
			int j;

			fprintf(f, "@.str%d = private unnamed_addr constant [%d x i8] c\"", i, len + 1);
			for (j = 0; j < len; j++) {
				char c = str[j];

				if (c == '\\') {
					if (str[j+1] == '\\') {
						j++;
						c = '\\';
					} else if (str[j+1] == '\'') {
						j++;
						c = '\'';
					} else if (str[j+1] == '"') {
						j++;
						c = '"';
					} else if (str[j+1] == 'a') {
						j++;
						c = '\a';
					} else if (str[j+1] == 'b') {
						j++;
						c = '\b';
					} else if (str[j+1] == 'e') {
						j++;
						c = 27; /* '\e'; */
					} else if (str[j+1] == 'f') {
						j++;
						c = '\f';
					} else if (str[j+1] == 'n') {
						j++;
						c = '\n';
					} else if (str[j+1] == 'r') {
						j++;
						c = '\r';
					} else if (str[j+1] == 't') {
						j++;
						c = '\t';
					} else if (str[j+1] == 'v') {
						j++;
						c = '\v';
					} else if (str[j+1] == '?') {
						j++;
						c = 0x3f;
					}
				}
				if (c < ' ' || c >= 127) {
					char c1 = c >> 8;
					char c2 = c & 15;
					c1 = (c1 < 10) ? (c1 + '0') : (c1 - 10 + 'A');
					c2 = (c2 < 10) ? (c2 + '0') : (c2 - 10 + 'A');
					fprintf(f, "\\%c%c", c1, c2);
				} else {
					fprintf(f, "%c", c);
				}
			}
			fprintf(f, "\\00\"\n");
		}
	}

	return 1;
}

int ir_emit_llvm(ir_ctx *ctx, const char *name, FILE *f)
{
	return ir_emit_func(ctx, name, f);
}
