/*
 * IR - Lightweight JIT Compilation Framework
 * (CFG - Control Flow Graph)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h"

static ir_ref ir_merge_blocks(ir_ctx *ctx, ir_ref end, ir_ref begin)
{
	ir_ref prev, next;
	ir_use_list *use_list;
	ir_ref j, n, *p;

	IR_ASSERT(ctx->ir_base[begin].op == IR_BEGIN);
	IR_ASSERT(ctx->ir_base[end].op == IR_END);
	IR_ASSERT(ctx->ir_base[begin].op1 == end);
	IR_ASSERT(ctx->use_lists[end].count == 1);

	prev = ctx->ir_base[end].op1;

	use_list = &ctx->use_lists[begin];
	IR_ASSERT(use_list->count == 1);
	next = ctx->use_edges[use_list->refs];

	/* remove BEGIN and END */
	ctx->ir_base[begin].op = IR_NOP;
	ctx->ir_base[begin].op1 = IR_UNUSED;
	ctx->use_lists[begin].count = 0;
	ctx->ir_base[end].op = IR_NOP;
	ctx->ir_base[end].op1 = IR_UNUSED;
	ctx->use_lists[end].count = 0;

	/* connect their predecessor and successor */
	ctx->ir_base[next].op1 = prev;
	use_list = &ctx->use_lists[prev];
	n = use_list->count;
	for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
		if (*p == end) {
			*p = next;
		}
	}

	return next;
}

IR_ALWAYS_INLINE void _ir_add_successors(ir_ctx *ctx, ir_ref ref, ir_worklist *worklist)
{
	ir_use_list *use_list = &ctx->use_lists[ref];
	ir_ref *p, use, n = use_list->count;

	if (n < 2) {
		if (n == 1) {
			use = ctx->use_edges[use_list->refs];
			IR_ASSERT(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL);
			ir_worklist_push(worklist, use);
		}
	} else {
		p = &ctx->use_edges[use_list->refs];
		if (n == 2) {
			use = *p;
			IR_ASSERT(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL);
			ir_worklist_push(worklist, use);
			use = *(p + 1);
			IR_ASSERT(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL);
			ir_worklist_push(worklist, use);
		} else {
			for (; n > 0; p++, n--) {
				use = *p;
				IR_ASSERT(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL);
				ir_worklist_push(worklist, use);
			}
		}
	}
}

IR_ALWAYS_INLINE void _ir_add_predecessors(ir_insn *insn, ir_worklist *worklist)
{
	ir_ref n, j, *p, ref;

	if (insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) {
		n = ir_variable_inputs_count(insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			ref = *p;
			IR_ASSERT(ref);
			ir_worklist_push(worklist, ref);
		}
	} else if (insn->op != IR_START && insn->op != IR_ENTRY) {
		IR_ASSERT(insn->op1);
		ir_worklist_push(worklist, insn->op1);
	}
}

