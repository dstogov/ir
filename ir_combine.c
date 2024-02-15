/*
 * IR - Lightweight JIT Compilation Framework
 * (Instruction Combinator)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h"

#define MAKE_NOP(_insn) do { \
		ir_insn *__insn = _insn; \
		__insn->optx = IR_NOP; \
		__insn->op1 = __insn->op2 = __insn->op3 = IR_UNUSED; \
	} while (0)

#define CLEAR_USES(_ref) do { \
		ir_use_list *__use_list = &ctx->use_lists[_ref]; \
		__use_list->count = 0; \
		__use_list->refs = 0; \
	} while (0)

#define SWAP_REFS(_ref1, _ref2) do { \
		ir_ref _tmp = _ref1; \
		_ref1 = _ref2; \
		_ref2 = _tmp; \
	} while (0)

#define SWAP_INSNS(_insn1, _insn2) do { \
		ir_insn *_tmp = _insn1; \
		_insn1 = _insn2; \
		_insn2 = _tmp; \
	} while (0)

void ir_get_true_false_refs(const ir_ctx *ctx, ir_ref if_ref, ir_ref *if_true_ref, ir_ref *if_false_ref)
{
	ir_use_list *use_list = &ctx->use_lists[if_ref];
	ir_ref *p = &ctx->use_edges[use_list->refs];

	IR_ASSERT(use_list->count == 2);
	if (ctx->ir_base[*p].op == IR_IF_TRUE) {
		IR_ASSERT(ctx->ir_base[*(p + 1)].op == IR_IF_FALSE);
		*if_true_ref = *p;
		*if_false_ref = *(p + 1);
	} else {
		IR_ASSERT(ctx->ir_base[*p].op == IR_IF_FALSE);
		IR_ASSERT(ctx->ir_base[*(p + 1)].op == IR_IF_TRUE);
		*if_false_ref = *p;
		*if_true_ref = *(p + 1);
	}
}

static void ir_use_list_remove(ir_ctx *ctx, ir_ref from, ir_ref ref)
{
	ir_ref j, n, *p, *q, use;
	ir_use_list *use_list = &ctx->use_lists[from];
	ir_ref skip = 0;

	n = use_list->count;
	for (j = 0, p = q = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
		use = *p;
		if (use == ref) {
			skip++;
		} else {
			if (p != q) {
				*q = use;
			}
			q++;
		}
	}
	use_list->count -= skip;
	if (skip) {
		do {
			*q = IR_UNUSED;
			q++;
		} while (--skip);
	}
}

static void ir_use_list_replace(ir_ctx *ctx, ir_ref ref, ir_ref use, ir_ref new_use)
{
	ir_use_list *use_list = &ctx->use_lists[ref];
	ir_ref i, n, *p;

	n = use_list->count;
	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
		if (*p == use) {
			*p = new_use;
			break;
		}
	}
}

static int ir_use_list_add(ir_ctx *ctx, ir_ref to, ir_ref ref)
{
	ir_use_list *use_list = &ctx->use_lists[to];
	ir_ref n = use_list->refs + use_list->count;

	if (n < ctx->use_edges_count && ctx->use_edges[n] == IR_UNUSED) {
		ctx->use_edges[n] = ref;
		use_list->count++;
		return 0;
	} else {
		/* Reallocate the whole edges buffer (this is inefficient) */
		ctx->use_edges = ir_mem_realloc(ctx->use_edges, (ctx->use_edges_count + use_list->count + 1) * sizeof(ir_ref));
		memcpy(ctx->use_edges + ctx->use_edges_count, ctx->use_edges + use_list->refs, use_list->count * sizeof(ir_ref));
		use_list->refs = ctx->use_edges_count;
		ctx->use_edges[use_list->refs + use_list->count] = ref;
		use_list->count++;
		ctx->use_edges_count += use_list->count;
		return 1;
	}
}

