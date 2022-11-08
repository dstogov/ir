/*
 * IR - Lightweight JIT Compilation Framework
 * (SCCP - Sparse Conditional Constant Propagation)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 *
 * The SCCP algorithm is based on M. N. Wegman and F. K. Zadeck publication
 * See:  M. N. Wegman and F. K. Zadeck. "Constant propagation with conditional branches"
 * ACM Transactions on Programming Languages and Systems, 13(2):181-210, April 1991
 */

#include "ir.h"
#include "ir_private.h"

#define IR_TOP                  IR_UNUSED
#define IR_BOTTOM               IR_LAST_OP

#define IR_MAKE_TOP(ref)        do {IR_ASSERT(ref > 0); _values[ref].optx = IR_TOP;} while (0)
#define IR_MAKE_BOTTOM(ref)     do {IR_ASSERT(ref > 0); _values[ref].optx = IR_BOTTOM;} while (0)

#define IR_IS_TOP(ref)          (ref >= 0 && _values[ref].optx == IR_TOP)
#define IR_IS_BOTTOM(ref)       (ref >= 0 && _values[ref].optx == IR_BOTTOM)
#define IR_IS_REACHABLE(ref)    (ref >= 0 && _values[ref].optx != IR_TOP)

#define IR_COMBO_COPY_PROPAGATION 1

#if IR_COMBO_COPY_PROPAGATION
IR_ALWAYS_INLINE ir_ref ir_sccp_identity(ir_insn *_values, ir_ref a)
{
	if (a > 0 && _values[a].op == IR_COPY) {
		a = _values[a].op1;
		IR_ASSERT(a <= 0 || _values[a].op != IR_COPY);
	}
	return a;
}
#endif

static ir_ref ir_sccp_fold(ir_ctx *ctx, ir_insn *_values, ir_ref res, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3)
{
	ir_insn *op1_insn, *op2_insn, *op3_insn, *insn;

#if IR_COMBO_COPY_PROPAGATION
	op1 = ir_sccp_identity(_values, op1);
	op2 = ir_sccp_identity(_values, op2);
	op3 = ir_sccp_identity(_values, op3);
#endif

restart:
	op1_insn = (op1 > 0 && IR_IS_CONST_OP(_values[op1].op)) ? _values + op1 : ctx->ir_base + op1;
	op2_insn = (op2 > 0 && IR_IS_CONST_OP(_values[op2].op)) ? _values + op2 : ctx->ir_base + op2;
	op3_insn = (op3 > 0 && IR_IS_CONST_OP(_values[op3].op)) ? _values + op3 : ctx->ir_base + op3;

	switch (ir_folding(ctx, opt, op1, op2, op3, op1_insn, op2_insn, op3_insn)) {
		case IR_FOLD_DO_RESTART:
			opt = ctx->fold_insn.optx;
			op1 = ctx->fold_insn.op1;
			op2 = ctx->fold_insn.op2;
			op3 = ctx->fold_insn.op3;
			goto restart;
		case IR_FOLD_DO_EMIT:
			IR_MAKE_BOTTOM(res);
			return 1;
		case IR_FOLD_DO_COPY:
			op1 = ctx->fold_insn.op1;
#if IR_COMBO_COPY_PROPAGATION
			op1 = ir_sccp_identity(_values, op1);
#endif
			insn = (op1 > 0 && IR_IS_CONST_OP(_values[op1].op)) ? _values + op1 : ctx->ir_base + op1;
			if (IR_IS_CONST_OP(insn->op)) {
				/* pass */
#if IR_COMBO_COPY_PROPAGATION
			} else if (IR_IS_TOP(res)) {
				_values[res].optx = IR_OPT(IR_COPY, insn->type);
				_values[res].op1 = op1;
				return 1;
			} else if (_values[res].op == IR_COPY && _values[res].op1 == op1) {
				return 0; /* not changed */
#endif
			} else {
				IR_MAKE_BOTTOM(res);
				return 1;
			}
			break;
		case IR_FOLD_DO_CONST:
			insn = &ctx->fold_insn;
			break;
		default:
			IR_ASSERT(0);
			return 0;
	}

	if (IR_IS_TOP(res)) {
		_values[res].optx = IR_OPT(insn->type, insn->type);
		_values[res].val.u64 = insn->val.u64;
		return 1;
	} else if (_values[res].opt != IR_OPT(insn->type, insn->type) || _values[res].val.u64 != insn->val.u64) {
		IR_MAKE_BOTTOM(res);
		return 1;
	}
	return 0; /* not changed */
}