int ir_build_cfg(ir_ctx *ctx)
{
	ir_ref n, j, *p, ref, start, end;
	uint32_t b;
	ir_insn *insn;
	ir_worklist worklist;
	uint32_t count, bb_count = 0;
	uint32_t edges_count = 0;
	ir_block *blocks, *bb;
	uint32_t *_blocks, *edges;
	ir_use_list *use_list;
	ir_bitset bb_starts = ir_bitset_malloc(ctx->insns_count);

	_blocks = ir_mem_calloc(ctx->insns_count, sizeof(uint32_t));
	ir_worklist_init(&worklist, ctx->insns_count);

	/* Add START node */
	ir_worklist_push(&worklist, 1);

	/* Add all ENTRY nodes */
	ref = ctx->ir_base[1].op2;
	while (ref) {
		ir_worklist_push(&worklist, ref);
		ref = ctx->ir_base[ref].op2;
	}

	/* Add all "stop" nodes */
	ref = ctx->ir_base[1].op1;
	while (ref) {
		ir_worklist_push(&worklist, ref);
		ref = ctx->ir_base[ref].op3;
	}

	while (ir_worklist_len(&worklist)) {
		ref = ir_worklist_pop(&worklist);
		insn = &ctx->ir_base[ref];

		if (_blocks[ref]) {
			/* alredy processed */
		} else if (IR_IS_BB_END(insn->op)) {
			/* Mark BB end */
			bb_count++;
			end = ref;
			/* Add successors */
			_ir_add_successors(ctx, ref, &worklist);
			/* Skip control nodes untill BB start */
			while (1) {
				insn = &ctx->ir_base[ref];
				if (IR_IS_BB_START(insn->op)) {
					if (insn->op == IR_BEGIN
					 && (ctx->flags & IR_OPT_CFG)
					 && ctx->ir_base[insn->op1].op == IR_END
					 && ctx->use_lists[ref].count == 1) {
						ref = ir_merge_blocks(ctx, insn->op1, ref);
						ref = ctx->ir_base[ref].op1;
						continue;
					}
					break;
				}
				ref = insn->op1; // follow connected control blocks untill BB start
			}
			/* Mark BB start */
			_blocks[ref] = end;
			_blocks[end] = end;
			ir_bitset_incl(bb_starts, ref);
			ir_bitset_incl(worklist.visited, ref);
			/* Add predecessors */
			_ir_add_predecessors(insn, &worklist);
		} else {
			IR_ASSERT(IR_IS_BB_START(insn->op));
			/* Mark BB start */
			bb_count++;
			start = ref;
			ir_bitset_incl(bb_starts, ref);
			/* Add predecessors */
			_ir_add_predecessors(insn, &worklist);
			/* Skip control nodes untill BB end */
			while (1) {
				use_list = &ctx->use_lists[ref];
				n = use_list->count;
				ref = IR_UNUSED;
				for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
					ref = *p;
					insn = &ctx->ir_base[ref];
					if (ir_op_flags[insn->op] & IR_OP_FLAG_CONTROL) {
						break;
					}
				}
next_successor:
				IR_ASSERT(ref != IR_UNUSED);
				if (IR_IS_BB_END(insn->op)) {
					if (insn->op == IR_END && (ctx->flags & IR_OPT_CFG)) {
						ir_ref next;

						use_list = &ctx->use_lists[ref];
						IR_ASSERT(use_list->count == 1);
						next = ctx->use_edges[use_list->refs];

						if (ctx->ir_base[next].op == IR_BEGIN
						 && ctx->use_lists[next].count == 1) {
							ref = ir_merge_blocks(ctx, ref, next);
							insn = &ctx->ir_base[ref];
							goto next_successor;
						}
					}
					break;
				}
			}
			/* Mark BB end */
			_blocks[start] = ref;
			_blocks[ref] = ref;
			ir_bitset_incl(worklist.visited, ref);
			/* Add successors */
			_ir_add_successors(ctx, ref, &worklist);
		}
	}
	ir_worklist_clear(&worklist);

	IR_ASSERT(bb_count > 0);

	/* Create array of basic blocks and count succcessor edges for each BB */
	blocks = ir_mem_calloc(bb_count + 1, sizeof(ir_block));
	b = 1;
	bb = blocks + 1;
	IR_BITSET_FOREACH(bb_starts, ir_bitset_len(ctx->insns_count), start) {
		IR_ASSERT(IR_IS_BB_START(ctx->ir_base[start].op));
		end = _blocks[start];
		_blocks[start] = b;
		_blocks[end] = b;
		bb->start = start;
		bb->end = end;
		bb++;
		b++;
	} IR_BITSET_FOREACH_END();
	ir_mem_free(bb_starts);

	for (b = 1, bb = blocks + 1; b <= bb_count; b++, bb++) {
		insn = &ctx->ir_base[bb->start];
		if (insn->op == IR_START) {
			bb->flags |= IR_BB_START;
			ir_worklist_push(&worklist, b);
		} else if (insn->op == IR_ENTRY) {
			bb->flags |= IR_BB_ENTRY;
			ir_worklist_push(&worklist, b);
		} else {
			bb->flags |= IR_BB_UNREACHABLE; /* all blocks are marked as UNREACHABLE first */
		}
		if (insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) {
			n = ir_variable_inputs_count(insn);
			bb->predecessors_count += n;
			edges_count += n;
			for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
				ir_ref pred_ref = *p;
				IR_ASSERT(pred_ref);
				blocks[_blocks[pred_ref]].successors_count++;
			}
		} else if (insn->op != IR_START && insn->op != IR_ENTRY) {
			ir_ref pred_ref = insn->op1;
			IR_ASSERT(pred_ref);
			IR_ASSERT(IR_OPND_KIND(ir_op_flags[insn->op], 1) == IR_OPND_CONTROL);
			bb->predecessors_count++;
			blocks[_blocks[pred_ref]].successors_count++;
			edges_count++;
		}
	}

	bb = blocks + 1;
	count = 0;
	for (b = 1; b <= bb_count; b++, bb++) {
		bb->successors = count;
		count += bb->successors_count;
		bb->successors_count = 0;
		bb->predecessors = count;
		count += bb->predecessors_count;
		bb->predecessors_count = 0;
	}
	IR_ASSERT(count == edges_count * 2);

	/* Create an array of successor control edges */
	edges = ir_mem_malloc(edges_count * 2 * sizeof(uint32_t));
	bb = blocks + 1;
	for (b = 1; b <= bb_count; b++, bb++) {
		insn = &ctx->ir_base[bb->start];
		if (insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) {
			n = ir_variable_inputs_count(insn);
			for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
				ref = *p;
				IR_ASSERT(ref);
				ir_ref pred_b = _blocks[ref];
				ir_block *pred_bb = &blocks[pred_b];
				edges[bb->predecessors + bb->predecessors_count] = pred_b;
				bb->predecessors_count++;
				pred_bb->end = ref;
				edges[pred_bb->successors + pred_bb->successors_count] = b;
				pred_bb->successors_count++;
			}
		} else if (insn->op != IR_START && insn->op != IR_ENTRY) {
			ref = insn->op1;
			IR_ASSERT(ref);
			IR_ASSERT(IR_OPND_KIND(ir_op_flags[insn->op], 1) == IR_OPND_CONTROL);
			ir_ref pred_b = _blocks[ref];
			ir_block *pred_bb = &blocks[pred_b];
			edges[bb->predecessors + bb->predecessors_count] = pred_b;
			bb->predecessors_count++;
			pred_bb->end = ref;
			edges[pred_bb->successors + pred_bb->successors_count] = b;
			pred_bb->successors_count++;
		}
	}

    ctx->cfg_blocks_count = bb_count;
    ctx->cfg_edges_count = edges_count * 2;
    ctx->cfg_blocks = blocks;
    ctx->cfg_edges = edges;
    ctx->cfg_map = _blocks;

	/* Mark reachable blocks */
	while (ir_worklist_len(&worklist) != 0) {
		uint32_t *p, succ_b;
		ir_block *succ_bb;

		b = ir_worklist_pop(&worklist);
		bb = &blocks[b];
		n = bb->successors_count;
		for (p = ctx->cfg_edges + bb->successors; n > 0; p++, n--) {
			succ_b = *p;
			succ_bb = &blocks[succ_b];
			if (succ_bb->flags & IR_BB_UNREACHABLE) {
				succ_bb->flags &= ~IR_BB_UNREACHABLE;
				ir_worklist_push(&worklist, succ_b);
			}
		}
	}
	ir_worklist_free(&worklist);

	return 1;
}