static void ir_replace_inputs(ir_ctx *ctx, ir_ref ref, ir_ref input, ir_ref new_input)
{
	ir_use_list *use_list = &ctx->use_lists[ref];
	ir_ref n = use_list->count;
	ir_ref *p = &ctx->use_edges[use_list->refs];

	for (; n; p++, n--) {
		ir_ref use = *p;
		ir_insn *insn = &ctx->ir_base[use];
		ir_ref k, l = insn->inputs_count;

		for (k = 1; k <= l; k++) {
			if (ir_insn_op(insn, k) == input) {
				ir_insn_set_op(insn, k, new_input);
			}
		}
	}
}

static bool ir_may_combine_d2f_op(ir_ctx *ctx, ir_ref ref)
{
	ir_insn *insn = &ctx->ir_base[ref];

	IR_ASSERT(insn->type == IR_DOUBLE);
	if (IR_IS_CONST_REF(ref)) {
		return !IR_IS_SYM_CONST(insn->op) && insn->val.d == (double)(float)insn->val.d;
	} else {
		switch (insn->op) {
			case IR_FP2FP:
				return 1;
//			case IR_INT2FP:
//				return ctx->use_lists[ref].count == 1;
			case IR_NEG:
			case IR_ABS:
				return ctx->use_lists[ref].count == 1 &&
					ir_may_combine_d2f_op(ctx, insn->op1);
			case IR_ADD:
			case IR_SUB:
			case IR_MUL:
			case IR_DIV:
			case IR_MIN:
			case IR_MAX:
				return ctx->use_lists[ref].count == 1 &&
					ir_may_combine_d2f_op(ctx, insn->op1) &&
					ir_may_combine_d2f_op(ctx, insn->op2);
			default:
				break;
		}
	}
	return 0;
}

static bool ir_may_combine_f2d_op(ir_ctx *ctx, ir_ref ref)
{
	ir_insn *insn = &ctx->ir_base[ref];

	IR_ASSERT(insn->type == IR_FLOAT);
	if (IR_IS_CONST_REF(ref)) {
		return !IR_IS_SYM_CONST(insn->op) && insn->val.f == (float)(double)insn->val.f;
	} else {
		switch (insn->op) {
			case IR_FP2FP:
				return 1;
			case IR_INT2FP:
				return ctx->use_lists[ref].count == 1;
			case IR_NEG:
			case IR_ABS:
				return ctx->use_lists[ref].count == 1 &&
					ir_may_combine_f2d_op(ctx, insn->op1);
			case IR_ADD:
			case IR_SUB:
			case IR_MUL:
//			case IR_DIV:
			case IR_MIN:
			case IR_MAX:
				return ctx->use_lists[ref].count == 1 &&
					ir_may_combine_f2d_op(ctx, insn->op1) &&
					ir_may_combine_f2d_op(ctx, insn->op2);
			default:
				break;
		}
	}
	return 0;
}

static ir_ref ir_combine_d2f_op(ir_ctx *ctx, ir_ref ref, ir_ref use)
{
	ir_insn *insn = &ctx->ir_base[ref];

	IR_ASSERT(insn->type == IR_DOUBLE);
	if (IR_IS_CONST_REF(ref)) {
		return ir_const_float(ctx, (float)insn->val.d);
	} else {
		switch (insn->op) {
			case IR_FP2FP:
				ir_use_list_remove(ctx, ref, use);
				if (ctx->use_lists[ref].count == 0) {
					ir_use_list_replace(ctx, insn->op1, ref, use);
					ref = insn->op1;
					insn->optx = IR_NOP;
					insn->op1 = IR_UNUSED;
					return ref;
				} else {
					ir_use_list_add(ctx, insn->op1, use);
				}
				return insn->op1;
//			case IR_INT2FP:
//				insn->type = IR_FLOAT;
//				return ref;
			case IR_NEG:
			case IR_ABS:
				insn->op1 = ir_combine_d2f_op(ctx, insn->op1, ref);
				insn->type = IR_FLOAT;
				return ref;
			case IR_ADD:
			case IR_SUB:
			case IR_MUL:
			case IR_DIV:
			case IR_MIN:
			case IR_MAX:
				if (insn->op1 == insn->op2) {
					insn->op2 = insn->op1 = ir_combine_d2f_op(ctx, insn->op1, ref);
				} else {
					insn->op1 = ir_combine_d2f_op(ctx, insn->op1, ref);
					insn->op2 = ir_combine_d2f_op(ctx, insn->op2, ref);
				}
				insn->type = IR_FLOAT;
				return ref;
			default:
				break;
		}
	}
	IR_ASSERT(0);
	return ref;
}

