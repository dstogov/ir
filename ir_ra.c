/*
 * IR - Lightweight JIT Compilation Framework
 * (RA - Register Allocation, Liveness, Coalescing, SSA Deconstruction)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 *
 * See: "Linear Scan Register Allocation on SSA Form", Christian Wimmer and
 * Michael Franz, CGO'10 (2010)
 * See: "Optimized Interval Splitting in a Linear Scan Register Allocator",
 * Christian Wimmer VEE'10 (2005)
 */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <stdlib.h>
#include "ir.h"

#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
# include "ir_x86.h"
#elif defined(IR_TARGET_AARCH64)
# include "ir_aarch64.h"
#else
# error "Unknown IR target"
#endif

#include "ir_private.h"

#define IR_SKIP      IR_LAST_OP
#define IR_SKIP_MEM  (IR_LAST_OP+1)

int ir_regs_number(void)
{
	return IR_REG_NUM;
}

bool ir_reg_is_int(int32_t reg)
{
	IR_ASSERT(reg >= 0 && reg < IR_REG_NUM);
	return reg >= IR_REG_GP_FIRST && reg <= IR_REG_GP_LAST;
}

int ir_assign_virtual_registers(ir_ctx *ctx)
{
	uint32_t *vregs;
	uint32_t vregs_count = 0;
	uint32_t b;
	ir_ref i, n;
	ir_block *bb;
	ir_insn *insn;
	uint32_t flags;

	/* Assign unique virtual register to each data node */
	vregs = ir_mem_calloc(ctx->insns_count, sizeof(ir_ref));
	n = 1;
	for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		i = bb->start;

		/* skip first instruction */
		insn = ctx->ir_base + i;
		n = ir_operands_count(ctx, insn);
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		i += n;
		insn += n;
		while (i < bb->end) {
			flags = ir_op_flags[insn->op];
			if (((flags & IR_OP_FLAG_DATA) && ctx->use_lists[i].count > 0)
			 || ((flags & IR_OP_FLAG_MEM) && ctx->use_lists[i].count > 1)) {
				if (!ctx->rules || ir_needs_vreg(ctx, i)) {
					vregs[i] = ++vregs_count;
				}
			}
			n = ir_operands_count(ctx, insn);
			n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
			i += n;
			insn += n;
		}
	}
	ctx->vregs_count = vregs_count;
	ctx->vregs = vregs;

	return 1;
}

/* Lifetime intervals construction */

static void ir_add_local_var(ir_ctx *ctx, int v, uint8_t type)
{
	ir_live_interval *ival = ctx->live_intervals[v];

	IR_ASSERT(!ival);

	ival = ir_mem_malloc(sizeof(ir_live_interval));
	IR_ASSERT(type != IR_VOID);
	ival->type = type;
	ival->reg = IR_REG_NONE;
	ival->flags = IR_LIVE_INTERVAL_VAR;
	ival->vreg = v;
	ival->stack_spill_pos = -1; // not allocated
	ival->range.start = IR_START_LIVE_POS_FROM_REF(1);
	ival->range.end = ival->end = IR_END_LIVE_POS_FROM_REF(ctx->insns_count - 1);
	ival->range.next = NULL;
	ival->use_pos = NULL;

	ival->top = ival;
	ival->next = NULL;

	ctx->live_intervals[v] = ival;
	ctx->flags |= IR_LR_HAVE_VARS;
}

static void ir_add_live_range(ir_ctx *ctx, ir_live_range **unused, int v, uint8_t type, ir_live_pos start, ir_live_pos end)
{
	ir_live_interval *ival = ctx->live_intervals[v];
	ir_live_range *p, *q, *next, *prev;

	if (!ival) {
		ival = ir_mem_malloc(sizeof(ir_live_interval));
		IR_ASSERT(type != IR_VOID);
		ival->type = type;
		ival->reg = IR_REG_NONE;
		ival->flags = 0;
		ival->vreg = v;
		ival->stack_spill_pos = -1; // not allocated
		ival->range.start = start;
		ival->range.end = ival->end = end;
		ival->range.next = NULL;
		ival->use_pos = NULL;

		ival->top = ival;
		ival->next = NULL;

		ctx->live_intervals[v] = ival;
		return;
	}

	IR_ASSERT(type == IR_VOID || type == ival->type);
	p = &ival->range;
	prev = NULL;
	while (p && end >= p->start) {
		if (p->end >= start) {
			if (start < p->start) {
				p->start = start;
			}
			if (end > p->end) {
				p->end = end;
				/* merge with next */
				next = p->next;
				while (next && p->end >= next->start) {
					if (next->end > p->end) {
						p->end = next->end;
					}
					p->next = next->next;
					/* list of deleted structures is keapt in "unused" list */
					next->next = *unused;
					*unused = next;
					next = p->next;
				}
				if (!p->next) {
					ival->end = p->end;
				}
			}
			return;
		}
		prev = p;
		p = prev->next;
	}
	if (*unused) {
		/* reuse */
		q = *unused;
		*unused = q->next;
	} else {
		q = ir_mem_malloc(sizeof(ir_live_range));
	}
	if (prev) {
		prev->next = q;
	} else {
		q->start = ival->range.start;
		q->end = ival->range.end;
		q->next = ival->range.next;
		p = q;
		q = &ival->range;
	}
	q->start = start;
	q->end = end;
	q->next = p;
	if (!p) {
		ival->end = end;
	}
}

static void ir_add_fixed_live_range(ir_ctx *ctx, ir_live_range **unused, ir_reg reg, ir_live_pos start, ir_live_pos end)
{
	int v = ctx->vregs_count + 1 + reg;
	ir_live_interval *ival = ctx->live_intervals[v];
	if (!ival) {
		ival = ir_mem_malloc(sizeof(ir_live_interval));
		ival->type = IR_VOID;
		ival->reg = reg;
		ival->flags = IR_LIVE_INTERVAL_FIXED;
		ival->vreg = v;
		ival->stack_spill_pos = -1; // not allocated
		ival->range.start = start;
		ival->range.end = ival->end = end;
		ival->range.next = NULL;
		ival->use_pos = NULL;

		ival->top = ival;
		ival->next = NULL;

		ctx->live_intervals[v] = ival;
		return;
	}
	ir_add_live_range(ctx, unused, v, IR_VOID, start, end);
}

static void ir_add_tmp(ir_ctx *ctx, ir_ref ref, ir_tmp_reg tmp_reg)
{
	ir_live_interval *ival = ir_mem_malloc(sizeof(ir_live_interval));

	ival->type = tmp_reg.type;
	ival->reg = IR_REG_NONE;
	ival->flags = IR_LIVE_INTERVAL_TEMP | tmp_reg.num;
	ival->vreg = 0;
	ival->stack_spill_pos = -1; // not allocated
	ival->range.start = IR_START_LIVE_POS_FROM_REF(ref) + tmp_reg.start;
	ival->range.end = ival->end = IR_START_LIVE_POS_FROM_REF(ref) + tmp_reg.end;
	ival->range.next = NULL;
	ival->use_pos = NULL;

	if (!ctx->live_intervals[0]) {
		ival->top = ival;
		ival->next = NULL;
		ctx->live_intervals[0] = ival;
	} else if (ival->range.start >= ctx->live_intervals[0]->range.start) {
		ir_live_interval *prev = ctx->live_intervals[0];

		while (prev->next && ival->range.start >= prev->next->range.start) {
			prev = prev->next;
		}
		ival->top = prev->top;
		ival->next = prev->next;
		prev->next = ival;
	} else {
		ir_live_interval *next = ctx->live_intervals[0];

		ival->top = ival;
		ival->next = next;
		ctx->live_intervals[0] = ival;
		while (next) {
			next->top = ival;
			next = next->next;
		}
	}
	return;
}

static bool ir_has_tmp(ir_ctx *ctx, ir_ref ref, uint8_t num)
{
	ir_live_interval *ival = ctx->live_intervals[0];

	if (ival) {
		while (ival && IR_LIVE_POS_TO_REF(ival->range.start) <= ref) {
			if (IR_LIVE_POS_TO_REF(ival->range.start) == ref
			 && ival->flags == (IR_LIVE_INTERVAL_TEMP | num)) {
				return 1;
			}
			ival = ival->next;
		}
	}
	return 0;
}

static void ir_fix_live_range(ir_ctx *ctx, int v, ir_live_pos old_start, ir_live_pos new_start)
{
	ir_live_range *p = &ctx->live_intervals[v]->range;

	while (p && p->start < old_start) {
		p = p->next;
	}
	IR_ASSERT(p && p->start == old_start);
	p->start = new_start;
}

static void ir_add_use_pos(ir_ctx *ctx, int v, ir_use_pos *use_pos)
{
	ir_live_interval *ival = ctx->live_intervals[v];
	ir_use_pos *prev = NULL;
	ir_use_pos *p = ival->use_pos;

	if (use_pos->hint != IR_REG_NONE || use_pos->hint_ref != 0) {
		ival->flags |= IR_LIVE_INTERVAL_HAS_HINTS;
	}

	while (p && (p->pos < use_pos->pos ||
			(p->pos == use_pos->pos &&
				(use_pos->op_num == 0 || p->op_num < use_pos->op_num)))) {
		prev = p;
		p = p->next;
	}

	if (prev) {
		use_pos->next = prev->next;
		prev->next = use_pos;
	} else {
		use_pos->next = ival->use_pos;
		ival->use_pos = use_pos;
	}
}

static void ir_add_use(ir_ctx *ctx, int v, int op_num, ir_live_pos pos, ir_reg hint, uint8_t use_flags, ir_ref hint_ref)
{
	ir_use_pos *use_pos;

	use_pos = ir_mem_malloc(sizeof(ir_use_pos));
	use_pos->op_num = op_num;
	use_pos->hint = hint;
	use_pos->flags = use_flags;
	use_pos->hint_ref = hint_ref;
	use_pos->pos = pos;

	ir_add_use_pos(ctx, v, use_pos);
}

static void ir_add_phi_use(ir_ctx *ctx, int v, int op_num, ir_live_pos pos, ir_ref phi_ref)
{
	ir_use_pos *use_pos;

	use_pos = ir_mem_malloc(sizeof(ir_use_pos));
	use_pos->op_num = op_num;
	use_pos->hint = IR_REG_NONE;
	use_pos->flags = IR_PHI_USE | IR_USE_SHOULD_BE_IN_REG; // TODO: ???
	use_pos->hint_ref = phi_ref;
	use_pos->pos = pos;

	ir_add_use_pos(ctx, v, use_pos);
}

