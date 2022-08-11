#include "ir.h"
#include "ir_private.h"

/* GCM - Global Code Motion
 *
 * C. Click. "Global code motion, global value numbering" Submitted to PLDI ‘95.
 */
static void ir_gcm_schedule_early(ir_ctx *ctx, uint32_t *_blocks, ir_ref ref)
{
	ir_ref j, n, *p;
	ir_insn *insn;
	uint32_t flags;

	if (_blocks[ref] > 0) {
		return;
	}
	_blocks[ref] = 1;

	insn = &ctx->ir_base[ref];
	flags = ir_op_flags[insn->op];
	if (IR_OPND_KIND(flags, 1) == IR_OPND_CONTROL_DEP) {
		IR_ASSERT(_blocks[insn->op1] > 0);
		_blocks[ref] = _blocks[insn->op1];
	}
	n = ir_input_edges_count(ctx, insn);
	for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
		ir_ref input = *p;
		if (input > 0) {
			if (_blocks[input] == 0) {
				ir_gcm_schedule_early(ctx, _blocks, input);
			}
			if (IR_OPND_KIND(flags, 1) != IR_OPND_CONTROL_DEP
			 && ctx->cfg_blocks[_blocks[ref]].dom_depth < ctx->cfg_blocks[_blocks[input]].dom_depth) {
				_blocks[ref] = _blocks[input];
			}
		}
	}
}

/* Last Common Ancestor */
static uint32_t ir_gcm_find_lca(ir_ctx *ctx, uint32_t b1, uint32_t b2)
{
	while (ctx->cfg_blocks[b1].dom_depth > ctx->cfg_blocks[b2].dom_depth) {
		b1 = ctx->cfg_blocks[b1].dom_parent;
	}
	while (ctx->cfg_blocks[b2].dom_depth > ctx->cfg_blocks[b1].dom_depth) {
		b2 = ctx->cfg_blocks[b2].dom_parent;
	}
	while (b1 != b2) {
		b1 = ctx->cfg_blocks[b1].dom_parent;
		b2 = ctx->cfg_blocks[b2].dom_parent;
	}
	return b2;
}

static void ir_gcm_schedule_late(ir_ctx *ctx, uint32_t *_blocks, ir_bitset visited, ir_ref ref)
{
	ir_ref i, n, *p, use;
	ir_insn *insn;
	uint32_t flags;

	ir_bitset_incl(visited, ref);
	n = ctx->use_lists[ref].count;
	if (n) {
		uint32_t lca, b;

		for (i = 0, p = &ctx->use_edges[ctx->use_lists[ref].refs]; i < n; i++, p++) {
			use = *p;
			if (!ir_bitset_in(visited, use) && _blocks[use]) {
				ir_gcm_schedule_late(ctx, _blocks, visited, use);
			}
		}
		insn = &ctx->ir_base[ref];
		flags = ir_op_flags[insn->op];
		if (IR_OPND_KIND(flags, 1) == IR_OPND_CONTROL_DEP) {
			return;
		}
		lca = 0;
		for (i = 0, p -= n; i < n; i++, p++) {
			use = *p;
			b = _blocks[use];
			if (!b) {
				continue;
			}
			insn = &ctx->ir_base[use];
			if (insn->op == IR_PHI) {
				if (insn->op2 == ref) {
					b = _blocks[ctx->ir_base[insn->op1].op1];
				} else if (insn->op3 == ref) {
					b = _blocks[ctx->ir_base[insn->op1].op2];
				} else {
					IR_ASSERT(0);
				}
			}
			lca = !lca ? b : ir_gcm_find_lca(ctx, lca, b);
		}
		b = lca;
		while (ctx->cfg_blocks[b].loop_depth && lca != ctx->cfg_blocks[_blocks[ref]].dom_parent) {
			if (ctx->cfg_blocks[lca].loop_depth < ctx->cfg_blocks[b].loop_depth) {
				b = lca;
			}
			lca = ctx->cfg_blocks[lca].dom_parent;
		}
		_blocks[ref] = b;
	}
}