static ir_ref ir_combine_f2d_op(ir_ctx *ctx, ir_ref ref, ir_ref use)
{
	ir_insn *insn = &ctx->ir_base[ref];

	IR_ASSERT(insn->type == IR_FLOAT);
	if (IR_IS_CONST_REF(ref)) {
		return ir_const_double(ctx, (double)insn->val.f);
	} else {
		switch (insn->op) {
			case IR_FP2FP:
				ir_use_list_remove(ctx, ref, use);
				if (ctx->use_lists[ref].count == 0) {
					ir_use_list_replace(ctx, insn->op1, ref, use);
					ref = insn->op1;
					insn->optx = IR_NOP;
					insn->op1 = IR_UNUSED;
					return ref;
				} else {
					ir_use_list_add(ctx, insn->op1, use);
				}
				return insn->op1;
			case IR_INT2FP:
				insn->type = IR_DOUBLE;
				return ref;
			case IR_NEG:
			case IR_ABS:
				insn->op1 = ir_combine_f2d_op(ctx, insn->op1, ref);
				insn->type = IR_DOUBLE;
				return ref;
			case IR_ADD:
			case IR_SUB:
			case IR_MUL:
//			case IR_DIV:
			case IR_MIN:
			case IR_MAX:
				if (insn->op1 == insn->op2) {
					insn->op2 = insn->op1 = ir_combine_f2d_op(ctx, insn->op1, ref);
				} else {
					insn->op1 = ir_combine_f2d_op(ctx, insn->op1, ref);
					insn->op2 = ir_combine_f2d_op(ctx, insn->op2, ref);
				}
				insn->type = IR_DOUBLE;
				return ref;
			default:
				break;
		}
	}
	IR_ASSERT(0);
	return ref;
}

static void ir_combine_d2f(ir_ctx *ctx, ir_ref ref, ir_insn *insn)
{
	if (ir_may_combine_d2f_op(ctx, insn->op1)) {
		ir_ref new_ref = ir_combine_d2f_op(ctx, insn->op1, ref);
		if (insn->op1 == new_ref) {
			ir_replace_inputs(ctx, ref, ref, insn->op1);
			ctx->use_lists[insn->op1] = ctx->use_lists[ref];
			ctx->use_lists[ref].count = 0;
			ctx->use_lists[ref].refs = 0;
			insn->optx = IR_NOP;
			insn->op1 = IR_UNUSED;
		} else {
			insn->optx = IR_OPTX(IR_COPY, IR_FLOAT, 1);
			insn->op1 = new_ref;
		}
	}
}

static void ir_combine_f2d(ir_ctx *ctx, ir_ref ref, ir_insn *insn)
{
	if (ir_may_combine_f2d_op(ctx, insn->op1)) {
		ir_ref new_ref = ir_combine_f2d_op(ctx, insn->op1, ref);
		if (insn->op1 == new_ref) {
			ir_replace_inputs(ctx, ref, ref, insn->op1);
			ctx->use_lists[insn->op1] = ctx->use_lists[ref];
			ctx->use_lists[ref].count = 0;
			ctx->use_lists[ref].refs = 0;
			insn->optx = IR_NOP;
			insn->op1 = IR_UNUSED;
		} else {
			insn->optx = IR_OPTX(IR_COPY, IR_DOUBLE, 1);
			insn->op1 = new_ref;
		}
	}
}

