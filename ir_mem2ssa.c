/*
 * IR - Lightweight JIT Compilation Framework
 * (MEM2SSA - Static Single Assignment Form Construction for VAR/ALLOCA)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h"

static ir_ref ir_uninitialized(ir_ctx *ctx, ir_type type)
{
	/* read of uninitialized variable (use 0) */
	ir_val c;

	c.i64 = 0;
	return ir_const(ctx, c, type);
}

static void ir_mem2ssa_convert(ir_ctx      *ctx,
                               ir_ref      *ssa_vars,
                               ir_list     *queue,
                               ir_bitset    defs,
                               ir_bitset    merges,
                               ir_bitqueue *iter_worklist,
                               ir_ref       var,
                               ir_ref       next,
                               ir_type      type)
{
	ir_ref *p, i, n, use, next_ctrl;
	ir_insn *use_insn;
	uint32_t b, *q;

	/* For each usage of VAR (use list must be sorted) */
	next_ctrl = next;
	n = ctx->use_lists[var].count;
	for (p = ctx->use_edges + ctx->use_lists[var].refs; n > 0; p++, n--) {
		use = *p;
		IR_ASSERT(use);
		if (use == next_ctrl) {
			next_ctrl = IR_UNUSED;
			continue;
		}
		use_insn = &ctx->ir_base[use];
		if (use_insn->op == IR_VSTORE || use_insn->op == IR_STORE) {
			IR_ASSERT(use_insn->op2 == var && use_insn->op3 != var);
			b = ctx->cfg_map[use];
			if (EXPECTED(b)) {
				/* Mark VAR as defined and alive at the end of BB */
				ir_bitset_incl(defs, b);
				ssa_vars[b] = var;
			}
		} else {
			IR_ASSERT(use_insn->op == IR_VLOAD || use_insn->op == IR_LOAD);
			IR_ASSERT(use_insn->op2 == var);
			b = ctx->cfg_map[use];
			if (EXPECTED(b) && ssa_vars[b] != var) {
				ssa_vars[b] = var;  /* mark VAR as alive at start of BB */
				ir_list_push(queue, b); /* schedule BB for backward path exploration on the next step */
			}
		}
	}

	/* Find all MERGEs where VAR is alive, exploring paths backward from "uses" to "definitions".
	 *
	 * See algorithms 6 and 7 from "Computing Liveness Sets for SSA-Form Programs"
	 * by Florian Brandner, Benoit Boissinot, Alain Darte, Benoit Dupont de Dinechin,
	 * Fabrice Rastello. TR Inria RR-7503, 2011
	 */
	while (ir_list_len(queue)) {
		ir_block *bb;

		b = ir_list_pop(queue);
next:
		bb = &ctx->cfg_blocks[b];
		if (bb->predecessors_count > 1) {
			IR_ASSERT(ctx->ir_base[bb->start].op == IR_MERGE || ctx->ir_base[bb->start].op == IR_LOOP_BEGIN);
			ir_bitset_incl(merges, b); /* collect all MERGEs for the next step */
			n = bb->predecessors_count;

			for (q = ctx->cfg_edges + bb->predecessors; n > 0; q++, n--) {
				b = *q;
				if (ssa_vars[b] != var) {
					ssa_vars[b] = var;
					if (n == 1) goto next;
					ir_list_push(queue, b);
				}
			}
		} else if (bb->predecessors_count == 1) {
			IR_ASSERT(ctx->ir_base[bb->start].op != IR_START && ctx->ir_base[bb->start].op1);
			b = ctx->cfg_edges[bb->predecessors];
			if (ssa_vars[b] != var) {
				ssa_vars[b] = var;
				goto next;
			}
		}
	}

	/* Insert PHI nodes into Domintar Frontiers where VAR is alive.
	 * Find the DFs on-the-fly using the algoritm (Figure 5) from
	 * "A Simple, Fast Dominance Algorithm" by Cooper, Harvey and Kennedy. */
	bool changed;
	do {
		changed = 0;
		IR_BITSET_FOREACH(merges, ir_bitset_len(ctx->cfg_blocks_count + 1), b) {
			uint32_t dom;
			bool need_phi = 0;

			ir_block *bb = &ctx->cfg_blocks[b];
			ir_ref start = bb->start;
			dom = bb->idom;
			IR_ASSERT(ctx->ir_base[start].op == IR_MERGE || ctx->ir_base[start].op == IR_LOOP_BEGIN);
			n = bb->predecessors_count;
			for (q = ctx->cfg_edges + bb->predecessors; n > 0; q++, n--) {
				uint32_t runner = *q;
				while (runner != dom) {
					if (ir_bitset_in(defs, runner)) {
						need_phi = 1;
						break;
					}
					runner = ctx->cfg_blocks[runner].idom;
					IR_ASSERT(runner);
				}
			}
			if (need_phi) {
				ir_ref phi = ir_emit_N(ctx, IR_OPT(IR_PHI, type), bb->predecessors_count + 1);
				ir_set_op(ctx, phi, 1, start);
				ir_use_list_add(ctx, start, phi);
				ir_bitset_excl(merges, b);
				ir_list_push(queue, phi);
				ssa_vars[b] = phi;
				if (!ir_bitset_in(defs, b)) {
					/* Mark VAR as defined and alive at the end of BB */
					ir_bitset_incl(defs, b);
					changed = 1;
				}
			}
		} IR_BITSET_FOREACH_END();
	} while (changed);

	ir_bitqueue_grow(iter_worklist, ctx->insns_count);

	/* Renaming: remove STROREs and replace LOADs by last STOREed values or by PHIs */
	next_ctrl = next;
	n = ctx->use_lists[var].count;
	for (i = 0; i < n; i++) {
		use = ctx->use_edges[ctx->use_lists[var].refs + i];
		if (use == next_ctrl) {
			next_ctrl = IR_UNUSED;
			continue;
		}
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
			ir_use_list_replace_one(ctx, prev, use, next);

			if (ctx->ir_base[use_insn->op3].type == type) {
//				ir_use_list_remove_one(ctx, use, next);
				if (!IR_IS_CONST_REF(use_insn->op3)) {
					ir_use_list_remove_one(ctx, use_insn->op3, use);
					/* op3 may became dead */
					if (ctx->use_lists[use_insn->op3].count == 0
					 || (ctx->use_lists[use_insn->op3].count == 1
					  && (ir_op_flags[ctx->ir_base[use_insn->op3].op] & IR_OP_FLAG_MEM)
					  && (ir_op_flags[ctx->ir_base[use_insn->op3].op] & (IR_OP_FLAG_MEM_LOAD|IR_OP_FLAG_MEM_ALLOC)))) {
						ir_bitqueue_add(iter_worklist, use_insn->op3);
					}
				}

				b = ctx->cfg_map[use];
				if (EXPECTED(b)) {
					ssa_vars[b] = use_insn->op3;
				}

				MAKE_NOP(use_insn);
				CLEAR_USES(use);
			} else {
				ir_use_list_remove_one(ctx, use, next);

				use_insn->opt = IR_OPT(IR_BITCAST, type);
				use_insn->inputs_count = 1;
				use_insn->op1 = use_insn->op3;
				use_insn->op2 = IR_UNUSED;
				use_insn->op3 = IR_UNUSED;

				ir_bitqueue_add(iter_worklist, use);

				b = ctx->cfg_map[use];
				if (EXPECTED(b)) {
					ssa_vars[b] = use;
				}
			}
			ctx->cfg_map[use] = 0;
		} else if (use_insn->op == IR_VLOAD || use_insn->op == IR_LOAD) {
			uint32_t b0;

			/*
			 *  prev    VAR      prev   ssa_val
			 *      \  /          |     |
			 *      VLOAD    =>   |     |
			 *      /  \          |     |
			 *  next    use      next   use
			 *
			 */

			ir_ref val;
			ir_ref prev = use_insn->op1;
			ir_ref next = ir_next_control(ctx, use);

			ctx->ir_base[next].op1 = prev;
			ir_use_list_remove_one(ctx, use, next);
			ir_use_list_replace_one(ctx, prev, use, next);

			b0 = b = ctx->cfg_map[use];
			while (1) {
				if (!b) {
					val = ir_uninitialized(ctx, type);
					break;
				}
				val = ssa_vars[b];
				if (val != var) {
					/* compress the descovered dominance path */
					while (b0 != b) {
						ssa_vars[b0] = val;
						b0 = ctx->cfg_blocks[b0].idom;
					}
					break;
				}
				b = ctx->cfg_blocks[b].idom;
			}
			IR_ASSERT(ctx->ir_base[val].type == type);
			if (use_insn->type == type) {
				ir_iter_replace(ctx, use, val, iter_worklist);

				use_insn = &ctx->ir_base[use];
				MAKE_NOP(use_insn);
				CLEAR_USES(use);
			} else {
				use_insn->op = IR_BITCAST;
				use_insn->inputs_count = 1;
				use_insn->op1 = val;
				use_insn->op2 = IR_UNUSED;
				if (val > 0) {
					ir_use_list_add(ctx, val, use);
				}
			}
			ctx->cfg_map[use] = 0;
		}
	}

	/* Renaming: set PHI operands */
	while (ir_list_len(queue)) {
		ir_ref phi = ir_list_pop(queue);
		ir_insn *phi_insn = &ctx->ir_base[phi];

		IR_ASSERT(phi_insn->op == IR_PHI);
		ir_ref start = phi_insn->op1;
		ir_insn *start_insn = &ctx->ir_base[start];
		IR_ASSERT(start_insn->op == IR_MERGE || start_insn->op == IR_LOOP_BEGIN);
		n = start_insn->inputs_count;
		for (i = 0, p = start_insn->ops + 1; i < n; p++, i++) {
			ir_ref val, end = *p;
			b = ctx->cfg_map[end];
			val = ssa_vars[b];
			while (val == var) {
				b = ctx->cfg_blocks[b].idom;
				if (!b) {
					val = ir_uninitialized(ctx, type);
					break;
			    }
				val = ssa_vars[b];
			}
			ir_set_op(ctx, phi, i + 2, val);
			if (!IR_IS_CONST_REF(val)) {
				ir_use_list_add(ctx, val, phi);
			}
		}
	}
}