static bool ir_sccp_join_values(ir_ctx *ctx, ir_insn *_values, ir_ref a, ir_ref b)
{
	ir_insn *v;

	if (!IR_IS_BOTTOM(a) && !IR_IS_TOP(b)) {
		b = ir_sccp_identity(_values, b);
		v = IR_IS_CONST_REF(b) ? &ctx->ir_base[b] : &_values[b];
		if (IR_IS_TOP(a)) {
#if IR_COMBO_COPY_PROPAGATION
			if (v->op == IR_BOTTOM) {
				_values[a].optx = IR_OPT(IR_COPY, ctx->ir_base[b].type);
				_values[a].op1 = b;
				return 1;
			}
#endif
			_values[a].optx = v->opt;
			_values[a].val.u64 = v->val.u64;
		    return 1;
		} else if (_values[a].opt == v->opt && _values[a].val.u64 == v->val.u64) {
			/* pass */
#if IR_COMBO_COPY_PROPAGATION
		} else if (_values[a].op == IR_COPY && _values[a].op1 == b) {
			/* pass */
#endif
		} else {
			IR_MAKE_BOTTOM(a);
			return 1;
		}
	}
	return 0;
}

static bool ir_sccp_is_true(ir_ctx *ctx, ir_insn *_values, ir_ref a)
{
	ir_insn *v = IR_IS_CONST_REF(a) ? &ctx->ir_base[a] : &_values[a];

	if (v->type == IR_BOOL) {
		return v->val.b;
	} else if (IR_IS_TYPE_INT(v->type)) {
		return v->val.i64 != 0;
	} else if (v->type == IR_DOUBLE) {
		return v->val.d != 0.0;
	} else if (v->type == IR_FLOAT) {
		return v->val.f != 0.0;
	}
	IR_ASSERT(0 && "NYI");
	return 0;
}

static bool ir_sccp_is_equal(ir_ctx *ctx, ir_insn *_values, ir_ref a, ir_ref b)
{
	ir_insn *v1 = IR_IS_CONST_REF(a) ? &ctx->ir_base[a] : &_values[a];
	ir_insn *v2 = IR_IS_CONST_REF(b) ? &ctx->ir_base[b] : &_values[b];

	return v1->val.u64 == v2->val.u64;
}

static void ir_sccp_remove_from_use_list(ir_ctx *ctx, ir_ref from, ir_ref ref)
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
#if IR_COMBO_COPY_PROPAGATION
	if (skip) {
		do {
			*q = IR_UNUSED;
			q++;
		} while (--skip);
	}
#endif
}

#if IR_COMBO_COPY_PROPAGATION
static int ir_sccp_add_to_use_list(ir_ctx *ctx, ir_ref to, ir_ref ref)
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
#endif

static void ir_sccp_make_nop(ir_ctx *ctx, ir_ref ref)
{
	ir_ref j, n, *p;
	ir_use_list *use_list = &ctx->use_lists[ref];
	ir_insn *insn;

	use_list->refs = 0;
	use_list->count = 0;

	insn = &ctx->ir_base[ref];
	n = ir_input_edges_count(ctx, insn);
	insn->optx = IR_NOP;
	for (j = 1, p = insn->ops + j; j <= n; j++, p++) {
		*p = IR_UNUSED;
	}
}