static void compute_postnum(const ir_ctx *ctx, uint32_t *cur, uint32_t b)
{
	uint32_t i, *p;
	ir_block *bb = &ctx->cfg_blocks[b];

	if (bb->postnum != 0) {
		return;
	}

	if (bb->successors_count) {
		bb->postnum = -1; /* Marker for "currently visiting" */
		p = ctx->cfg_edges + bb->successors;
		i = bb->successors_count;
		do {
			compute_postnum(ctx, cur, *p);
			p++;
		} while (--i);
	}
	bb->postnum = (*cur)++;
}

/* Computes dominator tree using algorithm from "A Simple, Fast Dominance Algorithm" by
 * Cooper, Harvey and Kennedy. */
int ir_build_dominators_tree(ir_ctx *ctx)
{
	uint32_t blocks_count, b, postnum;
	ir_block *blocks, *bb;
	uint32_t *edges;
	bool changed;

	postnum = 1;
	compute_postnum(ctx, &postnum, 1);

	if (ctx->ir_base[1].op2) {
		for (b = 2, bb = &ctx->cfg_blocks[2]; b <= ctx->cfg_blocks_count; b++, bb++) {
			if (bb->flags & IR_BB_ENTRY) {
				compute_postnum(ctx, &postnum, b);
				bb->idom = 1;
			}
		}
		ctx->cfg_blocks[1].postnum = postnum;
	}

	/* Find immediate dominators */
	blocks = ctx->cfg_blocks;
	edges  = ctx->cfg_edges;
	blocks_count = ctx->cfg_blocks_count;
	blocks[1].idom = 1;
	do {
		changed = 0;
		/* Iterating in Reverse Post Oorder */
		for (b = 2, bb = &blocks[2]; b <= blocks_count; b++, bb++) {
			if (bb->flags & IR_BB_UNREACHABLE) {
				continue;
			}
			if (bb->predecessors_count == 1) {
				uint32_t idom = 0;
				uint32_t pred_b = edges[bb->predecessors];
				ir_block *pred_bb = &blocks[pred_b];

				if (pred_bb->idom > 0) {
					idom = pred_b;
				}
				if (idom > 0 && bb->idom != idom) {
					bb->idom = idom;
					changed = 1;
				}
			} else if (bb->predecessors_count) {
				uint32_t idom = 0;
				uint32_t k = bb->predecessors_count;
				uint32_t *p = edges + bb->predecessors;
				do {
					uint32_t pred_b = *p;
					ir_block *pred_bb = &blocks[pred_b];

					if (pred_bb->idom > 0 && !(pred_bb->flags & IR_BB_ENTRY)) {
						if (idom == 0) {
							idom = pred_b;
						} else if (idom != pred_b) {
							ir_block *idom_bb = &blocks[idom];

							do {
								while (pred_bb->postnum < idom_bb->postnum) {
									pred_b = pred_bb->idom;
									pred_bb = &blocks[pred_b];
								}
								while (idom_bb->postnum < pred_bb->postnum) {
									idom = idom_bb->idom;
									idom_bb = &blocks[idom];
								}
							} while (idom != pred_b);
						}
					}
					p++;
				} while (--k > 0);

				if (idom > 0 && bb->idom != idom) {
					bb->idom = idom;
					changed = 1;
				}
			}
		}
	} while (changed);
	blocks[1].idom = 0;
	blocks[1].dom_depth = 0;

	/* Construct dominators tree */
	for (b = 2, bb = &blocks[2]; b <= blocks_count; b++, bb++) {
		if (bb->flags & IR_BB_UNREACHABLE) {
			continue;
		}
		if (bb->flags & IR_BB_ENTRY) {
			bb->idom = 0;
			bb->dom_depth = 0;
		} else if (bb->idom > 0) {
			ir_block *idom_bb = &blocks[bb->idom];

			bb->dom_depth = idom_bb->dom_depth + 1;
			/* Sort by block number to traverse children in pre-order */
			if (idom_bb->dom_child == 0) {
				idom_bb->dom_child = b;
			} else if (b < idom_bb->dom_child) {
				bb->dom_next_child = idom_bb->dom_child;
				idom_bb->dom_child = b;
			} else {
				int child = idom_bb->dom_child;
				ir_block *child_bb = &blocks[child];

				while (child_bb->dom_next_child > 0 && b > child_bb->dom_next_child) {
					child = child_bb->dom_next_child;
					child_bb = &blocks[child];
				}
				bb->dom_next_child = child_bb->dom_next_child;
				child_bb->dom_next_child = b;
			}
		}
	}

	return 1;
}

