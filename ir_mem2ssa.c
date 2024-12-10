/*
 * IR - Lightweight JIT Compilation Framework
 * (MEM2SSA - Static Single Assignment Form Construction for VAR/ALLOCA)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 *
 * See: Matthias Braun, Sebastian Buchwald, Sebastian Hack, Roland Leißa,
 * Christoph Mallon, Andreas Zwinkau "Simple and Efficient Construction
 * of Static Single Assignment Form", Jhala, Ranjit and Bosschere, Koen (Ed.),
 * Compiler Construction, pp. 102--122, Springer Berlin Heidelberg, 2013
 */

#include "ir.h"
#include "ir_private.h"

static void ir_replace_insn(ir_ctx *ctx, ir_ref ref, ir_ref new_ref)
{
	ir_ref j, n, use, k, l;
	ir_insn *insn;

	IR_ASSERT(ref != new_ref);

	n = ctx->use_lists[ref].count;
	for (j = 0; j < n; j++) {
		use = ctx->use_edges[ctx->use_lists[ref].refs + j];
		insn = &ctx->ir_base[use];
		l = insn->inputs_count;
		for (k = 1; k <= l; k++) {
			if (ir_insn_op(insn, k) == ref) {
				ir_insn_set_op(insn, k, new_ref);
				if (!IR_IS_CONST_REF(new_ref)) {
					ir_use_list_add(ctx, new_ref, use);
				}
			}
		}
	}
}

static void ir_ssa_set_var(ir_ctx *ctx, ir_ref *ssa_vars, ir_ref var, ir_ref ref, ir_ref val)
{
	ssa_vars[ref] = val;
}

static ir_ref ir_ssa_get_var(ir_ctx *ctx, ir_ref *ssa_vars, ir_ref var, ir_ref ref)
{
	ir_ref ctrl, val;
	ir_insn *ctrl_insn;
	ir_type type;

	ctrl = ref;
	while (1) {
		val = ssa_vars[ctrl];
		if (val) {
			ir_ssa_set_var(ctx, ssa_vars, var, ref, val);
			return val;
		}
		ctrl_insn = &ctx->ir_base[ctrl];
		if ((ctrl_insn->op == IR_VSTORE || ctrl_insn->op == IR_STORE)
		 && ctrl_insn->op2 == var) {
			val = ctrl_insn->op3;
			if (!IR_IS_CONST_REF(val) && ssa_vars[val]) {
				val = ssa_vars[val];
			}
			return val;
		} else if (IR_IS_BB_START(ctrl_insn->op)) {
			break;
		}
		ctrl = ctrl_insn->op1;
	}

	if (ctrl_insn->op == IR_MERGE || ctrl_insn->op == IR_LOOP_BEGIN) {
		uint32_t i, n = ctrl_insn->inputs_count;

		if (ctx->ir_base[var].op == IR_VAR) {
			type = ctx->ir_base[var].type;
		} else if (ctx->ir_base[ref].op == IR_LOAD) {
			type = ctx->ir_base[var].type;
		} else {
			IR_ASSERT(0);
			type = IR_VOID;
		}
		val = ir_emit_N(ctx, IR_OPT(IR_PHI, type), n + 1);
		ir_set_op(ctx, val, 1, ctrl);
		ir_use_list_add(ctx, ctrl, val);
		ir_ssa_set_var(ctx, ssa_vars, var, ctrl, val);
		ir_ssa_set_var(ctx, ssa_vars, var, ref, val);
		for (i = 1; i <= n; i++) {
			ir_ref end = ir_get_op(ctx, ctrl, i);
			IR_ASSERT(end);
			ir_ref op = ir_ssa_get_var(ctx, ssa_vars, var, end);
			ir_set_op(ctx, val, i + 1, op);
			if (!IR_IS_CONST_REF(op)) {
				ir_use_list_add(ctx, op, val);
			}
		}
//		val = jit_ssa_try_remove_trivial_phi(jit, val);
	} else if (ctrl_insn->op != IR_START && ctrl_insn->op1) {
		val = ir_ssa_get_var(ctx, ssa_vars, var, ctrl_insn->op1);
	} else {
		/* read of uninitialized variable (use 0) */
		ir_val c;

		if (ctx->ir_base[var].op == IR_VAR) {
			type = ctx->ir_base[var].type;
		} else if (ctx->ir_base[ref].op == IR_LOAD) {
			type = ctx->ir_base[ref].type;
		} else {
			IR_ASSERT(0);
			type = IR_VOID;
		}
		c.i64 = 0;
		val = ir_const(ctx, c, type);
	}
	ir_ssa_set_var(ctx, ssa_vars, var, ref, val);
	return val;
}