static void ir_sccp_replace_insn(ir_ctx *ctx, ir_insn *_values, ir_ref ref, ir_ref new_ref, ir_bitqueue *worklist)
{
	ir_ref j, n, *p, use, k, l;
	ir_insn *insn;
	ir_use_list *use_list;

	IR_ASSERT(ref != new_ref);

	insn = &ctx->ir_base[ref];
	n = ir_input_edges_count(ctx, insn);
	for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
		ir_ref input = *p;
		if (input > 0) {
			ir_sccp_remove_from_use_list(ctx, input, ref);
			/* schedule DCE */
			if (worklist
			 && ((IR_IS_FOLDABLE_OP(ctx->ir_base[input].op) && ctx->use_lists[input].count == 0)
			  || ((ir_op_flags[ctx->ir_base[input].op] & (IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_MASK)) == (IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_LOAD)
					&& ctx->use_lists[input].count == 1))) {
				ir_bitqueue_add(worklist, input);
			}
		}
	}

	use_list = &ctx->use_lists[ref];
	n = use_list->count;
	for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
		use = *p;
		if (IR_IS_REACHABLE(use)) {
			insn = &ctx->ir_base[use];
			l = ir_input_edges_count(ctx, insn);
			for (k = 1; k <= l; k++) {
				if (ir_insn_op(insn, k) == ref) {
					ir_insn_set_op(insn, k, new_ref);
				}
			}
#if IR_COMBO_COPY_PROPAGATION
			if (new_ref > 0 && IR_IS_BOTTOM(use)) {
				if (ir_sccp_add_to_use_list(ctx, new_ref, use)) {
					/* restore after reallocation */
					use_list = &ctx->use_lists[ref];
					n = use_list->count;
					p = &ctx->use_edges[use_list->refs + j];
				}
			}
#endif
			/* schedule folding */
			if (worklist && _values[use].op == IR_BOTTOM) {
				ir_bitqueue_add(worklist, use);
			}
		}
	}

	ir_sccp_make_nop(ctx, ref);
}

static void ir_sccp_fold2(ir_ctx *ctx, ir_insn *_values, ir_ref ref, ir_bitqueue *worklist)
{
	uint32_t opt;
	ir_ref op1, op2, op3;
	ir_insn *op1_insn, *op2_insn, *op3_insn, *insn;

	insn = &ctx->ir_base[ref];
	opt = insn->opt;
	op1 = insn->op1;
	op2 = insn->op2;
	op3 = insn->op3;

restart:
	op1_insn = ctx->ir_base + op1;
	op2_insn = ctx->ir_base + op2;
	op3_insn = ctx->ir_base + op3;

	switch (ir_folding(ctx, opt, op1, op2, op3, op1_insn, op2_insn, op3_insn)) {
		case IR_FOLD_DO_RESTART:
			opt = ctx->fold_insn.optx;
			op1 = ctx->fold_insn.op1;
			op2 = ctx->fold_insn.op2;
			op3 = ctx->fold_insn.op3;
			goto restart;
		case IR_FOLD_DO_EMIT:
			insn = &ctx->ir_base[ref];
			if (insn->opt != ctx->fold_insn.opt
			 || insn->op1 != ctx->fold_insn.op1
			 || insn->op2 != ctx->fold_insn.op2
			 || insn->op3 != ctx->fold_insn.op3) {

				ir_use_list *use_list;
				ir_ref n, j, *p, use;

				insn->optx = ctx->fold_insn.opt;
				insn->op1 = ctx->fold_insn.op1;
				insn->op2 = ctx->fold_insn.op2;
				insn->op3 = ctx->fold_insn.op3;

				use_list = &ctx->use_lists[ref];
				n = use_list->count;
				for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
					use = *p;
					if (IR_IS_REACHABLE(use)) {
						if (worklist && _values[use].op == IR_BOTTOM) {
							ir_bitqueue_add(worklist, use);
						}
					}
				}
			}
			break;
		case IR_FOLD_DO_COPY:
			op1 = ctx->fold_insn.op1;
			ir_sccp_replace_insn(ctx, _values, ref, op1, worklist);
			break;
		case IR_FOLD_DO_CONST:
			op1 = ir_const(ctx, ctx->fold_insn.val, ctx->fold_insn.type);
			ir_sccp_replace_insn(ctx, _values, ref, op1, worklist);
			break;
		default:
			IR_ASSERT(0);
			break;
	}
}

