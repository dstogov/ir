#define _GNU_SOURCE

#include "ir.h"
#include "ir_private.h"
#include <stdlib.h>

#include "ir_x86.h"

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
//			if (flags & IR_OP_FLAG_DATA) {
			if ((flags & IR_OP_FLAG_DATA) || ((flags & IR_OP_FLAG_MEM) && insn->type != IR_VOID)) {
				if ((insn->op == IR_PARAM || insn->op == IR_VAR) && ctx->use_lists[i].count == 0) {
					/* pass */
				} else if (!ctx->rules || ir_needs_def_reg(ctx, i)) {
					vregs[i] = ++vregs_count;
				}
			}
			n = ir_operands_count(ctx, insn);
			n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
			i += n;
			insn += n;
		}
	}
#if 0
	n = 1;
	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count;) {
		insn->prev_len = n;
		flags = ir_op_flags[insn->op];
		if (flags & IR_OP_FLAG_DATA) {
			if ((insn->op == IR_PARAM || insn->op == IR_VAR) && ctx->use_lists[i].count == 0) {
				/* pass */
			} else {
				vregs[i] = ++vregs_count;
			}
		}
		n = ir_operands_count(ctx, insn);
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		i += n;
		insn += n;
	}
#endif
	ctx->vregs_count = vregs_count;
	ctx->vregs = vregs;

	return 1;
}

/* Lifetime intervals construction
 *
 * See "Linear Scan Register Allocation on SSA Form", Christian Wimmer and
 * Michael Franz, CGO'10 (2010), Figure 4.
 */