int ir_compute_live_ranges(ir_ctx *ctx)
{
	uint32_t b, i, j, k, n, succ, *p;
	ir_ref ref;
	uint32_t len;
	ir_insn *insn;
	ir_block *bb, *succ_bb;
#ifdef IR_DEBUG
	ir_bitset visited;
#endif
	ir_bitset live, bb_live;
	ir_bitset loops = NULL;
	ir_bitqueue queue;
	ir_live_range *unused = NULL;

	if (!(ctx->flags & IR_LINEAR) || !ctx->vregs) {
		return 0;
	}

	/* Compute Live Ranges */
	ctx->flags &= ~(IR_LR_HAVE_VARS|IR_LR_HAVE_DESSA_MOVES);
#ifdef IR_DEBUG
	visited = ir_bitset_malloc(ctx->cfg_blocks_count + 1);
#endif
	len = ir_bitset_len(ctx->vregs_count + 1);
	bb_live = ir_mem_malloc((ctx->cfg_blocks_count + 1) * len * sizeof(ir_bitset_base_t));
	ctx->live_intervals = ir_mem_calloc(ctx->vregs_count + 1 + IR_REG_NUM + 1, sizeof(ir_live_interval*));
	for (b = ctx->cfg_blocks_count; b > 0; b--) {
		bb = &ctx->cfg_blocks[b];
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		/* for each successor of b */
#ifdef IR_DEBUG
		ir_bitset_incl(visited, b);
#endif
		live = bb_live + (len * b);
		n = bb->successors_count;
		if (n == 0) {
			ir_bitset_clear(live, len);
		} else {
			p = &ctx->cfg_edges[bb->successors];
			succ = *p;
			/* blocks must be ordered where all dominators of a block are before this block */
#ifdef IR_DEBUG
            IR_ASSERT(ir_bitset_in(visited, succ) || bb->loop_header == succ);
#endif

			if (n > 1) {
				IR_ASSERT(succ > b);
				/* live = union of successors.liveIn */
				ir_bitset_copy(live, bb_live + (len * succ), len);
				for (p++, n--; n > 0; p++, n--) {
					succ = *p;
					IR_ASSERT(succ > b);
					ir_bitset_union(live, bb_live + (len * succ), len);
				}
			} else {
				/* live = successor.liveIn */
				if (EXPECTED(succ > b)) {
					ir_bitset_copy(live, bb_live + (len * succ), len);
				} else {
					IR_ASSERT(ctx->cfg_blocks[succ].flags & IR_BB_LOOP_HEADER);
					ir_bitset_clear(live, len);
				}

				/* for each phi function phi of successor */
				succ_bb = &ctx->cfg_blocks[succ];
				if (succ_bb->predecessors_count > 1) {
					ir_use_list *use_list = &ctx->use_lists[succ_bb->start];

					if (use_list->count > 1) {
						k = ir_phi_input_number(ctx, succ_bb, b);
						IR_ASSERT(k != 0);
						for (ref = 0; ref < use_list->count; ref++) {
							ir_ref use = ctx->use_edges[use_list->refs + ref];
							insn = &ctx->ir_base[use];
							if (insn->op == IR_PHI) {
								ir_ref input = ir_insn_op(insn, k);
								if (input > 0) {
									/* live.add(phi.inputOf(b)) */
									IR_ASSERT(ctx->vregs[input]);
									ir_bitset_incl(live, ctx->vregs[input]);
									// TODO: ir_add_live_range() is used just to set ival->type
									/* intervals[phi.inputOf(b)].addRange(b.from, b.to) */
									ir_add_live_range(ctx, &unused, ctx->vregs[input], insn->type,
										IR_START_LIVE_POS_FROM_REF(bb->start),
										IR_END_LIVE_POS_FROM_REF(bb->end));
									ir_add_phi_use(ctx, ctx->vregs[input], k, IR_DEF_LIVE_POS_FROM_REF(bb->end), use);
								}
							}
						}
					}
				}
			}
		}

		/* for each opd in live */
		IR_BITSET_FOREACH(live, len, i) {
			/* intervals[opd].addRange(b.from, b.to) */
			ir_add_live_range(ctx, &unused, i, IR_VOID,
				IR_START_LIVE_POS_FROM_REF(bb->start),
				IR_END_LIVE_POS_FROM_REF(bb->end));
		} IR_BITSET_FOREACH_END();

		/* for each operation op of b in reverse order */
		ref = bb->end;
		insn = &ctx->ir_base[ref];
		if (insn->op == IR_END || insn->op == IR_LOOP_END) {
			ref = ctx->prev_ref[ref];
		}
		for (; ref > bb->start; ref = ctx->prev_ref[ref]) {
			uint32_t def_flags;
			uint32_t flags;
			ir_ref *p;
			ir_target_constraints constraints;

			if (ctx->rules) {
				int n;

				if (ctx->rules[ref] == IR_SKIP_MEM) {
					continue;
				}

				def_flags = ir_get_target_constraints(ctx, ref, &constraints);
				n = constraints.tmps_count;
				while (n > 0) {
					n--;
					if (constraints.tmp_regs[n].type) {
						ir_add_tmp(ctx, ref, constraints.tmp_regs[n]);
					} else {
						/* CPU specific constraints */
						ir_add_fixed_live_range(ctx, &unused, constraints.tmp_regs[n].reg,
							IR_START_LIVE_POS_FROM_REF(ref) + constraints.tmp_regs[n].start,
							IR_START_LIVE_POS_FROM_REF(ref) + constraints.tmp_regs[n].end);
					}
				}
			} else {
				def_flags = 0;
				constraints.def_reg = IR_REG_NONE;
				constraints.hints_count = 0;
			}

			insn = &ctx->ir_base[ref];
			if (ctx->vregs[ref]) {
				if (ir_bitset_in(live, ctx->vregs[ref])) {
					if (insn->op == IR_RLOAD) {
						ir_fix_live_range(ctx, ctx->vregs[ref],
							IR_START_LIVE_POS_FROM_REF(bb->start), IR_DEF_LIVE_POS_FROM_REF(ref));
						ctx->live_intervals[ctx->vregs[ref]]->flags = IR_LIVE_INTERVAL_REG_LOAD;
						ctx->live_intervals[ctx->vregs[ref]]->reg = insn->op2;
						/* live.remove(opd) */
						ir_bitset_excl(live, ctx->vregs[ref]);
						continue;
					} else if (insn->op != IR_PHI) {
						ir_live_pos def_pos;
						ir_ref hint_ref = 0;
						ir_reg reg = constraints.def_reg;

						if (reg != IR_REG_NONE) {
							def_pos = IR_SAVE_LIVE_POS_FROM_REF(ref);
							if (insn->op == IR_PARAM) {
								/* parameter register must be kept before it's copied */
								ir_add_fixed_live_range(ctx, &unused, reg,
									IR_START_LIVE_POS_FROM_REF(bb->start), def_pos);
							} else {
								ir_add_fixed_live_range(ctx, &unused, reg,
									IR_DEF_LIVE_POS_FROM_REF(ref), def_pos);
							}
						} else if (def_flags & IR_DEF_REUSES_OP1_REG) {
							/* We add two uses to emulate move from op1 to res */
							ir_add_use(ctx, ctx->vregs[ref], 0, IR_DEF_LIVE_POS_FROM_REF(ref), reg, def_flags, 0);
							def_pos = IR_LOAD_LIVE_POS_FROM_REF(ref);
							if (!IR_IS_CONST_REF(insn->op1)) {
								IR_ASSERT(ctx->vregs[insn->op1]);
								hint_ref = insn->op1;
							}
						} else if (def_flags & IR_DEF_CONFLICTS_WITH_INPUT_REGS) {
							def_pos = IR_LOAD_LIVE_POS_FROM_REF(ref);
						} else {
							if (insn->op == IR_PARAM) {
								/* We may reuse parameter stack slot for spilling */
								ctx->live_intervals[ctx->vregs[ref]]->flags |= IR_LIVE_INTERVAL_MEM_PARAM;
							} else if (insn->op == IR_VLOAD) {
								/* Load may be fused into the useage instruction */
								ctx->live_intervals[ctx->vregs[ref]]->flags |= IR_LIVE_INTERVAL_MEM_LOAD;
							}
							def_pos = IR_DEF_LIVE_POS_FROM_REF(ref);
						}
						/* intervals[opd].setFrom(op.id) */
						ir_fix_live_range(ctx, ctx->vregs[ref],
							IR_START_LIVE_POS_FROM_REF(bb->start), def_pos);
						ir_add_use(ctx, ctx->vregs[ref], 0, def_pos, reg, def_flags, hint_ref);
						/* live.remove(opd) */
						ir_bitset_excl(live, ctx->vregs[ref]);
					} else {
						ir_add_use(ctx, ctx->vregs[ref], 0, IR_DEF_LIVE_POS_FROM_REF(ref), IR_REG_NONE, IR_USE_SHOULD_BE_IN_REG, 0);
						/* live.remove(opd) */
						ir_bitset_excl(live, ctx->vregs[ref]);
						/* PHIs inputs must not be processed */
						continue;
					}
				} else {
					IR_ASSERT(insn->op == IR_VAR);
					IR_ASSERT(ctx->use_lists[ref].count > 0);
					ir_add_local_var(ctx, ctx->vregs[ref], insn->type);
					continue;
				}
			}

			IR_ASSERT(insn->op != IR_PHI && (!ctx->rules || ctx->rules[ref] != IR_SKIP_MEM));
			flags = ir_op_flags[insn->op];
			n = ir_input_edges_count(ctx, insn);
			j = (flags & IR_OP_FLAG_CONTROL) ? 2 : 1;
			for (p = insn->ops + j; j <= n; j++, p++) {
				if (IR_OPND_KIND(flags, j) == IR_OPND_DATA) {
					ir_ref input = *p;
					uint8_t use_flags = IR_USE_FLAGS(def_flags, j);
					ir_reg reg = (j < constraints.hints_count) ? constraints.hints[j] : IR_REG_NONE;

					if (input > 0 && ctx->rules && ctx->rules[input] == IR_SKIP_MEM) {
						do {
							if (ctx->ir_base[input].op == IR_LOAD) {
								input = ctx->ir_base[input].op2;
								if (input < 0 || ctx->rules[input] != IR_SKIP_MEM) {
									break;
								}
							}
							if (ctx->ir_base[input].op == IR_RLOAD) {
								/* pass */
							} else if (ctx->ir_base[input].op == IR_ADD) {
								IR_ASSERT(!IR_IS_CONST_REF(ctx->ir_base[input].op1));
								IR_ASSERT(IR_IS_CONST_REF(ctx->ir_base[input].op2));
								input = ctx->ir_base[input].op1;
								use_flags = IR_USE_MUST_BE_IN_REG;
							} else {
								input = 0;
							}
						} while (0);
					}
					if (input > 0 && ctx->vregs[input]) {
						ir_live_pos use_pos;
						ir_ref hint_ref = 0;

						if ((def_flags & IR_DEF_REUSES_OP1_REG) && j == 1) {
							use_pos = IR_LOAD_LIVE_POS_FROM_REF(ref);
							IR_ASSERT(ctx->vregs[ref]);
							hint_ref = ref;
							if (reg != IR_REG_NONE) {
								ir_add_fixed_live_range(ctx, &unused, reg,
									use_pos, IR_USE_LIVE_POS_FROM_REF(ref));
							}
						} else {
							if (reg != IR_REG_NONE) {
								use_pos = IR_LOAD_LIVE_POS_FROM_REF(ref);
								ir_add_fixed_live_range(ctx, &unused, reg,
									use_pos, IR_USE_LIVE_POS_FROM_REF(ref));
							} else if ((def_flags & IR_DEF_REUSES_OP1_REG) && input == insn->op1) {
								/* Input is the same as "op1" */
								use_pos = IR_LOAD_LIVE_POS_FROM_REF(ref);
							} else {
								use_pos = IR_USE_LIVE_POS_FROM_REF(ref);
							}
						}
						/* intervals[opd].addRange(b.from, op.id) */
						ir_add_live_range(ctx, &unused, ctx->vregs[input], ctx->ir_base[input].type,
							IR_START_LIVE_POS_FROM_REF(bb->start), use_pos);
						ir_add_use(ctx, ctx->vregs[input], j, use_pos, reg, use_flags, hint_ref);
						/* live.add(opd) */
						ir_bitset_incl(live, ctx->vregs[input]);
					} else {
						if (reg != IR_REG_NONE) {
							ir_add_fixed_live_range(ctx, &unused, reg,
								IR_LOAD_LIVE_POS_FROM_REF(ref), IR_USE_LIVE_POS_FROM_REF(ref));
						}
					}
				}
			}
		}

		/* if b is loop header */
		if ((bb->flags & IR_BB_LOOP_HEADER)
		 && !ir_bitset_empty(live, len)) {
			/* variables live at loop header are alive at the whole loop body */
			uint32_t bb_set_len = ir_bitset_len(ctx->cfg_blocks_count + 1);
			int child;
			ir_block *child_bb;

			if (!loops) {
				loops = ir_bitset_malloc(ctx->cfg_blocks_count + 1);
				ir_bitqueue_init(&queue, ctx->cfg_blocks_count + 1);
			} else {
				ir_bitset_clear(loops, bb_set_len);
				ir_bitqueue_clear(&queue);
			}
			ir_bitset_incl(loops, b);
			child = b;
			do {
				child_bb = &ctx->cfg_blocks[child];

				IR_BITSET_FOREACH(live, len, i) {
					ir_add_live_range(ctx, &unused, i, IR_VOID,
						IR_START_LIVE_POS_FROM_REF(child_bb->start),
						IR_END_LIVE_POS_FROM_REF(child_bb->end));
				} IR_BITSET_FOREACH_END();

				child = child_bb->dom_child;
				while (child) {
					child_bb = &ctx->cfg_blocks[child];
					if (child_bb->loop_header && ir_bitset_in(loops, child_bb->loop_header)) {
						ir_bitqueue_add(&queue, child);
						if (child_bb->flags & IR_BB_LOOP_HEADER) {
							ir_bitset_incl(loops, child);
						}
					}
					child = child_bb->dom_next_child;
				}
			} while ((child = ir_bitqueue_pop(&queue)) >= 0);
		}
	}

	if (unused) {
		ir_free_live_ranges(unused);
	}

	if (loops) {
		ir_mem_free(loops);
		ir_bitqueue_free(&queue);
	}

	ir_mem_free(bb_live);
#ifdef IR_DEBUG
	ir_mem_free(visited);
#endif

	return 1;
}