static bool ir_dominates(ir_block *blocks, uint32_t b1, uint32_t b2)
{
	uint32_t b1_depth = blocks[b1].dom_depth;
	ir_block *bb2 = &blocks[b2];

	while (bb2->dom_depth > b1_depth) {
		b2 = bb2->dom_parent;
		bb2 = &blocks[b2];
	}
	return b1 == b2;
}

int ir_find_loops(ir_ctx *ctx)
{
	uint32_t i, j, n, count;
	uint32_t *entry_times, *exit_times, *sorted_blocks, time = 1;
	ir_block *blocks = ctx->cfg_blocks;
	uint32_t *edges = ctx->cfg_edges;
	ir_worklist work;

	/* We don't materialize the DJ spanning tree explicitly, as we are only interested in ancestor
	 * queries. These are implemented by checking entry/exit times of the DFS search. */
	ir_worklist_init(&work, ctx->cfg_blocks_count + 1);
	entry_times = ir_mem_malloc((ctx->cfg_blocks_count + 1) * 3 * sizeof(uint32_t));
	exit_times = entry_times + ctx->cfg_blocks_count + 1;
	sorted_blocks = exit_times + ctx->cfg_blocks_count + 1;

	memset(entry_times, 0, (ctx->cfg_blocks_count + 1) * sizeof(uint32_t));

	ir_worklist_push(&work, 1);
	while (ir_worklist_len(&work)) {
		ir_block *bb;
		int child;

next:
		i = ir_worklist_peek(&work);
		if (!entry_times[i]) {
			entry_times[i] = time++;
		}

		/* Visit blocks immediately dominated by i. */
		bb = &blocks[i];
		for (child = bb->dom_child; child > 0; child = blocks[child].dom_next_child) {
			if (ir_worklist_push(&work, child)) {
				goto next;
			}
		}

		/* Visit join edges. */
		if (bb->successors_count) {
			uint32_t *p = edges + bb->successors;
			for (j = 0; j < bb->successors_count; j++,p++) {
				uint32_t succ = *p;

				if (blocks[succ].idom == i) {
					continue;
				} else if (ir_worklist_push(&work, succ)) {
					goto next;
				}
			}
		}
		exit_times[i] = time++;
		ir_worklist_pop(&work);
	}

	/* Sort blocks by level, which is the opposite order in which we want to process them */
	sorted_blocks[1] = 1;
	j = 1;
	n = 2;
	while (j != n) {
		i = j;
		j = n;
		for (; i < j; i++) {
			int child;
			for (child = blocks[sorted_blocks[i]].dom_child; child > 0; child = blocks[child].dom_next_child) {
				sorted_blocks[n++] = child;
			}
		}
	}
	count = n;

	/* Identify loops. See Sreedhar et al, "Identifying Loops Using DJ Graphs". */
	while (n > 1) {
		i = sorted_blocks[--n];
		ir_block *bb = &blocks[i];

		if (bb->predecessors_count > 1) {
			bool irreducible = 0;
			uint32_t *p = &edges[bb->predecessors];

			j = bb->predecessors_count;
			do {
				uint32_t pred = *p;

				/* A join edge is one for which the predecessor does not
				   immediately dominate the successor.  */
				if (bb->idom != pred) {
					/* In a loop back-edge (back-join edge), the successor dominates
					   the predecessor.  */
					if (ir_dominates(blocks, i, pred)) {
						if (!ir_worklist_len(&work)) {
							ir_bitset_clear(work.visited, ir_bitset_len(ir_worklist_capasity(&work)));
						}
						ir_worklist_push(&work, pred);
					} else {
						/* Otherwise it's a cross-join edge.  See if it's a branch
						   to an ancestor on the DJ spanning tree.  */
						irreducible = (entry_times[pred] > entry_times[i] && exit_times[pred] < exit_times[i]);
					}
				}
				p++;
			} while (--j);

			if (UNEXPECTED(irreducible)) {
				// TODO: Support for irreducible loops ???
				bb->flags |= IR_BB_IRREDUCIBLE_LOOP;
				ctx->flags |= IR_IRREDUCIBLE_CFG;
				while (ir_worklist_len(&work)) {
					ir_worklist_pop(&work);
				}
			} else if (ir_worklist_len(&work)) {
				bb->flags |= IR_BB_LOOP_HEADER;
				while (ir_worklist_len(&work)) {
					j = ir_worklist_pop(&work);
					while (blocks[j].loop_header > 0) {
						j = blocks[j].loop_header;
					}
					if (j != i) {
						ir_block *bb = &blocks[j];
						if (bb->idom == 0 && j != 1) {
							/* Ignore blocks that are unreachable or only abnormally reachable. */
							continue;
						}
						bb->loop_header = i;
						if (bb->predecessors_count) {
							uint32_t *p = &edges[bb->predecessors];
							j = bb->predecessors_count;
							do {
								ir_worklist_push(&work, *p);
								p++;
							} while (--j);
						}
					}
				}
			}
		}
	}

	for (n = 1; n < count; n++) {
		i = sorted_blocks[n];
		ir_block *bb = &blocks[i];
		if (bb->loop_header > 0) {
			bb->loop_depth = blocks[bb->loop_header].loop_depth;
		}
		if (bb->flags & IR_BB_LOOP_HEADER) {
			bb->loop_depth++;
		}
	}

	ir_mem_free(entry_times);
	ir_worklist_free(&work);

	return 1;
}