static bool ir_mem2ssa_may_convert_alloca(ir_ctx *ctx, ir_ref var, ir_ref next, ir_insn *insn, ir_type *type_ptr)
{
	ir_ref n, *p, use;
	ir_insn *use_insn;
	ir_use_list *use_list;
	ir_type type = IR_VOID;
	size_t size;
	ir_ref last_use = IR_UNUSED;
	bool needs_sorting = 0;

	if (!IR_IS_CONST_REF(insn->op2)) {
		return 0;
	}
	if (!(ctx->ir_base[insn->op2].type >= IR_U8 && ctx->ir_base[insn->op2].type <= IR_U64)
	 && !(ctx->ir_base[insn->op2].type >= IR_I8 && ctx->ir_base[insn->op2].type <= IR_I64)) {
		return 0;
	}

	size = ctx->ir_base[insn->op2].val.u64;
	if (size != 1 && size != 2 && size != 4 && size != 8 && size != sizeof(double)) {
		return 0;
	}

	use_list = &ctx->use_lists[var];
	n = use_list->count;

	p = &ctx->use_edges[use_list->refs];
	do {
		use = *p;
		IR_ASSERT(use);
		if (use < last_use) {
			needs_sorting = 1;
		}
		last_use = use;
		use_insn = &ctx->ir_base[use];
		if (use == next) {
			next = IR_UNUSED;
			p++;
			continue; /* skip control link */
		}
		if (use_insn->op == IR_LOAD) {
			if (use_insn->op2 != var
			 || ir_type_size[use_insn->type] != size) {
				return 0;
			}
			if (!type) {
				type = use_insn->type;
			}
		} else if (use_insn->op == IR_STORE) {
			if (use_insn->op2 != var
			 || use_insn->op3 == var
			 || ir_type_size[ctx->ir_base[use_insn->op3].type] != size) {
				return 0;
			}
			if (!type) {
				type = ctx->ir_base[use_insn->op3].type;
			}
		} else {
			return 0;
		}
		p++;
	} while (--n > 0);

	*type_ptr = type;

	if (needs_sorting) {
		ir_use_list_sort(ctx, var);
	}

	return 1;
}