void ir_free_live_ranges(ir_live_range *live_range)
{
	ir_live_range *p;

	while (live_range) {
		p = live_range;
		live_range = live_range->next;
		ir_mem_free(p);
	}
}

void ir_free_live_intervals(ir_live_interval **live_intervals, int count)
{
	int i;
	ir_live_interval *ival, *next;
	ir_use_pos *use_pos;

	count += IR_REG_NUM + 1;
	for (i = 0; i <= count; i++) {
		ival = live_intervals[i];
		while (ival) {
			if (ival->range.next) {
				ir_free_live_ranges(ival->range.next);
			}
			use_pos = ival->use_pos;
			while (use_pos) {
				ir_use_pos *p = use_pos;
				use_pos = p->next;
				ir_mem_free(p);
			}
			next = ival->next;
			ir_mem_free(ival);
			ival = next;
		}
	}
	ir_mem_free(live_intervals);
}


/* Live Ranges coalescing */

static ir_live_pos ir_ivals_overlap(ir_live_range *lrg1, ir_live_range *lrg2)
{
	while (1) {
		if (lrg2->start < lrg1->end) {
			if (lrg1->start < lrg2->end) {
				return IR_MAX(lrg1->start, lrg2->start);
			} else {
				lrg2 = lrg2->next;
				if (!lrg2) {
					return 0;
				}
			}
		} else {
			lrg1 = lrg1->next;
			if (!lrg1) {
				return 0;
			}
		}
	}
}

static ir_live_pos ir_vregs_overlap(ir_ctx *ctx, uint32_t r1, uint32_t r2)
{
	return ir_ivals_overlap(&ctx->live_intervals[r1]->range, &ctx->live_intervals[r2]->range);
}

static void ir_vregs_join(ir_ctx *ctx, ir_live_range **unused, uint32_t r1, uint32_t r2)
{
	ir_live_interval *ival = ctx->live_intervals[r2];
	ir_live_range *live_range = &ival->range;
	ir_live_range *next;
	ir_use_pos *use_pos;

#if 0
	fprintf(stderr, "COALESCE %d -> %d\n", r2, r1);
#endif

	ir_add_live_range(ctx, unused, r1, ival->type, live_range->start, live_range->end);
	live_range = live_range->next;
	while (live_range) {
		next = live_range->next;
		live_range->next = *unused;
		*unused = live_range;
		ir_add_live_range(ctx, unused, r1, ival->type, live_range->start, live_range->end);
		live_range = next;
	}

	use_pos = ctx->live_intervals[r1]->use_pos;
	while (use_pos) {
		if (!(use_pos->flags & IR_PHI_USE) && ctx->vregs[use_pos->hint_ref] == r2) {
			use_pos->hint_ref = 0;
		}
		use_pos = use_pos->next;
	}

	use_pos = ival->use_pos;
	while (use_pos) {
		ir_use_pos *next_use_pos = use_pos->next;
		if (!(use_pos->flags & IR_PHI_USE) && ctx->vregs[use_pos->hint_ref] == r1) {
			use_pos->hint_ref = 0;
		}
		ir_add_use_pos(ctx, r1, use_pos);
		use_pos = next_use_pos;
	}

	ir_mem_free(ival);
	ctx->live_intervals[r2] = NULL;
	ctx->live_intervals[r1]->flags |= IR_LIVE_INTERVAL_COALESCED;
}

static bool ir_try_coalesce(ir_ctx *ctx, ir_live_range **unused, ir_ref from, ir_ref to)
{
	ir_ref i;
	uint32_t v1 = ctx->vregs[from];
	uint32_t v2 = ctx->vregs[to];

	if (v1 != v2 && !ir_vregs_overlap(ctx, v1, v2)) {
		uint8_t f1 = ctx->live_intervals[v1]->flags;
		uint8_t f2 = ctx->live_intervals[v2]->flags;

		if ((f1 & IR_LIVE_INTERVAL_COALESCED) && !(f2 & IR_LIVE_INTERVAL_COALESCED)) {
			ir_vregs_join(ctx, unused, v1, v2);
			ctx->vregs[to] = v1;
		} else if ((f2 & IR_LIVE_INTERVAL_COALESCED) && !(f1 & IR_LIVE_INTERVAL_COALESCED)) {
			ir_vregs_join(ctx, unused, v2, v1);
			ctx->vregs[from] = v2;
		} else if (from < to) {
			ir_vregs_join(ctx, unused, v1, v2);
			if (f2 & IR_LIVE_INTERVAL_COALESCED) {
				for (i = 0; i < ctx->insns_count; i++) {
					if (ctx->vregs[i] == v2) {
						ctx->vregs[i] = v1;
					}
				}
			} else {
				ctx->vregs[to] = v1;
			}
		} else {
			ir_vregs_join(ctx,unused, v2, v1);
			if (f1 & IR_LIVE_INTERVAL_COALESCED) {
				for (i = 0; i < ctx->insns_count; i++) {
					if (ctx->vregs[i] == v1) {
						ctx->vregs[i] = v2;
					}
				}
			} else {
				ctx->vregs[from] = v2;
			}
		}
		return 1;
	}
	return 0;
}

static void ir_add_phi_move(ir_ctx *ctx, uint32_t b, ir_ref from, ir_ref to)
{
	if (IR_IS_CONST_REF(from) || ctx->vregs[from] != ctx->vregs[to]) {
		ctx->cfg_blocks[b].flags |= IR_BB_DESSA_MOVES;
		ctx->flags |= IR_LR_HAVE_DESSA_MOVES;
#if 0
		fprintf(stderr, "BB%d: MOV %d -> %d\n", b, from, to);
#endif
	}
}

static int ir_block_cmp(const void *b1, const void *b2, void *data)
{
	ir_ctx *ctx = data;
	int d1 = ctx->cfg_blocks[*(ir_ref*)b1].loop_depth;
	int d2 = ctx->cfg_blocks[*(ir_ref*)b2].loop_depth;

	if (d1 > d2) {
		return -1;
	} else if (d1 == d2) {
		return 0;
	} else {
		return 1;
	}
}

static void ir_swap_operands(ir_ctx *ctx, ir_ref i, ir_insn *insn)
{
	ir_live_pos pos = IR_USE_LIVE_POS_FROM_REF(i);
	ir_live_pos load_pos = IR_LOAD_LIVE_POS_FROM_REF(i);
	ir_live_interval *ival;
	ir_live_range *r;
	ir_use_pos *p, *p1 = NULL, *p2 = NULL;
	ir_ref tmp;

	tmp = insn->op1;
	insn->op1 = insn->op2;
	insn->op2 = tmp;

	ival = ctx->live_intervals[ctx->vregs[insn->op1]];
	p = ival->use_pos;
	while (p) {
		if (p->pos == pos) {
			p->pos = load_pos;
			p->op_num = 1;
			p1 = p;
			break;
		}
		p = p->next;
	}

	ival = ctx->live_intervals[ctx->vregs[i]];
	p = ival->use_pos;
	while (p) {
		if (p->pos == load_pos) {
			p->hint_ref = insn->op1;
			break;
		}
		p = p->next;
	}

	if (insn->op2 > 0 && ctx->vregs[insn->op2]) {
		ival = ctx->live_intervals[ctx->vregs[insn->op2]];
		r = &ival->range;
		while (r) {
			if (r->end == load_pos) {
				r->end = pos;
				if (!r->next) {
					ival->end = pos;
				}
				break;
			}
			r = r->next;
		}
		p = ival->use_pos;
		while (p) {
			if (p->pos == load_pos) {
				p->pos = pos;
				p->op_num = 2;
				p2 = p;
				break;
			}
			p = p->next;
		}
	}
	if (p1 && p2) {
		uint8_t tmp = p1->flags;
		p1->flags = p2->flags;
		p2->flags = tmp;
	}
}

static int ir_hint_conflict(ir_ctx *ctx, ir_ref ref, int use, int def)
{
	ir_use_pos *p;
	ir_reg r1 = IR_REG_NONE;
	ir_reg r2 = IR_REG_NONE;

	p = ctx->live_intervals[use]->use_pos;
	while (p) {
		if (IR_LIVE_POS_TO_REF(p->pos) == ref) {
			break;
		}
		if (p->hint != IR_REG_NONE) {
			r1 = p->hint;
		}
		p = p->next;
	}

	p = ctx->live_intervals[def]->use_pos;
	while (p) {
		if (IR_LIVE_POS_TO_REF(p->pos) > ref) {
			if (p->hint != IR_REG_NONE) {
				r2 = p->hint;
				break;
			}
		}
		p = p->next;
	}
	return r1 != r2 && r1 != IR_REG_NONE && r2 != IR_REG_NONE;
}

static int ir_try_swap_operands(ir_ctx *ctx, ir_ref i, ir_insn *insn)
{
	if (ctx->vregs[insn->op1]
	 && ctx->vregs[insn->op1] != ctx->vregs[i]
	 && !ir_vregs_overlap(ctx, ctx->vregs[insn->op1], ctx->vregs[i])
	 && !ir_hint_conflict(ctx, i, ctx->vregs[insn->op1], ctx->vregs[i])) {
		/* pass */
	} else {
		if (ctx->vregs[insn->op2] && ctx->vregs[insn->op2] != ctx->vregs[i]) {
			ir_live_pos pos = IR_USE_LIVE_POS_FROM_REF(i);
			ir_live_pos load_pos = IR_LOAD_LIVE_POS_FROM_REF(i);
			ir_live_interval *ival = ctx->live_intervals[ctx->vregs[insn->op2]];
			ir_live_range *r = &ival->range;

			while (r) {
				if (r->end == pos) {
					r->end = load_pos;
					if (!r->next) {
						ival->end = load_pos;
					}
					if (!ir_vregs_overlap(ctx, ctx->vregs[insn->op2], ctx->vregs[i])
					 && !ir_hint_conflict(ctx, i, ctx->vregs[insn->op2], ctx->vregs[i])) {
						ir_swap_operands(ctx, i, insn);
						return 1;
					} else {
						r->end = pos;
						if (!r->next) {
							ival->end = pos;
						}
					}
					break;
				}
				r = r->next;
			}
		}
	}
	return 0;
}