static bool ir_may_combine_i2i_op(ir_ctx *ctx, ir_type type, ir_ref ref)
{
	ir_insn *insn = &ctx->ir_base[ref];

	if (IR_IS_CONST_REF(ref)) {
		return !IR_IS_SYM_CONST(insn->op);
	} else {
		switch (insn->op) {
			case IR_ZEXT:
			case IR_SEXT:
				return ctx->ir_base[insn->op1].type == type;
			case IR_NEG:
			case IR_ABS:
			case IR_NOT:
				return ctx->use_lists[ref].count == 1 &&
					ir_may_combine_i2i_op(ctx, type, insn->op1);
			case IR_ADD:
			case IR_SUB:
			case IR_MUL:
//			case IR_DIV:
			case IR_MIN:
			case IR_MAX:
			case IR_OR:
			case IR_AND:
			case IR_XOR:
				return ctx->use_lists[ref].count == 1 &&
					ir_may_combine_i2i_op(ctx, type, insn->op1) &&
					ir_may_combine_i2i_op(ctx, type, insn->op2);
			default:
				break;
		}
	}
	return 0;
}

static ir_ref ir_combine_i2i_op(ir_ctx *ctx, ir_type type, ir_ref ref, ir_ref use)
{
	ir_insn *insn = &ctx->ir_base[ref];

	if (IR_IS_CONST_REF(ref)) {
		return ir_const(ctx, insn->val, type);
	} else {
		switch (insn->op) {
			case IR_ZEXT:
			case IR_SEXT:
				ir_use_list_remove(ctx, ref, use);
				if (ctx->use_lists[ref].count == 0) {
					ir_use_list_replace(ctx, insn->op1, ref, use);
					ref = insn->op1;
					insn->optx = IR_NOP;
					insn->op1 = IR_UNUSED;
					return ref;
				} else {
					ir_use_list_add(ctx, insn->op1, use);
				}
				return insn->op1;
			case IR_NEG:
			case IR_ABS:
			case IR_NOT:
				insn->op1 = ir_combine_i2i_op(ctx, type, insn->op1, ref);
				insn->type = type;
				return ref;
			case IR_ADD:
			case IR_SUB:
			case IR_MUL:
//			case IR_DIV:
			case IR_MIN:
			case IR_MAX:
			case IR_OR:
			case IR_AND:
			case IR_XOR:
				if (insn->op1 == insn->op2) {
					insn->op2 = insn->op1 = ir_combine_i2i_op(ctx, type, insn->op1, ref);
				} else {
					insn->op1 = ir_combine_i2i_op(ctx, type, insn->op1, ref);
					insn->op2 = ir_combine_i2i_op(ctx, type, insn->op2, ref);
				}
				insn->type = type;
				return ref;
			default:
				break;
		}
	}
	IR_ASSERT(0);
	return ref;
}

static void ir_combine_trunc(ir_ctx *ctx, ir_ref ref, ir_insn *insn)
{
	if (ir_may_combine_i2i_op(ctx, insn->type, insn->op1)) {
		ir_ref new_ref = ir_combine_i2i_op(ctx, insn->type, insn->op1, ref);
		if (insn->op1 == new_ref) {
			ir_replace_inputs(ctx, ref, ref, insn->op1);
			ctx->use_lists[insn->op1] = ctx->use_lists[ref];
			ctx->use_lists[ref].count = 0;
			ctx->use_lists[ref].refs = 0;
			insn->optx = IR_NOP;
			insn->op1 = IR_UNUSED;
		} else {
			insn->optx = IR_OPTX(IR_COPY, insn->type, 1);
			insn->op1 = new_ref;
		}
	}
}