static bool ir_mem2ssa_may_convert_var(ir_ctx *ctx, ir_ref var, ir_insn *insn)
{
	ir_ref n, *p, use;
	ir_insn *use_insn;
	ir_use_list *use_list;
	ir_type type;
	ir_ref last_use = IR_UNUSED;
	bool needs_sorting = 0;

	use_list = &ctx->use_lists[var];
	n = use_list->count;
	if (!n) {
		return 0;
	}

	p = &ctx->use_edges[use_list->refs];
	type = insn->type;
	do {
		use = *p;
		IR_ASSERT(use);
		if (use < last_use) {
			needs_sorting = 1;
		}
		last_use = use;
		use_insn = &ctx->ir_base[use];
		if (use_insn->op == IR_VLOAD) {
			if (use_insn->op2 != var
			 || (use_insn->type != type
			  && ir_type_size[use_insn->type] != ir_type_size[type])) {
				return 0;
			}
		} else if (use_insn->op == IR_VSTORE) {
			if (use_insn->op2 != var
			 || use_insn->op3 == var
			 || (ctx->ir_base[use_insn->op3].type != type
			  && ir_type_size[ctx->ir_base[use_insn->op3].type] != ir_type_size[type])) {
				return 0;
			}
		} else {
			return 0;
		}
		p++;
	} while (--n > 0);

	if (needs_sorting) {
		ir_use_list_sort(ctx, var);
	}

	return 1;
}