int ir_coalesce(ir_ctx *ctx)
{
	uint32_t b, n, succ;
	ir_ref *p, use, input, k, j;
	ir_block *bb, *succ_bb;
	ir_use_list *use_list;
	ir_insn *insn;
	ir_worklist blocks;
	bool compact = 0;
	ir_live_range *unused = NULL;

	/* Collect a list of blocks which are predecossors to block with phi finctions */
	ir_worklist_init(&blocks, ctx->cfg_blocks_count + 1);
	for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		if (bb->predecessors_count > 1) {
			uint32_t i;

			use_list = &ctx->use_lists[bb->start];
			n = use_list->count;
			if (n > 1) {
				k = ir_variable_inputs_count(&ctx->ir_base[bb->start]) + 1;
				for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
					use = *p;
					insn = &ctx->ir_base[use];
					if (insn->op == IR_PHI) {
						for (j = 2; j <= k; j++) {
							ir_worklist_push(&blocks, ctx->cfg_edges[bb->predecessors + (j-2)]);
						}
					}
				}
			}
		}
	}

	qsort_r(blocks.l.a.refs, ir_worklist_len(&blocks), sizeof(ir_ref), ir_block_cmp, ctx);

	while (ir_worklist_len(&blocks)) {
		uint32_t i;

		b = ir_worklist_pop(&blocks);
		bb = &ctx->cfg_blocks[b];
		IR_ASSERT(bb->successors_count == 1);
		succ = ctx->cfg_edges[bb->successors];
		succ_bb = &ctx->cfg_blocks[succ];
		IR_ASSERT(succ_bb->predecessors_count > 1);
		k = ir_phi_input_number(ctx, succ_bb, b);
		use_list = &ctx->use_lists[succ_bb->start];
		n = use_list->count;
		for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
			use = *p;
			insn = &ctx->ir_base[use];
			if (insn->op == IR_PHI) {
				input = ir_insn_op(insn, k);
				if (input > 0) {
					if (!ir_try_coalesce(ctx, &unused, input, use)) {
						ir_add_phi_move(ctx, b, input, use);
					} else {
						compact = 1;
					}
				} else {
					/* Move for constant input */
					ir_add_phi_move(ctx, b, input, use);
				}
			}
		}
	}
	if (unused) {
		ir_free_live_ranges(unused);
	}
	ir_worklist_free(&blocks);

	if (ctx->rules) {
		/* try to swap operands of commutative instructions for better register allocation */
		for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
			ir_ref i;

			IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
			i = bb->end;

			/* skip last instruction */
			i = ctx->prev_ref[i];

			while (i != bb->start) {
				insn = &ctx->ir_base[i];
				if ((ir_op_flags[insn->op] & IR_OP_FLAG_COMMUTATIVE)
				 && ctx->vregs[i]
				 && ctx->live_intervals[ctx->vregs[i]]->use_pos
				 && (ctx->live_intervals[ctx->vregs[i]]->use_pos->flags & IR_DEF_REUSES_OP1_REG)
				 && insn->op2 > 0
				 && insn->op1 > 0
				 && insn->op1 != insn->op2) {
					ir_try_swap_operands(ctx, i, insn);
				}
				i = ctx->prev_ref[i];
			}
		}
	}

	if (compact) {
		ir_ref i, n;
		uint32_t *xlat = ir_mem_malloc((ctx->vregs_count + 1) * sizeof(uint32_t));

		for (i = 1, n = 1; i <= ctx->vregs_count; i++) {
			if (ctx->live_intervals[i]) {
				xlat[i] = n;
				if (i != n) {
					ctx->live_intervals[n] = ctx->live_intervals[i];
					ctx->live_intervals[n]->vreg = n;
				}
				n++;
			}
		}
		n--;
		if (n != ctx->vregs_count) {
			j = ctx->vregs_count - n;
			for (i = n + 1; i <= n + IR_REG_NUM + 1; i++) {
				ctx->live_intervals[i] = ctx->live_intervals[i + j];
				if (ctx->live_intervals[i]) {
					ctx->live_intervals[i]->vreg = i;
				}
			}
			for (j = 1; j < ctx->insns_count; j++) {
				if (ctx->vregs[j]) {
					ctx->vregs[j] = xlat[ctx->vregs[j]];
				}
			}
			ctx->vregs_count = n;
		}
		ir_mem_free(xlat);
	}

	return 1;
}

/* SSA Deconstruction */

int ir_compute_dessa_moves(ir_ctx *ctx)
{
	uint32_t b, i, n;
	ir_ref j, k, *p, use;
	ir_block *bb;
	ir_use_list *use_list;
	ir_insn *insn;

	for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		if (bb->predecessors_count > 1) {
			use_list = &ctx->use_lists[bb->start];
			n = use_list->count;
			if (n > 1) {
				k = ir_variable_inputs_count(&ctx->ir_base[bb->start]) + 1;
				for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
					use = *p;
					insn = &ctx->ir_base[use];
					if (insn->op == IR_PHI) {
						for (j = 2; j <= k; j++) {
							if (IR_IS_CONST_REF(ir_insn_op(insn, j)) || ctx->vregs[ir_insn_op(insn, j)] != ctx->vregs[use]) {
								int pred = ctx->cfg_edges[bb->predecessors + (j-2)];
								ctx->cfg_blocks[pred].flags |= IR_BB_DESSA_MOVES;
								ctx->flags |= IR_LR_HAVE_DESSA_MOVES;
							}
						}
					}
				}
			}
		}
	}
	return 1;
}

int ir_gen_dessa_moves(ir_ctx *ctx, uint32_t b, emit_copy_t emit_copy)
{
	uint32_t succ, k, n = 0;
	ir_block *bb, *succ_bb;
	ir_use_list *use_list;
	ir_ref *loc, *pred, i, *p, ref, input;
	ir_insn *insn;
	uint32_t len;
	ir_bitset todo, ready;

	bb = &ctx->cfg_blocks[b];
	if (!(bb->flags & IR_BB_DESSA_MOVES)) {
		return 0;
	}
	IR_ASSERT(bb->successors_count == 1);
	succ = ctx->cfg_edges[bb->successors];
	succ_bb = &ctx->cfg_blocks[succ];
	IR_ASSERT(succ_bb->predecessors_count > 1);
	use_list = &ctx->use_lists[succ_bb->start];

	k = ir_phi_input_number(ctx, succ_bb, b);

	loc = ir_mem_malloc(ctx->insns_count * 2 * sizeof(ir_ref));
	pred = loc + ctx->insns_count;
	len = ir_bitset_len(ctx->insns_count);
	todo = ir_bitset_malloc(ctx->insns_count);

	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < use_list->count; i++, p++) {
		ref = *p;
		insn = &ctx->ir_base[ref];
		if (insn->op == IR_PHI) {
			input = ir_insn_op(insn, k);
			if (IR_IS_CONST_REF(input)) {
				emit_copy(ctx, insn->type, input, ref);
			} else if (ctx->vregs[input] != ctx->vregs[ref]) {
				loc[ref] = pred[input] = 0;
				ir_bitset_incl(todo, ref);
				n++;
			}
		}
	}

	if (n == 0) {
		ir_mem_free(todo);
		ir_mem_free(loc);
		return 1;
	}

	ready = ir_bitset_malloc(ctx->insns_count);
	IR_BITSET_FOREACH(todo, len, ref) {
		insn = &ctx->ir_base[ref];
		IR_ASSERT(insn->op == IR_PHI);
		input = ir_insn_op(insn, k);
		loc[input] = input;
		pred[ref] = input;
	} IR_BITSET_FOREACH_END();

	IR_BITSET_FOREACH(todo, len, i) {
		if (!loc[i]) {
			ir_bitset_incl(ready, i);
		}
	} IR_BITSET_FOREACH_END();

	while ((i = ir_bitset_pop_first(todo, len)) >= 0) {
		ir_ref a, b, c;

		while ((b = ir_bitset_pop_first(ready, len)) >= 0) {
			a = pred[b];
			c = loc[a];
			emit_copy(ctx, ctx->ir_base[b].type, c, b);
			loc[a] = b;
			if (a == c && pred[a]) {
				ir_bitset_incl(ready, a);
			}
		}
		b = i;
		if (b != loc[pred[b]]) {
			emit_copy(ctx, ctx->ir_base[b].type, b, 0);
			loc[b] = 0;
			ir_bitset_incl(ready, b);
		}
	}

	ir_mem_free(ready);
	ir_mem_free(todo);
	ir_mem_free(loc);

	return 1;
}

/* Linear Scan Register Allocation */

#ifdef IR_DEBUG
# define IR_LOG_LSRA(action, ival, comment) do { \
		if (ctx->flags & IR_DEBUG_RA) { \
			ir_live_interval *_ival = (ival); \
			ir_live_pos _start = _ival->range.start; \
			ir_live_pos _end = ir_ival_end(_ival); \
			fprintf(stderr, action " R%d [%d.%d...%d.%d)" comment "\n", \
				_ival->vreg, \
				IR_LIVE_POS_TO_REF(_start), IR_LIVE_POS_TO_SUB_REF(_start), \
				IR_LIVE_POS_TO_REF(_end), IR_LIVE_POS_TO_SUB_REF(_end)); \
		} \
	} while (0)
# define IR_LOG_LSRA_ASSIGN(action, ival, comment) do { \
		if (ctx->flags & IR_DEBUG_RA) { \
			ir_live_interval *_ival = (ival); \
			ir_live_pos _start = _ival->range.start; \
			ir_live_pos _end = ir_ival_end(_ival); \
			fprintf(stderr, action " R%d [%d.%d...%d.%d) to %s" comment "\n", \
				_ival->vreg, \
				IR_LIVE_POS_TO_REF(_start), IR_LIVE_POS_TO_SUB_REF(_start), \
				IR_LIVE_POS_TO_REF(_end), IR_LIVE_POS_TO_SUB_REF(_end), \
				ir_reg_name(_ival->reg, _ival->type)); \
		} \
	} while (0)
# define IR_LOG_LSRA_SPLIT(ival, pos) do { \
		if (ctx->flags & IR_DEBUG_RA) { \
			ir_live_interval *_ival = (ival); \
			ir_live_pos _start = _ival->range.start; \
			ir_live_pos _end = ir_ival_end(_ival); \
			ir_live_pos _pos = (pos); \
			fprintf(stderr, "      ---- Split R%d [%d.%d...%d.%d) at %d.%d\n", \
				_ival->vreg, \
				IR_LIVE_POS_TO_REF(_start), IR_LIVE_POS_TO_SUB_REF(_start), \
				IR_LIVE_POS_TO_REF(_end), IR_LIVE_POS_TO_SUB_REF(_end), \
				IR_LIVE_POS_TO_REF(_pos), IR_LIVE_POS_TO_SUB_REF(_pos)); \
		} \
	} while (0)
# define IR_LOG_LSRA_CONFLICT(action, ival, pos) do { \
		if (ctx->flags & IR_DEBUG_RA) { \
			ir_live_interval *_ival = (ival); \
			ir_live_pos _start = _ival->range.start; \
			ir_live_pos _end = ir_ival_end(_ival); \
			ir_live_pos _pos = (pos); \
			fprintf(stderr, action " R%d [%d.%d...%d.%d) assigned to %s at %d.%d\n", \
				_ival->vreg, \
				IR_LIVE_POS_TO_REF(_start), IR_LIVE_POS_TO_SUB_REF(_start), \
				IR_LIVE_POS_TO_REF(_end), IR_LIVE_POS_TO_SUB_REF(_end), \
				ir_reg_name(_ival->reg, _ival->type), \
				IR_LIVE_POS_TO_REF(_pos), IR_LIVE_POS_TO_SUB_REF(_pos)); \
		} \
	} while (0)
