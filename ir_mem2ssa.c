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
		IR_ASSERT(use != ref);
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

static void ir_replace_phi_insn(ir_ctx *ctx, ir_ref ref, ir_ref new_ref)
{
	ir_ref j, n, use, k, l;
	ir_insn *insn;

	IR_ASSERT(ref != new_ref);

	n = ctx->use_lists[ref].count;
	for (j = 0; j < n; j++) {
		use = ctx->use_edges[ctx->use_lists[ref].refs + j];
		if (use == ref) {
			continue;
		}
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

static ir_ref ir_uninitialized(ir_ctx *ctx, ir_type type)
{
	/* read of uninitialized variable (use 0) */
	ir_val c;

	c.i64 = 0;
	return ir_const(ctx, c, type);
}

static void ir_ssa_set_var(ir_ctx *ctx, ir_ref *ssa_vars, ir_ref var, ir_ref ref, ir_ref val)
{
	ssa_vars[ref] = val;
}

static ir_ref ir_ssa_try_remove_trivial_phi(ir_ctx *ctx, ir_ref *ssa_vars, ir_ref var, ir_ref ref)
{
	ir_insn *insn = &ctx->ir_base[ref];
	ir_ref n, *p, op, same = IR_UNUSED;

	IR_ASSERT(insn->op == IR_PHI);
	n = insn->inputs_count - 1;
	p = insn->ops + 2;
	for (; n > 0; p++, n--) {
		op = *p;
		if (op == same || op == ref) {
			continue;
		}
		if (same != IR_UNUSED) {
			return ref;
		}
		same = op;
	}

	IR_ASSERT(same != IR_UNUSED);
	ir_replace_phi_insn(ctx, ref, same);

	ir_use_list_remove_one(ctx, insn->op1, ref);
	if (!IR_IS_CONST_REF(same)) {
		ir_use_list_remove_all(ctx, same, ref);
	}

	n = insn->inputs_count;
//	if (ref + (int)ir_insn_inputs_to_len(n) == ctx->insns_count) {
//		ctx->insns_count = ref;
//	} else {
		p = insn->ops + 1;
		insn->optx = IR_NOP;
		for (; n > 0; p++, n--) {
			*p = IR_UNUSED;
		}
//	}
	CLEAR_USES(ref);

//	ir_ssa_set_var(ctx, ssa_vars, ref, var, same);

//	if (ctx->ir_base[same].op == IR_PHI) {
//		same = ir_ssa_try_remove_trivial_phi(ctx, same);
//	}

	return same;
}

static ir_ref ir_ssa_get_var(ir_ctx *ctx, ir_ref *ssa_vars, ir_ref var, ir_type type, ir_ref ref)
{
	ir_ref val;
	ir_insn *insn;

	IR_ASSERT(ref > 0);
	while (1) {
		val = ssa_vars[ref];
		if (val) {
//			while (!IR_IS_CONST_REF(val) && ssa_vars[val]) {
//				val = ssa_vars[val];
//			}
			return val;
		}
		insn = &ctx->ir_base[ref];
		if (insn->op == IR_VSTORE || insn->op == IR_STORE) {
			if (insn->op2 == var) {
				val = insn->op3;
//				while (!IR_IS_CONST_REF(val) && ssa_vars[val]) {
//					val = ssa_vars[val];
//				}
				return val;
			}
		} else if (insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) {
			uint32_t i, n = insn->inputs_count;

			val = ir_emit_N(ctx, IR_OPT(IR_PHI, type), n + 1);
			ir_set_op(ctx, val, 1, ref);
			ir_use_list_add(ctx, ref, val);
			ir_ssa_set_var(ctx, ssa_vars, var, ref, val);
			for (i = 1; i <= n; i++) {
				ir_ref end = ir_get_op(ctx, ref, i);
				IR_ASSERT(end);
				ir_ref op = ir_ssa_get_var(ctx, ssa_vars, var, type, end);
				ir_set_op(ctx, val, i + 1, op);
				if (!IR_IS_CONST_REF(op)) {
					ir_use_list_add(ctx, op, val);
				}
			}
//			val = ir_ssa_try_remove_trivial_phi(ctx, ssa_vars, var, val);
			ir_ssa_set_var(ctx, ssa_vars, var, ref, val);
			return val;
		} else if (insn->op == IR_START || !insn->op1) {
			/* read of uninitialized variable */
			return ir_uninitialized(ctx, type);
		}
		ref = insn->op1;
	}
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

			ir_ref val = ir_ssa_get_var(ctx, ssa_vars, var, use_insn->type, use);
			ir_ssa_set_var(ctx, ssa_vars, var, next, val);
			ir_replace_insn(ctx, use, val);

			use_insn = &ctx->ir_base[use];
			MAKE_NOP(use_insn);
			CLEAR_USES(use);
		}
	}
}