static void ir_add_live_range(ir_ctx *ctx, int v, uint8_t type, ir_ref start, ir_ref end)
{
	ir_live_interval *ival = ctx->live_intervals[v];
	ir_live_range *p, *q, *next, *prev;

	if (!ival) {
		ival = ir_mem_malloc(sizeof(ir_live_interval));
		IR_ASSERT(type != IR_VOID);
		ival->type = type;
		ival->reg = -1; // IR_REG_NONE
		ival->stack_spill_pos = 0; // not allocated
		ival->range.start = start;
		ival->range.end = end;
		ival->range.next = NULL;
		ival->use_pos = NULL;
		ctx->live_intervals[v] = ival;
		return;
	}

	IR_ASSERT(type == IR_VOID || type == ival->type);
	p = &ival->range;
	prev = NULL;
	while (p && end >= p->start - 1) {
		if (start < p->end + 1) {
			if (start < p->start) {
				p->start = start;
			}
			if (end > p->end) {
				p->end = end;
				/* merge with next */
				next = p->next;
				while (next && p->end >= next->start - 1) {
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

static void ir_add_fixed_live_range(ir_ctx *ctx, ir_reg reg, ir_ref start, ir_ref end)
{
	int v = ctx->vregs_count + 1 + reg;
	ir_live_interval *ival = ctx->live_intervals[v];
	ir_live_range *p, *q, *prev;

	if (!ival) {
		ival = ir_mem_malloc(sizeof(ir_live_interval));
		ival->type = IR_VOID;
		ival->reg = reg;
		ival->stack_spill_pos = 0; // not allocated
		ival->range.start = start;
		ival->range.end = end;
		ival->range.next = NULL;
		ival->use_pos = NULL;
		ctx->live_intervals[v] = ival;
		return;
	}

	IR_ASSERT(ival->type == IR_VOID);
	p = &ival->range;
	prev = NULL;
	while (p && end >= p->start) {
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

static void ir_fix_live_range(ir_ctx *ctx, int v, ir_ref old_start, ir_ref new_start)
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

	while (p && (p->pos < use_pos->pos || (p->pos == use_pos->pos && p->op_num < use_pos->op_num))) {
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

static void ir_add_use(ir_ctx *ctx, int v, int op_num, ir_ref pos, ir_reg hint)
{
	ir_use_pos *use_pos;

	use_pos = ir_mem_malloc(sizeof(ir_use_pos));
	use_pos->op_num = op_num;
	use_pos->hint = hint;
	use_pos->reg = IR_REG_NONE;
	use_pos->pos = pos;

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
							ir_add_live_range(ctx, ctx->vregs[insn->ops[k]], insn->type, bb->start * 2, bb->end * 2 + 1);
						}
					}
				}
			}
		}

		/* for each opd in live */
		IR_BITSET_FOREACH(live, len, i) {
			/* intervals[opd].addRange(b.from, b.to) */
			ir_add_live_range(ctx, i, IR_VOID, bb->start * 2, bb->end * 2 + 1);
		} IR_BITSET_FOREACH_END();

		/* for each operation op of b in reverse order */
		for (i = bb->end; i > bb->start; i -= ctx->prev_insn_len[i]) {
			insn = &ctx->ir_base[i];
			flags = ir_op_flags[insn->op];
			if ((flags & IR_OP_FLAG_DATA) || ((flags & IR_OP_FLAG_MEM) && insn->type != IR_VOID)) {
				if (ctx->vregs[i] && ir_bitset_in(live, ctx->vregs[i])) {
					if (insn->op != IR_PHI) {
						/* intervals[opd].setFrom(op.id) */
						ir_fix_live_range(ctx, ctx->vregs[i], bb->start * 2, i * 2);
					}
					reg = ctx->rules ? ir_uses_fixed_reg(ctx, i, 0) : IR_REG_NONE;
					if (reg != IR_REG_NONE) {
						ir_add_fixed_live_range(ctx, reg, i * 2, i * 2);
					}
					ir_add_use(ctx, ctx->vregs[i], 0, i * 2, reg);
					/* live.remove(opd) */
					ir_bitset_excl(live, ctx->vregs[i]);
				}
			}
			if (insn->op != IR_PHI) {
				int l = 0;

				n = ir_input_edges_count(ctx, insn);
				for (j = 1; j <= n; j++) {
					if (IR_OPND_KIND(flags, j) == IR_OPND_DATA) {
						ir_ref input = insn->ops[j];
						if (input > 0 && ctx->vregs[input]) {
							/* intervals[opd].addRange(b.from, op.id) */
							ir_add_live_range(ctx, ctx->vregs[input], ctx->ir_base[input].type, bb->start * 2, i * 2 + l);
							reg = ctx->rules ? ir_uses_fixed_reg(ctx, i, j) : IR_REG_NONE;
							if (reg != IR_REG_NONE) {
								ir_add_fixed_live_range(ctx, reg, i * 2 + 1, i * 2 + 1);
							}
							ir_add_use(ctx, ctx->vregs[input], j, i * 2 + l, reg);
							/* live.add(opd) */
							ir_bitset_incl(live, ctx->vregs[input]);
						}
						if (ctx->rules && j == 1 && ir_result_reuses_op1(ctx, i)) {
							l = 1; /* TODO: op2 may overlap with result */
						}
					}
				}
				/* CPU specific constraints */
				if (ctx->rules && ctx->rules[i] > IR_LAST_OP) {
					ir_regset regset = ir_get_fixed_regset(ctx, i);
					ir_reg reg;

					if (regset != IR_REGSET_EMPTY) {
						IR_REGSET_FOREACH(regset, reg) {
							ir_add_fixed_live_range(ctx, reg, i * 2, i * 2 + 1);
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
					ir_add_live_range(ctx, i, IR_VOID, child_bb->start * 2, child_bb->end * 2 + 1);
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

static bool ir_vregs_overlap(ir_ctx *ctx, uint32_t r1, uint32_t r2)
{
	ir_live_range *lrg1 = &ctx->live_intervals[r1]->range;
	ir_live_range *lrg2 = &ctx->live_intervals[r2]->range;

	while (1) {
		if (lrg1->start < lrg2->end) { // TODO: less or less-or-equal
			if (lrg2->start < lrg1->end) { // TODO: less or less-or-equal
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
		ir_add_use_pos(ctx, r1, use_pos);
		use_pos = next_use_pos;
	}

	ir_mem_free(ival);
	ctx->live_intervals[r2] = NULL;
}

static bool ir_try_coalesce(ir_ctx *ctx, ir_ref from, ir_ref to)
{
	if (ctx->vregs[from] != ctx->vregs[to]
	 && !ir_vregs_overlap(ctx, ctx->vregs[from], ctx->vregs[to])) {
		if (ctx->vregs[from] < ctx->vregs[to]) {
#if 0
			fprintf(stderr, "COALESCE %d -> %d\n", ctx->vregs[to], ctx->vregs[from]);
#endif
			ir_vregs_join(ctx, ctx->vregs[from], ctx->vregs[to]);
			ctx->vregs[to] = ctx->vregs[from];
		} else {
#if 0
			fprintf(stderr, "COALESCE %d -> %d\n", ctx->vregs[from], ctx->vregs[to]);
#endif
			ir_vregs_join(ctx, ctx->vregs[to], ctx->vregs[from]);
			ctx->vregs[from] = ctx->vregs[to];
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
				ctx->live_intervals[i] = NULL;//ctx->live_intervals[i + j];
			}
			for (j = 1; j < ctx->insns_count; j++) {
				if (ctx->vregs[j]) {
					ctx->vregs[j] -= offsets[ctx->vregs[j]];
				}
			}
			ctx->vregs_count = n;
		}
		ir_mem_free(offsets);
	}
#endif

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

static ir_ref ir_live_range_end(ir_live_interval *ival)
{
	ir_live_range *live_range = &ival->range;

	while (live_range->next) {
		live_range = live_range->next;
	}
	return live_range->end;
}

static bool ir_live_range_covers(ir_live_interval *ival, ir_ref position)
{
	ir_live_range *live_range = &ival->range;

	do {
		if (position >= live_range->start && position < live_range->end) { // TODO: less or less-or-equal
			return 1;
		}
		live_range = live_range->next;
	} while (live_range);

	return 0;
}

static ir_reg ir_try_allocate_free_reg(ir_ctx *ctx, int current, uint32_t len, ir_bitset active, ir_bitset inactive)
{
	ir_ref freeUntilPos[IR_REG_NUM];
	int i, reg;
	ir_ref pos, next;
	ir_live_interval *ival = ctx->live_intervals[current];
	ir_use_pos *use_pos;
	ir_regset available;

	if (IR_IS_TYPE_FP(ival->type)) {
		available = IR_REGSET_FP;
		//available = IR_REGSET_INTERVAL(IR_REG_XMM2, IR_REG_XMM7); //TODO: spill-testing
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
	use_pos = ival->use_pos;
	while (use_pos) {
		if (use_pos->hint >= 0) {
			if (ir_live_range_end(ival) <= freeUntilPos[use_pos->hint]) {
				/* register available for the whole interval */
				ival->reg = use_pos->hint;
				return reg;
			}
		}
		use_pos = use_pos->next;
	}

	reg = IR_REGSET_FIRST(available);
	IR_REGSET_EXCL(available, reg);
	pos = freeUntilPos[reg];
	IR_REGSET_FOREACH(available, i) {
		if (freeUntilPos[i] > pos) {
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
	} else {
		/* register available for the first part of the interval */
		// regs[current] = reg;
		// split current before freeUntilPos[reg]
		return IR_REG_NONE;
	}
}

static ir_reg ir_allocate_blocked_reg(ir_ctx *ctx)
{
	return IR_REG_NONE;
}

static void ir_allocate_spill_slot(ir_ctx *ctx, int current, ir_lsra_data *data)
{
	ir_live_interval *ival = ctx->live_intervals[current];

	IR_ASSERT(ival->stack_spill_pos == 0);
	data->stack_frame_size += 8; // ir_type_size[insn->type]; // TODO: alignment
	ival->stack_spill_pos = data->stack_frame_size;
}

static int ir_live_range_cmp(const void *r1, const void *r2, void *data)
{
	ir_ctx *ctx = data;
	ir_live_range *lrg1 = &ctx->live_intervals[*(ir_ref*)r1]->range;
	ir_live_range *lrg2 = &ctx->live_intervals[*(ir_ref*)r2]->range;

	return lrg2->start - lrg1->start;
}

static int ir_linear_scan(ir_ctx *ctx)
{
	ir_worklist unhandled;
	ir_bitset active, inactive;
	ir_live_interval *ival;
	int current, i;
	uint32_t len;
	ir_ref position;
	ir_reg reg;
	ir_lsra_data data;

	if (!ctx->live_intervals) {
		return 0;
	}

	data.stack_frame_size = 0;
	ir_worklist_init(&unhandled, ctx->vregs_count + 1);
	len = ir_bitset_len(ctx->vregs_count + 1 + IR_REG_NUM);
	active = ir_bitset_malloc(ctx->vregs_count + 1 + IR_REG_NUM);
	inactive = ir_bitset_malloc(ctx->vregs_count + 1 + IR_REG_NUM);

	for (i = 1; i <= ctx->vregs_count; i++) {
		if (ctx->live_intervals[i]) {
			ir_worklist_push(&unhandled, i);
		}
	}

	for (i = ctx->vregs_count + 1; i <= ctx->vregs_count + IR_REG_NUM; i++) {
		if (ctx->live_intervals[i]) {
			ir_bitset_incl(inactive, i);
		}
	}

	qsort_r(unhandled.l.a.refs, ir_worklist_len(&unhandled), sizeof(ir_ref), ir_live_range_cmp, ctx);

	while (ir_worklist_len(&unhandled) != 0) {
		current = ir_worklist_pop(&unhandled);
		position = ctx->live_intervals[current]->range.start;

		/* for each interval i in active */
		IR_BITSET_FOREACH(active, len, i) {
			ival = ctx->live_intervals[i];
			if (ir_live_range_end(ival) <= position) { // TODO: less or less-or-equal
				/* move i from active to handled */
				ir_bitset_excl(active, i);
			} else if (!ir_live_range_covers(ival, position)) {
				/* move i from active to inactive */
				ir_bitset_excl(active, i);
				ir_bitset_incl(inactive, i);
			}
		} IR_BITSET_FOREACH_END();

		/* for each interval i in inactive */
		IR_BITSET_FOREACH(inactive, len, i) {
			ival = ctx->live_intervals[i];
			if (ir_live_range_end(ival) <= position) { // TODO: less or less-or-equal
				/* move i from inactive to handled */
				ir_bitset_excl(inactive, i);
			} else if (ir_live_range_covers(ival, position)) {
				/* move i from active to inactive */
				ir_bitset_excl(inactive, i);
				ir_bitset_incl(active, i);
			}
		} IR_BITSET_FOREACH_END();

		reg = ir_try_allocate_free_reg(ctx, current, len, active, inactive);
		if (reg == IR_REG_NONE) {
			reg = ir_allocate_blocked_reg(ctx);
		}
		if (reg != IR_REG_NONE) {
			ir_bitset_incl(active, current);
		} else {
			ir_allocate_spill_slot(ctx, current, &data);
		}
	}

	ir_mem_free(inactive);
	ir_mem_free(active);
	ir_worklist_free(&unhandled);

	return 1;
}

int ir_reg_alloc(ir_ctx *ctx)
{
	return ir_linear_scan(ctx);
}
