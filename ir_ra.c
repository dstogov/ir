#define _GNU_SOURCE

#include "ir.h"
#include "ir_private.h"
#include <stdlib.h>

#include "ir_x86.h"

#ifdef IR_DEBUG
uint32_t debug_regset = 0xffffffff; /* all 32 regisers */
#endif

/* RA - Register Allocation, Liveness, Coalescing and SSA Resolution */

int ir_assign_virtual_registers(ir_ctx *ctx)
{
	uint32_t *vregs;
	uint32_t vregs_count = 0;
	int b, i, n;
	ir_block *bb;
	ir_insn *insn;
	uint32_t flags;

	/* Assign unique virtual register to each data node */
	if (!ctx->prev_insn_len) {
		ctx->prev_insn_len = ir_mem_malloc(ctx->insns_count * sizeof(uint32_t));
	}
	vregs = ir_mem_calloc(ctx->insns_count, sizeof(ir_ref));
	for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
		n = b; /* The first insn of BB keeps BB number in prev_insn_len[] */
		for (i = bb->start, insn = ctx->ir_base + i; i <= bb->end;) {
			ctx->prev_insn_len[i] = n; /* The first insn of BB keeps BB number in prev_insn_len[] */
			flags = ir_op_flags[insn->op];
			if ((flags & IR_OP_FLAG_DATA) || ((flags & IR_OP_FLAG_MEM) && insn->type != IR_VOID)) {
				if ((insn->op == IR_PARAM || insn->op == IR_VAR) && ctx->use_lists[i].count == 0) {
					/* pass */
				} else if (insn->op == IR_VAR && ctx->use_lists[i].count > 0) {
					vregs[i] = ++vregs_count; /* for spill slot */
				} else if (!ctx->rules || ir_needs_vreg(ctx, i)) {
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

/* Lifetime intervals construction
 *
 * See "Linear Scan Register Allocation on SSA Form", Christian Wimmer and
 * Michael Franz, CGO'10 (2010), Figure 4.
 */
static void ir_add_local_var(ir_ctx *ctx, int v, uint8_t type)
{
	ir_live_interval *ival = ctx->live_intervals[v];

	IR_ASSERT(!ival);

	ival = ir_mem_malloc(sizeof(ir_live_interval));
	IR_ASSERT(type != IR_VOID);
	ival->type = type;
	ival->reg = IR_REG_NONE;
	ival->flags = 0;
	ival->stack_spill_pos = 0; // not allocated
	ival->range.start = 0;
	ival->range.end = ctx->insns_count;
	ival->range.next = NULL;
	ival->use_pos = NULL;

	ival->top = ival;
	ival->next = NULL;

	ctx->live_intervals[v] = ival;
}

static void ir_add_live_range(ir_ctx *ctx, int v, uint8_t type, ir_live_pos start, ir_live_pos end)
{
	ir_live_interval *ival = ctx->live_intervals[v];
	ir_live_range *p, *q, *next, *prev;

	if (!ival) {
		ival = ir_mem_malloc(sizeof(ir_live_interval));
		IR_ASSERT(type != IR_VOID);
		ival->type = type;
		ival->reg = IR_REG_NONE;
		ival->flags = 0;
		ival->stack_spill_pos = 0; // not allocated
		ival->range.start = start;
		ival->range.end = end;
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
					/* list of deleted structures is keapt at ctx->unused_live_ranges for reuse */
					next->next = ctx->unused_live_ranges;
					ctx->unused_live_ranges = next;
					next = p->next;
				}
			}
			return;
		}
		prev = p;
		p = prev->next;
	}
	if (ctx->unused_live_ranges) {
		/* reuse */
		q = ctx->unused_live_ranges;
		ctx->unused_live_ranges = q->next;
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
}

static void ir_add_fixed_live_range(ir_ctx *ctx, ir_reg reg, ir_live_pos start, ir_live_pos end)
{
	int v = ctx->vregs_count + 1 + reg;
	ir_live_interval *ival = ctx->live_intervals[v];
	if (!ival) {
		ival = ir_mem_malloc(sizeof(ir_live_interval));
		ival->type = IR_VOID;
		ival->reg = reg;
		ival->flags = 0;
		ival->stack_spill_pos = 0; // not allocated
		ival->range.start = start;
		ival->range.end = end;
		ival->range.next = NULL;
		ival->use_pos = NULL;

		ival->top = ival;
		ival->next = NULL;

		ctx->live_intervals[v] = ival;
		return;
	}
	ir_add_live_range(ctx, v, IR_VOID, start, end);
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

static void ir_add_use(ir_ctx *ctx, int v, int op_num, ir_live_pos pos, ir_reg hint, ir_ref hint_ref)
{
	ir_use_pos *use_pos;

	use_pos = ir_mem_malloc(sizeof(ir_use_pos));
	use_pos->op_num = op_num;
	use_pos->hint = hint;
	use_pos->hint_ref = hint_ref;
	use_pos->pos = pos;
	use_pos->flags = ctx->rules ? ir_get_use_flags(ctx, IR_LIVE_POS_TO_REF(pos), op_num) : 0;

	ir_add_use_pos(ctx, v, use_pos);
}

int ir_compute_live_ranges(ir_ctx *ctx)
{
	int i, j, k, n;
	int b, succ;
	uint32_t flags, len;
	ir_insn *insn;
	ir_block *bb, *succ_bb;
	ir_bitset visited, live;
	ir_bitset loops = NULL;
	ir_bitset queue = NULL;
	ir_reg reg;

	if (!(ctx->flags & IR_LINEAR) || !ctx->vregs) {
		return 0;
	}

	/* Compute Live Ranges */
	visited = ir_bitset_malloc(ctx->cfg_blocks_count + 1);
	len = ir_bitset_len(ctx->vregs_count + 1);
	live = ir_bitset_malloc((ctx->cfg_blocks_count + 1) * len * 8 * sizeof(*live));
	ctx->live_intervals = ir_mem_calloc(ctx->vregs_count + 1 + IR_REG_NUM, sizeof(ir_live_interval*));
	for (b = ctx->cfg_blocks_count; b > 0; b--) {
		bb = &ctx->cfg_blocks[b];
		/* for each successor of b */
		ir_bitset_incl(visited, b);
		ir_bitset_clear(live, len);
		for (i = 0; i < bb->successors_count; i++) {
			succ = ctx->cfg_edges[bb->successors + i];
			/* blocks must be ordered where all dominators of a block are before this block */
            IR_ASSERT(ir_bitset_in(visited, succ) || bb->loop_header == succ);
			/* live = union of successors.liveIn */
			ir_bitset_union(live, live + (len * succ), len);
			/* for each phi function phi of successor */
			succ_bb = &ctx->cfg_blocks[succ];
			if (succ_bb->predecessors_count > 1) {
				ir_use_list *use_list = &ctx->use_lists[succ_bb->start];

				k = 0;
				for (j = 0; j < succ_bb->predecessors_count; j++) {
					if (ctx->cfg_edges[succ_bb->predecessors + j] == b) {
						k = j + 2;
						break;
					}
				}
				IR_ASSERT(k != 0);
				for (j = 0; j < use_list->count; j++) {
					insn = &ctx->ir_base[ctx->use_edges[use_list->refs + j]];
					if (insn->op == IR_PHI) {
						if (insn->ops[k] > 0) {
							/* live.add(phi.inputOf(b)) */
							IR_ASSERT(ctx->vregs[insn->ops[k]]);
							ir_bitset_incl(live, ctx->vregs[insn->ops[k]]);
							// TODO: ir_add_live_range() is used just to set ival->type
							/* intervals[phi.inputOf(b)].addRange(b.from, b.to) */
							ir_add_live_range(ctx, ctx->vregs[insn->ops[k]], insn->type,
								IR_START_LIVE_POS_FROM_REF(bb->start),
								IR_END_LIVE_POS_FROM_REF(bb->end));
						}
					}
				}
			}
		}

		/* for each opd in live */
		IR_BITSET_FOREACH(live, len, i) {
			/* intervals[opd].addRange(b.from, b.to) */
			ir_add_live_range(ctx, i, IR_VOID,
				IR_START_LIVE_POS_FROM_REF(bb->start),
				IR_END_LIVE_POS_FROM_REF(bb->end));
		} IR_BITSET_FOREACH_END();

		/* for each operation op of b in reverse order */
		for (i = bb->end; i > bb->start; i -= ctx->prev_insn_len[i]) {
			insn = &ctx->ir_base[i];
			flags = ir_op_flags[insn->op];
			if ((flags & IR_OP_FLAG_DATA) || ((flags & IR_OP_FLAG_MEM) && insn->type != IR_VOID)) {
				if (ctx->vregs[i]) {
					if (ir_bitset_in(live, ctx->vregs[i])) {
						if (insn->op != IR_PHI) {
							ir_live_pos def_pos;
							ir_ref hint_ref = 0;

							reg = ctx->rules ? ir_uses_fixed_reg(ctx, i, 0) : IR_REG_NONE;
							if (reg != IR_REG_NONE) {
								def_pos = IR_SAVE_LIVE_POS_FROM_REF(i);
								if (insn->op == IR_PARAM) {
									/* parameter register must be kept before it's copied */
									ir_add_fixed_live_range(ctx, reg,
										IR_START_LIVE_POS_FROM_REF(bb->start), def_pos);
								} else {
									ir_add_fixed_live_range(ctx, reg,
										IR_DEF_LIVE_POS_FROM_REF(i), def_pos);
								}
							} else if (ctx->rules && ir_result_reuses_op1_reg(ctx, i)) {
								def_pos = IR_LOAD_LIVE_POS_FROM_REF(i);
								hint_ref = insn->op1;
							} else {
								def_pos = IR_DEF_LIVE_POS_FROM_REF(i);
							}
							/* intervals[opd].setFrom(op.id) */
							ir_fix_live_range(ctx, ctx->vregs[i],
								IR_START_LIVE_POS_FROM_REF(bb->start), def_pos);
							ir_add_use(ctx, ctx->vregs[i], 0, def_pos, reg, hint_ref);
						} else {
							ir_add_use(ctx, ctx->vregs[i], 0, IR_DEF_LIVE_POS_FROM_REF(i), IR_REG_NONE, 0);
						}
						/* live.remove(opd) */
						ir_bitset_excl(live, ctx->vregs[i]);
					} else if (insn->op == IR_VAR) {
						if (ctx->use_lists[i].count > 0) {
							ir_add_local_var(ctx, ctx->vregs[i], insn->type);
						}
					}
				}
			}
			if (insn->op != IR_PHI) {
				n = ir_input_edges_count(ctx, insn);
				for (j = 1; j <= n; j++) {
					if (IR_OPND_KIND(flags, j) == IR_OPND_DATA) {
						ir_ref input = insn->ops[j];
						if (input > 0 && ctx->vregs[input]) {
							ir_live_pos use_pos;

							if (ctx->rules && j == 1 && ir_result_reuses_op1_reg(ctx, i)) {
								use_pos = IR_LOAD_LIVE_POS_FROM_REF(i);
								reg = ctx->rules ? ir_uses_fixed_reg(ctx, i, j) : IR_REG_NONE;
								if (reg != IR_REG_NONE) {
									ir_add_fixed_live_range(ctx, reg,
										use_pos, IR_USE_LIVE_POS_FROM_REF(i));
								}
							} else {
								reg = ctx->rules ? ir_uses_fixed_reg(ctx, i, j) : IR_REG_NONE;
								if (reg != IR_REG_NONE) {
									use_pos = IR_LOAD_LIVE_POS_FROM_REF(i);
									ir_add_fixed_live_range(ctx, reg,
										use_pos, IR_USE_LIVE_POS_FROM_REF(i));
								} else if (j > 1 && input == insn->op1 && ctx->rules && ir_result_reuses_op1_reg(ctx, i)) {
									/* Input is the same as "op1" */
									use_pos = IR_LOAD_LIVE_POS_FROM_REF(i);
								} else {
									use_pos = IR_USE_LIVE_POS_FROM_REF(i);
								}
							}
							/* intervals[opd].addRange(b.from, op.id) */
							ir_add_live_range(ctx, ctx->vregs[input], ctx->ir_base[input].type,
								IR_START_LIVE_POS_FROM_REF(bb->start), use_pos);
							ir_add_use(ctx, ctx->vregs[input], j, use_pos, reg, 0);
							/* live.add(opd) */
							ir_bitset_incl(live, ctx->vregs[input]);
						}
					}
				}
				/* CPU specific constraints */
				if (ctx->rules) {
					ir_regset regset = ir_get_scratch_regset(ctx, i);

					if (regset != IR_REGSET_EMPTY) {
						IR_REGSET_FOREACH(regset, reg) {
							ir_add_fixed_live_range(ctx, reg,
								IR_LOAD_LIVE_POS_FROM_REF(i), // TODO: LOAD instead of USE disables register usage for input
								                              // this is necessary for DIV and MOD, but not for MUL
								IR_DEF_LIVE_POS_FROM_REF(i));
						}  IR_REGSET_FOREACH_END();
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
				queue = ir_bitset_malloc(ctx->cfg_blocks_count + 1);
			} else {
				ir_bitset_clear(loops, bb_set_len);
				ir_bitset_clear(queue, bb_set_len);
			}
			ir_bitset_incl(loops, b);
			ir_bitset_incl(queue, b);
			do {
				child = ir_bitset_pop_first(queue, bb_set_len);
				child_bb = &ctx->cfg_blocks[child];

				IR_BITSET_FOREACH(live, len, i) {
					ir_add_live_range(ctx, i, IR_VOID,
						IR_START_LIVE_POS_FROM_REF(child_bb->start),
						IR_END_LIVE_POS_FROM_REF(child_bb->end));
				} IR_BITSET_FOREACH_END();

				child = child_bb->dom_child;
				while (child) {
					child_bb = &ctx->cfg_blocks[child];
					if (child_bb->loop_header && ir_bitset_in(loops, child_bb->loop_header)) {
						ir_bitset_incl(queue, child);
						if (child_bb->flags & IR_BB_LOOP_HEADER) {
							ir_bitset_incl(loops, child);
						}
					}
					child = child_bb->dom_next_child;
				}
			} while (!ir_bitset_empty(queue, bb_set_len));
		}

		/* b.liveIn = live */
		ir_bitset_copy(live + (len * b), live, len);
	}

	if (loops) {
		ir_mem_free(loops);
		ir_mem_free(queue);
	}

	ir_mem_free(live);
	ir_mem_free(visited);

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
	uint32_t i;
	ir_live_interval *ival;

	for (i = 1; i <= count; i++) {
		ival = live_intervals[i];
		if (ival) {
			if (ival->range.next) {
				ir_free_live_ranges(ival->range.next);
			}
			ir_mem_free(ival);
		}
	}
	ir_mem_free(live_intervals);
}


/* Live Ranges coalescing */

static ir_live_pos ir_vregs_overlap(ir_ctx *ctx, uint32_t r1, uint32_t r2)
{
	ir_live_range *lrg1 = &ctx->live_intervals[r1]->range;
	ir_live_range *lrg2 = &ctx->live_intervals[r2]->range;

	while (1) {
		if (lrg1->start < lrg2->end) {
			if (lrg2->start < lrg1->end) {
				return IR_MAX(lrg1->start, lrg2->start);
			} else {
				lrg1 = lrg1->next;
				if (!lrg1) {
					return 0;
				}
			}
		} else {
			lrg2 = lrg2->next;
			if (!lrg2) {
				return 0;
			}
		}
	}
}

static void ir_vregs_join(ir_ctx *ctx, uint32_t r1, uint32_t r2)
{
	ir_live_interval *ival = ctx->live_intervals[r2];
	ir_live_range *live_range = &ival->range;
	ir_live_range *next;
	ir_use_pos *use_pos;

#if 0
	fprintf(stderr, "COALESCE %d -> %d\n", r2, r1);
#endif

	ir_add_live_range(ctx, r1, ival->type, live_range->start, live_range->end);
	live_range = live_range->next;
	while (live_range) {
		ir_add_live_range(ctx, r1, ival->type, live_range->start, live_range->end);
		next = live_range->next;
		live_range->next = ctx->unused_live_ranges;
		ctx->unused_live_ranges = live_range;
		live_range = next;
	} while (live_range);

	use_pos = ival->use_pos;
	while (use_pos) {
		ir_use_pos *next_use_pos = use_pos->next;
		if (ctx->vregs[use_pos->hint_ref] == r1) {
			use_pos->hint_ref = 0;
		}
		ir_add_use_pos(ctx, r1, use_pos);
		use_pos = next_use_pos;
	}

	ir_mem_free(ival);
	ctx->live_intervals[r2] = NULL;
	ctx->live_intervals[r1]->flags |= IR_LIVE_INTERVAL_COALESCED;
}

static bool ir_try_coalesce(ir_ctx *ctx, ir_ref from, ir_ref to)
{
	ir_ref i;
	int v1 = ctx->vregs[from];
	int v2 = ctx->vregs[to];

	if (v1 != v2 && !ir_vregs_overlap(ctx, v1, v2)) {
		uint8_t f1 = ctx->live_intervals[v1]->flags;
		uint8_t f2 = ctx->live_intervals[v2]->flags;

		if ((f1 & IR_LIVE_INTERVAL_COALESCED) && !(f2 & IR_LIVE_INTERVAL_COALESCED)) {
			ir_vregs_join(ctx, v1, v2);
			ctx->vregs[to] = v1;
		} else if ((f2 & IR_LIVE_INTERVAL_COALESCED) && !(f1 & IR_LIVE_INTERVAL_COALESCED)) {
			ir_vregs_join(ctx, v2, v1);
			ctx->vregs[from] = v2;
		} else if (v1 < v2) {
			ir_vregs_join(ctx, v1, v2);
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
			ir_vregs_join(ctx, v2, v1);
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

static void ir_add_phi_move(ir_ctx *ctx, int b, ir_ref from, ir_ref to)
{
	if (IR_IS_CONST_REF(from) || ctx->vregs[from] != ctx->vregs[to]) {
		ctx->cfg_blocks[b].flags |= IR_BB_DESSA_MOVES;
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
	ir_use_pos *p;
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
				break;
			}
			r = r->next;
		}
		p = ival->use_pos;
		while (p) {
			if (p->pos == load_pos) {
				p->pos = pos;
				p->op_num = 2;
				break;
			}
			p = p->next;
		}
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
	if (insn->op1 > 0
	 && ctx->vregs[insn->op1] != ctx->vregs[i]
	 && !ir_vregs_overlap(ctx, ctx->vregs[insn->op1], ctx->vregs[i])
	 && !ir_hint_conflict(ctx, i, ctx->vregs[insn->op1], ctx->vregs[i])) {
		/* pass */
	} else if (insn->op2 > 0 && insn->op1 != insn->op2
		&& (ir_op_flags[insn->op] & IR_OP_FLAG_COMMUTATIVE)) {
		if (ctx->vregs[insn->op2] != ctx->vregs[i]) {
			ir_live_pos pos = IR_USE_LIVE_POS_FROM_REF(i);
			ir_live_pos load_pos = IR_LOAD_LIVE_POS_FROM_REF(i);
			ir_live_interval *ival = ctx->live_intervals[ctx->vregs[insn->op2]];
			ir_live_range *r = &ival->range;

			while (r) {
				if (r->end == pos) {
					r->end = load_pos;
					if (!ir_vregs_overlap(ctx, ctx->vregs[insn->op2], ctx->vregs[i])
					 && !ir_hint_conflict(ctx, i, ctx->vregs[insn->op2], ctx->vregs[i])) {
						ir_swap_operands(ctx, i, insn);
						return 1;
					} else {
						r->end = pos;
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
	int b, i, n, succ;
	ir_ref *p, use, input, k, j;
	ir_block *bb, *succ_bb;
	ir_use_list *use_list;
	ir_insn *insn;
	uint32_t *offsets;
	ir_worklist blocks;
	bool compact = 0;

	/* Collect a list of blocks which are predecossors to block with phi finctions */
	ir_worklist_init(&blocks, ctx->cfg_blocks_count + 1);
	for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
		if (bb->predecessors_count > 1) {
			use_list = &ctx->use_lists[bb->start];
			n = use_list->count;
			for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
				use = *p;
				insn = &ctx->ir_base[use];
				if (insn->op == IR_PHI) {
					k = ir_input_edges_count(ctx, insn);
					for (j = 2; j <= k; j++) {
						ir_worklist_push(&blocks, ctx->cfg_edges[bb->predecessors + (j-2)]);
					}
				}
			}
		}
	}

	qsort_r(blocks.l.a.refs, ir_worklist_len(&blocks), sizeof(ir_ref), ir_block_cmp, ctx);

	while (ir_worklist_len(&blocks)) {
		b = ir_worklist_pop(&blocks);
		bb = &ctx->cfg_blocks[b];
		IR_ASSERT(bb->successors_count == 1);
		succ = ctx->cfg_edges[bb->successors];
		succ_bb = &ctx->cfg_blocks[succ];
		IR_ASSERT(succ_bb->predecessors_count > 1);
		k = 0;
		for (j = 0; j < succ_bb->predecessors_count; j++) {
			if (ctx->cfg_edges[succ_bb->predecessors + j] == b) {
				k = j + 2;
				break;
			}
		}
		IR_ASSERT(k != 0);
		use_list = &ctx->use_lists[succ_bb->start];
		n = use_list->count;
		for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
			use = *p;
			insn = &ctx->ir_base[use];
			if (insn->op == IR_PHI) {
				input = insn->ops[k];
				if (input > 0) {
					if (!ir_try_coalesce(ctx, input, use)) {
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
	ir_worklist_free(&blocks);

#if 1
	if (ctx->rules) {
		/* try to swap operands of commutative instructions for better register allocation */
		for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
			for (i = bb->start, insn = ctx->ir_base + i; i <= bb->end;) {
				if (ir_result_reuses_op1_reg(ctx, i)) {
					if (insn->op2 > 0 && insn->op1 != insn->op2
					 && (ir_op_flags[insn->op] & IR_OP_FLAG_COMMUTATIVE)) {
						ir_try_swap_operands(ctx, i, insn);
					}
//					if (insn->op1 > 0) {
//						ir_try_coalesce(ctx, insn->op1, i);
//					}
//				} else if (insn->op == IR_COPY) {
//					if (insn->op1 > 0) {
//						ir_try_coalesce(ctx, insn->op1, i);
//					}
				}
				n = ir_operands_count(ctx, insn);
				n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
				i += n;
				insn += n;
			}
		}
	}
#endif

	if (compact) {
#if 1
		offsets = ir_mem_calloc(ctx->vregs_count + 1, sizeof(uint32_t));
		for (i = 1, n = 1; i <= ctx->vregs_count; i++) {
			if (ctx->live_intervals[i]) {
				if (i != n) {
					ctx->live_intervals[n] = ctx->live_intervals[i];
					offsets[i] = i - n;
				}
				n++;
			}
		}
		n--;
		if (n != ctx->vregs_count) {
			j = ctx->vregs_count - n;
			for (i = n + 1; i <= ctx->vregs_count + IR_REG_NUM; i++) {
				ctx->live_intervals[i] = ctx->live_intervals[i + j];
			}
			for (j = 1; j < ctx->insns_count; j++) {
				if (ctx->vregs[j]) {
					ctx->vregs[j] -= offsets[ctx->vregs[j]];
				}
			}
			ctx->vregs_count = n;
		}
		ir_mem_free(offsets);
#endif
	}

	return 1;
}

/* SSA Deconstruction */

int ir_compute_dessa_moves(ir_ctx *ctx)
{
	int b, i, n;
	ir_ref j, k, *p, use;
	ir_block *bb;
	ir_use_list *use_list;
	ir_insn *insn;

	for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
		if (bb->predecessors_count > 1) {
			use_list = &ctx->use_lists[bb->start];
			n = use_list->count;
			for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
				use = *p;
				insn = &ctx->ir_base[use];
				if (insn->op == IR_PHI) {
					k = ir_input_edges_count(ctx, insn);
					for (j = 2; j <= k; j++) {
						if (IR_IS_CONST_REF(insn->ops[j]) || ctx->vregs[insn->ops[j]] != ctx->vregs[use]) {
							int pred = ctx->cfg_edges[bb->predecessors + (j-2)];
							ctx->cfg_blocks[pred].flags |= IR_BB_DESSA_MOVES;
						}
					}
				}
			}
		}
	}
	return 1;
}

int ir_gen_dessa_moves(ir_ctx *ctx, int b, emit_copy_t emit_copy)
{
	int succ, j, k = 0, n = 0;
	ir_block *bb, *succ_bb;
	ir_use_list *use_list;
	uint8_t *type;
	uint32_t *loc, *pred;
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

	for (j = 0; j < succ_bb->predecessors_count; j++) {
		if (ctx->cfg_edges[succ_bb->predecessors + j] == b) {
			k = j + 2;
			break;
		}
	}
	IR_ASSERT(k != 0);

	type = ir_mem_calloc((ctx->vregs_count + 1), sizeof(uint8_t));
	loc = ir_mem_calloc((ctx->vregs_count + 1) * 2, sizeof(uint32_t));
	pred = loc + (ctx->vregs_count + 1);
	len = ir_bitset_len(ctx->vregs_count + 1);
	todo = ir_bitset_malloc(ctx->vregs_count + 1);
	ready = ir_bitset_malloc(ctx->vregs_count + 1);

	for (j = 0; j < use_list->count; j++) {
		ir_ref ref = ctx->use_edges[use_list->refs + j];
		ir_insn *insn = &ctx->ir_base[ref];
		if (insn->op == IR_PHI) {
			ir_ref input = insn->ops[k];
			if (IR_IS_CONST_REF(input)) {
				emit_copy(ctx, insn->type, input, ctx->vregs[ref]);
			} else if (ctx->vregs[input] != ctx->vregs[ref]) {
				loc[ctx->vregs[input]] = ctx->vregs[input];
				pred[ctx->vregs[ref]] = ctx->vregs[input];
				type[ctx->vregs[ref]] = insn->type;
				ir_bitset_incl(todo, ctx->vregs[ref]);
				n++;
			}
		}
	}

	IR_BITSET_FOREACH(todo, len, j) {
		if (!loc[j]) {
			ir_bitset_incl(ready, j);
		}
	} IR_BITSET_FOREACH_END();

	while (!ir_bitset_empty(todo, len)) {
		uint32_t a, b, c;

		while (!ir_bitset_empty(ready, len)) {
			b = ir_bitset_pop_first(ready, len);
			a = pred[b];
			c = loc[a];
			emit_copy(ctx, type[b], c, b);
			loc[a] = b;
			if (a == c && pred[a]) {
				ir_bitset_incl(ready, a);
			}
		}
		b = ir_bitset_pop_first(todo, len);
		if (b != loc[pred[b]]) {
			emit_copy(ctx, type[b], b, 0);
			loc[b] = 0;
			ir_bitset_incl(ready, b);
		}
	}

	ir_mem_free(ready);
	ir_mem_free(todo);
	ir_mem_free(loc);
	ir_mem_free(type);

	return 1;
}

/* Linear Scan Register Allocation
 *
 * See "Optimized Interval Splitting in a Linear Scan Register Allocator",
 * Christian Wimmer VEE'10 (2005), Figure 2.
 */
typedef struct _ir_lsra_data {
	uint32_t stack_frame_size;
} ir_lsra_data;

static ir_live_pos ir_live_range_end(ir_live_interval *ival)
{
	ir_live_range *live_range = &ival->range;

	while (live_range->next) {
		live_range = live_range->next;
	}
	return live_range->end;
}

static bool ir_live_range_covers(ir_live_interval *ival, ir_live_pos position)
{
	ir_live_range *live_range = &ival->range;

	do {
		if (position >= live_range->start && position < live_range->end) {
			return 1;
		}
		live_range = live_range->next;
	} while (live_range);

	return 0;
}

static ir_live_interval *ir_split_interval_at(ir_live_interval *ival, ir_live_pos pos)
{
	ir_live_interval *child;
	ir_live_range *p, *prev;
	ir_use_pos *use_pos, *prev_use_pos;

	IR_ASSERT(pos > ival->range.start);

	p = &ival->range;
	prev = NULL;
	while (p && pos >= p->end) {
		prev = p;
		p = prev->next;
	}
	IR_ASSERT(p);

	use_pos = ival->use_pos;
	prev_use_pos = NULL;

	if (p->start == pos) {
		while (use_pos && pos > use_pos->pos) {
			prev_use_pos = use_pos;
			use_pos = use_pos->next;
		}
	} else {
		while (use_pos && pos >= use_pos->pos) {
			prev_use_pos = use_pos;
			use_pos = use_pos->next;
		}
	}

	child = ir_mem_malloc(sizeof(ir_live_interval));
	child->type = ival->type;
	child->reg = IR_REG_NONE;
	child->flags = 0;
	child->stack_spill_pos = 0; // not allocated
	child->range.start = pos;
	child->range.end = p->end;
	child->range.next = p->next;
	child->use_pos = prev_use_pos ? prev_use_pos->next : use_pos;

	child->top = ival->top;
	child->next = ival->next;
	ival->next = child;

	if (pos == p->start) {
		prev->next = NULL;
	} else {
		p->end = pos;
		p->next = NULL;
	}
	if (prev_use_pos) {
		prev_use_pos->next = NULL;
	} else {
		ival->use_pos = NULL;
	}

	return child;
}

static void ir_allocate_spill_slot(ir_ctx *ctx, int current, ir_lsra_data *data)
{
	ir_live_interval *ival = ctx->live_intervals[current]->top;

	if (ival->stack_spill_pos == 0) {
		data->stack_frame_size += 8; // ir_type_size[insn->type]; // TODO: alignment
		ival->stack_spill_pos = data->stack_frame_size;
	}
}

static ir_reg ir_try_allocate_preferred_reg(ir_ctx *ctx, ir_live_interval *ival, ir_live_pos *freeUntilPos)
{
	ir_use_pos *use_pos;

	use_pos = ival->use_pos;
	while (use_pos) {
		if (use_pos->hint >= 0) {
			if (ir_live_range_end(ival) <= freeUntilPos[use_pos->hint]) {
				/* register available for the whole interval */
				return use_pos->hint;
			}
		}
		use_pos = use_pos->next;
	}

	use_pos = ival->use_pos;
	while (use_pos) {
		if (use_pos->hint_ref) {
			ir_reg reg = ctx->live_intervals[ctx->vregs[use_pos->hint_ref]]->reg;
			if (reg >= 0) {
				if (ir_live_range_end(ival) <= freeUntilPos[reg]) {
					/* register available for the whole interval */
					return reg;
				}
			}
		}
		use_pos = use_pos->next;
	}

	return IR_REG_NONE;
}

static void ir_add_to_unhandled(ir_ctx *ctx, ir_list *unhandled, int current)
{
	ir_live_pos pos = ctx->live_intervals[current]->range.start;

	if (ir_list_len(unhandled) == 0 || pos < ctx->live_intervals[ir_list_peek(unhandled)]->range.start) {
		ir_list_push(unhandled, current);
	} else {
		uint32_t i = ir_list_len(unhandled);
		while (i > 0) {
			i--;
			if (pos < ctx->live_intervals[ir_list_at(unhandled, i)]->range.start) {
				i++;
				break;
			}
		}
		ir_list_insert(unhandled, i, current);
	}
}

static ir_block *ir_block_from_live_pos(ir_ctx *ctx, ir_live_pos pos)
{
	int b;
	ir_block *bb;
	ir_ref ref = IR_LIVE_POS_TO_REF(pos);

	// TODO: use binary search or map
	for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; b++, bb++) {
		if (ref >= bb->start && ref <= bb->end) {
			return bb;
		}
	}
	IR_ASSERT(0);
}

static ir_live_pos ir_find_optimal_split_position(ir_ctx *ctx, ir_live_pos min_pos, ir_live_pos max_pos)
{
	ir_block *min_bb, *max_bb;

	if (min_pos == max_pos) {
		return max_pos;
	}

	IR_ASSERT(min_pos < max_pos);
	min_bb = ir_block_from_live_pos(ctx, min_pos);
	max_bb = ir_block_from_live_pos(ctx, max_pos);

	if (min_bb == max_bb) {
		return max_pos;
	}

	// TODO: search for an optimal block boundary

	return max_pos;
}

static ir_reg ir_try_allocate_free_reg(ir_ctx *ctx, int current, uint32_t len, ir_bitset active, ir_bitset inactive)
{
	ir_live_pos freeUntilPos[IR_REG_NUM];
	int i, reg;
	ir_live_pos pos, next;
	ir_live_interval *ival = ctx->live_intervals[current];
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

#ifdef IR_DEBUG
	available &= debug_regset;
#endif

	/* for each interval it in active */
	IR_BITSET_FOREACH(active, len, i) {
		/* freeUntilPos[it.reg] = 0 */
		reg = ctx->live_intervals[i]->reg;
		IR_ASSERT(reg >= 0);
		if (IR_REGSET_IN(available, reg)) {
			freeUntilPos[reg] = 0;
		}
	} IR_BITSET_FOREACH_END();

	/* for each interval it in inactive intersecting with current
	 *
	 * This loop is not necessary for program in SSA form (see LSRA on SSA fig. 6),
	 * but it is still necessary after coalescing and splitting
	 */
	IR_BITSET_FOREACH(inactive, len, i) {
		/* freeUntilPos[it.reg] = next intersection of it with current */
		reg = ctx->live_intervals[i]->reg;
		IR_ASSERT(reg >= 0);
		if (IR_REGSET_IN(available, reg)) {
			next = ir_vregs_overlap(ctx, current, i);
			if (next && next < freeUntilPos[reg]) {
				freeUntilPos[reg] = next;
			}
		}
	} IR_BITSET_FOREACH_END();

	/* Try to use hint */
	reg = ir_try_allocate_preferred_reg(ctx, ival, freeUntilPos);
	if (reg != IR_REG_NONE) {
		ival->reg = reg;
		return reg;
	}

	/* reg = register with highest freeUntilPos */
	reg = IR_REGSET_FIRST(available);
	IR_REGSET_EXCL(available, reg);
	pos = freeUntilPos[reg];
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
	} else if (ir_live_range_end(ival) <= pos) {
		/* register available for the whole interval */
		ival->reg = reg;
		return reg;
	} else if (pos > ival->range.start) {
		/* register available for the first part of the interval */
		ival->reg = reg;
		/* split current before freeUntilPos[reg] */
		ir_split_interval_at(ival, pos); // TODO: Split/Spill Pos

#ifdef IR_DEBUG
		if (ctx->flags & IR_DEBUG_RA) {
			ir_dump_live_ranges(ctx, stderr);
		}
#endif

		return reg;
	} else {
		return IR_REG_NONE;
	}
}

static ir_reg ir_allocate_blocked_reg(ir_ctx *ctx, int current, uint32_t len, ir_bitset active, ir_bitset inactive, ir_list *unhandled)
{
	ir_live_pos nextUsePos[IR_REG_NUM];
	ir_live_pos blockPos[IR_REG_NUM];
	int i, reg;
	ir_live_pos pos, next_use_pos;
	ir_live_interval *ival = ctx->live_intervals[current];
	ir_use_pos *use_pos;
	ir_regset available;

	use_pos = ival->use_pos;
	while (use_pos && use_pos->pos < ival->range.start) {
		// TODO: skip usages that don't require register
		use_pos = use_pos->next;
	}
	while (use_pos && !(use_pos->flags & IR_USE_MUST_BE_IN_REG)) {
		use_pos = use_pos->next;
	}
	if (!use_pos) {
		/* spill */
		return IR_REG_NONE;
	}
	next_use_pos = use_pos->pos;

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

#ifdef IR_DEBUG
	available &= debug_regset;
#endif

	/* for each interval it in active */
	IR_BITSET_FOREACH(active, len, i) {
		/* nextUsePos[it.reg] = next use of it after start of current */
		reg = ctx->live_intervals[i]->reg;
		IR_ASSERT(reg >= 0);
		if (IR_REGSET_IN(available, reg)) {
			// TODO: intervals that can't be spilled should be handled as fixed
			if (ctx->live_intervals[i]->type == IR_VOID) {
				/* fixed intervals */
				blockPos[reg] = nextUsePos[reg] = 0;
			} else {
				use_pos = ctx->live_intervals[i]->use_pos;
				while (use_pos && use_pos->pos <= ival->range.start) { // TODO: less or less-or-equal
					use_pos = use_pos->next;
				}
				while (use_pos && !(use_pos->flags & (IR_USE_MUST_BE_IN_REG|IR_USE_SHOULD_BE_IN_REG))) {
					use_pos = use_pos->next;
				}
				if (use_pos && use_pos->pos < nextUsePos[reg]) {
					nextUsePos[reg] = use_pos->pos;
				}
			}
		}
	} IR_BITSET_FOREACH_END();

	/* for each interval it in inactive intersecting with current */
	IR_BITSET_FOREACH(inactive, len, i) {
		/* freeUntilPos[it.reg] = next intersection of it with current */
		reg = ctx->live_intervals[i]->reg;
		IR_ASSERT(reg >= 0);
		if (IR_REGSET_IN(available, reg)) {
			ir_live_pos overlap = ir_vregs_overlap(ctx, current, i);

			if (overlap) {
				if (ctx->live_intervals[i]->type == IR_VOID) {
					/* fixed intervals */
					if (overlap < nextUsePos[reg]) {
						nextUsePos[reg] = overlap;
					}
					if (overlap < blockPos[reg]) {
						blockPos[reg] = overlap;
					}
				} else {
					use_pos = ctx->live_intervals[i]->use_pos;
					while (use_pos && use_pos->pos < ival->range.start) {
						use_pos = use_pos->next;
					}
					while (use_pos && !(use_pos->flags & (IR_USE_MUST_BE_IN_REG|IR_USE_SHOULD_BE_IN_REG))) {
						use_pos = use_pos->next;
					}
					if (use_pos && use_pos->pos < nextUsePos[reg]) {
						nextUsePos[reg] = use_pos->pos;
					}
				}
			}
		}
	} IR_BITSET_FOREACH_END();

	// TODO: support for register hinting

	/* reg = register with highest nextUsePos */
	reg = IR_REGSET_FIRST(available);
	IR_REGSET_EXCL(available, reg);
	pos = nextUsePos[reg];
	IR_REGSET_FOREACH(available, i) {
		if (nextUsePos[i] > pos) {
			pos = nextUsePos[i];
			reg = i;
		}
	} IR_REGSET_FOREACH_END();

	/* if first usage of current is after nextUsePos[reg] then */
	if (next_use_pos > pos) {
		/* all other intervals are used before current, so it is best to spill current itself */
		/* assign spill slot to current */
		/* split current before its first use position that requires a register */
		ir_live_pos split_pos = ir_find_optimal_split_position(ctx, ival->range.start, next_use_pos - 1);

		if (split_pos > ival->range.start) {
			ir_live_interval *child = ir_split_interval_at(ival, split_pos);
			ctx->live_intervals[current] = child;
			ir_add_to_unhandled(ctx, unhandled, current);

#ifdef IR_DEBUG
			if (ctx->flags & IR_DEBUG_RA) {
				ir_dump_live_ranges(ctx, stderr);
			}
#endif

			return IR_REG_NONE;
		}
	}

	/* current.reg = reg */
	ival->reg = reg;

	if (ir_live_range_end(ival) > blockPos[reg]) {
		/* spilling make a register free only for the first part of current */
		/* split current at optimal position before block_pos[reg] */
		ir_split_interval_at(ival, blockPos[reg]); // TODO: Split Pos
	}

	/* spill intervals that currently block reg */
	IR_BITSET_FOREACH(active, len, i) {
		if (reg == ctx->live_intervals[i]->reg) {
			/* split active interval for reg at position */
			ir_live_pos overlap = ir_vregs_overlap(ctx, current, i);

			if (overlap) {
				IR_ASSERT(ctx->live_intervals[i]->type != IR_VOID);
				ir_live_interval *child = ir_split_interval_at(ctx->live_intervals[i], ival->range.start); // TODO: Split Pos
				ir_bitset_excl(active, i);
				ctx->live_intervals[i] = child;
				if (child->use_pos) {
					ir_live_pos split_pos = ir_find_optimal_split_position(ctx, ival->range.start, child->use_pos->pos - 1);

					if (split_pos > ival->range.start) {
						child = ir_split_interval_at(child, split_pos);
						ctx->live_intervals[i] = child;
					}
					ir_add_to_unhandled(ctx, unhandled, i);
				}
			}
			break;
		}
	} IR_BITSET_FOREACH_END();

	/* split any inactive interval for reg at the end of its lifetime hole */
	IR_BITSET_FOREACH(inactive, len, i) {
		/* freeUntilPos[it.reg] = next intersection of it with current */
		if (reg == ctx->live_intervals[i]->reg) {
			ir_live_pos overlap = ir_vregs_overlap(ctx, current, i);

			if (overlap) {
				IR_ASSERT(ctx->live_intervals[i]->type != IR_VOID);
				ir_split_interval_at(ctx->live_intervals[i], overlap); // TODO: Split Pos
			}
		}
	} IR_BITSET_FOREACH_END();

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_RA) {
		ir_dump_live_ranges(ctx, stderr);
	}
#endif

	return reg;
}

static int ir_live_range_cmp(const void *r1, const void *r2, void *data)
{
	ir_ctx *ctx = data;
	ir_live_range *lrg1 = &ctx->live_intervals[*(ir_ref*)r1]->range;
	ir_live_range *lrg2 = &ctx->live_intervals[*(ir_ref*)r2]->range;

	return lrg2->start - lrg1->start;
}

static int ir_fix_dessa_tmps(ir_ctx *ctx, uint8_t type, int from, int to)
{
	if (to == 0) {
		ir_block *bb = ctx->data;
		ir_reg reg;

		if (IR_IS_TYPE_INT(type)) {
			reg = IR_REG_R0; // TODO: Temporary register
		} else if (IR_IS_TYPE_FP(type)) {
			reg = IR_REG_XMM0; // TODO: Temporary register
		} else {
			IR_ASSERT(0);
			return 0;
		}
		ir_add_fixed_live_range(ctx, reg,
			IR_START_LIVE_POS_FROM_REF(bb->end),
			IR_END_LIVE_POS_FROM_REF(bb->end));
	}
	return 1;
}

static int ir_linear_scan(ir_ctx *ctx)
{
	int b;
	ir_block *bb;
	ir_list unhandled;
	ir_bitset active, inactive;
	ir_live_interval *ival;
	int current, i;
	uint32_t len;
	ir_live_pos position;
	ir_reg reg;
	ir_lsra_data data;

	if (!ctx->live_intervals) {
		return 0;
	}

	/* Add fixed intervals for temporary registers used for DESSA moves */
	for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
		if (bb->flags & IR_BB_DESSA_MOVES) {
			ctx->data = bb;
			ir_gen_dessa_moves(ctx, b, ir_fix_dessa_tmps);
		}
	}

	ctx->data = &data;
	data.stack_frame_size = 0;
	ir_list_init(&unhandled, ctx->vregs_count + 1);
	len = ir_bitset_len(ctx->vregs_count + 1 + IR_REG_NUM);
	active = ir_bitset_malloc(ctx->vregs_count + 1 + IR_REG_NUM);
	inactive = ir_bitset_malloc(ctx->vregs_count + 1 + IR_REG_NUM);

	for (i = 1; i <= ctx->vregs_count; i++) {
		if (ctx->live_intervals[i] && ctx->live_intervals[i]->range.start > 0) {
			ir_list_push(&unhandled, i);
		}
	}

	for (i = ctx->vregs_count + 1; i <= ctx->vregs_count + IR_REG_NUM; i++) {
		if (ctx->live_intervals[i]) {
			ir_bitset_incl(inactive, i);
		}
	}

	qsort_r(unhandled.a.refs, ir_list_len(&unhandled), sizeof(ir_ref), ir_live_range_cmp, ctx);

	while (1) {
		if (ir_list_len(&unhandled) == 0) {
			position = 0x7fffffff;
			IR_BITSET_FOREACH(active, len, i) {
				ival = ctx->live_intervals[i];
				if (ival->next) {
					if (ival->next->range.start < position) {
						position = ival->next->range.start;
						current = i;
					}
				}
			} IR_BITSET_FOREACH_END();

			if (position < 0x7fffffff) {
				ir_bitset_excl(active, current);
				ctx->live_intervals[current] = ctx->live_intervals[current]->next;
			} else {
				break;
			}
		} else {
			current = ir_list_pop(&unhandled);
			position = ctx->live_intervals[current]->range.start;
		}

		/* for each interval i in active */
		IR_BITSET_FOREACH(active, len, i) {
			ival = ctx->live_intervals[i];
			if (ir_live_range_end(ival) <= position) {
				/* move i from active to handled */
				ir_bitset_excl(active, i);
				if (ival->next) {
					ctx->live_intervals[i] = ival->next;
					ir_add_to_unhandled(ctx, &unhandled, i);
				}
			} else if (!ir_live_range_covers(ival, position)) {
				/* move i from active to inactive */
				ir_bitset_excl(active, i);
				ir_bitset_incl(inactive, i);
			}
		} IR_BITSET_FOREACH_END();

		/* for each interval i in inactive */
		IR_BITSET_FOREACH(inactive, len, i) {
			ival = ctx->live_intervals[i];
			if (ir_live_range_end(ival) <= position) {
				/* move i from inactive to handled */
				ir_bitset_excl(inactive, i);
				if (ival->next) {
					ctx->live_intervals[i] = ival->next;
					ir_add_to_unhandled(ctx, &unhandled, i);
				}
			} else if (ir_live_range_covers(ival, position)) {
				/* move i from active to inactive */
				ir_bitset_excl(inactive, i);
				ir_bitset_incl(active, i);
			}
		} IR_BITSET_FOREACH_END();

#if 1 && IR_DEBUG
		ival = ctx->live_intervals[current];
		ir_insn *insn = &ctx->ir_base[IR_LIVE_POS_TO_REF(ival->range.start)];
		if (insn->op == IR_VLOAD) {
			ir_insn *var = &ctx->ir_base[insn->op2];
			IR_ASSERT(var->op == IR_VAR);
			if (strcmp(ir_get_str(ctx, var->op2), "_spill_") == 0) {
				if (ctx->live_intervals[ctx->vregs[insn->op2]]->stack_spill_pos) {
					ctx->live_intervals[current]->stack_spill_pos =
						ctx->live_intervals[ctx->vregs[insn->op2]]->stack_spill_pos;
				} else {
					ir_allocate_spill_slot(ctx, current, &data);
					ctx->live_intervals[ctx->vregs[insn->op2]]->stack_spill_pos =
						ctx->live_intervals[current]->stack_spill_pos;
				}
				continue;
			}
		}
		insn = &ctx->ir_base[IR_LIVE_POS_TO_REF(ival->range.end)];
		if (insn->op == IR_VSTORE) {
			ir_insn *var = &ctx->ir_base[insn->op2];
			IR_ASSERT(var->op == IR_VAR);
			if (strcmp(ir_get_str(ctx, var->op2), "_spill_") == 0) {
				if (ctx->live_intervals[ctx->vregs[insn->op2]]->stack_spill_pos) {
					ctx->live_intervals[current]->stack_spill_pos =
						ctx->live_intervals[ctx->vregs[insn->op2]]->stack_spill_pos;
				} else {
					ir_allocate_spill_slot(ctx, current, &data);
					ctx->live_intervals[ctx->vregs[insn->op2]]->stack_spill_pos =
						ctx->live_intervals[current]->stack_spill_pos;
				}
				continue;
			}
		}
#endif

		reg = ir_try_allocate_free_reg(ctx, current, len, active, inactive);
		if (reg == IR_REG_NONE) {
			reg = ir_allocate_blocked_reg(ctx, current, len, active, inactive, &unhandled);
		}
		if (reg != IR_REG_NONE) {
			if (ctx->live_intervals[current]->reg != IR_REG_NONE) {
				ir_bitset_incl(active, current);
			}
		}
	}

#ifdef IR_DEBUG
	/* all intervals must be processed */
	IR_BITSET_FOREACH(active, len, i) {
		ival = ctx->live_intervals[i];
		IR_ASSERT(!ival->next);
	} IR_BITSET_FOREACH_END();
	IR_BITSET_FOREACH(inactive, len, i) {
		ival = ctx->live_intervals[i];
		IR_ASSERT(!ival->next);
	} IR_BITSET_FOREACH_END();
#endif

	ir_mem_free(inactive);
	ir_mem_free(active);
	ir_list_free(&unhandled);

	for (i = 1; i <= ctx->vregs_count; i++) {
		ival = ctx->live_intervals[i];
		if (ival) {
			ival = ival->top;
			ctx->live_intervals[i] = ival;
			if (ival->next || ival->reg == IR_REG_NONE) {
				ir_allocate_spill_slot(ctx, i, &data);
			}
		}
	}

	return 1;
}

int ir_reg_alloc(ir_ctx *ctx)
{
	return ir_linear_scan(ctx);
}