int ir_gcm(ir_ctx *ctx)
{
	ir_ref i, j, k, n, *p, ref;
	ir_bitset visited;
	ir_block *bb;
	ir_list queue;
	uint32_t *_blocks;
	ir_insn *insn;
	uint32_t flags;

	_blocks = ir_mem_calloc(ctx->insns_count, sizeof(uint32_t));
	ir_list_init(&queue, ctx->insns_count);

	/* pin control instructions and collect their direct inputs */
	for (i = 1, bb = ctx->cfg_blocks + 1; i <= ctx->cfg_blocks_count; i++, bb++) {
		if (bb->flags & IR_BB_UNREACHABLE) {
			continue;
		}
		j = bb->end;
		while (1) {
			insn = &ctx->ir_base[j];
			_blocks[j] = i; /* pin to block */
			flags = ir_op_flags[insn->op];
			n = ir_input_edges_count(ctx, insn);
			for (k = 1, p = insn->ops + 1; k <= n; k++, p++) {
				ref = *p;
				if (ref > 0) {
					if (IR_OPND_KIND(flags, k) == IR_OPND_DATA || IR_OPND_KIND(flags, k) == IR_OPND_VAR) {
						ir_list_push(&queue, ref);
					}
				}
			}
			if (j == bb->start) {
				break;
			}
			j = insn->op1; /* control predecessor */
		}
	}

	n = ir_list_len(&queue);
	for (i = 0; i < n; i++) {
		ref = ir_list_at(&queue, i);
		if (_blocks[ref] == 0) {
			ir_gcm_schedule_early(ctx, _blocks, ref);
		}
	}

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_GCM) {
		fprintf(stderr, "GCM Schedule Early\n");
		for (i = 1; i < ctx->insns_count; i++) {
			fprintf(stderr, "%d -> %d\n", i, _blocks[i]);
		}
	}
#endif

	/* collect uses of control instructions */
	visited = ir_bitset_malloc(ctx->insns_count);
	ir_list_clear(&queue);
	for (i = 1, bb = ctx->cfg_blocks + 1; i <= ctx->cfg_blocks_count; i++, bb++) {
		if (bb->flags & IR_BB_UNREACHABLE) {
			continue;
		}
		j = bb->end;
		while (1) {
			ir_bitset_incl(visited, j);
			insn = &ctx->ir_base[j];
			n = ctx->use_lists[j].count;
			if (n > 0) {
				for (k = 0, p = &ctx->use_edges[ctx->use_lists[j].refs]; k < n; k++, p++) {
					ref = *p;
					if (ctx->ir_base[ref].op == IR_PARAM || ctx->ir_base[ref].op == IR_VAR) {
						_blocks[ref] = _blocks[j];
					} else if (ir_op_flags[ctx->ir_base[ref].op] & IR_OP_FLAG_DATA) {
						ir_list_push(&queue, ref);
					}
				}
			}
			if (j == bb->start) {
				break;
			}
			j = insn->op1; /* control predecessor */
		}
	}

	n = ir_list_len(&queue);
	for (i = 0; i < n; i++) {
		ref = ir_list_at(&queue, i);
		if (!ir_bitset_in(visited, ref)) {
			ir_gcm_schedule_late(ctx, _blocks, visited, ref);
		}
	}

	ir_mem_free(visited);
	ir_list_free(&queue);

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_GCM) {
		fprintf(stderr, "GCM Schedule Late\n");
		for (i = 1; i < ctx->insns_count; i++) {
			fprintf(stderr, "%d -> %d\n", i, _blocks[i]);
		}
	}
#endif

	ctx->cfg_map = _blocks;

	return 1;
}