#else
# define IR_LOG_LSRA(action, ival, comment)
# define IR_LOG_LSRA_ASSIGN(action, ival, comment)
# define IR_LOG_LSRA_SPLIT(ival, pos)
# define IR_LOG_LSRA_CONFLICT(action, ival, pos);
#endif

IR_ALWAYS_INLINE ir_live_pos ir_ival_end(ir_live_interval *ival)
{
#if 1
	return ival->end;
#else
	ir_live_range *live_range = &ival->range;

	while (live_range->next) {
		live_range = live_range->next;
	}
	return live_range->end;
#endif
}

#ifdef IR_DEBUG
static bool ir_ival_covers(ir_live_interval *ival, ir_live_pos position)
{
	ir_live_range *live_range = &ival->range;

	do {
		if (position < live_range->end) {
			return position >= live_range->start;
		}
		live_range = live_range->next;
	} while (live_range);

	return 0;
}
#endif

static bool ir_ival_has_hole_between(ir_live_interval *ival, ir_live_pos from, ir_live_pos to)
{
	ir_live_range *r = &ival->range;

	while (r) {
		if (from < r->start) {
			return 1;
		} else if (to <= r->end) {
			return 0;
		} else if (from >= r->end) {
			return 1;
		}
		r = r->next;
	}
	return 0;
}


static ir_live_pos ir_last_use_pos_before(ir_live_interval *ival, ir_live_pos pos, uint8_t flags)
{
	ir_live_pos ret = 0;
	ir_use_pos *p = ival->use_pos;

	while (p && p->pos <= pos) {
		if (p->flags & flags) {
			ret = p->pos;
		}
		p = p->next;
	}
	return ret;
}

static ir_live_pos ir_first_use_pos_after(ir_live_interval *ival, ir_live_pos pos, uint8_t flags)
{
	ir_use_pos *p = ival->use_pos;

	while (p && p->pos <= pos) {
		p = p->next;
	}
	while (p && !(p->flags & flags)) {
		p = p->next;
	}
	return p ? p->pos : 0x7fffffff;
}

static ir_block *ir_block_from_live_pos(ir_ctx *ctx, ir_live_pos pos)
{
	uint32_t b;
	ir_block *bb;
	ir_ref ref = IR_LIVE_POS_TO_REF(pos);

	// TODO: use binary search or map
	for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; b++, bb++) {
		if (ref >= bb->start && ref <= bb->end) {
			return bb;
		}
	}
	IR_ASSERT(0);
	return NULL;
}

static ir_live_pos ir_find_optimal_split_position(ir_ctx *ctx, ir_live_interval *ival, ir_live_pos min_pos, ir_live_pos max_pos, bool prefer_max)
{
	ir_block *min_bb, *max_bb;

	if (min_pos == max_pos) {
		return max_pos;
	}

	IR_ASSERT(min_pos < max_pos);
	IR_ASSERT(min_pos >= ival->range.start);
	IR_ASSERT(max_pos < ir_ival_end(ival));

	min_bb = ir_block_from_live_pos(ctx, min_pos);
	max_bb = ir_block_from_live_pos(ctx, max_pos);

	if (min_bb == max_bb
	 || ir_ival_has_hole_between(ival, min_pos, max_pos)) {  // TODO: ???
		return (prefer_max) ? max_pos : min_pos;
	}

	if (min_bb->loop_depth < max_bb->loop_depth) {
		/* Split at the end of the loop entry */
		do {
			if (max_bb->loop_header) {
				max_bb = &ctx->cfg_blocks[max_bb->loop_header];
			}
			max_bb = &ctx->cfg_blocks[ctx->cfg_edges[max_bb->predecessors]];
			IR_ASSERT(ir_ival_covers(ival, IR_DEF_LIVE_POS_FROM_REF(max_bb->end)));
		} while (min_bb->loop_depth < max_bb->loop_depth);

		return IR_DEF_LIVE_POS_FROM_REF(max_bb->end);
	}

	IR_ASSERT(min_bb->loop_depth == max_bb->loop_depth); // TODO: Can "min_bb" be in a deeper loop than "max_bb" ???

	return IR_LOAD_LIVE_POS_FROM_REF(max_bb->start);
}

static ir_live_interval *ir_split_interval_at(ir_ctx *ctx, ir_live_interval *ival, ir_live_pos pos)
{
	ir_live_interval *child;
	ir_live_range *p, *prev;
	ir_use_pos *use_pos, *prev_use_pos;

	IR_LOG_LSRA_SPLIT(ival, pos);
	IR_ASSERT(pos > ival->range.start);
	ctx->flags |= IR_RA_HAVE_SPLITS;

	p = &ival->range;
	prev = NULL;
	while (p && pos >= p->end) {
		prev = p;
		p = prev->next;
	}
	IR_ASSERT(p);

	if (pos < p->start) {
		/* split between ranges */
		pos = p->start;
	}

	use_pos = ival->use_pos;
	prev_use_pos = NULL;

	ival->flags &= ~IR_LIVE_INTERVAL_HAS_HINTS;
	if (p->start == pos) {
		while (use_pos && pos > use_pos->pos) {
			if (use_pos->hint != IR_REG_NONE || use_pos->hint_ref != 0) {
				ival->flags |= IR_LIVE_INTERVAL_HAS_HINTS;
			}
			prev_use_pos = use_pos;
			use_pos = use_pos->next;
		}
	} else {
		while (use_pos && pos >= use_pos->pos) {
			if (use_pos->hint != IR_REG_NONE || use_pos->hint_ref != 0) {
				ival->flags |= IR_LIVE_INTERVAL_HAS_HINTS;
			}
			prev_use_pos = use_pos;
			use_pos = use_pos->next;
		}
	}

	child = ir_mem_malloc(sizeof(ir_live_interval));
	child->type = ival->type;
	child->reg = IR_REG_NONE;
	child->flags = 0;
	child->vreg = ival->vreg;
	child->stack_spill_pos = -1; // not allocated
	child->range.start = pos;
	child->range.end = p->end;
	child->range.next = p->next;
	child->end = ival->end;
	child->use_pos = prev_use_pos ? prev_use_pos->next : use_pos;

	child->top = ival->top;
	child->next = ival->next;
	ival->next = child;

	if (pos == p->start) {
		prev->next = NULL;
		ival->end = prev->end;
		ir_mem_free(p);
	} else {
		p->end = ival->end = pos;
		p->next = NULL;
	}
	if (prev_use_pos) {
		prev_use_pos->next = NULL;
	} else {
		ival->use_pos = NULL;
	}

	use_pos = child->use_pos;
	while (use_pos) {
		if (use_pos->hint != IR_REG_NONE || use_pos->hint_ref != 0) {
			child->flags |= IR_LIVE_INTERVAL_HAS_HINTS;
		}
		use_pos = use_pos->next;
	}

	return child;
}

int32_t ir_allocate_spill_slot(ir_ctx *ctx, ir_type type, ir_reg_alloc_data *data)
{
	int32_t ret;
	uint8_t size = ir_type_size[type];

	if (size == 8) {
		ret = data->stack_frame_size;
		data->stack_frame_size += 8;
	} else if (size == 4) {
		if (data->unused_slot_4) {
			ret = data->unused_slot_4;
			data->unused_slot_4 = 0;
		} else {
			ret = data->stack_frame_size;
			if (sizeof(void*) == 8) {
				data->unused_slot_4 = data->stack_frame_size + 4;
				data->stack_frame_size += 8;
			} else {
				data->stack_frame_size += 4;
			}
		}
	} else if (size == 2) {
		if (data->unused_slot_2) {
			ret = data->unused_slot_2;
			data->unused_slot_2 = 0;
		} else if (data->unused_slot_4) {
			ret = data->unused_slot_4;
			data->unused_slot_2 = data->unused_slot_4 + 2;
			data->unused_slot_4 = 0;
		} else {
			ret = data->stack_frame_size;
			data->unused_slot_2 = data->stack_frame_size + 2;
			if (sizeof(void*) == 8) {
				data->unused_slot_4 = data->stack_frame_size + 4;
				data->stack_frame_size += 8;
			} else {
				data->stack_frame_size += 4;
			}
		}
	} else if (size == 1) {
		if (data->unused_slot_1) {
			ret = data->unused_slot_1;
			data->unused_slot_1 = 0;
		} else if (data->unused_slot_2) {
			ret = data->unused_slot_2;
			data->unused_slot_1 = data->unused_slot_2 + 1;
			data->unused_slot_2 = 0;
		} else if (data->unused_slot_4) {
			ret = data->unused_slot_4;
			data->unused_slot_1 = data->unused_slot_4 + 1;
			data->unused_slot_2 = data->unused_slot_4 + 2;
			data->unused_slot_4 = 0;
		} else {
			ret = data->stack_frame_size;
			data->unused_slot_1 = data->stack_frame_size + 1;
			data->unused_slot_2 = data->stack_frame_size + 2;
			if (sizeof(void*) == 8) {
				data->unused_slot_4 = data->stack_frame_size + 4;
				data->stack_frame_size += 8;
			} else {
				data->stack_frame_size += 4;
			}
		}
	} else {
		IR_ASSERT(0);
		ret = -1;
	}
	return ret;
}

static ir_reg ir_try_allocate_preferred_reg(ir_ctx *ctx, ir_live_interval *ival, ir_regset available, ir_live_pos *freeUntilPos)
{
	ir_use_pos *use_pos;
	ir_reg reg;

	use_pos = ival->use_pos;
	while (use_pos) {
		reg = use_pos->hint;
		if (reg >= 0 && IR_REGSET_IN(available, reg)) {
			if (ir_ival_end(ival) <= freeUntilPos[reg]) {
				/* register available for the whole interval */
				return reg;
			}
		}
		use_pos = use_pos->next;
	}

	use_pos = ival->use_pos;
	while (use_pos) {
		if (use_pos->hint_ref) {
			reg = ctx->live_intervals[ctx->vregs[use_pos->hint_ref]]->reg;
			if (reg >= 0 && IR_REGSET_IN(available, reg)) {
				if (ir_ival_end(ival) <= freeUntilPos[reg]) {
					/* register available for the whole interval */
					return reg;
				}
			}
		}
		use_pos = use_pos->next;
	}

	return IR_REG_NONE;
}

static ir_reg ir_get_preferred_reg(ir_ctx *ctx, ir_live_interval *ival, ir_regset available)
{
	ir_use_pos *use_pos;
	ir_reg reg;

	use_pos = ival->use_pos;
	while (use_pos) {
		reg = use_pos->hint;
		if (reg >= 0 && IR_REGSET_IN(available, reg)) {
			return reg;
		} else if (use_pos->hint_ref) {
			reg = ctx->live_intervals[ctx->vregs[use_pos->hint_ref]]->reg;
			if (reg >= 0 && IR_REGSET_IN(available, reg)) {
				return reg;
			}
		}
		use_pos = use_pos->next;
	}

	return IR_REG_NONE;
}

static void ir_add_to_unhandled(ir_live_interval **unhandled, ir_live_interval *ival)
{
	ir_live_pos pos = ival->range.start;

	if (*unhandled == NULL
	 || pos < (*unhandled)->range.start
	 || (pos == (*unhandled)->range.start
	  && (ival->flags & IR_LIVE_INTERVAL_HAS_HINTS)
	  && !((*unhandled)->flags & IR_LIVE_INTERVAL_HAS_HINTS))
	 || (pos == (*unhandled)->range.start
	  && ival->vreg > (*unhandled)->vreg)) {
		ival->list_next = *unhandled;
		*unhandled = ival;
	} else {
		ir_live_interval *prev = *unhandled;

		while (prev->list_next) {
			if (pos < prev->list_next->range.start
			 || (pos == prev->list_next->range.start
			  && (ival->flags & IR_LIVE_INTERVAL_HAS_HINTS)
			  && !(prev->list_next->flags & IR_LIVE_INTERVAL_HAS_HINTS))
			 || (pos == prev->list_next->range.start
			  && ival->vreg > prev->list_next->vreg)) {
				break;
			}
			prev = prev->list_next;
		}
		ival->list_next = prev->list_next;
		prev->list_next = ival;
	}
}