static ir_ref ir_mem2ssa_go_up(ir_ctx *ctx, ir_ref *live_in, ir_list *queue, ir_ref var, ir_ref ctrl)
{
	ir_insn *ctrl_insn = &ctx->ir_base[ctrl];

	while (1) {
		if (IR_IS_BB_START(ctrl_insn->op)) {
			if (live_in[ctrl] != var) {
				live_in[ctrl] = var;
				ir_list_push(queue, ctrl);
			}
			break;
		} else if ((ctrl_insn->op == IR_VSTORE || ctrl_insn->op == IR_STORE) && ctrl_insn->op2 == var) {
			break;
		}
		ctrl = ctrl_insn->op1;
		ctrl_insn = &ctx->ir_base[ctrl];
	}
	return ctrl;
}

static void ir_mem2ssa_convert2(ir_ctx *ctx, ir_ref *idom, ir_ref *live_in, ir_list *queue, ir_ref var, ir_type type)
{
	ir_ref *p, i, n, use, start, end;
	ir_insn *use_insn, *start_insn;
	uint32_t len = ir_bitset_len(ctx->insns_count);
	ir_bitset def = ir_mem_calloc(len * 2, IR_BITSET_BITS / 8);
	ir_bitset merges = def + len;

	/* For each usage of VAR */
	n = ctx->use_lists[var].count;
	for (p = ctx->use_edges + ctx->use_lists[var].refs; n > 0; p++, n--) {
		use = *p;
		use_insn = &ctx->ir_base[use];
		if (use_insn->op == IR_VSTORE || use_insn->op == IR_STORE) {
			IR_ASSERT(use_insn->op2 == var && use_insn->op3 != var);
			end = idom[use];
			/* Mark VAR as defined and alive at the end of BB */
			ir_bitset_incl(def, end);
			/* The value of the VAR at the end of the BB is defined by this STORE */
			live_in[use] = use_insn->op3;
			live_in[end] = use;
		} else {
			IR_ASSERT(use_insn->op == IR_VLOAD || use_insn->op == IR_LOAD);
			IR_ASSERT(use_insn->op2 == var);
			end = idom[use];
			if (!ir_bitset_in(def, end)) {
				live_in[use] = 0;
				start = idom[end];
				if (live_in[start] != var) {
					/* Mark VAR as alive at the start of BB */
					live_in[start] = var;
					ir_list_push(queue, end);
				}
			} else {
				/* Replace result of the LOAD by the value used in the last STORE from the same BB */
				live_in[use] = live_in[end];
			}
		}
	}

	/* Calculate BBs where VAR is alive at start exploring paths from "uses" to "definitions" */
	while (ir_list_len(queue)) {
		end = ir_list_pop(queue);
		start = idom[end];
		start_insn = &ctx->ir_base[start];
		if (start_insn->op == IR_MERGE || start_insn->op == IR_LOOP_BEGIN) {
			ir_bitset_incl(merges, end);
			n = start_insn->inputs_count;
			for (p = start_insn->ops + 1; n > 0; p++, n--) {
				end = *p; // BB_END
				if (!ir_bitset_in(def, end)) {
					start = idom[end];
					if (live_in[start] != var) {
						live_in[start] = var;
						ir_list_push(queue, end);
					}
				}
			}
		} else if (start_insn->op != IR_START && start_insn->op1) {
			end = start_insn->op1;
			if (!ir_bitset_in(def, end)) {
				start = idom[end];
				if (live_in[start] != var) {
					live_in[start] = var;
					ir_list_push(queue, end);
				}
			}
		}
	}

	/* Insert PHI nodes */
	bool changed;
	do {
		changed = 0;
		IR_BITSET_FOREACH(merges, len, end) {
			ir_ref dom;
			bool need_phi = 0;

			start = idom[end];
			live_in[start] = 0;
			dom = idom[start]; // dominator
			start_insn = &ctx->ir_base[start];
			IR_ASSERT(start_insn->op == IR_MERGE || start_insn->op == IR_LOOP_BEGIN);
			n = start_insn->inputs_count;
			for (p = start_insn->ops + 1; n > 0; p++, n--) {
				ir_ref runner = *p;
				while (runner != dom) {
					if (ir_bitset_in(def, runner)) {
						need_phi = 1;
						break;
					}
					runner = idom[idom[runner]];
					if (!runner) break;
				}
			}
			if (need_phi) {
//fprintf(stderr, "Add PHI for var d_%d at d_%d\n", var, start);
				ir_ref phi = ir_emit_N(ctx, IR_OPT(IR_PHI, type), start_insn->inputs_count + 1);
				ir_set_op(ctx, phi, 1, start);
				ir_use_list_add(ctx, start, phi);
				ir_bitset_excl(merges, end);
				ir_list_push(queue, phi);
				ir_bitset_incl(def, start);
				live_in[start] = phi;
				if (!ir_bitset_in(def, end)) {
					/* Mark VAR as defined and alive at the end of BB */
					ir_bitset_incl(def, end);
					live_in[end] = phi;
					changed = 1;
				}
			}
		} IR_BITSET_FOREACH_END();
	} while (changed);

	/* Renaming: remove STROREs and LOADs */
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

			ir_ref val;
			if (live_in[use]) {
				val = live_in[use];
				while (!IR_IS_CONST_REF(val) && ctx->ir_base[val].op != IR_PHI && live_in[val]) {
					val = live_in[val];
				}
			} else {
				end = idom[use];
				while (1) {
					end = idom[end];
					if (!end) {
						val = ir_uninitialized(ctx, type);
						break;
					} else if (ir_bitset_in(def, end)) {
						val = live_in[end];
						while (!IR_IS_CONST_REF(val) && ctx->ir_base[val].op != IR_PHI && live_in[val]) {
							val = live_in[val];
						}
						break;
				    }
				}
			}
			live_in[use] = val;
			ir_replace_insn(ctx, use, val);

			use_insn = &ctx->ir_base[use];
			MAKE_NOP(use_insn);
			CLEAR_USES(use);
		}
	}

	/* Renaming: set PHI operands */
	while (ir_list_len(queue)) {
		ir_ref phi = ir_list_pop(queue);
		ir_insn *phi_insn = &ctx->ir_base[phi];

		IR_ASSERT(phi_insn->op == IR_PHI);
		start = phi_insn->op1;
		start_insn = &ctx->ir_base[start];
		IR_ASSERT(start_insn->op == IR_MERGE || start_insn->op == IR_LOOP_BEGIN);
		n = start_insn->inputs_count;
		for (i = 0, p = start_insn->ops + 1; i < n; p++, i++) {
			ir_ref val;

			end = *p;
			while (1) {
				if (ir_bitset_in(def, end)) {
					val = live_in[end];
					while (!IR_IS_CONST_REF(val) && ctx->ir_base[val].op != IR_PHI && live_in[val]) {
						val = live_in[val];
					}
					break;
				}
				end = idom[end];
				if (!end) {
					val = ir_uninitialized(ctx, type);
					break;
			    }
			}
			ir_set_op(ctx, phi, i + 2, val);
			if (!IR_IS_CONST_REF(val)) {
				ir_use_list_add(ctx, val, phi);
			}
		}
	}

	ir_mem_free(def);
}