int ir_schedule(ir_ctx *ctx)
{
	ir_ctx new_ctx;
	ir_ref i, j, k, n, *p, ref, new_ref, insns_count, consts_count;
	ir_ref *_xlat;
	uint32_t flags;
	uint32_t edges_count;
	ir_use_list *lists;
	ir_ref *edges;
	ir_bitset used;
	ir_ref b;
	uint32_t *_blocks = ctx->cfg_map;
	ir_ref *_next = ir_mem_calloc(ctx->insns_count, sizeof(ir_ref));
	ir_ref *_prev = ir_mem_calloc(ctx->insns_count, sizeof(ir_ref));
	ir_ref _rest = 0;
	ir_block *bb;
	ir_insn *insn, *new_insn;

    /* Create double-linked list of nodes ordered by BB, respecting BB->start and BB->end */
	IR_ASSERT(_blocks[1]);
	_prev[1] = 0;
	for (i = 2, j = 1; i < ctx->insns_count; i++) {
		b = _blocks[i];
		if (b) {
			bb = &ctx->cfg_blocks[b];
			if (_blocks[j] == b || i == bb->start) {
				/* add to the end of list */
				_next[j] = i;
				_prev[i] = j;
				j = i;
			} else if (_prev[bb->end]) {
				/* move up, insert before the end of alredy scheduled BB */
				k = bb->end;
				_prev[i] = _prev[k];
				_next[i] = k;
				_next[_prev[k]] = i;
				_prev[k] = i;
			} else {
				/* move down late */
				_next[i] = _rest;
				_rest = i;
			}
		}
	}
	_next[j] = 0;

	while (_rest) {
		i = _rest;
		_rest = _next[i];
		b = _blocks[i];
		bb = &ctx->cfg_blocks[b];
		if (i == bb->end) {
			// TODO: When removing MERGE, SCCP may move END of the block below other blocks ???
			/* insert at the end of the block */
			k = _next[bb->start];
			while (_blocks[k] == b) {
				k = _next[k];
			}
		} else {
			/* insert after start of the block and all PARAM, VAR, PI, PHI */
			k = _next[bb->start];
			insn = &ctx->ir_base[k];
			while (insn->op == IR_PARAM || insn->op == IR_VAR || insn->op == IR_PI || insn->op == IR_PHI) {
				k = _next[k];
				insn = &ctx->ir_base[k];
			}
		}
		/* insert before "k" */
		_prev[i] = _prev[k];
		_next[i] = k;
		_next[_prev[k]] = i;
		_prev[k] = i;
	}

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_SCHEDULE) {
		fprintf(stderr, "Before Schedule\n");
		for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; b++, bb++) {
			for (i = bb->start; i <= bb->end && i > 0; i = _next[i]) {
				fprintf(stderr, "%d -> %d\n", i, b);
			}
		}
	}
#endif

	/* Topological sort according dependencies inside each basic block */
	ir_bitset scheduled = ir_bitset_malloc(ctx->insns_count);
	for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; b++, bb++) {
		if (bb->flags & IR_BB_UNREACHABLE) {
			continue;
		}
		i = bb->start;
		ir_bitset_incl(scheduled, i);
		i = _next[i];
		insn = &ctx->ir_base[i];
		while (insn->op == IR_PARAM || insn->op == IR_VAR || insn->op == IR_PI || insn->op == IR_PHI) {
			ir_bitset_incl(scheduled, i);
			i = _next[i];
			insn = &ctx->ir_base[i];
		}
		for (; i != bb->end; i = _next[i]) {
			ir_ref n, j, *p, def;

restart:
			insn = &ctx->ir_base[i];
			n = ir_input_edges_count(ctx, insn);
			for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
				def = *p;
				if (def > 0
						&& _blocks[def] == b
						&& !ir_bitset_in(scheduled, def)
						&& insn->op != IR_PHI
						&& insn->op != IR_PI) {
					/* "def" should be before "i" to satisfy dependency */
#ifdef IR_DEBUG
					if (ctx->flags & IR_DEBUG_SCHEDULE) {
						fprintf(stderr, "Wrong dependency %d:%d -> %d\n", b, def, i);
					}
#endif
					/* remove "def" */
					_prev[_next[def]] = _prev[def];
					_next[_prev[def]] = _next[def];
					/* insert before "i" */
					_prev[def] = _prev[i];
					_next[def] = i;
					_next[_prev[i]] = def;
					_prev[i] = def;
					/* restart from "def" */
					i = def;
					goto restart;
				}
			}
			ir_bitset_incl(scheduled, i);
		}
	}
	ir_mem_free(scheduled);

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_SCHEDULE) {
		fprintf(stderr, "After Schedule\n");
		for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; b++, bb++) {
			for (i = bb->start; i <= bb->end && i > 0; i = _next[i]) {
				fprintf(stderr, "%d -> %d\n", i, b);
			}
		}
	}