static void ir_add_to_unhandled_spill(ir_live_interval **unhandled, ir_live_interval *ival)
{
	ir_live_pos pos = ival->range.start;

	if (*unhandled == NULL
	 || pos <= (*unhandled)->range.start) {
		ival->list_next = *unhandled;
		*unhandled = ival;
	} else {
		ir_live_interval *prev = *unhandled;

		while (prev->list_next) {
			if (pos <= prev->list_next->range.start) {
				break;
			}
			prev = prev->list_next;
		}
		ival->list_next = prev->list_next;
		prev->list_next = ival;
	}
}

static ir_reg ir_try_allocate_free_reg(ir_ctx *ctx, ir_live_interval *ival, ir_live_interval **active, ir_live_interval *inactive, ir_live_interval **unhandled)
{
	ir_live_pos freeUntilPos[IR_REG_NUM];
	int i, reg;
	ir_live_pos pos, next;
	ir_live_interval *other;
	ir_regset available;

	if (IR_IS_TYPE_FP(ival->type)) {
		available = IR_REGSET_FP;
		/* set freeUntilPos of all physical registers to maxInt */
		for (i = IR_REG_FP_FIRST; i <= IR_REG_FP_LAST; i++) {
			freeUntilPos[i] = 0x7fffffff;
		}
	} else {
		available = IR_REGSET_GP;
		if (ctx->flags & IR_USE_FRAME_POINTER) {
			IR_REGSET_EXCL(available, IR_REG_FRAME_POINTER);
		}
		/* set freeUntilPos of all physical registers to maxInt */
		for (i = IR_REG_GP_FIRST; i <= IR_REG_GP_LAST; i++) {
			freeUntilPos[i] = 0x7fffffff;
		}
	}

	available = IR_REGSET_DIFFERENCE(available, (ir_regset)ctx->fixed_regset);

	/* for each interval it in active */
	other = *active;
	while (other) {
		/* freeUntilPos[it.reg] = 0 */
		reg = other->reg;
		IR_ASSERT(reg >= 0);
		if (reg == IR_REG_NUM) {
			ir_regset regset = IR_REGSET_INTERSECTION(available, IR_REGSET_SCRATCH);

			IR_REGSET_FOREACH(regset, reg) {
				freeUntilPos[reg] = 0;
			} IR_REGSET_FOREACH_END();
		} else if (IR_REGSET_IN(available, reg)) {
			freeUntilPos[reg] = 0;
		}
		other = other->list_next;
	}

	/* for each interval it in inactive intersecting with current
	 *
	 * This loop is not necessary for program in SSA form (see LSRA on SSA fig. 6),
	 * but it is still necessary after coalescing and splitting
	 */
	other = inactive;
	while (other) {
		/* freeUntilPos[it.reg] = next intersection of it with current */
		reg = other->reg;
		IR_ASSERT(reg >= 0);
		if (reg == IR_REG_NUM) {
			next = ir_ivals_overlap(&ival->range, other->current_range);
			if (next) {
				ir_regset regset = IR_REGSET_INTERSECTION(available, IR_REGSET_SCRATCH);

				IR_REGSET_FOREACH(regset, reg) {
					if (next < freeUntilPos[reg]) {
						freeUntilPos[reg] = next;
					}
				} IR_REGSET_FOREACH_END();
			}
		} else if (IR_REGSET_IN(available, reg)) {
			next = ir_ivals_overlap(&ival->range, other->current_range);
			if (next && next < freeUntilPos[reg]) {
				freeUntilPos[reg] = next;
			}
		}
		other = other->list_next;
	}

	/* Try to use hint */
	reg = ir_try_allocate_preferred_reg(ctx, ival, available, freeUntilPos);
	if (reg != IR_REG_NONE) {
		ival->reg = reg;
		IR_LOG_LSRA_ASSIGN("    ---- Assign", ival, " (hint available without spilling)");
		ival->list_next = *active;
		*active = ival;
		return reg;
	}

	/* reg = register with highest freeUntilPos */
	reg = IR_REG_NONE;
	pos = 0;
	IR_REGSET_FOREACH(available, i) {
		if (freeUntilPos[i] > pos) {
			pos = freeUntilPos[i];
			reg = i;
		} else if (freeUntilPos[i] == pos
				&& !IR_REGSET_IN(IR_REGSET_SCRATCH, reg)
				&& IR_REGSET_IN(IR_REGSET_SCRATCH, i)) {
			/* prefer caller-saved registers to avoid save/restore in prologue/epilogue */
			pos = freeUntilPos[i];
			reg = i;
		}
	} IR_REGSET_FOREACH_END();

	if (!pos) {
		/* no register available without spilling */
		return IR_REG_NONE;
	} else if (ir_ival_end(ival) <= pos) {
		/* register available for the whole interval */
		ival->reg = reg;
		IR_LOG_LSRA_ASSIGN("    ---- Assign", ival, " (available without spilling)");
		ival->list_next = *active;
		*active = ival;
		return reg;
	} else if (pos > ival->range.start) {
		/* register available for the first part of the interval */
		/* split current before freeUntilPos[reg] */
		ir_live_pos split_pos = ir_last_use_pos_before(ival, pos,
			IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG);
		if (split_pos > ival->range.start) {
			ir_reg pref_reg;

			split_pos = ir_find_optimal_split_position(ctx, ival, split_pos, pos, 0);
			other = ir_split_interval_at(ctx, ival, split_pos);
			pref_reg = ir_try_allocate_preferred_reg(ctx, ival, available, freeUntilPos);
			if (pref_reg != IR_REG_NONE) {
				ival->reg = pref_reg;
			} else {
				ival->reg = reg;
			}
			IR_LOG_LSRA_ASSIGN("    ---- Assign", ival, " (available without spilling for the first part)");
			ival->list_next = *active;
			*active = ival;
			ir_add_to_unhandled(unhandled, other);
			IR_LOG_LSRA("      ---- Queue", other, "");
			return reg;
		}
	}
	return IR_REG_NONE;
}