static void ir_sccp_replace_use(ir_ctx *ctx, ir_ref ref, ir_ref use, ir_ref new_use)
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

static void ir_sccp_remove_if(ir_ctx *ctx, ir_insn *_values, ir_ref ref, ir_ref dst)
{
	ir_ref j, n, *p, use, next;
	ir_insn *insn, *next_insn;
	ir_use_list *use_list = &ctx->use_lists[ref];

	insn = &ctx->ir_base[ref];
	n = use_list->count;
	for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
		use = *p;
		if (use == dst) {
			next = ctx->use_edges[ctx->use_lists[use].refs];
			next_insn = &ctx->ir_base[next];
			/* remove IF and IF_TRUE/FALSE from double linked control list */
			next_insn->op1 = insn->op1;
			ir_sccp_replace_use(ctx, insn->op1, ref, next);
			/* remove IF and IF_TRUE/FALSE instructions */
			ir_sccp_make_nop(ctx, ref);
			ir_sccp_make_nop(ctx, use);
			break;
		}
	}
}

static void ir_sccp_remove_unreachable_merge_inputs(ir_ctx *ctx, ir_insn *_values, ir_ref ref, ir_ref unreachable_inputs)
{
	ir_ref i, j, n, k, *p, use;
	ir_insn *insn, *use_insn;
	ir_use_list *use_list;
	ir_bitset life_inputs;

	insn = &ctx->ir_base[ref];
	IR_ASSERT(insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN);
	n = insn->inputs_count;
	if (n == 0) {
		n = 2;
	}
	if (n - unreachable_inputs == 1) {
		/* remove MERGE completely */
		for (j = 1; j <= n; j++) {
			if (ir_insn_op(insn, j) && IR_IS_REACHABLE(ir_insn_op(insn, j))) {
				ir_ref prev, next = IR_UNUSED, input = ir_insn_op(insn, j);
				ir_insn *next_insn = NULL, *input_insn = &ctx->ir_base[input];

				IR_ASSERT(input_insn->op == IR_END || input_insn->op == IR_IJMP || input_insn->op == IR_UNREACHABLE);
				prev = input_insn->op1;
				use_list = &ctx->use_lists[ref];
				for (k = 0, p = &ctx->use_edges[use_list->refs]; k < use_list->count; k++, p++) {
					use = *p;
					use_insn = &ctx->ir_base[use];
					IR_ASSERT((use_insn->op != IR_PHI) && "PHI must be already removed");
					if (ir_op_flags[use_insn->op] & IR_OP_FLAG_CONTROL) {
						next = use;
						next_insn = use_insn;
						break;
					}
				}
				IR_ASSERT(prev && next);
				/* remove MERGE and input END from double linked control list */
				next_insn->op1 = prev;
				ir_sccp_replace_use(ctx, prev, input, next);
				/* remove MERGE and input END instructions */
				ir_sccp_make_nop(ctx, ref);
				ir_sccp_make_nop(ctx, input);
				break;
			}
		}
	} else {
		n = insn->inputs_count;
		if (n == 0) {
			n = 3;
		}
		i = 1;
		life_inputs = ir_bitset_malloc(n + 1);
		for (j = 1; j <= n; j++) {
			if (ir_insn_op(insn, j)) {
				if (i != j) {
					ir_insn_set_op(insn, i, ir_insn_op(insn, j));
				}
				ir_bitset_incl(life_inputs, j);
				i++;
			}
		}
		i--;
		if (i == 2) {
			i = 0;
		}
		insn->inputs_count = i;

		n++;
		use_list = &ctx->use_lists[ref];
		for (k = 0, p = &ctx->use_edges[use_list->refs]; k < use_list->count; k++, p++) {
			use = *p;
			use_insn = &ctx->ir_base[use];
			if (use_insn->op == IR_PHI) {
			    i = 2;
				for (j = 2; j <= n; j++) {
					if (ir_bitset_in(life_inputs, j - 1)) {
						IR_ASSERT(ir_insn_op(use_insn, j));
						if (i != j) {
							ir_insn_set_op(use_insn, i, ir_insn_op(use_insn, j));
						}
						i++;
					} else {
						IR_ASSERT(ir_insn_op(use_insn, j) <= 0);
					}
				}
			}
		}
		ir_mem_free(life_inputs);
	}
}