/* A variation of "Top-down Positioning" algorithm described by
 * Karl Pettis and Robert C. Hansen "Profile Guided Code Positioning"
 *
 * TODO: Switch to "Bottom-up Positioning" algorithm
 */
int ir_schedule_blocks(ir_ctx *ctx)
{
	ir_bitqueue blocks;
	uint32_t b, *p, successor, best_successor, j, last_non_empty = 0;
	ir_block *bb, *successor_bb, *best_successor_bb;
	ir_insn *insn;
	uint32_t *list, *map;
	uint32_t prob, best_successor_prob;
	uint32_t count = 0;
	bool reorder = 0;

	ir_bitqueue_init(&blocks, ctx->cfg_blocks_count + 1);
	blocks.pos = 0;
	list = ir_mem_malloc(sizeof(uint32_t) * (ctx->cfg_blocks_count + 1) * 2);
	map = list + (ctx->cfg_blocks_count + 1);
	for (b = 1, bb = &ctx->cfg_blocks[1]; b <= ctx->cfg_blocks_count; b++, bb++) {
		if (bb->end - ctx->prev_insn_len[bb->end] == bb->start
		 && bb->successors_count == 1
		 && !(bb->flags & IR_BB_DESSA_MOVES)) {
			bb->flags |= IR_BB_EMPTY;
		}
		ir_bitset_incl(blocks.set, b);
	}

	while ((b = ir_bitqueue_pop(&blocks)) != (uint32_t)-1) {
		bb = &ctx->cfg_blocks[b];
		/* Start trace */
		do {
			if (bb->predecessors_count > 1) {
				/* Insert empty ENTRY blocks */
				for (j = 0, p = &ctx->cfg_edges[bb->predecessors]; j < bb->predecessors_count; j++, p++) {
					uint32_t predecessor = *p;

					if (ir_bitqueue_in(&blocks, predecessor)
					 && (ctx->cfg_blocks[predecessor].flags & IR_BB_ENTRY)
					 && ctx->cfg_blocks[predecessor].end == ctx->cfg_blocks[predecessor].start + 1) {
						ir_bitqueue_del(&blocks, predecessor);
						count++;
						list[count] = predecessor;
						map[predecessor] = count;
						if (predecessor != count) {
							reorder = 1;
						}
						if (!(bb->flags & IR_BB_EMPTY)) {
							last_non_empty = b;
						}
					}
				}
			}
			count++;
			list[count] = b;
			map[b] = count;
			if (b != count) {
				reorder = 1;
			}
			if (!(bb->flags & IR_BB_EMPTY)) {
				last_non_empty = b;
			}
			best_successor_bb = NULL;
			for (b = 0, p = &ctx->cfg_edges[bb->successors]; b < bb->successors_count; b++, p++) {
				successor = *p;
				if (ir_bitqueue_in(&blocks, successor)) {
					successor_bb = &ctx->cfg_blocks[successor];
					insn = &ctx->ir_base[successor_bb->start];
					if (insn->op == IR_IF_TRUE || insn->op == IR_IF_FALSE || insn->op == IR_CASE_DEFAULT) {
						prob = insn->op2;
					} else if (insn->op == IR_CASE_VAL) {
						prob = insn->op3;
					} else {
						prob = 0;
					}
					if (!best_successor_bb
					 || successor_bb->loop_depth > best_successor_bb->loop_depth) {
						// TODO: use block frequency
						best_successor = successor;
						best_successor_bb = successor_bb;
						best_successor_prob = prob;
					} else if ((best_successor_prob && prob
								&& prob > best_successor_prob)
							|| (!best_successor_prob && prob
								&& prob > 100 / bb->successors_count)
							|| (best_successor_prob && !prob
								&& best_successor_prob < 100 / bb->successors_count)
							|| (!best_successor_prob && !prob
								&& (best_successor_bb->flags & IR_BB_EMPTY)
								&& !(successor_bb->flags & IR_BB_EMPTY))) {
						best_successor = successor;
						best_successor_bb = successor_bb;
						best_successor_prob = prob;
					}
				}
			}
			if (!best_successor_bb) {
				/* Try to continue trace using the other successor of the last IF */
				if ((bb->flags & IR_BB_EMPTY) && last_non_empty) {
					bb = &ctx->cfg_blocks[last_non_empty];
					if (bb->successors_count == 2) {
						b = ctx->cfg_edges[bb->successors];

						if (!ir_bitqueue_in(&blocks, b)) {
							b = ctx->cfg_edges[bb->successors + 1];
						}
						if (ir_bitqueue_in(&blocks, b)) {
							bb = &ctx->cfg_blocks[b];
							ir_bitqueue_del(&blocks, b);
							continue;
						}
					}
				}
				/* End trace */
				break;
			}
			b = best_successor;
			bb = best_successor_bb;
			ir_bitqueue_del(&blocks, b);
		} while (1);
	}

	if (reorder) {
		ir_block *cfg_blocks = ir_mem_malloc(sizeof(ir_block) * (ctx->cfg_blocks_count + 1));

		memset(ctx->cfg_blocks, 0, sizeof(ir_block));
		for (b = 1, bb = cfg_blocks + 1; b <= count; b++, bb++) {
			*bb = ctx->cfg_blocks[list[b]];
			if (bb->dom_parent > 0) {
				bb->dom_parent = map[bb->dom_parent];
			}
			if (bb->dom_child > 0) {
				bb->dom_child = map[bb->dom_child];
			}
			if (bb->dom_next_child > 0) {
				bb->dom_next_child = map[bb->dom_next_child];
			}
			if (bb->loop_header > 0) {
				bb->loop_header = map[bb->loop_header];
			}
		}
		for (j = 0; j < ctx->cfg_edges_count; j++) {
			if (ctx->cfg_edges[j] > 0) {
				ctx->cfg_edges[j] = map[ctx->cfg_edges[j]];
			}
		}
		ir_mem_free(ctx->cfg_blocks);
		ctx->cfg_blocks = cfg_blocks;
	}

	ir_mem_free(list);
	ir_bitqueue_free(&blocks);

	return 1;
}