static void ir_combine_merge(ir_ctx *ctx, ir_ref ref, ir_insn *insn)
{
	if (ctx->use_lists[ref].count == 1) {
		if (insn->inputs_count == 2) {
			ir_ref end1_ref = insn->op1, end2_ref = insn->op2;
			ir_insn *end1 = &ctx->ir_base[end1_ref];
			ir_insn *end2 = &ctx->ir_base[end2_ref];

			if (end1->op != IR_END || end2->op != IR_END) {
				return;
			}

			ir_ref start1_ref = end1->op1, start2_ref = end2->op1;
			ir_insn *start1 = &ctx->ir_base[start1_ref];
			ir_insn *start2 = &ctx->ir_base[start2_ref];

			if (start1->op1 != start2->op1) {
				return;
			}

			ir_ref root_ref = start1->op1;
			ir_insn *root = &ctx->ir_base[root_ref];

			if (root->op != IR_IF
			 && !(root->op == IR_SWITCH && ctx->use_lists[root_ref].count == 2)) {
				return;
			}

			/* Empty Diamond
			 *
			 *    prev                     prev
			 *    |  condition             |  condition
			 *    | /                      |
			 *    IF                       |
			 *    | \                      |
			 *    |  +-----+               |
			 *    |        IF_FALSE        |
			 *    IF_TRUE  |           =>  |
			 *    |        END             |
			 *    END     /                |
			 *    |  +---+                 |
			 *    | /                      |
			 *    MERGE                    |
			 *    |                        |
			 *    next                     next
			 */

			ir_ref next_ref = ctx->use_edges[ctx->use_lists[ref].refs];
			ir_insn *next = &ctx->ir_base[next_ref];

			IR_ASSERT(ctx->use_lists[start1_ref].count == 1);
			IR_ASSERT(ctx->use_lists[start2_ref].count == 1);

			next->op1 = root->op1;
			ir_use_list_replace(ctx, root->op1, root_ref, next_ref);
			ir_use_list_remove(ctx, root->op2, root_ref);

			MAKE_NOP(root);   CLEAR_USES(root_ref);
			MAKE_NOP(start1); CLEAR_USES(start1_ref);
			MAKE_NOP(start2); CLEAR_USES(start2_ref);
			MAKE_NOP(end1);   CLEAR_USES(end1_ref);
			MAKE_NOP(end2);   CLEAR_USES(end2_ref);
			MAKE_NOP(insn);   CLEAR_USES(ref);
		} else {
			ir_ref i, count = insn->inputs_count, *ops = insn->ops + 1;
			ir_ref root_ref = IR_UNUSED;

			for (i = 0; i < count; i++) {
				ir_ref end_ref, start_ref;
				ir_insn *end, *start;

				end_ref = ops[i];
				end = &ctx->ir_base[end_ref];
				if (end->op != IR_END) {
					return;
				}
				start_ref = end->op1;
				start = &ctx->ir_base[start_ref];
				if (start->op != IR_CASE_VAL && start->op != IR_CASE_DEFAULT) {
					return;
				}
				IR_ASSERT(ctx->use_lists[start_ref].count == 1);
				if (!root_ref) {
					root_ref = start->op1;
					if (ctx->use_lists[root_ref].count != count) {
						return;
					}
				} else if (start->op1 != root_ref) {
					return;
				}
			}

			/* Empty N-Diamond */
			ir_ref next_ref = ctx->use_edges[ctx->use_lists[ref].refs];
			ir_insn *next = &ctx->ir_base[next_ref];
			ir_insn *root = &ctx->ir_base[root_ref];

			next->op1 = root->op1;
			ir_use_list_replace(ctx, root->op1, root_ref, next_ref);
			ir_use_list_remove(ctx, root->op2, root_ref);

			MAKE_NOP(root);   CLEAR_USES(root_ref);

			for (i = 0; i < count; i++) {
				ir_ref end_ref = ops[i];
				ir_insn *end = &ctx->ir_base[end_ref];
				ir_ref start_ref = end->op1;
				ir_insn *start = &ctx->ir_base[start_ref];

				MAKE_NOP(start); CLEAR_USES(start_ref);
				MAKE_NOP(end);   CLEAR_USES(end_ref);
			}

			MAKE_NOP(insn);   CLEAR_USES(ref);
		}
	}
}