static void ir_mem2ssa_convert(ir_ctx *ctx, ir_ref *ssa_vars, ir_ref var)
{
	ir_ref i, n, use;
	ir_insn *use_insn;

	/* Remove STROREs */
	n = ctx->use_lists[var].count;
	for (i = 0; i < n; i++) {
		use = ctx->use_edges[ctx->use_lists[var].refs + i];
		use_insn = &ctx->ir_base[use];
		if (use_insn->op == IR_VSTORE || use_insn->op == IR_STORE) {
			/*
			 *  prev VAR val      prev VAR val
			 *      \ |  /         |
			 *      VSTORE    =>   |
			 *      /              |
			 *  next              next
			 *
			 */

			ir_ref prev = use_insn->op1;
			ir_ref next = ir_next_control(ctx, use);
			ctx->ir_base[next].op1 = prev;
			ir_use_list_remove_one(ctx, use, prev);
		    ir_use_list_replace_one(ctx, prev, use, next);
			if (!IR_IS_CONST_REF(use_insn->op3)) {
				ir_use_list_remove_one(ctx, use_insn->op3, use);
			}

			ir_ssa_set_var(ctx, ssa_vars, var, next, use_insn->op3);

			MAKE_NOP(use_insn);
			CLEAR_USES(use);
		} else if (use_insn->op == IR_VLOAD || use_insn->op == IR_LOAD) {
			/*
			 *  prev    VAR      prev   ssa_val
			 *      \  /          |     |
			 *      VLOAD    =>   |     |
			 *      /  \          |     |
			 *  next    use      next   use
			 *
			 */

			ir_ref prev = use_insn->op1;
			ir_ref next = ir_next_control(ctx, use);
			ctx->ir_base[next].op1 = prev;
			ir_use_list_remove_one(ctx, use, prev);
			ir_use_list_replace_one(ctx, prev, use, next);

			ir_ref val = ir_ssa_get_var(ctx, ssa_vars, var, use);
			ir_replace_insn(ctx, use, val);

			use_insn = &ctx->ir_base[use];
			MAKE_NOP(use_insn);
			CLEAR_USES(use);
		}
	}
}

static bool ir_mem2ssa_may_convert_alloca(ir_ctx *ctx, ir_ref var, ir_insn *insn)
{
	ir_ref n, *p, use;
	ir_insn *use_insn;
	ir_use_list *use_list;
	ir_type type;
	size_t size;

	if (!IR_IS_CONST_REF(insn->op2)) {
		return 0;
	}
	if (!(ctx->ir_base[insn->op2].type >= IR_U8 && ctx->ir_base[insn->op2].type >= IR_U64)
	 && !(ctx->ir_base[insn->op2].type >= IR_I8 && ctx->ir_base[insn->op2].type >= IR_I64)) {
		return 0;
	}

	size = ctx->ir_base[insn->op2].val.u64;
	if (size != 1 && size != 2 && size != 4 && size != 8 && size != sizeof(double)) {
		return 0;
	}

	use_list = &ctx->use_lists[var];
	n = use_list->count;
	if (!n) {
		return 0;
	}

	p = &ctx->use_edges[use_list->refs];
	use = *p;
	use_insn = &ctx->ir_base[use];
	if (use_insn->op == IR_LOAD) {
		type = use_insn->type;
		if (use_insn->op2 != var || ir_type_size[type] != size) {
			return 0;
		}
	} else if (use_insn->op == IR_STORE) {
		type = ctx->ir_base[use_insn->op3].type;
		if (use_insn->op2 != var || use_insn->op3 == var || ir_type_size[type] != size) {
			return 0;
		}
	} else {
		return 0;
	}

	while (--n) {
		p++;
		use = *p;
		use_insn = &ctx->ir_base[use];
		if (use_insn->op == IR_LOAD) {
			if (use_insn->op2 != var || use_insn->type != type) {
				return 0;
			}
		} else if (use_insn->op == IR_STORE) {
			if (use_insn->op2 != var || use_insn->op3 == var || ctx->ir_base[use_insn->op3].type != type) {
				return 0;
			}
		} else {
			return 0;
		}
	}

	return 1;
}