static bool ir_mem2ssa_may_convert_alloca(ir_ctx *ctx, ir_ref var, ir_insn *insn, ir_type *type_ptr)
{
	ir_ref n, *p, use;
	ir_insn *use_insn;
	ir_use_list *use_list;
	ir_type type = IR_VOID;
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

	*type_ptr = type;
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

/*
   idom[BB_END node] -> BB_START node of the same BB
   idom[BB_START node] -> BB_END node of immediate dominator
 */
static void ir_calc_idom_tree(ir_ctx *ctx, ir_ref *idom)
{
	ir_ref ref, end, start, n, *p;
	ir_insn *insn;
	ir_use_list *use_list;
	ir_worklist worklist;
	bool changed;
	uint32_t len = ir_bitset_len(ctx->insns_count);
	ir_bitset bb_starts = ir_mem_calloc(len * 3, IR_BITSET_BITS / 8);
	ir_bitset bb_leaks = bb_starts + len;
	ir_bitset merges = bb_leaks + len;

	ir_worklist_init(&worklist, ctx->insns_count);

	/* First try to perform backward DFS search starting from "stop" nodes */

	/* Add all "stop" nodes */
	ref = ctx->ir_base[1].op1;
	while (ref) {
		ir_worklist_push(&worklist, ref);
		ref = ctx->ir_base[ref].op3;
	}

	while (ir_worklist_len(&worklist)) {
		ref = ir_worklist_pop(&worklist);
		insn = &ctx->ir_base[ref];
		IR_ASSERT(IR_IS_BB_END(insn->op));
		/* Remember BB end */
		end = ref;
		/* Some successors of IF and SWITCH nodes may be inaccessible by backward DFS */
		use_list = &ctx->use_lists[end];
		n = use_list->count;
		if (n > 1 || (n == 1 && (ir_op_flags[insn->op] & IR_OP_FLAG_TERMINATOR) != 0)) {
			for (p = &ctx->use_edges[use_list->refs]; n > 0; p++, n--) {
				/* Remember possible inaccessible successors */
				ir_bitset_incl(bb_leaks, *p);
			}
		}

		/* Skip control nodes untill BB start */
		ref = insn->op1;
		while (1) {
			insn = &ctx->ir_base[ref];
			if (IR_IS_BB_START(insn->op)) {
				break;
			}
			idom[ref] = end; // link inner-block control nodes to BB_END
			ref = insn->op1; // follow connected control nodes untill BB start
		}
		idom[end] = ref; // link BB_END to BB_START
		/* Mark BB Start */
		ir_bitset_incl(bb_starts, ref);
		/* Add predecessors */
		if (insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) {
			ir_ref *p, n = insn->inputs_count;

			ir_bitset_incl(merges, ref); // find dominator later
			for (p = insn->ops + 1; n > 0; p++, n--) {
				ir_ref input = *p;
				IR_ASSERT(input);
				ir_worklist_push(&worklist, input);
			}
		} else if (insn->op != IR_START && EXPECTED(insn->op1 != IR_UNUSED)) {
			idom[ref] = insn->op1; // link BB_START to single predeessor BB_END (immediate dominator)
			ir_worklist_push(&worklist, insn->op1);
		}
	}

	/* Backward DFS way miss some branches ending by infinite loops.  */
	/* Try forward DFS. (in most cases all nodes are already proceed). */

	/* START node may be inaccessible from "stop" nodes */
	ir_bitset_incl(bb_leaks, 1);

	/* Add not processed START and successor of IF and SWITCH */
	IR_BITSET_FOREACH_DIFFERENCE(bb_leaks, bb_starts, len, start) {
		ir_worklist_push(&worklist, start);
	} IR_BITSET_FOREACH_END();

	if (ir_worklist_len(&worklist)) {
		ir_bitset_union(worklist.visited, bb_starts, len);
		do {
			ref = ir_worklist_pop(&worklist);
			insn = &ctx->ir_base[ref];

			IR_ASSERT(IR_IS_BB_START(insn->op));
			if (insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) {
				ir_bitset_incl(merges, ref); // find dominator later
			} else if (insn->op != IR_START && EXPECTED(insn->op1 != IR_UNUSED)) {
				idom[ref] = insn->op1; // link BB_START to single predeessor BB_END (immediate dominator)
			}
			/* Remember BB start */
			start = ref;
			/* Skip control nodes untill BB end */
			while (1) {
				ref = ir_next_control(ctx, ref);
				insn = &ctx->ir_base[ref];
				if (IR_IS_BB_END(insn->op)) {
					break;
				}
			}
			/* Mark BB Start */
			idom[ref] = start; // link BB_END to BB_START
			ir_bitset_incl(bb_starts, start);
			ir_ref t = ctx->ir_base[ref].op1;
			while (t != start) {
				idom[t] = ref; // link inner-block control nodes to BB_END
				t = ctx->ir_base[t].op1;
			}
			/* Add successors */
			use_list = &ctx->use_lists[ref];
			n = use_list->count;

			if (n < 2) {
				if (n == 1) {
					ir_ref use = ctx->use_edges[use_list->refs];
					IR_ASSERT(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL);
					ir_worklist_push(&worklist, use);
				}
			} else {
				p = &ctx->use_edges[use_list->refs];
				if (n == 2) {
					ir_ref use = *p;
					IR_ASSERT(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL);
					ir_worklist_push(&worklist, use);
					use = *(p + 1);
					IR_ASSERT(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL);
					ir_worklist_push(&worklist, use);
				} else {
					for (; n > 0; p++, n--) {
						ir_ref use = *p;
						IR_ASSERT(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL);
						ir_worklist_push(&worklist, use);
					}
				}
			}
		} while (ir_worklist_len(&worklist));
	}

	/* Iteratively find diminators of MERGE nodes */
	do {
		changed = 0;
		IR_BITSET_FOREACH(merges, len, ref) {
			ir_ref *p, n;
			ir_ref dom;

			dom = idom[ref];
			insn = &ctx->ir_base[ref];
			n = insn->inputs_count;
			for (p = insn->ops + 1; n > 0; p++, n--) {
				ir_ref input = *p;
				IR_ASSERT(input);
				if (!dom) {
					dom = input;
				} else {
					ir_ref dom2 = input;
					while (dom != dom2) {
						while (dom2 > dom) dom2 = idom[dom2];
						if (!dom2) {
							break;
						}
						while (dom > dom2) dom = idom[dom];
						if (!dom) {
							dom = dom2;
							break;
						}
					}
				}
			}
			if (dom != idom[ref]) {
				idom[ref] = dom;
				changed = 1;
			}
		} IR_BITSET_FOREACH_END();
	} while (changed);

	ir_mem_free(bb_starts);
	ir_worklist_free(&worklist);
}

int ir_mem2ssa(ir_ctx *ctx)
{
	ir_ref i, n;
	ir_insn *insn;
	ir_ref *ssa_vars = NULL;
	ir_ref *idom = NULL;
	size_t ssa_vars_len = 0;
	ir_list queue;

	IR_ASSERT(ctx->use_lists);

	for (i = IR_UNUSED + 1; i < ctx->insns_count;) {
		insn = &ctx->ir_base[i];
		if (insn->op == IR_ALLOCA) {
			ir_type type;

			if (ir_mem2ssa_may_convert_alloca(ctx, i, insn, &type)) {
				if (!ssa_vars) {
					ssa_vars_len = ctx->insns_count;
					ssa_vars = ir_mem_calloc(ssa_vars_len, sizeof(ir_ref));
					idom = ir_mem_calloc(ssa_vars_len, sizeof(ir_ref));
					ir_calc_idom_tree(ctx, idom);
					ir_list_init(&queue, 256);
//				} else {
//					memset(ssa_vars, 0, ssa_vars_len * sizeof(ir_ref));
				}

				/* remove from control list */
				ir_ref prev = insn->op1;
				ir_ref next = ir_next_control(ctx, i);
				ctx->ir_base[next].op1 = prev;
				ir_use_list_replace_one(ctx, prev, i, next);

				ir_mem2ssa_convert2(ctx, idom, ssa_vars, &queue, i, type);

				insn = &ctx->ir_base[i];
				MAKE_NOP(insn);
				CLEAR_USES(i);
			}
		} else if (insn->op == IR_VAR) {
			if (ir_mem2ssa_may_convert_var(ctx, i, insn)) {
				if (!ssa_vars) {
					ssa_vars_len = ctx->insns_count;
					ssa_vars = ir_mem_calloc(ssa_vars_len, sizeof(ir_ref));
					idom = ir_mem_calloc(ssa_vars_len, sizeof(ir_ref));
					ir_calc_idom_tree(ctx, idom);
					ir_list_init(&queue, 256);
//				} else {
//					memset(ssa_vars, 0, ssa_vars_len * sizeof(ir_ref));
				}

				ir_use_list_remove_one(ctx, insn->op1, i);

				ir_mem2ssa_convert2(ctx, idom, ssa_vars, &queue, i, insn->type);

				insn = &ctx->ir_base[i];
				MAKE_NOP(insn);
				CLEAR_USES(i);
			}
		}
		n = ir_insn_len(insn);
		i += n;
	}

	if (ssa_vars) {
		ir_list_free(&queue);
		ir_mem_free(idom);
		ir_mem_free(ssa_vars);
	}

	return 1;
}