static void ir_combine_phi(ir_ctx *ctx, ir_ref ref, ir_insn *insn)
{
	if (insn->inputs_count == 3) {
		ir_ref merge_ref = insn->op1;
		ir_insn *merge = &ctx->ir_base[merge_ref];

		if (ctx->use_lists[merge_ref].count == 2) {
			ir_ref end1_ref = merge->op1, end2_ref = merge->op2;
			ir_insn *end1 = &ctx->ir_base[end1_ref];
			ir_insn *end2 = &ctx->ir_base[end2_ref];

			if (end1->op == IR_END && end2->op == IR_END) {
				ir_ref start1_ref = end1->op1, start2_ref = end2->op1;
				ir_insn *start1 = &ctx->ir_base[start1_ref];
				ir_insn *start2 = &ctx->ir_base[start2_ref];

				if (start1->op1 == start2->op1) {
					ir_ref root_ref = start1->op1;
					ir_insn *root = &ctx->ir_base[root_ref];

					if (root->op == IR_IF && ctx->use_lists[root->op2].count == 1) {
						ir_ref cond_ref = root->op2;
						ir_insn *cond = &ctx->ir_base[cond_ref];
						ir_type type = insn->type;
						bool is_cmp, is_less;

						if (IR_IS_TYPE_FP(type)) {
							is_cmp = (cond->op == IR_LT || cond->op == IR_LE || cond->op == IR_GT || cond->op == IR_GE ||
								cond->op == IR_ULT || cond->op == IR_ULE || cond->op == IR_UGT || cond->op == IR_UGE);
						} else if (IR_IS_TYPE_SIGNED(type)) {
							is_cmp = (cond->op == IR_LT || cond->op == IR_LE || cond->op == IR_GT || cond->op == IR_GE);
						} else if (IR_IS_TYPE_UNSIGNED(type)) {
							is_cmp = (cond->op == IR_ULT || cond->op == IR_ULE || cond->op == IR_UGT || cond->op == IR_UGE);
						}

						if (is_cmp
						 && ((insn->op2 == cond->op1 && insn->op3 == cond->op2)
						   || (insn->op2 == cond->op2 && insn->op3 == cond->op1))) {
							/* MAX/MIN
							 *
							 *    prev                     prev
							 *    |  LT(A, B)              |
							 *    | /                      |
							 *    IF                       |
							 *    | \                      |
							 *    |  +-----+               |
							 *    |        IF_FALSE        |
							 *    IF_TRUE  |           =>  |
							 *    |        END             |
							 *    END     /                |
							 *    |  +---+                 |
							 *    | /                      |
							 *    MERGE                    |
							 *    |    \                   |
							 *    |     PHI(A, B)          |    MIN(A, B)
							 *    next                     next
							 */
							ir_ref next_ref = ctx->use_edges[ctx->use_lists[merge_ref].refs];
							ir_insn *next;

							if (next_ref == ref) {
								next_ref = ctx->use_edges[ctx->use_lists[merge_ref].refs + 1];
							}
							next = &ctx->ir_base[next_ref];

							IR_ASSERT(ctx->use_lists[start1_ref].count == 1);
							IR_ASSERT(ctx->use_lists[start2_ref].count == 1);

							if (IR_IS_TYPE_FP(type)) {
								is_less = (cond->op == IR_LT || cond->op == IR_LE ||
									cond->op == IR_ULT || cond->op == IR_ULE);
							} else if (IR_IS_TYPE_SIGNED(type)) {
								is_less = (cond->op == IR_LT || cond->op == IR_LE);
							} else if (IR_IS_TYPE_UNSIGNED(type)) {
								is_less = (cond->op == IR_ULT || cond->op == IR_ULE);
							}
							insn->op = (
								(is_less ? cond->op1 : cond->op2)
								==
								((start1->op == IR_IF_TRUE) ? insn->op2 : insn->op3)
								) ? IR_MIN : IR_MAX;
							insn->inputs_count = 2;
							if (insn->op2 > insn->op3) {
								insn->op1 = insn->op2;
								insn->op2 = insn->op3;
							} else {
								insn->op1 = insn->op3;
							}
							insn->op3 = IR_UNUSED;

							next->op1 = root->op1;
							ir_use_list_replace(ctx, root->op1, root_ref, next_ref);
							ir_use_list_remove(ctx, root->op2, root_ref);
							if (!IR_IS_CONST_REF(insn->op1)) {
								ir_use_list_remove(ctx, insn->op1, cond_ref);
							}
							if (!IR_IS_CONST_REF(insn->op2)) {
								ir_use_list_remove(ctx, insn->op2, cond_ref);
							}

							MAKE_NOP(cond);   CLEAR_USES(cond_ref);
							MAKE_NOP(root);   CLEAR_USES(root_ref);
							MAKE_NOP(start1); CLEAR_USES(start1_ref);
							MAKE_NOP(start2); CLEAR_USES(start2_ref);
							MAKE_NOP(end1);   CLEAR_USES(end1_ref);
							MAKE_NOP(end2);   CLEAR_USES(end2_ref);
							MAKE_NOP(merge);  CLEAR_USES(merge_ref);
#if 0
						} else {
							/* COND
							 *
							 *    prev                     prev
							 *    |  cond                  |
							 *    | /                      |
							 *    IF                       |
							 *    | \                      |
							 *    |  +-----+               |
							 *    |        IF_FALSE        |
							 *    IF_TRUE  |           =>  |
							 *    |        END             |
							 *    END     /                |
							 *    |  +---+                 |
							 *    | /                      |
							 *    MERGE                    |
							 *    |    \                   |
							 *    |     PHI(A, B)          |    COND(cond, A, B)
							 *    next                     next
							 */
							ir_ref next_ref = ctx->use_edges[ctx->use_lists[merge_ref].refs];
							ir_insn *next;

							if (next_ref == ref) {
								next_ref = ctx->use_edges[ctx->use_lists[merge_ref].refs + 1];
							}
							next = &ctx->ir_base[next_ref];

							IR_ASSERT(ctx->use_lists[start1_ref].count == 1);
							IR_ASSERT(ctx->use_lists[start2_ref].count == 1);

							insn->op = IR_COND;
							insn->inputs_count = 3;
							insn->op1 = cond_ref;
							if (start1->op == IR_IF_FALSE) {
								SWAP_REFS(insn->op2, insn->op3);
							}

							next->op1 = root->op1;
							ir_use_list_replace(ctx, cond_ref, root_ref, ref);
							ir_use_list_replace(ctx, root->op1, root_ref, next_ref);
							ir_use_list_remove(ctx, root->op2, root_ref);

							MAKE_NOP(root);   CLEAR_USES(root_ref);
							MAKE_NOP(start1); CLEAR_USES(start1_ref);
							MAKE_NOP(start2); CLEAR_USES(start2_ref);
							MAKE_NOP(end1);   CLEAR_USES(end1_ref);
							MAKE_NOP(end2);   CLEAR_USES(end2_ref);
							MAKE_NOP(merge);  CLEAR_USES(merge_ref);
#endif
						}
					}
				}
			}
		}
	}
}