int ir_mem2ssa(ir_ctx *ctx)
{
	uint32_t b;
	ir_block *bb;
	ir_ref *ssa_vars = NULL;
	ir_list queue;
	ir_bitset defs;
	ir_bitqueue iter_worklist;

	ctx->flags2 &= ~IR_MEM2SSA_VARS;
	IR_ASSERT(ctx->use_lists && ctx->cfg_blocks);
	for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; bb++, b++) {
		ir_ref ref = bb->end;

		IR_ASSERT(ctx->cfg_map[ref] == b);
		ref = ctx->ir_base[ref].op1;
		while (ref != bb->start) {
			ctx->cfg_map[ref] = b;
			ref = ctx->ir_base[ref].op1;
		}
		IR_ASSERT(ctx->cfg_map[ref] == b);
	}

	for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; bb++, b++) {
		ir_ref ref, next, start = bb->start;
		ir_ref i, j, n = ctx->use_lists[start].count;
		ir_insn *insn;

		if (ssa_vars
		 && (ctx->ir_base[start].op == IR_MERGE
		  || (ctx->ir_base[start].op == IR_BEGIN
		   && ctx->ir_base[ctx->ir_base[start].op1].op == IR_END))) {
			/* CFG reduction may open oportunities for more redundand LOADs and STOREs elimination */
			ir_bitqueue_add(&iter_worklist, start);
		}

		if (n > 1) { /* BB start node is used at least by next control or by BB end node */
			ir_use_list *use_list = &ctx->use_lists[start];

			for (i = 0, j = 0; i < use_list->count; i++) {
				ir_ref use = ctx->use_edges[use_list->refs + i];

				insn = &ctx->ir_base[use];
				if (insn->op == IR_VAR) {
					if (ctx->use_lists[use].count == 0) {
						MAKE_NOP(insn);
						continue;
					} else if (ir_mem2ssa_may_convert_var(ctx, use, insn)) {
						uint32_t len = ir_bitset_len(ctx->cfg_blocks_count + 1);

						if (!ssa_vars) {
							ir_bitqueue_init(&iter_worklist, ctx->insns_count);
							ssa_vars = ir_mem_calloc(ctx->cfg_blocks_count + 1, sizeof(ir_ref));
							ir_list_init(&queue, ctx->cfg_blocks_count);
							defs = ir_mem_calloc(len * 2, IR_BITSET_BITS / 8);
						} else {
							memset(defs, 0, len * 2 * IR_BITSET_BITS / 8);
						}

						ir_mem2ssa_convert(ctx, ssa_vars, &queue, defs, defs + len, &iter_worklist, use, IR_UNUSED, insn->type);

						insn = &ctx->ir_base[use];
						MAKE_NOP(insn);
						CLEAR_USES(use);
						ctx->flags2 |= IR_MEM2SSA_VARS;

						use_list = &ctx->use_lists[start]; /* reload */
						continue;
					}
				}

				/* compact use list */
				if (i != j) {
					ctx->use_edges[use_list->refs + j] = use;
				}
				j++;
			}
			if (i != j) {
				use_list->count = j;
				do {
					ctx->use_edges[use_list->refs + j] = IR_UNUSED; /* clenu-op the removed tail */
					j++;
				} while (j != i);
			}
		}

		ref = bb->end;
		insn = &ctx->ir_base[ref];
		next = ref;
		ref = insn->op1;
		while (ref != start) {
			insn = &ctx->ir_base[ref];
			if (insn->op == IR_ALLOCA) {
				ir_type type;

				if (ctx->use_lists[ref].count == 1) {
					ir_ref prev = insn->op1;
					ctx->ir_base[next].op1 = prev;
					if (!IR_IS_CONST_REF(insn->op2)) {
						ir_use_list_remove_one(ctx, insn->op2, ref);
					}
					ir_use_list_replace_one(ctx, prev, ref, next);
					MAKE_NOP(insn);
					CLEAR_USES(ref);
					ctx->cfg_map[ref] = 0;
					ref = prev;
					continue;
				} else if (ir_mem2ssa_may_convert_alloca(ctx, ref, next, insn, &type)) {
					uint32_t len = ir_bitset_len(ctx->cfg_blocks_count + 1);
					ir_ref prev;

					if (!ssa_vars) {
						ir_bitqueue_init(&iter_worklist, ctx->insns_count);
						ssa_vars = ir_mem_calloc(ctx->cfg_blocks_count + 1, sizeof(ir_ref));
						ir_list_init(&queue, ctx->cfg_blocks_count);
						defs = ir_mem_calloc(len * 2, IR_BITSET_BITS / 8);
					} else {
						memset(defs, 0, len * 2 * IR_BITSET_BITS / 8);
					}

					ir_mem2ssa_convert(ctx, ssa_vars, &queue, defs, defs + len, &iter_worklist, ref, next, type);

					insn = &ctx->ir_base[ref];
					prev = insn->op1;
					next = ir_next_control(ctx, ref);
					ctx->ir_base[next].op1 = prev;
					if (!IR_IS_CONST_REF(insn->op2)) {
						ir_use_list_remove_one(ctx, insn->op2, ref);
					}
					ir_use_list_replace_one(ctx, prev, ref, next);
					MAKE_NOP(insn);
					CLEAR_USES(ref);
					ctx->cfg_map[ref] = 0;
					ref = prev;
					ctx->flags2 |= IR_MEM2SSA_VARS;
					continue;
				}
			} else if (ssa_vars
					&& (insn->op == IR_LOAD
					 || insn->op == IR_STORE
					 || insn->op == IR_VLOAD
					 || insn->op == IR_VSTORE)) {
				/* After removing SSA related LOADs and STOREs unrelated LOADs and STOREs may become redundand */
				ir_bitqueue_add(&iter_worklist, ref);
			}
			next = ref;
			ref = insn->op1;
		}
	}

	if (ssa_vars) {
		ir_mem_free(defs);
		ir_list_free(&queue);
		ir_mem_free(ssa_vars);

		// TODO: remove BOLCK_BEGIN and BLOCK_END without ALLOCAs between them
		ir_bitqueue_grow(&iter_worklist, ctx->insns_count);
		ir_iter_opt(ctx, &iter_worklist);

		ir_bitqueue_free(&iter_worklist);
	}

	return 1;
}
