#include "ir.h"
#include "ir_private.h"

/* GCM - Global Code Motion
 *
 * C. Click. "Global code motion, global value numbering" Submitted to PLDI ‘95.
 */
static void ir_gcm_schedule_early(ir_ctx *ctx, int *_blocks, ir_ref ref)
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
static int ir_gcm_find_lca(ir_ctx *ctx, int b1, int b2)
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

static void ir_gcm_schedule_late(ir_ctx *ctx, int *_blocks, ir_bitset visited, ir_ref ref)
{
	ir_ref i, n, *p, use;
	ir_insn *insn;
	uint32_t flags;

	ir_bitset_incl(visited, ref);
	n = ctx->use_lists[ref].count;
	if (n) {
		int lca, b;

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

		while (lca != ctx->cfg_blocks[_blocks[ref]].dom_parent) {
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
	int *_blocks;
	ir_insn *insn;
	uint32_t flags;

	_blocks = ir_mem_malloc(ctx->insns_count * sizeof(int));
	memset(_blocks, 0, ctx->insns_count * sizeof(int));
	ir_list_init(&queue, ctx->insns_count);

	/* pin control instructions and collect their direct inputs */
	for (i = 1, bb = ctx->cfg_blocks + 1; i <= ctx->cfg_blocks_count; i++, bb++) {
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

	ctx->gcm_blocks = _blocks;

	return 1;
}

static int ir_copy(ir_ctx *new_ctx, ir_ctx *ctx, ir_ref *_next, bool preserve_constants_order)
{
	ir_ref i, j, k, n, *p, ref, new_ref;
	ir_ref *_xlat;
	ir_insn *insn;
	uint32_t flags;

	if (preserve_constants_order) {
		ir_bitset used = ir_bitset_malloc(ctx->consts_count + 1);

		_xlat = ir_mem_calloc(ctx->consts_count + ctx->insns_count, sizeof(ir_ref));
		_xlat += ctx->consts_count;

		for (j = 1, i = 1; i != 0; i = _next[i]) {
			_xlat[i] = j;
			insn = &ctx->ir_base[i];
			flags = ir_op_flags[insn->op];
			n = ir_operands_count(ctx, insn);
			for (k = 1, p = insn->ops + 1; k <= n; k++, p++) {
				ref = *p;
				if (IR_OPND_KIND(flags, k) == IR_OPND_DATA && IR_IS_CONST_REF(ref)) {
					ir_bitset_incl(used, -ref);
				}
			}
			n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
			j += n;
		}

		IR_BITSET_FOREACH(used, ir_bitset_len(ctx->consts_count + 1), ref) {
			if (ctx->ir_base[-ref].op == IR_FUNC) {
				_xlat[-ref] = ir_const_func(new_ctx, ir_str(new_ctx, ir_get_str(ctx, ctx->ir_base[-ref].val.addr)));
			} else if (ctx->ir_base[-ref].op == IR_STR) {
				_xlat[-ref] = ir_const_str(new_ctx, ir_str(new_ctx, ir_get_str(ctx, ctx->ir_base[-ref].val.addr)));
			} else {
				_xlat[-ref] = ir_const(new_ctx, ctx->ir_base[-ref].val, ctx->ir_base[-ref].type);
			}
		} IR_BITSET_FOREACH_END();
		ir_mem_free(used);
	} else {
		_xlat = ir_mem_calloc(ctx->insns_count, sizeof(ir_ref));
		for (j = 1, i = 1; i != 0; i = _next[i]) {
			_xlat[i] = j;
			insn = &ctx->ir_base[i];
			n = ir_operands_count(ctx, insn);
			n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
			j += n;
		}
	}

	for (i = 1; i != 0; i = _next[i]) {
		insn = &ctx->ir_base[i];
		flags = ir_op_flags[insn->op];
		n = ir_operands_count(ctx, insn);
		if (insn->op == IR_MERGE && n != 2) {
			new_ref = ir_emit_N(new_ctx, insn->opt, n);
		} else if (n <= 3) {
			new_ref = ir_emit0(new_ctx, insn->opt);
		} else {
			new_ref = ir_emit_N(new_ctx, insn->opt, n);
		}
		for (k = 1, p = insn->ops + 1; k <= n; k++, p++) {
			ref = *p;
			switch (IR_OPND_KIND(flags, k)) {
				case IR_OPND_DATA:
				case IR_OPND_VAR:
					if (IR_IS_CONST_REF(ref) && !preserve_constants_order) {
						if (ctx->ir_base[-ref].op == IR_FUNC) {
							ref = ir_const_func(new_ctx, ir_str(new_ctx, ir_get_str(ctx, ctx->ir_base[ref].val.addr)));
						} else if (ctx->ir_base[-ref].op == IR_STR) {
							ref = ir_const_str(new_ctx, ir_str(new_ctx, ir_get_str(ctx, ctx->ir_base[ref].val.addr)));
						} else {
							ref = ir_const(new_ctx, ctx->ir_base[ref].val, ctx->ir_base[ref].type);
						}
						break;
					}
				case IR_OPND_CONTROL:
				case IR_OPND_CONTROL_DEP:
				case IR_OPND_CONTROL_REF:
					ref = _xlat[ref];
					break;
				case IR_OPND_STR:
					ref = ir_str(new_ctx, ir_get_str(ctx, ref));
					break;
				case IR_OPND_NUM:
				case IR_OPND_PROB:
					break;
				default:
					IR_ASSERT(0);
					break;
			}
			ir_set_op(new_ctx, new_ref, k, ref);
		}
	}

	if (ctx->cfg_blocks) {
		int b;
		ir_block *bb;

		new_ctx->cfg_blocks_count = ctx->cfg_blocks_count;
		if (ctx->cfg_edges_count) {
			new_ctx->cfg_edges_count = ctx->cfg_edges_count;
			new_ctx->cfg_edges = ir_mem_malloc(ctx->cfg_edges_count * sizeof(uint32_t));
			memcpy(new_ctx->cfg_edges, ctx->cfg_edges, ctx->cfg_edges_count * sizeof(uint32_t));
		}
		new_ctx->cfg_blocks = ir_mem_malloc((ctx->cfg_blocks_count + 1) * sizeof(ir_block));
		memcpy(new_ctx->cfg_blocks, ctx->cfg_blocks, (ctx->cfg_blocks_count + 1) * sizeof(ir_block));
		for (b = 1, bb = new_ctx->cfg_blocks + 1; b <= new_ctx->cfg_blocks_count; b++, bb++) {
			bb->start = _xlat[bb->start];
			bb->end = _xlat[bb->end];
		}
	}

	if (preserve_constants_order) {
		_xlat -= ctx->consts_count;
	}
	ir_mem_free(_xlat);

	return 1;
}

int ir_schedule(ir_ctx *ctx)
{
	ir_ctx new_ctx;
	ir_ref i, j, k;
	ir_ref b;
	ir_ref *_blocks = ctx->gcm_blocks;
	ir_ref *_next = ir_mem_calloc(ctx->insns_count, sizeof(ir_ref));
	ir_ref *_prev = ir_mem_calloc(ctx->insns_count, sizeof(ir_ref));
	ir_ref _rest = 0;
	ir_block *bb;
	ir_insn *insn;

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
		/* insert after start of the block and all PARAM, VAR, PI, PHI */
		k = _next[bb->start];
		insn = &ctx->ir_base[k];
		while (insn->op == IR_PARAM || insn->op == IR_VAR || insn->op == IR_PI || insn->op == IR_PHI) {
			k = _next[k];
			insn = &ctx->ir_base[k];
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
	uint32_t len = ir_bitset_len(ctx->insns_count);
	ir_bitset scheduled = ir_bitset_malloc(ctx->insns_count);
	for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; b++, bb++) {
		ir_bitset_clear(scheduled, len);
		for (i = bb->start; i <= bb->end && i > 0; i = _next[i]) {
			ir_ref n, j, *p, def;

restart:
			insn = &ctx->ir_base[i];
			n = ir_input_edges_count(ctx, insn);
			for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
				def = *p;
				if (def && _blocks[def] == b
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

	/* Linearization */
	ir_init(&new_ctx, ctx->consts_count, ctx->insns_count);
	new_ctx.flags = ctx->flags;
	/* TODO: linearize without reallocation and reconstruction ??? */
	if (!ir_copy(&new_ctx, ctx, _next, 1)) {
		ir_free(&new_ctx);
		return 0;
	}
	ir_free(ctx);
	ir_build_def_use_lists(&new_ctx);
	ir_truncate(&new_ctx);
	memcpy(ctx, &new_ctx, sizeof(ir_ctx));
	ctx->flags |= IR_LINEAR;
	ir_mem_free(_next);

	return 1;
}