static void ir_combine_if(ir_ctx *ctx, ir_ref ref, ir_insn *insn)
{
	ir_ref cond_ref = insn->op2;
	ir_insn *cond = &ctx->ir_base[cond_ref];

	if (cond->op == IR_PHI
	 && cond->inputs_count == 3
	 && cond->op1 == insn->op1
	 && (IR_IS_CONST_REF(cond->op2) || IR_IS_CONST_REF(cond->op3))) {
		ir_ref merge_ref = insn->op1;
		ir_insn *merge = &ctx->ir_base[merge_ref];

		if (ctx->use_lists[merge_ref].count == 2) {
			ir_ref end1_ref = merge->op1, end2_ref = merge->op2;
			ir_insn *end1 = &ctx->ir_base[end1_ref];
			ir_insn *end2 = &ctx->ir_base[end2_ref];

			if (end1->op == IR_END && end2->op == IR_END) {
				ir_ref if_true_ref, if_false_ref;
				ir_insn *if_true, *if_false;
				ir_op op = IR_IF_FALSE;

				ir_get_true_false_refs(ctx, ref, &if_true_ref, &if_false_ref);

				if (!IR_IS_CONST_REF(cond->op2)) {
					IR_ASSERT(IR_IS_CONST_REF(cond->op3));
					SWAP_REFS(cond->op2, cond->op3);
					SWAP_REFS(merge->op1, merge->op2);
					SWAP_REFS(end1_ref, end2_ref);
					SWAP_INSNS(end1, end2);
				}
				if (ir_const_is_true(&ctx->ir_base[cond->op2])) {
					SWAP_REFS(if_true_ref, if_false_ref);
					op = IR_IF_TRUE;
				}
				if_true = &ctx->ir_base[if_true_ref];
				if_false = &ctx->ir_base[if_false_ref];

				/* Simple IF Split
				 *
				 *    |        |               |        |
				 *    |        END             |        IF(X)
				 *    END     /                END     / \
				 *    |  +---+                 |   +--+   +
				 *    | /                      |  /       |
				 *    MERGE                    | IF_FALSE |
				 *    | \                      | |        |
				 *    |  PHI(false, X)         | |        |
				 *    | /                      | |        |
				 *    IF                   =>  | END      |
				 *    | \                      | |        |
				 *    |  +------+              | |        |
				 *    |         IF_TRUE        | |        IF_TRUE
				 *    IF_FALSE  |              MERGE
				 *    |                        |
				 */
				ir_use_list_remove(ctx, merge_ref, cond_ref);
				ir_use_list_remove(ctx, ref, if_true_ref);
				ir_use_list_replace(ctx, cond->op3, cond_ref, end2_ref);
				ir_use_list_replace(ctx, end1_ref, merge_ref, if_false_ref);
				ir_use_list_add(ctx, end2_ref, if_true_ref);

				end2->optx = IR_OPTX(IR_IF, IR_VOID, 2);
				end2->op2 = cond->op3;

				merge->optx = IR_OPTX(op, IR_VOID, 1);
				merge->op1 = end2_ref;
				merge->op2 = IR_UNUSED;

				MAKE_NOP(cond);
				CLEAR_USES(cond_ref);

				insn->optx = IR_OPTX(IR_END, IR_VOID, 1);
				insn->op1 = merge_ref;
				insn->op2 = IR_UNUSED;

				if_true->op1 = end2_ref;

				if_false->optx = IR_OPTX(IR_MERGE, IR_VOID, 2);
				if_false->op1 = end1_ref;
				if_false->op2 = ref;

				// TODO: ???
				ir_combine_if(ctx, end2_ref, end2);
			}
		}
	}
}