#endif

	ir_mem_free(_prev);

	/* TODO: linearize without reallocation and reconstruction ??? */

	_xlat = ir_mem_calloc(ctx->consts_count + ctx->insns_count, sizeof(ir_ref));
	_xlat += ctx->consts_count;
	_xlat[IR_TRUE] = IR_TRUE;
	_xlat[IR_FALSE] = IR_FALSE;
	_xlat[IR_NULL] = IR_NULL;
	used = ir_bitset_malloc(ctx->consts_count + 1);
	insns_count = 1;
	consts_count = -(IR_TRUE - 1);

	for (i = 1; i != 0; i = _next[i]) {
		_xlat[i] = insns_count;
		insn = &ctx->ir_base[i];
		flags = ir_op_flags[insn->op];
		n = ir_input_edges_count(ctx, insn);
		for (k = 1, p = insn->ops + 1; k <= n; k++, p++) {
			ref = *p;
			if (ref < IR_TRUE) {
				if (!ir_bitset_in(used, -ref)) {
					ir_bitset_incl(used, -ref);
					consts_count++;
				}
			}
		}
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		insns_count += n;
	}

#if 1
	if (consts_count == ctx->consts_count && insns_count == ctx->insns_count) {
		bool changed = 0;

		for (i = 1; i != 0; i = _next[i]) {
			if (_xlat[i] != i) {
				changed = 1;
				break;
			}
		}
		if (!changed) {
			ir_mem_free(used);
			_xlat -= ctx->consts_count;
			ir_mem_free(_xlat);
			ir_mem_free(_next);

			ctx->flags |= IR_LINEAR;
			ir_truncate(ctx);

			return 1;
		}
	}