/* JMP target optimisation */
uint32_t ir_skip_empty_target_blocks(ir_ctx *ctx, uint32_t b)
{
	ir_block *bb;

	while (1) {
		bb = &ctx->cfg_blocks[b];

		if (bb->end - ctx->prev_insn_len[bb->end] == bb->start
		 && bb->successors_count == 1
		 && !(bb->flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_DESSA_MOVES))) {
			b = ctx->cfg_edges[bb->successors];
		} else {
			break;
		}
	}
	return b;
}

uint32_t ir_skip_empty_next_blocks(ir_ctx *ctx, uint32_t b)
{
	ir_block *bb;

	while (1) {
		if (b > ctx->cfg_blocks_count) {
			return 0;
		}

		bb = &ctx->cfg_blocks[b];

		if (bb->end - ctx->prev_insn_len[bb->end] == bb->start
		 && bb->successors_count == 1
		 && !(bb->flags & (IR_BB_START|/*IR_BB_ENTRY|*/IR_BB_DESSA_MOVES))) {
			b++;
		} else {
			break;
		}
	}
	return b;
}

void ir_get_true_false_blocks(ir_ctx *ctx, uint32_t b, uint32_t *true_block, uint32_t *false_block, uint32_t *next_block)
{
	ir_block *bb;
	uint32_t *p, use_block;

	*true_block = 0;
	*false_block = 0;
	bb = &ctx->cfg_blocks[b];
	IR_ASSERT(ctx->ir_base[bb->end].op == IR_IF);
	IR_ASSERT(bb->successors_count == 2);
	p = &ctx->cfg_edges[bb->successors];
	use_block = *p;
	if (ctx->ir_base[ctx->cfg_blocks[use_block].start].op == IR_IF_TRUE) {
		*true_block = ir_skip_empty_target_blocks(ctx, use_block);
		use_block = *(p+1);
		IR_ASSERT(ctx->ir_base[ctx->cfg_blocks[use_block].start].op == IR_IF_FALSE);
		*false_block = ir_skip_empty_target_blocks(ctx, use_block);
	} else {
		IR_ASSERT(ctx->ir_base[ctx->cfg_blocks[use_block].start].op == IR_IF_FALSE);
		*false_block = ir_skip_empty_target_blocks(ctx, use_block);
		use_block = *(p+1);
		IR_ASSERT(ctx->ir_base[ctx->cfg_blocks[use_block].start].op == IR_IF_TRUE);
		*true_block = ir_skip_empty_target_blocks(ctx, use_block);
	}
	IR_ASSERT(*true_block && *false_block);
	*next_block = b == ctx->cfg_blocks_count ? 0 : ir_skip_empty_next_blocks(ctx, b + 1);
}