int ir_combine(ir_ctx *ctx)
{
	ir_ref i, n;
	ir_insn *insn;
	uint32_t flags;

	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count;) {
		switch (insn->op) {
			case IR_FP2FP:
				if (insn->type == IR_FLOAT) {
					ir_combine_d2f(ctx, i, insn);
				} else {
					ir_combine_f2d(ctx, i, insn);
				}
				break;
			case IR_FP2INT:
				if (ctx->ir_base[insn->op1].type == IR_DOUBLE) {
					if (ir_may_combine_d2f_op(ctx, insn->op1)) {
						insn->op1 = ir_combine_d2f_op(ctx, insn->op1, i);
					}
				} else {
					if (ir_may_combine_f2d_op(ctx, insn->op1)) {
						insn->op1 = ir_combine_f2d_op(ctx, insn->op1, i);
					}
				}
				break;
			case IR_TRUNC:
				ir_combine_trunc(ctx, i, insn);
				break;
			case IR_MERGE:
				ir_combine_merge(ctx, i, insn);
				break;
			case IR_PHI:
				ir_combine_phi(ctx, i, insn);
				break;
			case IR_IF:
				ir_combine_if(ctx, i, insn);
				break;
			default:
				break;
		}

		flags = ir_op_flags[insn->op];
		if (UNEXPECTED(IR_OP_HAS_VAR_INPUTS(flags))) {
			n = insn->inputs_count;
		} else {
			n = insn->inputs_count = IR_INPUT_EDGES_COUNT(flags);
		}
		n = ir_insn_inputs_to_len(n);
		i += n;
		insn += n;
	}

	return 1;
}