static void ir_sccp_mark_reachable_data(ir_ctx *ctx, ir_bitqueue *worklist, ir_insn *_values, ir_insn *insn)
{
	int j, n, use;
	uint32_t flags = ir_op_flags[insn->op];

	n = ir_input_edges_count(ctx, insn);
	for (j = 1; j <= n; j++) {
		if (IR_OPND_KIND(flags, j) == IR_OPND_DATA || IR_OPND_KIND(flags, j) == IR_OPND_VAR) {
			use = ir_insn_op(insn, j);
			if (use > 0 && IR_IS_TOP(use) && !ir_bitqueue_in(worklist, use)) {
				ir_bitqueue_add(worklist, use);
				ir_sccp_mark_reachable_data(ctx, worklist, _values, &ctx->ir_base[use]);
			}
		}
	}
}

int ir_sccp(ir_ctx *ctx)
{
	ir_ref i, j, n, *p, use;
	ir_use_list *use_list;
	ir_insn *insn, *use_insn;
	uint32_t flags;
	ir_bitqueue worklist;
	ir_insn *_values = ir_mem_calloc(ctx->insns_count, sizeof(ir_insn));

	ctx->flags |= IR_OPT_IN_SCCP;

	/* A bit modified SCCP algorith of M. N. Wegman and F. K. Zadeck */
	ir_bitqueue_init(&worklist, ctx->insns_count);
	worklist.pos = 0;
	ir_bitset_incl(worklist.set, 1);
	i = ctx->ir_base[1].op2;
	while (i) {
		ir_bitset_incl(worklist.set, i);
		i = ctx->ir_base[i].op2;
	}
	while ((i = ir_bitqueue_pop(&worklist)) >= 0) {
		insn = &ctx->ir_base[i];
		flags = ir_op_flags[insn->op];
		if (flags & IR_OP_FLAG_DATA) {
			if (insn->op == IR_PHI) {
				ir_insn *merge_insn = &ctx->ir_base[insn->op1];
				bool changed = 0;

				n = ir_input_edges_count(ctx, insn);
				if (IR_IS_TOP(i)) {
					for (j = 0; j < (n>>2); j++) {
						_values[i+j+1].optx = IR_BOTTOM; /* keep the tail of a long multislot instruction */
					}
				}
				for (j = 1; j < n; j++) {
					if (ir_insn_op(merge_insn, j) && IR_IS_REACHABLE(ir_insn_op(merge_insn, j))) {
						if (IR_IS_TOP(ir_insn_op(insn, j + 1))) {
							ir_bitqueue_add(&worklist, ir_insn_op(insn, j +1));
						} else if (ir_sccp_join_values(ctx, _values, i, ir_insn_op(insn, j + 1))) {
							changed = 1;
						}
					}
				}
				if (!changed) {
					continue;
				}
			} else if (IR_IS_FOLDABLE_OP(insn->op)) {
				bool may_benefit = 0;
				bool has_top = 0;
				n = ir_input_edges_count(ctx, insn);
				for (j = 1; j <= n; j++) {
					ir_ref input = ir_insn_op(insn, j);
					if (!IR_IS_CONST_REF(input)) {
						if (_values[input].optx == IR_TOP) {
							has_top = 1;
							ir_bitqueue_add(&worklist, ir_insn_op(insn, j));
						} else if (_values[input].optx != IR_BOTTOM) {
							/* Perform folding only if some of direct inputs
							 * is going to be replaced by a constant or copy.
							 * This approach may miss some folding optimizations
							 * dependent on indirect inputs. e.g reassociation.
							 */
							may_benefit = 1;
						}
					}
				}
				if (has_top) {
					continue;
				}
				if (!may_benefit) {
					IR_MAKE_BOTTOM(i);
				} else if (!ir_sccp_fold(ctx, _values, i, insn->opt, insn->op1, insn->op2, insn->op3)) {
					/* not changed */
					continue;
				}
			} else {
				IR_MAKE_BOTTOM(i);
			}
		} else {
			if (insn->op == IR_IF) {
				if (IR_IS_TOP(insn->op2)) {
					ir_bitqueue_add(&worklist, insn->op2);
					continue;
				}
				if (!IR_IS_BOTTOM(insn->op2)) {
					bool b = ir_sccp_is_true(ctx, _values, insn->op2);
					use_list = &ctx->use_lists[i];
					IR_ASSERT(use_list->count == 2);
					p = &ctx->use_edges[use_list->refs];
					use = *p;
					use_insn = &ctx->ir_base[use];
					IR_ASSERT(use_insn->op == IR_IF_TRUE || use_insn->op == IR_IF_FALSE);
					if ((use_insn->op == IR_IF_TRUE) != b) {
						use = *(p+1);
						use_insn = &ctx->ir_base[use];
						IR_ASSERT(use_insn->op == IR_IF_TRUE || use_insn->op == IR_IF_FALSE);
					}
					if (IR_IS_TOP(i)) {
						_values[i].optx = IR_IF;
						_values[i].op1 = use;
					} else if (_values[i].optx != IR_IF || _values[i].op1 != use) {
						IR_MAKE_BOTTOM(i);
					}
					if (!IR_IS_BOTTOM(use)) {
						ir_bitqueue_add(&worklist, use);
					}
					continue;
				}
				IR_MAKE_BOTTOM(i);
			} else if (insn->op == IR_SWITCH) {
				if (IR_IS_TOP(insn->op2)) {
					ir_bitqueue_add(&worklist, insn->op2);
					continue;
				}
				if (!IR_IS_BOTTOM(insn->op2)) {
					ir_ref default_case = IR_UNUSED;

					use_list = &ctx->use_lists[i];
					n = use_list->count;
					for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
						use = *p;
						IR_ASSERT(use > 0);
						use_insn = &ctx->ir_base[use];
						if (use_insn->op == IR_CASE_VAL) {
							if (ir_sccp_is_equal(ctx, _values, insn->op2, use_insn->op2)) {
								if (IR_IS_TOP(i)) {
									_values[i].optx = IR_IF;
									_values[i].op1 = use;
								} else if (_values[i].optx != IR_IF || _values[i].op1 != use) {
									IR_MAKE_BOTTOM(i);
								}
								if (!IR_IS_BOTTOM(use)) {
									ir_bitqueue_add(&worklist, use);
								}
								default_case = IR_UNUSED;
								break;
							}
						} else if (use_insn->op == IR_CASE_DEFAULT) {
							default_case = use;
						}
					}
					if (default_case) {
						use_insn = &ctx->ir_base[default_case];
						if (IR_IS_TOP(i)) {
							_values[i].optx = IR_IF;
							_values[i].op1 = default_case;
						} else if (_values[i].optx != IR_IF || _values[i].op1 != default_case) {
							IR_MAKE_BOTTOM(i);
						}
						if (!IR_IS_BOTTOM(default_case)) {
							ir_bitqueue_add(&worklist, default_case);
						}
					}
					if (!IR_IS_BOTTOM(i)) {
						continue;
					}
				}
				IR_MAKE_BOTTOM(i);
			} else if (insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) {
				ir_ref unreachable_inputs = 0;

				n = ir_input_edges_count(ctx, insn);
				if (IR_IS_TOP(i)) {
					for (j = 0; j < (n>>2); j++) {
						_values[i+j+1].optx = IR_BOTTOM; /* keep the tail of a long multislot instruction */
					}
				}
				for (j = 1; j <= n; j++) {
					if (ir_insn_op(insn, j)) {
						if (!IR_IS_REACHABLE(ir_insn_op(insn, j))) {
							unreachable_inputs++;
						}
					}
				}
				if (unreachable_inputs == 0) {
					IR_MAKE_BOTTOM(i);
				} else if (_values[i].op1 != unreachable_inputs) {
					_values[i].optx = insn->op;
					_values[i].op1 = unreachable_inputs;
				} else {
					continue;
				}
			} else if ((flags & (IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_MASK)) == (IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_LOAD)
					&& ctx->use_lists[i].count == 1) {
				/* dead load */
				_values[i].optx = IR_LOAD;
			} else {
				IR_MAKE_BOTTOM(i);

				if (insn->op == IR_CALL || insn->op == IR_TAILCALL || insn->op == IR_SNAPSHOT) {
					n = ir_input_edges_count(ctx, insn);
					for (j = 0; j < (n>>2); j++) {
						_values[i+j+1].optx = IR_BOTTOM; /* keep the tail of a long multislot instruction */
					}
				}

				flags = ir_op_flags[insn->op];
				n = ir_input_edges_count(ctx, insn);
				/* first input is source control (we may skip it) */
				IR_ASSERT(n == 0 || IR_OPND_KIND(flags, 1) == IR_OPND_CONTROL);
				for (j = 2; j <= n; j++) {
					if (IR_OPND_KIND(flags, j) == IR_OPND_DATA || IR_OPND_KIND(flags, j) == IR_OPND_VAR) {
						use = ir_insn_op(insn, j);
						if (use > 0 && IR_IS_TOP(use) && !ir_bitqueue_in(&worklist, use)) {
							ir_bitqueue_add(&worklist, use);
							ir_sccp_mark_reachable_data(ctx, &worklist, _values, &ctx->ir_base[use]);
						}
					}
				}
//				ir_sccp_mark_reachable_data(ctx, &worklist, _values, insn);
			}
		}
		use_list = &ctx->use_lists[i];
		n = use_list->count;
		for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
			use = *p;
			insn = &ctx->ir_base[use];
			if (ir_op_flags[insn->op] & IR_OP_FLAG_DATA) {
				if (insn->op != IR_PHI || IR_IS_REACHABLE(insn->op1)) {
					if (!IR_IS_BOTTOM(use)) {
						ir_bitqueue_add(&worklist, use);
					}
				}
			} else if (IR_IS_REACHABLE(insn->op1) || insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) {
				if (!IR_IS_BOTTOM(use)) {
					ir_bitqueue_add(&worklist, use);
				}
			}
		}
	}

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_SCCP) {
		for (i = 1; i < ctx->insns_count; i++) {
			if (IR_IS_CONST_OP(_values[i].op)) {
				fprintf(stderr, "%d. CONST(", i);
				ir_print_const(ctx, &_values[i], stderr);
				fprintf(stderr, ")\n");
#if IR_COMBO_COPY_PROPAGATION
			} else if (_values[i].op == IR_COPY) {
				fprintf(stderr, "%d. COPY(%d)\n", i, _values[i].op1);
#endif
			} else if (IR_IS_TOP(i)) {
				fprintf(stderr, "%d. TOP\n", i);
			} else if (_values[i].op == IR_IF) {
				fprintf(stderr, "%d. IF(%d)\n", i, _values[i].op1);
			} else if (_values[i].op == IR_MERGE || _values[i].op == IR_LOOP_BEGIN) {
				fprintf(stderr, "%d. MERGE(%d)\n", i, _values[i].op1);
			} else if (!IR_IS_BOTTOM(i)) {
				fprintf(stderr, "%d. %d\n", i, _values[i].op);
			}
		}
    }
