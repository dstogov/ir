#include "ir.h"
#include "ir_private.h"

int ir_build_cfg(ir_ctx *ctx)
{
	ir_ref n, j, *p, ref, b;
	ir_insn *insn;
	uint32_t flags;
	ir_worklist worklist;
	uint32_t bb_count = 0;
	uint32_t edges_count = 0;
	ir_block *blocks, *bb;
	uint32_t *_blocks, *edges;

	_blocks = ir_mem_malloc(ctx->insns_count * sizeof(uint32_t));
	memset(_blocks, 0, ctx->insns_count * sizeof(uint32_t));
	ir_worklist_init(&worklist, ctx->insns_count);

	/* Start from "stop" nodes */
	ref = ctx->ir_base[1].op1;
	while (ref) {
		ir_worklist_push(&worklist, ref);
		ref = ctx->ir_base[ref].op3;
	}

	while (ir_worklist_len(&worklist)) {
		ref = ir_worklist_pop(&worklist);
		/* Skip control nodes untill BB start */
		while (1) {
			insn = &ctx->ir_base[ref];
			_blocks[ref] = bb_count;
			if (IR_IS_BB_START(insn->op)) {
				ir_bitset_incl(worklist.visited, ref);
				break;
			}
			ref = insn->op1; // follow connected control blocks untill BB start
		}
		bb_count++;
		flags = ir_op_flags[insn->op];
		n = ir_input_edges_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			ref = *p;
			if (ref && IR_OPND_KIND(flags, j) == IR_OPND_CONTROL) {
				ir_worklist_push(&worklist, ref);
			}
		}
	}

	/* Backward search may miss some blocks, perform addional forward search */
	if (!ir_bitset_in(worklist.visited, 1)) {
		ir_worklist_push(&worklist, 1);
	}

	/* Check entries */
	ref = ctx->ir_base[1].op2;
	while (ref) {
		if (!ir_bitset_in(worklist.visited, ref)) {
			ir_worklist_push(&worklist, ref);
		}
		ref = ctx->ir_base[ref].op2;
	}

	while (ir_worklist_len(&worklist)) {
		ref = ir_worklist_pop(&worklist);
		insn = &ctx->ir_base[ref];
		IR_ASSERT(IR_IS_BB_START(insn->op));
		bb_count++;
		_blocks[ref] = bb_count;
		ir_bitset_incl(worklist.visited, ref);

		/* Skip control nodes untill BB end */
		while (1) {
			ir_use_list *use_list = &ctx->use_lists[ref];

			n = use_list->count;
			ref = IR_UNUSED;
			for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
				ref = *p;
				insn = &ctx->ir_base[ref];
				if (ir_op_flags[insn->op] & IR_OP_FLAG_CONTROL) {
					break;
				}
			}
			IR_ASSERT(ref != IR_UNUSED);
			_blocks[ref] = bb_count;
			if (IR_IS_BB_END(insn->op)) {
				ir_bitset_incl(worklist.visited, ref);
				use_list = &ctx->use_lists[ref];
				n = use_list->count;
				for (j = 0, p = &ctx->use_edges[use_list->refs]; j < n; j++, p++) {
					ref = *p;
					insn = &ctx->ir_base[ref];
					if ((ir_op_flags[insn->op] & IR_OP_FLAG_CONTROL)
					 && !ir_bitset_in(worklist.visited, ref)) {
						ir_worklist_push(&worklist, ref);
					}
				}
				break;
			}
		}
	}

	if (bb_count == 0) {
		IR_ASSERT(bb_count > 0);
		return 0;
	}

	/* Create array of basic blocks and count succcessor edges for each BB */
	blocks = ir_mem_malloc((bb_count + 1) * sizeof(ir_block));
	memset(blocks, 0, (bb_count + 1) * sizeof(ir_block));
	uint32_t *_xlat = ir_mem_malloc(bb_count * sizeof(uint32_t));
	memset(_xlat, 0, bb_count * sizeof(uint32_t));
	b = 0;
	IR_BITSET_FOREACH(worklist.visited, ir_bitset_len(ctx->insns_count), ref) {
		/* reorder blocks to reflect the original control flow (START - 0) */
		j = _blocks[ref];
		n = _xlat[j];
		if (n == 0) {
			_xlat[j] = n = ++b;
		}
		_blocks[ref] = n;
		bb = &blocks[n];
		insn = &ctx->ir_base[ref];
		if (IR_IS_BB_START(insn->op)) {
			bb->start = ref;
		} else {
			bb->end = ref;
		}
	} IR_BITSET_FOREACH_END();
	ir_mem_free(_xlat);
	ir_worklist_free(&worklist);

	for (b = 1, bb = blocks + 1; b <= bb_count; b++, bb++) {
		insn = &ctx->ir_base[bb->start];
		flags = ir_op_flags[insn->op];
		n = ir_input_edges_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			ir_ref pred_ref = *p;
			if (pred_ref) {
				if (IR_OPND_KIND(flags, j) == IR_OPND_CONTROL) {
					bb->predecessors_count++;
					blocks[_blocks[pred_ref]].successors_count++;
					edges_count++;
				 }
			}
		}
	}

	bb = blocks + 1;
	n = 0;
	for (b = 1; b <= bb_count; b++, bb++) {
		bb->successors = n;
		n += bb->successors_count;
		bb->successors_count = 0;
		bb->predecessors = n;
		n += bb->predecessors_count;
		bb->predecessors_count = 0;
	}
	IR_ASSERT(n == edges_count * 2);

	/* Create an array of successor control edges */
	edges = ir_mem_malloc(edges_count * 2 * sizeof(uint32_t));
	bb = blocks + 1;
	for (b = 1; b <= bb_count; b++, bb++) {
		insn = &ctx->ir_base[bb->start];
		flags = ir_op_flags[insn->op];
		n = ir_input_edges_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			ref = *p;
			if (ref) {
				if (IR_OPND_KIND(flags, j) == IR_OPND_CONTROL) {
					ir_ref pred_b = _blocks[ref];
					ir_block *pred_bb = &blocks[pred_b];
					edges[bb->predecessors + bb->predecessors_count] = pred_b;
					bb->predecessors_count++;
					pred_bb->end = ref;
					edges[pred_bb->successors + pred_bb->successors_count] = b;
					pred_bb->successors_count++;
				}
			}
		}
	}
	ir_mem_free(_blocks);

    ctx->cfg_blocks_count = bb_count;
    ctx->cfg_edges_count = edges_count * 2;
    ctx->cfg_blocks = blocks;
    ctx->cfg_edges = edges;

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
	uint32_t blocks_count, b;
	ir_block *blocks, *bb;
	uint32_t *edges;
	bool changed;

	b = 1;
	compute_postnum(ctx, &b, 1);

	/* Find immediate dominators */
	blocks = ctx->cfg_blocks;
	edges  = ctx->cfg_edges;
	blocks_count = ctx->cfg_blocks_count;
	blocks[1].idom = 1;
	do {
		changed = 0;
		/* Iterating in Reverse Post Oorder */
		for (b = 2, bb = &blocks[2]; b <= blocks_count; b++, bb++) {
//			if (bb->flags & IR_BB_UNREACHABLE) {
//				continue;
//			}
			if (bb->predecessors_count) {
				int idom = 0;
				uint32_t k = bb->predecessors_count;
				uint32_t *p = edges + bb->predecessors;
				do {
					uint32_t pred_b = *p;
					ir_block *pred_bb = &blocks[pred_b];

					if (pred_bb->idom > 0) {
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
//		if (bb->flags & IR_BB_UNREACHABLE) {
//			continue;
//		}
		if (bb->idom > 0) {
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
	entry_times = ir_mem_calloc((ctx->cfg_blocks_count + 1) * 3, sizeof(uint32_t));
	exit_times = entry_times + ctx->cfg_blocks_count + 1;
	sorted_blocks = exit_times + ctx->cfg_blocks_count + 1;

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
						if (bb->idom < 0 && j != 1) {
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
	uint32_t len = ir_bitset_len(ctx->cfg_blocks_count + 1);
	ir_bitset blocks = ir_bitset_malloc(ctx->cfg_blocks_count + 1);
	uint32_t b, *p, successor, best_successor, j;
	ir_block *bb, *successor_bb, *best_successor_bb;
	ir_insn *insn;
	uint32_t *list, *map;
	uint32_t prob, best_successor_prob;
	uint32_t count = 0;
	bool reorder = 0;

	list = ir_mem_malloc(sizeof(uint32_t) * (ctx->cfg_blocks_count + 1) * 2);
	map = list + (ctx->cfg_blocks_count + 1);
	for (b = 1; b <= ctx->cfg_blocks_count; b++) {
		ir_bitset_incl(blocks, b);
	}

	while (!ir_bitset_empty(blocks, len)) {
		b = ir_bitset_pop_first(blocks, len);
		bb = &ctx->cfg_blocks[b];
		do {
			if (bb->predecessors_count == 2) {
				uint32_t predecessor = ctx->cfg_edges[bb->predecessors];

				if (!ir_bitset_in(blocks, predecessor)) {
					predecessor = ctx->cfg_edges[bb->predecessors + 1];
				}
				if (ir_bitset_in(blocks, predecessor)) {
					ir_block *predecessor_bb = &ctx->cfg_blocks[predecessor];

					if (predecessor_bb->successors_count == 1
					 && predecessor_bb->predecessors_count == 1
					 && predecessor_bb->end == predecessor_bb->start + 1
					 && !(predecessor_bb->flags & IR_BB_DESSA_MOVES)) {
						ir_bitset_excl(blocks, predecessor);
						count++;
						list[count] = predecessor;
						map[predecessor] = count;
						if (predecessor != count) {
							reorder = 1;
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
			if (!bb->successors_count) {
				break;
			}
			best_successor_bb = NULL;
			for (b = 0, p = &ctx->cfg_edges[bb->successors]; b < bb->successors_count; b++, p++) {
				successor = *p;
				if (ir_bitset_in(blocks, successor)) {
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
					} else if ((best_successor_prob && prob && prob > best_successor_prob)
							|| (!best_successor_prob && prob && prob > 100 / bb->successors_count)
							|| (best_successor_prob && !prob && best_successor_prob < 100 / bb->successors_count)) {
						best_successor = successor;
						best_successor_bb = successor_bb;
						best_successor_prob = prob;
					}
				}
			}
			if (!best_successor_bb) {
				if (bb->successors_count == 1
				 && bb->predecessors_count == 1
				 && bb->end == bb->start + 1
				 && !(bb->flags & IR_BB_DESSA_MOVES)) {
					uint32_t predecessor = ctx->cfg_edges[bb->predecessors];
					ir_block *predecessor_bb = &ctx->cfg_blocks[predecessor];

					if (predecessor_bb->successors_count == 2) {
						b = ctx->cfg_edges[predecessor_bb->successors];

						if (!ir_bitset_in(blocks, b)) {
							b = ctx->cfg_edges[predecessor_bb->successors + 1];
						}
						if (ir_bitset_in(blocks, b)) {
							bb = &ctx->cfg_blocks[b];
							ir_bitset_excl(blocks, b);
							continue;
						}
					}
				}
				break;
			}
			b = best_successor;
			bb = best_successor_bb;
			ir_bitset_excl(blocks, b);
		} while (1);
	}

	if (reorder) {
		ir_block *cfg_blocks = ir_mem_calloc(sizeof(ir_block), ctx->cfg_blocks_count + 1);

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
	ir_mem_free(blocks);

	return 1;
}

/* JMP target optimisation */
int ir_skip_empty_blocks(ir_ctx *ctx, int b)
{
	while (ctx->cfg_blocks[b].flags & IR_BB_MAY_SKIP) {
		b++;
	}
	return b;
}

void ir_get_true_false_blocks(ir_ctx *ctx, int b, int *true_block, int *false_block, int *next_block)
{
	ir_block *bb;
	uint32_t n, *p, use_block;
	ir_insn *use_insn;

	*true_block = 0;
	*false_block = 0;
	bb = &ctx->cfg_blocks[b];
	IR_ASSERT(ctx->ir_base[bb->end].op == IR_IF);
	IR_ASSERT(bb->successors_count == 2);
	p = &ctx->cfg_edges[bb->successors];
	for (n = 2; n != 0; p++, n--) {
		use_block = *p;
		use_insn = &ctx->ir_base[ctx->cfg_blocks[use_block].start];
		if (use_insn->op == IR_IF_TRUE) {
			*true_block = ir_skip_empty_blocks(ctx, use_block);
		} else if (use_insn->op == IR_IF_FALSE) {
			*false_block = ir_skip_empty_blocks(ctx, use_block);
		} else {
			IR_ASSERT(0);
		}
	}
	IR_ASSERT(*true_block && *false_block);
	*next_block = b == ctx->cfg_blocks_count ? 0 : ir_skip_empty_blocks(ctx, b + 1);
}