static ir_reg ir_allocate_blocked_reg(ir_ctx *ctx, ir_live_interval *ival, ir_live_interval **active, ir_live_interval *inactive, ir_live_interval **unhandled)
{
	ir_live_pos nextUsePos[IR_REG_NUM];
	ir_live_pos blockPos[IR_REG_NUM];
	int i, reg;
	ir_live_pos pos, next_use_pos;
	ir_live_interval *other, *prev;
	ir_use_pos *use_pos;
	ir_regset available;

	if (!(ival->flags & IR_LIVE_INTERVAL_TEMP)) {
		use_pos = ival->use_pos;
		while (use_pos && !(use_pos->flags & IR_USE_MUST_BE_IN_REG)) {
			use_pos = use_pos->next;
		}
		if (!use_pos) {
			/* spill */
			IR_LOG_LSRA("    ---- Spill", ival, " (no use pos that must be in reg)");
			ctx->flags |= IR_RA_HAVE_SPILLS;
			return IR_REG_NONE;
		}
		next_use_pos = use_pos->pos;
	} else {
		next_use_pos = ival->range.end;
	}

	if (IR_IS_TYPE_FP(ival->type)) {
		available = IR_REGSET_FP;
		/* set nextUsePos of all physical registers to maxInt */
		for (i = IR_REG_FP_FIRST; i <= IR_REG_FP_LAST; i++) {
			nextUsePos[i] = 0x7fffffff;
			blockPos[i] = 0x7fffffff;
		}
	} else {
		available = IR_REGSET_GP;
		if (ctx->flags & IR_USE_FRAME_POINTER) {
			IR_REGSET_EXCL(available, IR_REG_FRAME_POINTER);
		}
		/* set nextUsePos of all physical registers to maxInt */
		for (i = IR_REG_GP_FIRST; i <= IR_REG_GP_LAST; i++) {
			nextUsePos[i] = 0x7fffffff;
			blockPos[i] = 0x7fffffff;
		}
	}

	available = IR_REGSET_DIFFERENCE(available, (ir_regset)ctx->fixed_regset);

	if (IR_REGSET_IS_EMPTY(available)) {
		fprintf(stderr, "LSRA Internal Error: No registers available. Allocation is not possible\n");
		IR_ASSERT(0);
		exit(-1);
	}

	/* for each interval it in active */
	other = *active;
	while (other) {
		/* nextUsePos[it.reg] = next use of it after start of current */
		reg = other->reg;
		IR_ASSERT(reg >= 0);
		if (reg == IR_REG_NUM) {
			ir_regset regset = IR_REGSET_INTERSECTION(available, IR_REGSET_SCRATCH);

			IR_REGSET_FOREACH(regset, reg) {
				blockPos[reg] = nextUsePos[reg] = 0;
			} IR_REGSET_FOREACH_END();
		} else if (IR_REGSET_IN(available, reg)) {
			if (other->flags & (IR_LIVE_INTERVAL_FIXED|IR_LIVE_INTERVAL_TEMP)) {
				blockPos[reg] = nextUsePos[reg] = 0;
			} else {
				pos = ir_first_use_pos_after(other, ival->range.start,
					IR_USE_MUST_BE_IN_REG /* | IR_USE_SHOULD_BE_IN_REG */); // TODO: ???
				if (pos < nextUsePos[reg]) {
					nextUsePos[reg] = pos;
				}
			}
		}
		other = other->list_next;
	}

	/* for each interval it in inactive intersecting with current */
	other = inactive;
	while (other) {
		/* freeUntilPos[it.reg] = next intersection of it with current */
		reg = other->reg;
		IR_ASSERT(reg >= 0);
		if (reg == IR_REG_NUM) {
			ir_live_pos overlap = ir_ivals_overlap(&ival->range, other->current_range);

			if (overlap) {
				ir_regset regset = IR_REGSET_INTERSECTION(available, IR_REGSET_SCRATCH);

				IR_REGSET_FOREACH(regset, reg) {
					if (overlap < nextUsePos[reg]) {
						nextUsePos[reg] = overlap;
					}
					if (overlap < blockPos[reg]) {
						blockPos[reg] = overlap;
					}
				} IR_REGSET_FOREACH_END();
			}
		} else if (IR_REGSET_IN(available, reg)) {
			ir_live_pos overlap = ir_ivals_overlap(&ival->range, other->current_range);

			if (overlap) {
				if (other->flags & (IR_LIVE_INTERVAL_FIXED|IR_LIVE_INTERVAL_TEMP)) {
					if (overlap < nextUsePos[reg]) {
						nextUsePos[reg] = overlap;
					}
					if (overlap < blockPos[reg]) {
						blockPos[reg] = overlap;
					}
				} else {
					pos = ir_first_use_pos_after(other, ival->range.start,
						IR_USE_MUST_BE_IN_REG /* | IR_USE_SHOULD_BE_IN_REG */); // TODO: ???
					if (pos < nextUsePos[reg]) {
						nextUsePos[reg] = pos;
					}
				}
			}
		}
		other = other->list_next;
	}

	/* register hinting */
	reg = ir_get_preferred_reg(ctx, ival, available);
	if (reg == IR_REG_NONE) {
		reg = IR_REGSET_FIRST(available);
	}

	/* reg = register with highest nextUsePos */
	IR_REGSET_EXCL(available, reg);
	pos = nextUsePos[reg];
	IR_REGSET_FOREACH(available, i) {
		if (nextUsePos[i] > pos) {
			pos = nextUsePos[i];
			reg = i;
		}
	} IR_REGSET_FOREACH_END();

	/* if first usage of current is after nextUsePos[reg] then */
	if (next_use_pos > pos && !(ival->flags & IR_LIVE_INTERVAL_TEMP)) {
		/* all other intervals are used before current, so it is best to spill current itself */
		/* assign spill slot to current */
		/* split current before its first use position that requires a register */
		ir_live_pos split_pos;

spill_current:
		if (next_use_pos == ival->range.start) {
			IR_ASSERT(ival->use_pos && ival->use_pos->op_num == 0);
			/* split right after definition */
			split_pos = next_use_pos + 1;
		} else {
			split_pos = ir_find_optimal_split_position(ctx, ival, ival->range.start, next_use_pos - 1, 1);
		}

		if (split_pos > ival->range.start) {
			IR_LOG_LSRA("    ---- Conflict with others", ival, " (all others are used before)");
			other = ir_split_interval_at(ctx, ival, split_pos);
			IR_LOG_LSRA("    ---- Spill", ival, "");
			ir_add_to_unhandled(unhandled, other);
			IR_LOG_LSRA("    ---- Queue", other, "");
			return IR_REG_NONE;
		}
	}

	if (ir_ival_end(ival) > blockPos[reg]) {
		/* spilling make a register free only for the first part of current */
		IR_LOG_LSRA("    ---- Conflict with others", ival, " (spilling make a register free only for the first part)");
		/* split current at optimal position before block_pos[reg] */
		ir_live_pos split_pos = ir_last_use_pos_before(ival,  blockPos[reg] + 1,
			IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG);
		if (split_pos == 0) {
			split_pos = ir_first_use_pos_after(ival, blockPos[reg],
				IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG) - 1;
			other = ir_split_interval_at(ctx, ival, split_pos);
			ir_add_to_unhandled(unhandled, other);
			IR_LOG_LSRA("      ---- Queue", other, "");
			return IR_REG_NONE;
		}
		split_pos = ir_find_optimal_split_position(ctx, ival, split_pos, blockPos[reg], 1);
		other = ir_split_interval_at(ctx, ival, split_pos);
		ir_add_to_unhandled(unhandled, other);
		IR_LOG_LSRA("      ---- Queue", other, "");
	}

	/* spill intervals that currently block reg */
	prev = NULL;
	other = *active;
	while (other) {
		ir_live_pos split_pos;

		if (reg == other->reg) {
			/* split active interval for reg at position */
			ir_live_pos overlap = ir_ivals_overlap(&ival->range, other->current_range);

			if (overlap) {
				ir_live_interval *child, *child2;

				IR_ASSERT(other->type != IR_VOID);
				IR_LOG_LSRA_CONFLICT("      ---- Conflict with active", other, overlap);

				split_pos = ir_last_use_pos_before(other, ival->range.start, IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG);
				if (split_pos == 0) {
					split_pos = ival->range.start;
				}
				split_pos = ir_find_optimal_split_position(ctx, other, split_pos, ival->range.start, 1);
				if (split_pos > other->range.start) {
					child = ir_split_interval_at(ctx, other, split_pos);
					IR_LOG_LSRA("      ---- Finish", other, "");
				} else {
					if (ir_first_use_pos_after(other, other->range.start, IR_USE_MUST_BE_IN_REG) < ir_ival_end(other)) {
						if (next_use_pos > ival->range.start && !(ival->flags & IR_LIVE_INTERVAL_TEMP)) {
							goto spill_current;
                        }
						fprintf(stderr, "LSRA Internal Error: Unsolvable conflict. Allocation is not possible\n");
						IR_ASSERT(0);
						exit(-1);
					}
					child = other;
					other->reg = IR_REG_NONE;
					if (prev) {
						prev->list_next = other->list_next;
					} else {
						*active = other->list_next;
					}
					IR_LOG_LSRA("      ---- Spill and Finish", other, " (it must not be in reg)");
				}

				split_pos = ir_first_use_pos_after(child, ival->range.start, IR_USE_MUST_BE_IN_REG | IR_USE_SHOULD_BE_IN_REG) - 1; // TODO: ???
				if (split_pos > child->range.start && split_pos < ir_ival_end(child)) {
					ir_live_pos opt_split_pos = ir_find_optimal_split_position(ctx, child, ival->range.start, split_pos, 1);
					if (opt_split_pos > child->range.start) {
						split_pos = opt_split_pos;
					}
					child2 = ir_split_interval_at(ctx, child, split_pos);
					IR_LOG_LSRA("      ---- Spill", child, "");
					ir_add_to_unhandled(unhandled, child2);
					IR_LOG_LSRA("      ---- Queue", child2, "");
				} else if (child != other) {
					// TODO: this may cause endless loop
					ir_add_to_unhandled(unhandled, child);
					IR_LOG_LSRA("      ---- Queue", child, "");
				}
			}
			break;
		}
		prev = other;
		other = other->list_next;
	}

	/* split any inactive interval for reg at the end of its lifetime hole */
	other = inactive;
	while (other) {
		/* freeUntilPos[it.reg] = next intersection of it with current */
		if (reg == other->reg) {
			ir_live_pos overlap = ir_ivals_overlap(&ival->range, other->current_range);

			if (overlap) {
				ir_live_interval *child;

				IR_ASSERT(other->type != IR_VOID);
				IR_LOG_LSRA_CONFLICT("      ---- Conflict with inactive", other, overlap);
				// TODO: optimal split position (this case is not tested)
				child = ir_split_interval_at(ctx, other, overlap);
				ir_add_to_unhandled(unhandled, child);
				IR_LOG_LSRA("      ---- Queue", child, "");
			}
		}
		other = other->list_next;
	}

	/* current.reg = reg */
	ival->reg = reg;
	IR_LOG_LSRA_ASSIGN("    ---- Assign", ival, " (after splitting others)");
	ival->list_next = *active;
	*active = ival;

	return reg;
}

static int ir_fix_dessa_tmps(ir_ctx *ctx, uint8_t type, ir_ref from, ir_ref to)
{
	ir_block *bb = ctx->data;
	ir_tmp_reg tmp_reg;

	if (to == 0) {
		if (IR_IS_TYPE_INT(type)) {
			tmp_reg.num = 0;
			tmp_reg.type = type;
			tmp_reg.start = IR_DEF_SUB_REF;
			tmp_reg.end = IR_SAVE_SUB_REF;
		} else if (IR_IS_TYPE_FP(type)) {
			tmp_reg.num = 2;
			tmp_reg.type = type;
			tmp_reg.start = IR_DEF_SUB_REF;
			tmp_reg.end = IR_SAVE_SUB_REF;
		} else {
			IR_ASSERT(0);
			return 0;
		}
	} else if (from != 0) {
		if (IR_IS_TYPE_INT(type)) {
			tmp_reg.num = 1;
			tmp_reg.type = type;
			tmp_reg.start = IR_DEF_SUB_REF;
			tmp_reg.end = IR_SAVE_SUB_REF;
		} else if (IR_IS_TYPE_FP(type)) {
			tmp_reg.num = 3;
			tmp_reg.type = type;
			tmp_reg.start = IR_DEF_SUB_REF;
			tmp_reg.end = IR_SAVE_SUB_REF;
		} else {
			IR_ASSERT(0);
			return 0;
		}
	} else {
		return 1;
	}
	if (!ir_has_tmp(ctx, bb->end, tmp_reg.num)) {
		ir_add_tmp(ctx, bb->end, tmp_reg);
	}
	return 1;
}

static bool ir_ival_spill_for_fuse_load(ir_ctx *ctx, ir_live_interval *ival, ir_reg_alloc_data *data)
{
	ir_use_pos *use_pos = ival->use_pos;
	ir_insn *insn;

	if (ival->flags & IR_LIVE_INTERVAL_MEM_PARAM) {
		IR_ASSERT(ival->top == ival && !ival->next && use_pos && use_pos->op_num == 0);
		insn = &ctx->ir_base[IR_LIVE_POS_TO_REF(use_pos->pos)];
		IR_ASSERT(insn->op == IR_PARAM);
		use_pos =use_pos->next;
		if (use_pos && (use_pos->next || (use_pos->flags & IR_USE_MUST_BE_IN_REG))) {
			return 0;
		}

		if (use_pos) {
			ir_block *bb = ir_block_from_live_pos(ctx, use_pos->pos);
			if (bb->loop_depth) {
				return 0;
			}
		}

		return 1;
	} else if (ival->flags & IR_LIVE_INTERVAL_MEM_LOAD) {
		ir_live_interval *var_ival;

		insn = &ctx->ir_base[IR_LIVE_POS_TO_REF(use_pos->pos)];
		IR_ASSERT(insn->op == IR_VLOAD);
		use_pos =use_pos->next;
		if (use_pos && (use_pos->next || (use_pos->flags & IR_USE_MUST_BE_IN_REG))) {
			return 0;
		}

		if (use_pos) {
			ir_block *bb = ir_block_from_live_pos(ctx, use_pos->pos);
			if (bb->loop_depth && bb != ir_block_from_live_pos(ctx, ival->use_pos->pos)) {
				return 0;
			}
		}

		IR_ASSERT(ctx->ir_base[insn->op2].op == IR_VAR);
		var_ival = ctx->live_intervals[ctx->vregs[insn->op2]];
		if (var_ival->stack_spill_pos == -1) {
			var_ival->stack_spill_pos = ir_allocate_spill_slot(ctx, var_ival->type, data);
		}
		ival->stack_spill_pos = var_ival->stack_spill_pos;

		return 1;
	}
	return 0;
}

static void ir_assign_bound_spill_slots(ir_ctx *ctx)
{
	ir_hashtab_bucket *b = ctx->binding->data;
	uint32_t n = ctx->binding->count;
	uint32_t v;
	ir_live_interval *ival;

	while (n > 0) {
		v = ctx->vregs[b->key];
		if (v) {
			ival = ctx->live_intervals[v];
			if (ival
			 && ival->stack_spill_pos == -1
			 && (ival->next || ival->reg == IR_REG_NONE)) {
				if (b->val < 0) {
					/* special spill slot */
					ival->stack_spill_pos = -b->val;
					ival->flags |= IR_LIVE_INTERVAL_SPILL_SPECIAL;
				} else {
					/* node is bound to VAR node */
					ir_live_interval *var_ival;

					IR_ASSERT(ctx->ir_base[b->val].op == IR_VAR);
					var_ival = ctx->live_intervals[ctx->vregs[b->val]];
					if (var_ival->stack_spill_pos == -1) {
						var_ival->stack_spill_pos = ir_allocate_spill_slot(ctx, var_ival->type, ctx->data);
					}
					ival->stack_spill_pos = var_ival->stack_spill_pos;
				}
			}
		}
		b++;
		n--;
	}
}