static bool ir_mem2ssa_may_convert_var(ir_ctx *ctx, ir_ref var, ir_insn *insn)
{
	ir_ref n, *p, use;
	ir_insn *use_insn;
	ir_use_list *use_list;
	ir_type type;

	use_list = &ctx->use_lists[var];
	n = use_list->count;
	if (!n) {
		return 0;
	}

	p = &ctx->use_edges[use_list->refs];
	type = insn->type;
	do {
		use = *p;
		use_insn = &ctx->ir_base[use];
		if (use_insn->op == IR_VLOAD) {
			if (use_insn->op2 != var || use_insn->type != type) {
				return 0;
			}
		} else if (use_insn->op == IR_VSTORE) {
			if (use_insn->op2 != var || use_insn->op3 == var || ctx->ir_base[use_insn->op3].type != type) {
				return 0;
			}
		} else {
			return 0;
		}
		p++;
	} while (--n > 0);

	return 1;
}

int ir_mem2ssa(ir_ctx *ctx)
{
	ir_ref i, n;
	ir_insn *insn;
	ir_ref *ssa_vars = NULL;
	size_t ssa_vars_len = 0;

	IR_ASSERT(ctx->use_lists);

	for (i = IR_UNUSED + 1; i < ctx->insns_count;) {
		insn = &ctx->ir_base[i];
		if (insn->op == IR_ALLOCA) {
			if (ir_mem2ssa_may_convert_alloca(ctx, i, insn)) {
				if (!ssa_vars) {
					ssa_vars_len = ctx->insns_count * 2;
					ssa_vars = ir_mem_calloc(ssa_vars_len, sizeof(ir_ref));
				} else {
					memset(ssa_vars, 0, ssa_vars_len * sizeof(ir_ref));
				}

				ir_mem2ssa_convert(ctx, ssa_vars, i);

				/* remove from control list */
				insn = &ctx->ir_base[i];
				ir_ref prev = insn->op1;
				ir_ref next = ir_next_control(ctx, i);
				ctx->ir_base[next].op1 = prev;
				ir_use_list_replace_one(ctx, prev, i, next);

				MAKE_NOP(insn);
				CLEAR_USES(i);
			}
		} else if (insn->op == IR_VAR) {
			if (ir_mem2ssa_may_convert_var(ctx, i, insn)) {
				if (!ssa_vars) {
					ssa_vars_len = ctx->insns_count * 2;
					ssa_vars = ir_mem_calloc(ssa_vars_len, sizeof(ir_ref));
				} else {
					memset(ssa_vars, 0, ssa_vars_len * sizeof(ir_ref));
				}

				ir_mem2ssa_convert(ctx, ssa_vars, i);

				insn = &ctx->ir_base[i];
				ir_use_list_remove_one(ctx, insn->op1, i);
				MAKE_NOP(insn);
				CLEAR_USES(i);
			}
		}
		n = ir_insn_len(insn);
		i += n;
	}

	if (ssa_vars) {
		ir_mem_free(ssa_vars);
	}

	return 1;
}