#endif

	lists = ir_mem_calloc(insns_count, sizeof(ir_use_list));
	ir_init(&new_ctx, consts_count, insns_count);
	new_ctx.flags = ctx->flags;
	new_ctx.fixed_stack_frame_size = ctx->fixed_stack_frame_size;
	new_ctx.fixed_regset = ctx->fixed_regset;
	new_ctx.fixed_save_regset = ctx->fixed_save_regset;

	IR_BITSET_FOREACH(used, ir_bitset_len(ctx->consts_count + 1), ref) {
		new_ref = new_ctx.consts_count;
		IR_ASSERT(new_ref < ctx->consts_limit);
		new_ctx.consts_count = new_ref + 1;
		_xlat[-ref] = new_ref = -new_ref;
		insn = &ctx->ir_base[-ref];
		new_insn = &new_ctx.ir_base[new_ref];
		new_insn->optx = insn->optx;
		new_insn->prev_const = 0;
		if (insn->op == IR_FUNC || insn->op == IR_STR) {
			new_insn->val.addr = ir_str(&new_ctx, ir_get_str(ctx, insn->val.addr));
		} else {
			new_insn->val.u64 = insn->val.u64;
		}
	} IR_BITSET_FOREACH_END();

	ir_mem_free(used);
	edges_count = 0;

	for (i = 1; i != 0; i = _next[i]) {
		insn = &ctx->ir_base[i];
		flags = ir_op_flags[insn->op];
		n = ir_operands_count(ctx, insn);

		new_ref = new_ctx.insns_count;
		new_ctx.insns_count = new_ref + 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI;
		IR_ASSERT(new_ctx.insns_count <= new_ctx.insns_limit);
		new_insn = &new_ctx.ir_base[new_ref];

		new_insn->optx = insn->optx;
		new_insn->op1 = IR_UNUSED;
		new_insn->op2 = IR_UNUSED;
		new_insn->op3 = IR_UNUSED;
		for (k = 1, p = insn->ops + 1; k <= n; k++, p++) {
			ref = *p;
			switch (IR_OPND_KIND(flags, k)) {
				case IR_OPND_DATA:
				case IR_OPND_VAR:
				case IR_OPND_CONTROL:
				case IR_OPND_CONTROL_DEP:
					ref = _xlat[ref];
					if (ref > 0) {
						lists[ref].refs = -1;
						lists[ref].count++;
						edges_count++;
					}
					break;
				case IR_OPND_CONTROL_REF:
					ref = _xlat[ref];
					break;
				case IR_OPND_STR:
					ref = ir_str(&new_ctx, ir_get_str(ctx, ref));
					break;
				case IR_OPND_NUM:
				case IR_OPND_PROB:
					break;
				default:
					IR_ASSERT(0);
					break;
			}
			new_insn->ops[k] = ref;
		}
	}

	edges = ir_mem_malloc(edges_count * sizeof(ir_ref));
	edges_count = 0;
	for (i = IR_UNUSED + 1, insn = new_ctx.ir_base + i; i < new_ctx.insns_count;) {
		n = ir_input_edges_count(&new_ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			ref = *p;
			if (ref > 0) {
				ir_use_list *use_list = &lists[ref];

				if (use_list->refs == -1) {
					use_list->refs = edges_count;
					edges_count += use_list->count;
					use_list->count = 0;
				}
				edges[use_list->refs + use_list->count++] = i;
			}
		}
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		i += n;
		insn += n;
	}

	new_ctx.use_edges = edges;
	new_ctx.use_lists = lists;

	if (ctx->cfg_blocks) {
		uint32_t b;
		ir_block *bb;

		new_ctx.cfg_blocks_count = ctx->cfg_blocks_count;
		if (ctx->cfg_edges_count) {
			new_ctx.cfg_edges_count = ctx->cfg_edges_count;
			new_ctx.cfg_edges = ir_mem_malloc(ctx->cfg_edges_count * sizeof(uint32_t));
			memcpy(new_ctx.cfg_edges, ctx->cfg_edges, ctx->cfg_edges_count * sizeof(uint32_t));
		}
		new_ctx.cfg_blocks = ir_mem_malloc((ctx->cfg_blocks_count + 1) * sizeof(ir_block));
		memcpy(new_ctx.cfg_blocks, ctx->cfg_blocks, (ctx->cfg_blocks_count + 1) * sizeof(ir_block));
		for (b = 1, bb = new_ctx.cfg_blocks + 1; b <= new_ctx.cfg_blocks_count; b++, bb++) {
			bb->start = _xlat[bb->start];
			bb->end = _xlat[bb->end];
		}
	}

	_xlat -= ctx->consts_count;
	ir_mem_free(_xlat);

	ir_free(ctx);
	IR_ASSERT(new_ctx.consts_count == new_ctx.consts_limit);
	IR_ASSERT(new_ctx.insns_count == new_ctx.insns_limit);
	memcpy(ctx, &new_ctx, sizeof(ir_ctx));
	ctx->flags |= IR_LINEAR;

	ir_mem_free(_next);

	return 1;
}