#endif

	for (i = 1; i < ctx->insns_count; i++) {
		if (_values[i].op == IR_BOTTOM) {
			continue;
		} else if (IR_IS_CONST_OP(_values[i].op)) {
			/* replace instruction by constant */
			j = ir_const(ctx, _values[i].val, _values[i].type);
			ir_sccp_replace_insn(ctx, _values, i, j, &worklist);
#if IR_COMBO_COPY_PROPAGATION
		} else if (_values[i].op == IR_COPY) {
			ir_sccp_replace_insn(ctx, _values, i, _values[i].op1, &worklist);
#endif
		} else if (_values[i].op == IR_TOP) {
			/* remove unreachable instruction */
			insn = &ctx->ir_base[i];
			if (ir_op_flags[insn->op] & (IR_OP_FLAG_DATA|IR_OP_FLAG_MEM)) {
				if (insn->op != IR_PARAM && insn->op != IR_VAR) {
					ir_sccp_make_nop(ctx, i);
				}
			} else {
				if (ir_op_flags[insn->op] & IR_OP_FLAG_TERMINATOR) {
					/* remove from terminators list */
					ir_ref prev = ctx->ir_base[1].op1;
					if (prev == i) {
						ctx->ir_base[1].op1 = insn->op3;
					} else {
						while (prev) {
							if (ctx->ir_base[prev].op3 == i) {
								ctx->ir_base[prev].op3 = insn->op3;
								break;
							}
							prev = ctx->ir_base[prev].op3;
						}
					}
				}
				ir_sccp_replace_insn(ctx, _values, i, IR_UNUSED, NULL);
			}
		} else if (_values[i].op == IR_IF) {
			/* remove one way IF/SWITCH */
			ir_sccp_remove_if(ctx, _values, i, _values[i].op1);
		} else if (_values[i].op == IR_MERGE || _values[i].op == IR_LOOP_BEGIN) {
			/* schedule merge to remove unreachable MERGE inputs */
			ir_bitqueue_add(&worklist, i);
		} else if (_values[i].op == IR_LOAD) {
			/* schedule dead load elimination */
			ir_bitqueue_add(&worklist, i);
		}
	}

	while ((i = ir_bitqueue_pop(&worklist)) >= 0) {
		if (_values[i].op == IR_MERGE || _values[i].op == IR_LOOP_BEGIN) {
			/* remove unreachable MERGE inputs */
			ir_sccp_remove_unreachable_merge_inputs(ctx, _values, i, _values[i].op1);
		} else {
			insn = &ctx->ir_base[i];
			if (IR_IS_FOLDABLE_OP(insn->op)) {
				if (ctx->use_lists[i].count == 0) {
					ir_sccp_replace_insn(ctx, _values, i, IR_UNUSED, &worklist);
				} else {
					ir_sccp_fold2(ctx, _values, i, &worklist);
				}
			} else if ((ir_op_flags[insn->op] & (IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_MASK)) == (IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_LOAD)
					&& ctx->use_lists[i].count == 1) {
				/* dead load */
				ir_ref next = ctx->use_edges[ctx->use_lists[i].refs];

				/* remove LOAD from double linked control list */
				ctx->ir_base[next].op1 = insn->op1;
				ir_sccp_replace_use(ctx, insn->op1, i, next);
				insn->op1 = IR_UNUSED;
				ir_sccp_replace_insn(ctx, _values, i, IR_UNUSED, &worklist);
			}
		}
	}

	ir_mem_free(_values);
	ir_bitqueue_free(&worklist);

	ctx->flags &= ~IR_OPT_IN_SCCP;

	return 1;
}