static int ir_linear_scan(ir_ctx *ctx)
{
	uint32_t b;
	ir_block *bb;
	ir_live_interval *unhandled = NULL;
	ir_live_interval *active = NULL;
	ir_live_interval *inactive = NULL;
	ir_live_interval *ival, *other, *prev;
	int j;
	ir_live_pos position;
	ir_reg reg;
	ir_reg_alloc_data data;

	if (!ctx->live_intervals) {
		return 0;
	}

	if (ctx->flags & IR_LR_HAVE_DESSA_MOVES) {
		/* Add fixed intervals for temporary registers used for DESSA moves */
		for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
			IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
			if (bb->flags & IR_BB_DESSA_MOVES) {
				ctx->data = bb;
				ir_gen_dessa_moves(ctx, b, ir_fix_dessa_tmps);
			}
		}
	}

	ctx->data = &data;
	data.stack_frame_size = 0;
	data.unused_slot_4 = 0;
	data.unused_slot_2 = 0;
	data.unused_slot_1 = 0;

	if (ctx->flags & IR_LR_HAVE_VARS) {
		for (j = 0; j <= ctx->vregs_count; j++) {
			ival = ctx->live_intervals[j];
			if (ival) {
				if (ival->flags & IR_LIVE_INTERVAL_VAR) {
					if (ival->stack_spill_pos == -1) {
						ival->stack_spill_pos = ir_allocate_spill_slot(ctx, ival->type, &data);
					}
				}
			}
		}
	}

	for (j = ctx->vregs_count; j != 0; j--) {
		ival = ctx->live_intervals[j];
		if (ival) {
			if (ival->flags & IR_LIVE_INTERVAL_VAR) {
				/* pass */
			} else if (ival->flags & IR_LIVE_INTERVAL_REG_LOAD) {
				/* pre-allocated fixed register */
				if (!IR_REGSET_IN(ctx->fixed_regset, ival->reg)) {
					ival->current_range = &ival->range;
					ival->list_next = inactive;
					inactive = ival;
				}
			} else if (!(ival->flags & (IR_LIVE_INTERVAL_MEM_PARAM|IR_LIVE_INTERVAL_MEM_LOAD))
					|| !ir_ival_spill_for_fuse_load(ctx, ival, &data)) {
				ir_add_to_unhandled(&unhandled, ival);
			}
		}
	}

	ival = ctx->live_intervals[0];
	while (ival) {
		ir_add_to_unhandled(&unhandled, ival);
		ival = ival->next;
	}

	for (j = ctx->vregs_count + 1; j <= ctx->vregs_count + IR_REG_NUM + 1; j++) {
		ival = ctx->live_intervals[j];
		if (ival) {
			ival->current_range = &ival->range;
			ival->list_next = inactive;
			inactive = ival;
		}
	}

	ctx->flags &= ~(IR_RA_HAVE_SPLITS|IR_RA_HAVE_SPILLS);

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_RA) {
		fprintf(stderr, "----\n");
		ir_dump_live_ranges(ctx, stderr);
		fprintf(stderr, "---- Start LSRA\n");
	}
#endif

	while (unhandled) {
		ival = unhandled;
		ival->current_range = &ival->range;
		unhandled = ival->list_next;
		position = ival->range.start;

		IR_LOG_LSRA("  ---- Processing", ival, "...");

		/* for each interval i in active */
		other = active;
		prev = NULL;
		while (other) {
			ir_live_range *r = other->current_range;

			if (r && r->end <= position) {
				do {
					r = r->next;
				} while (r && r->end <= position);
				other->current_range = r;
			}
			/* if (ir_ival_end(other) <= position) {*/
			if (!r) {
				/* move i from active to handled */
				if (prev) {
					prev->list_next = other->list_next;
				} else {
					active = other->list_next;
				}
			} else if (position < r->start) {
				/* move i from active to inactive */
				if (prev) {
					prev->list_next = other->list_next;
				} else {
					active = other->list_next;
				}
				other->list_next = inactive;
				inactive = other;
			} else {
				prev = other;
			}
			other = prev ? prev->list_next : active;
		}

		/* for each interval i in inactive */
		other = inactive;
		prev = NULL;
		while (other) {
			ir_live_range *r = other->current_range;

			if (r  && r->end <= position) {
				do {
					r = r->next;
				} while (r && r->end <= position);
				other->current_range = r;
			}
			/* if (ir_ival_end(other) <= position) {*/
			if (!r) {
				/* move i from inactive to handled */
				if (prev) {
					prev->list_next = other->list_next;
				} else {
					inactive = other->list_next;
				}
			/* } else if (ir_ival_covers(other, position)) {*/
			} else if (position >= r->start) {
				/* move i from active to inactive */
				if (prev) {
					prev->list_next = other->list_next;
				} else {
					inactive = other->list_next;
				}
				other->list_next = active;
				active = other;
			} else {
				prev = other;
			}
			other = prev ? prev->list_next : inactive;
		}

		reg = ir_try_allocate_free_reg(ctx, ival, &active, inactive, &unhandled);
		if (reg == IR_REG_NONE) {
			reg = ir_allocate_blocked_reg(ctx, ival, &active, inactive, &unhandled);
		}
	}

#if 0 //def IR_DEBUG
	/* all intervals must be processed */
	ival = active;
	while (ival) {
		IR_ASSERT(!ival->next);
		ival = ival->list_next;
	}
	ival = inactive;
	while (ival) {
		IR_ASSERT(!ival->next);
		ival = ival->list_next;
	}
#endif

	if (ctx->flags & (IR_RA_HAVE_SPLITS|IR_RA_HAVE_SPILLS)) {

		if (ctx->binding) {
			ir_assign_bound_spill_slots(ctx);
		}

		/* Use simple linear-scan (without holes) to allocate and reuse spill slots */
		unhandled = NULL;
		for (j = ctx->vregs_count; j != 0; j--) {
			ival = ctx->live_intervals[j];
			if (ival
			 && (ival->next || ival->reg == IR_REG_NONE)
			 && ival->stack_spill_pos == -1
			 && !(ival->flags & IR_LIVE_INTERVAL_MEM_PARAM)) {
				ir_live_range *r;

				other = ival;
				while (other->next) {
					other = other->next;
				}
				r = &other->range;
				while (r->next) {
					r = r->next;
				}
				ival->end = r->end;
				ir_add_to_unhandled_spill(&unhandled, ival);
			}
		}

		if (unhandled) {
			uint8_t size;
			ir_live_interval *handled[9] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

			active = NULL;
			while (unhandled) {
				ival = unhandled;
				ival->current_range = &ival->range;
				unhandled = ival->list_next;
				position = ival->range.start;

				/* for each interval i in active */
				other = active;
				prev = NULL;
				while (other) {
					if (other->end <= position) {
						/* move i from active to handled */
						if (prev) {
							prev->list_next = other->list_next;
						} else {
							active = other->list_next;
						}
						size = ir_type_size[other->type];
						IR_ASSERT(size == 1 || size == 2 || size == 4 || size == 8);
						other->list_next = handled[size];
						handled[size] = other;
					} else {
						prev = other;
					}
					other = prev ? prev->list_next : active;
				}

				size = ir_type_size[ival->type];
				IR_ASSERT(size == 1 || size == 2 || size == 4 || size == 8);
				if (handled[size] != NULL) {
					ival->stack_spill_pos = handled[size]->stack_spill_pos;
					handled[size] = handled[size]->list_next;
				} else {
					ival->stack_spill_pos = ir_allocate_spill_slot(ctx, ival->type, &data);
				}
				ival->list_next = active;
				active = ival;
			}
		}
	}

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_RA) {
		fprintf(stderr, "---- Finish LSRA\n");
		ir_dump_live_ranges(ctx, stderr);
		fprintf(stderr, "----\n");
	}
#endif

	return 1;
}

static void assign_regs(ir_ctx *ctx)
{
	ir_ref i;
	ir_live_interval *ival;
	ir_use_pos *use_pos;
	int8_t reg;
	ir_ref ref;

	ctx->regs = ir_mem_malloc(sizeof(ir_regs) * ctx->insns_count);
	memset(ctx->regs, IR_REG_NONE, sizeof(ir_regs) * ctx->insns_count);

	for (i = 1; i <= ctx->vregs_count; i++) {
		ival = ctx->live_intervals[i];
		if (ival) {
			do {
				if (ival->reg >= 0) {
					use_pos = ival->use_pos;
					while (use_pos) {
						ref = IR_LIVE_POS_TO_REF(use_pos->pos);
						reg = ival->reg;
						if (use_pos->op_num == 0
						 && (use_pos->flags & IR_DEF_REUSES_OP1_REG)
						 && ctx->regs[ref][1] != IR_REG_NONE
						 && (ctx->regs[ref][1] & IR_REG_SPILL_LOAD)
						 && (ctx->regs[ref][2] == IR_REG_NONE || IR_REG_NUM(ctx->regs[ref][2]) != reg)
						 && (ctx->regs[ref][3] == IR_REG_NONE || IR_REG_NUM(ctx->regs[ref][3]) != reg)) {
							/* load op1 directly into result (valid only when op1 register is not reused) */
							ctx->regs[ref][1] = reg | IR_REG_SPILL_LOAD;
						}
						if (ival->top->stack_spill_pos != -1) {
							// TODO: Insert spill loads and stotres in optimal positons (resolution)

							if (use_pos->op_num == 0) {
								reg |= IR_REG_SPILL_STORE;
							} else {
								if ((use_pos->flags & IR_USE_MUST_BE_IN_REG)
								 || ctx->ir_base[ref].op == IR_CALL
								 || ctx->ir_base[ref].op == IR_TAILCALL
								 || (use_pos->op_num == 2
								  && ctx->ir_base[ref].op1 == ctx->ir_base[ref].op2
								  && IR_REG_NUM(ctx->regs[ref][1]) == reg)) {
									reg |= IR_REG_SPILL_LOAD;
								} else {
									/* fuse spill load (valid only when register is not reused) */
									reg = IR_REG_NONE;
								}
							}
						}
						if (use_pos->flags & IR_PHI_USE) {
							IR_ASSERT(use_pos->hint_ref > 0);
							ref = use_pos->hint_ref;
							IR_ASSERT(use_pos->op_num <= IR_MAX(3, ir_input_edges_count(ctx, &ctx->ir_base[ref])));
							ctx->regs[ref][use_pos->op_num] = reg;
						} else {
							IR_ASSERT(use_pos->op_num <= IR_MAX(3, ir_input_edges_count(ctx, &ctx->ir_base[ref])));
							ctx->regs[ref][use_pos->op_num] = reg;
						}
						use_pos = use_pos->next;
					}
				}
				ival = ival->next;
			} while (ival);
		}
	}

	/* Temporary registers */
	ival = ctx->live_intervals[0];
	if (ival) {
		do {
			IR_ASSERT(ival->reg != IR_REG_NONE);
			ctx->regs[IR_LIVE_POS_TO_REF(ival->range.start)][ival->flags & IR_LIVE_INTERVAL_TEMP_NUM_MASK] = ival->reg;
			ival = ival->next;
		} while (ival);
	}
}

static void ir_add_hint(ir_ctx *ctx, ir_ref ref, ir_live_pos pos, ir_reg hint)
{
	ir_live_interval *ival = ctx->live_intervals[ctx->vregs[ref]];
	ir_use_pos *use_pos = ival->use_pos;

	while (use_pos) {
		if (use_pos->pos == pos) {
			if (use_pos->hint == IR_REG_NONE) {
				use_pos->hint = hint;
			}
		}
		use_pos = use_pos->next;
	}
}

static void ir_hint_propagation(ir_ctx *ctx)
{
	int i;
	ir_live_interval *ival;
	ir_use_pos *use_pos;
	ir_use_pos *hint_use_pos;

	for (i = 1; i <= ctx->vregs_count; i++) {
		ival = ctx->live_intervals[i];
		if (ival) {
			use_pos = ival->use_pos;
			hint_use_pos = NULL;
			while (use_pos) {
				if (use_pos->hint_ref) {
					hint_use_pos = use_pos;
				} else if (use_pos->hint != IR_REG_NONE) {
					if (hint_use_pos) {
						if (use_pos->op_num != 0) {
							ir_add_hint(ctx, hint_use_pos->hint_ref, hint_use_pos->pos, use_pos->hint);
						}
						hint_use_pos = NULL;
					}
				}
				use_pos = use_pos->next;
			}
		}
	}
}

int ir_reg_alloc(ir_ctx *ctx)
{
	ir_hint_propagation(ctx);
	if (ir_linear_scan(ctx)) {
		assign_regs(ctx);
		return 1;
	}
	return 0;
}
