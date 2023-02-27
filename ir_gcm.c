/*
 * IR - Lightweight JIT Compilation Framework
 * (GCM - Global Code Motion and Scheduler)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 *
 * The GCM algorithm is based on Cliff Click's publication
 * See: C. Click. "Global code motion, global value numbering" Submitted to PLDI'95.
 */

#include "ir.h"
#include "ir_private.h"

static void ir_gcm_schedule_early(ir_ctx *ctx, uint32_t *_blocks, ir_ref ref, ir_list *queue_rest)
{
	ir_ref n, *p, input;
	ir_insn *insn;
	uint32_t dom_depth, b;
	bool reschedule_late = 1;

	insn = &ctx->ir_base[ref];

	IR_ASSERT(insn->op != IR_PARAM && insn->op != IR_VAR);
	IR_ASSERT(insn->op != IR_PHI && insn->op != IR_PI);

	_blocks[ref] = 1;
	dom_depth = 0;

	n = ir_input_edges_count(ctx, insn);
	for (p = insn->ops + 1; n > 0; p++, n--) {
		input = *p;
		if (input > 0) {
			if (_blocks[input] == 0) {
				ir_gcm_schedule_early(ctx, _blocks, input, queue_rest);
			}
			b = _blocks[input];
			if (dom_depth < ctx->cfg_blocks[b].dom_depth) {
				dom_depth = ctx->cfg_blocks[b].dom_depth;
				_blocks[ref] = b;
			}
			reschedule_late = 0;
		}
	}
	if (UNEXPECTED(reschedule_late)) {
		/* Floating nodes that doesn't sepend on other nodes
		 * (e.g. only on constants), has to be scheduled to the
		 * last common ancestor. Otherwise they always goes to the
		 * first block.
		 *
		 * TODO:
		 * It's possible to reuse ir_gcm_schedule_late() and move
		 * these nodes out of the loops, but then we mgiht need
		 * to rematerialize them at proper place(s).
		 */
		ir_list_push_unchecked(queue_rest, ref);
	}
}

/* Last Common Ancestor */
static uint32_t ir_gcm_find_lca(ir_ctx *ctx, uint32_t b1, uint32_t b2)
{
	uint32_t dom_depth;

	dom_depth = ctx->cfg_blocks[b2].dom_depth;
	while (ctx->cfg_blocks[b1].dom_depth > dom_depth) {
		b1 = ctx->cfg_blocks[b1].dom_parent;
	}
	dom_depth = ctx->cfg_blocks[b1].dom_depth;
	while (ctx->cfg_blocks[b2].dom_depth > dom_depth) {
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
	ir_ref n, *p, use;
	ir_insn *insn;

	ir_bitset_incl(visited, ref);
	n = ctx->use_lists[ref].count;
	if (n) {
		uint32_t lca, b;

		insn = &ctx->ir_base[ref];
		IR_ASSERT(insn->op != IR_PARAM && insn->op != IR_VAR);
		IR_ASSERT(insn->op != IR_PHI && insn->op != IR_PI);

		lca = 0;
		for (p = &ctx->use_edges[ctx->use_lists[ref].refs]; n > 0; p++, n--) {
			use = *p;
			b = _blocks[use];
			if (!b) {
				continue;
			} else if (!ir_bitset_in(visited, use)) {
				ir_gcm_schedule_late(ctx, _blocks, visited, use);
				b = _blocks[use];
				IR_ASSERT(b != 0);
			}
			insn = &ctx->ir_base[use];
			if (insn->op == IR_PHI) {
				ir_ref *p = insn->ops + 2; /* PHI data inputs */
				ir_ref *q = ctx->ir_base[insn->op1].ops + 1; /* MERGE inputs */

				while (*p != ref) {
					p++;
					q++;
				}
				b = _blocks[*q];
				IR_ASSERT(b);
			}
			lca = !lca ? b : ir_gcm_find_lca(ctx, lca, b);
		}
		IR_ASSERT(lca != 0 && "No Common Antecessor");
		b = lca;
		uint32_t loop_depth = ctx->cfg_blocks[b].loop_depth;
		if (loop_depth) {
			while (lca != ctx->cfg_blocks[_blocks[ref]].dom_parent) {
				if (ctx->cfg_blocks[lca].loop_depth < loop_depth) {
					loop_depth = ctx->cfg_blocks[lca].loop_depth;
					b = lca;
					if (!loop_depth) {
						break;
					}
				}
				lca = ctx->cfg_blocks[lca].dom_parent;
			}
		}
		_blocks[ref] = b;
	}
}

static void ir_gcm_schedule_rest(ir_ctx *ctx, uint32_t *_blocks, ir_bitset visited, ir_ref ref)
{
	ir_ref n, *p, use;
	ir_insn *insn;

	ir_bitset_incl(visited, ref);
	n = ctx->use_lists[ref].count;
	if (n) {
		uint32_t lca, b;

		insn = &ctx->ir_base[ref];
		IR_ASSERT(insn->op != IR_PARAM && insn->op != IR_VAR);
		IR_ASSERT(insn->op != IR_PHI && insn->op != IR_PI);

		lca = 0;
		for (p = &ctx->use_edges[ctx->use_lists[ref].refs]; n > 0; p++, n--) {
			use = *p;
			b = _blocks[use];
			if (!b) {
				continue;
			} else if (!ir_bitset_in(visited, use)) {
				ir_gcm_schedule_late(ctx, _blocks, visited, use);
				b = _blocks[use];
				IR_ASSERT(b != 0);
			}
			insn = &ctx->ir_base[use];
			if (insn->op == IR_PHI) {
				ir_ref *p = insn->ops + 2; /* PHI data inputs */
				ir_ref *q = ctx->ir_base[insn->op1].ops + 1; /* MERGE inputs */

				while (*p != ref) {
					p++;
					q++;
				}
				b = _blocks[*q];
				IR_ASSERT(b);
			}
			lca = !lca ? b : ir_gcm_find_lca(ctx, lca, b);
		}
		IR_ASSERT(lca != 0 && "No Common Antecessor");
		b = lca;
		_blocks[ref] = b;
	}
}

int ir_gcm(ir_ctx *ctx)
{
	ir_ref k, n, *p, ref;
	ir_bitset visited;
	ir_block *bb;
	ir_list queue_early;
	ir_list queue_late;
	ir_list queue_rest;
	uint32_t *_blocks, b;
	ir_insn *insn, *use_insn;
	ir_use_list *use_list;
	uint32_t flags;

	IR_ASSERT(ctx->cfg_map);
	_blocks = ctx->cfg_map;

	ir_list_init(&queue_early, ctx->insns_count);

	if (ctx->cfg_blocks_count == 1) {
		ref = ctx->cfg_blocks[1].end;
		do {
			insn = &ctx->ir_base[ref];
			_blocks[ref] = 1; /* pin to block */
			flags = ir_op_flags[insn->op];
#if 1
			n = IR_INPUT_EDGES_COUNT(flags);
			if (!IR_IS_FIXED_INPUTS_COUNT(n) || n > 1) {
				ir_list_push_unchecked(&queue_early, ref);
			}
#else
			if (IR_OPND_KIND(flags, 2) == IR_OPND_DATA
			 || IR_OPND_KIND(flags, 3) == IR_OPND_DATA) {
				ir_list_push_unchecked(&queue_early, ref);
			}
#endif
			ref = insn->op1; /* control predecessor */
		} while (ref != 1); /* IR_START */
		_blocks[1] = 1; /* pin to block */

		use_list = &ctx->use_lists[1];
		n = use_list->count;
		for (p = &ctx->use_edges[use_list->refs]; n > 0; n--, p++) {
			ref = *p;
			use_insn = &ctx->ir_base[ref];
			if (use_insn->op == IR_PARAM || use_insn->op == IR_VAR) {
				_blocks[ref] = 1; /* pin to block */
			}
		}

		/* Place all live nodes to the first block */
		while (ir_list_len(&queue_early)) {
			ref = ir_list_pop(&queue_early);
			insn = &ctx->ir_base[ref];
			n = ir_input_edges_count(ctx, insn);
			for (p = insn->ops + 1; n > 0; p++, n--) {
				ref = *p;
				if (ref > 0 && _blocks[ref] == 0) {
					_blocks[ref] = 1;
					ir_list_push_unchecked(&queue_early, ref);
				}
			}
		}

		ir_list_free(&queue_early);

		return 1;
	}

	ir_list_init(&queue_late, ctx->insns_count);
	visited = ir_bitset_malloc(ctx->insns_count);

	/* pin and collect control and control depended (PARAM, VAR, PHI, PI) instructions */
	b = ctx->cfg_blocks_count;
	for (bb = ctx->cfg_blocks + b; b > 0; bb--, b--) {
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		ref = bb->end;
		do {
			insn = &ctx->ir_base[ref];
			ir_bitset_incl(visited, ref);
			_blocks[ref] = b; /* pin to block */
			flags = ir_op_flags[insn->op];
#if 1
			n = IR_INPUT_EDGES_COUNT(flags);
			if (!IR_IS_FIXED_INPUTS_COUNT(n) || n > 1) {
				ir_list_push_unchecked(&queue_early, ref);
			}
#else
			if (IR_OPND_KIND(flags, 2) == IR_OPND_DATA
			 || IR_OPND_KIND(flags, 3) == IR_OPND_DATA) {
				ir_list_push_unchecked(&queue_early, ref);
			}
#endif
			if (insn->type != IR_VOID) {
				IR_ASSERT(flags & IR_OP_FLAG_MEM);
				ir_list_push_unchecked(&queue_late, ref);
			}
			ref = insn->op1; /* control predecessor */
		} while (ref != bb->start);
		ir_bitset_incl(visited, ref);
		_blocks[ref] = b; /* pin to block */

		use_list = &ctx->use_lists[ref];
		n = use_list->count;
		for (p = &ctx->use_edges[use_list->refs]; n > 0; n--, p++) {
			ref = *p;
			use_insn = &ctx->ir_base[ref];
			if (use_insn->op == IR_PARAM || use_insn->op == IR_VAR) {
				_blocks[ref] = b; /* pin to block */
				ir_bitset_incl(visited, ref);
			} else if (use_insn->op == IR_PHI || use_insn->op == IR_PI) {
				ir_bitset_incl(visited, ref);
				if (EXPECTED(ctx->use_lists[ref].count != 0)) {
					_blocks[ref] = b; /* pin to block */
					ir_list_push_unchecked(&queue_early, ref);
					ir_list_push_unchecked(&queue_late, ref);
				}
			}
		}
	}

	ir_list_init(&queue_rest, ctx->insns_count);

	n = ir_list_len(&queue_early);
	while (n > 0) {
		n--;
		ref = ir_list_at(&queue_early, n);
		insn = &ctx->ir_base[ref];
		k = ir_input_edges_count(ctx, insn) - 1;
		for (p = insn->ops + 2; k > 0; p++, k--) {
			ref = *p;
			if (ref > 0 && _blocks[ref] == 0) {
				ir_gcm_schedule_early(ctx, _blocks, ref, &queue_rest);
			}
		}
	}

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_GCM) {
		fprintf(stderr, "GCM Schedule Early\n");
		for (n = 1; n < ctx->insns_count; n++) {
			fprintf(stderr, "%d -> %d\n", n, _blocks[n]);
		}
	}
#endif

	n = ir_list_len(&queue_late);
	while (n > 0) {
		n--;
		ref = ir_list_at(&queue_late, n);
		use_list = &ctx->use_lists[ref];
		k = use_list->count;
		for (p = &ctx->use_edges[use_list->refs]; k > 0; p++, k--) {
			ref = *p;
			if (!ir_bitset_in(visited, ref) && _blocks[ref]) {
				ir_gcm_schedule_late(ctx, _blocks, visited, ref);
			}
		}
	}

	n = ir_list_len(&queue_rest);
	while (n > 0) {
		n--;
		ref = ir_list_at(&queue_rest, n);
		ir_gcm_schedule_rest(ctx, _blocks, visited, ref);
	}

	ir_mem_free(visited);
	ir_list_free(&queue_early);
	ir_list_free(&queue_late);
	ir_list_free(&queue_rest);

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_GCM) {
		fprintf(stderr, "GCM Schedule Late\n");
		for (n = 1; n < ctx->insns_count; n++) {
			fprintf(stderr, "%d -> %d\n", n, _blocks[n]);
		}
	}
#endif

	return 1;
}

static void ir_xlat_binding(ir_ctx *ctx, ir_ref *_xlat)
{
	uint32_t n1, n2, pos;
	ir_ref key;
	ir_hashtab_bucket *b1, *b2;
	ir_hashtab *binding = ctx->binding;
	uint32_t hash_size = (uint32_t)(-(int32_t)binding->mask);

	memset((char*)binding->data - (hash_size * sizeof(uint32_t)), -1, hash_size * sizeof(uint32_t));
	n1 = binding->count;
	n2 = 0;
	pos = 0;
	b1 = binding->data;
	b2 = binding->data;
	while (n1 > 0) {
		key = b1->key;
		IR_ASSERT(key < ctx->insns_count);
		if (_xlat[key]) {
			key = _xlat[key];
			b2->key = key;
			if (b1->val > 0) {
				IR_ASSERT(_xlat[b1->val]);
				b2->val = _xlat[b1->val];
			} else {
				b2->val = b1->val;
			}
			key |= binding->mask;
			b2->next = ((uint32_t*)binding->data)[key];
			((uint32_t*)binding->data)[key] = pos;
			pos += sizeof(ir_hashtab_bucket);
			b2++;
			n2++;
		}
		b1++;
		n1--;
	}
	binding->count = n2;
}

IR_ALWAYS_INLINE ir_ref ir_count_constant(ir_bitset used, ir_ref ref)
{
	if (!ir_bitset_in(used, -ref)) {
		ir_bitset_incl(used, -ref);
		return 1;
	}
	return 0;
}

int ir_schedule(ir_ctx *ctx)
{
	ir_ctx new_ctx;
	ir_ref i, j, k, n, *p, *q, ref, new_ref, prev_ref, insns_count, consts_count, edges_count;
	ir_ref *_xlat;
	uint32_t flags;
	ir_ref *edges;
	ir_bitset used, scheduled;
	uint32_t b, prev_b;
	uint32_t *_blocks = ctx->cfg_map;
	ir_ref *_next = ir_mem_malloc(ctx->insns_count * sizeof(ir_ref));
	ir_ref *_prev = ir_mem_malloc(ctx->insns_count * sizeof(ir_ref));
	ir_ref _move_down = 0;
	ir_block *bb;
	ir_insn *insn, *new_insn;
	ir_use_list *lists, *use_list, *new_list;

	/* Create a double-linked list of nodes ordered by BB, respecting BB->start and BB->end */
	prev_b = _blocks[1];
	IR_ASSERT(prev_b);
	_prev[1] = 0;
	_prev[ctx->cfg_blocks[1].end] = 0;
	for (i = 2, j = 1; i < ctx->insns_count; i++) {
		b = _blocks[i];
		if (b == prev_b) {
			/* add to the end of the list */
			_next[j] = i;
			_prev[i] = j;
			j = i;
		} else if (b > prev_b) {
			bb = &ctx->cfg_blocks[b];
			if (i == bb->start) {
				IR_ASSERT(bb->end > bb->start);
				prev_b = b;
				_prev[bb->end] = 0;
				/* add to the end of the list */
				_next[j] = i;
				_prev[i] = j;
				j = i;
			} else {
				IR_ASSERT(i != bb->end);
				/* move down late (see the following loop) */
				_next[i] = _move_down;
				_move_down = i;
			}
        } else if (b) {
			bb = &ctx->cfg_blocks[b];
			IR_ASSERT(i != bb->start);
			if (_prev[bb->end]) {
				/* move up, insert before the end of the alredy scheduled BB */
				k = bb->end;
			} else {
				/* move up, insert at the end of the block */
				k = ctx->cfg_blocks[b + 1].start;
			}
			/* insert before "k" */
			_prev[i] = _prev[k];
			_next[i] = k;
			_next[_prev[k]] = i;
			_prev[k] = i;
		}
	}
	_next[j] = 0;

	while (_move_down) {
		i = _move_down;
		_move_down = _next[i];
		b = _blocks[i];
		bb = &ctx->cfg_blocks[b];

		/* insert after the start of the block and all PARAM, VAR, PI, PHI */
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
		for (i = 1; i != 0; i = _next[i]) {
			fprintf(stderr, "%d -> %d\n", i, _blocks[i]);
		}
	}
#endif

	_xlat = ir_mem_malloc((ctx->consts_count + ctx->insns_count) * sizeof(ir_ref));
	if (ctx->binding) {
		memset(_xlat, 0, (ctx->consts_count + ctx->insns_count) * sizeof(ir_ref));
	}
	_xlat += ctx->consts_count;
	_xlat[IR_TRUE] = IR_TRUE;
	_xlat[IR_FALSE] = IR_FALSE;
	_xlat[IR_NULL] = IR_NULL;
	_xlat[IR_UNUSED] = IR_UNUSED;
	insns_count = 1;
	consts_count = -(IR_TRUE - 1);

	/* Topological sort according dependencies inside each basic block */
	scheduled = ir_bitset_malloc(ctx->insns_count);
	used = ir_bitset_malloc(ctx->consts_count + 1);
	for (b = 1, bb = ctx->cfg_blocks + 1; b <= ctx->cfg_blocks_count; b++, bb++) {
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		/* Schedule BB start */
		i = bb->start;
		ir_bitset_incl(scheduled, i);
		_xlat[i] = bb->start = insns_count;
		insn = &ctx->ir_base[i];
		if (insn->op == IR_CASE_VAL) {
			IR_ASSERT(insn->op2 < IR_TRUE);
			consts_count += ir_count_constant(used, insn->op2);
		}
		n = ir_input_edges_count(ctx, insn);
		insns_count += 1 + (n >> 2); // support for multi-word instructions like MERGE
		i = _next[i];
		insn = &ctx->ir_base[i];
		/* Schedule PARAM, VAR, PI */
		while (insn->op == IR_PARAM || insn->op == IR_VAR || insn->op == IR_PI) {
			ir_bitset_incl(scheduled, i);
			_xlat[i] = insns_count;
			insns_count += 1;
			i = _next[i];
			insn = &ctx->ir_base[i];
		}
		/* Schedule PHIs */
		while (insn->op == IR_PHI) {
			ir_ref j, *p, input;

			ir_bitset_incl(scheduled, i);
			_xlat[i] = insns_count;
			/* Reuse "n" from MERGE and skip first input */
			insns_count += 1 + ((n + 1) >> 2); // support for multi-word instructions like PHI
			for (j = n, p = insn->ops + 2; j > 0; p++, j--) {
				input = *p;
				if (input < IR_TRUE) {
					consts_count += ir_count_constant(used, input);
				}
			}
			i = _next[i];
			insn = &ctx->ir_base[i];
		}
		while (i != bb->end) {
			ir_ref n, j, *p, input;

restart:
			n = ir_input_edges_count(ctx, insn);
			for (j = n, p = insn->ops + 1; j > 0; p++, j--) {
				input = *p;
				if (input > 0) {
					if (!ir_bitset_in(scheduled, input) && _blocks[input] == b) {
						/* "input" should be before "i" to satisfy dependency */
#ifdef IR_DEBUG
						if (ctx->flags & IR_DEBUG_SCHEDULE) {
							fprintf(stderr, "Wrong dependency %d:%d -> %d\n", b, input, i);
						}
#endif
						/* remove "input" */
						_prev[_next[input]] = _prev[input];
						_next[_prev[input]] = _next[input];
						/* insert before "i" */
						_prev[input] = _prev[i];
						_next[input] = i;
						_next[_prev[i]] = input;
						_prev[i] = input;
						/* restart from "input" */
						i = input;
						insn = &ctx->ir_base[i];
						goto restart;
					}
				} else if (input < IR_TRUE) {
					consts_count += ir_count_constant(used, input);
				}
			}
			ir_bitset_incl(scheduled, i);
			_xlat[i] = insns_count;
			insns_count += 1 + (n >> 2); // support for multi-word instructions like CALL
			i = _next[i];
			insn = &ctx->ir_base[i];
		}
		/* Schedule BB end */
		ir_bitset_incl(scheduled, i);
		_xlat[i] = bb->end = insns_count;
		insns_count++;
		if (IR_INPUT_EDGES_COUNT(ir_op_flags[insn->op]) == 2) {
			if (insn->op2 < IR_TRUE) {
				consts_count += ir_count_constant(used, insn->op2);
			}
		}
	}

#ifdef IR_DEBUG
	if (ctx->flags & IR_DEBUG_SCHEDULE) {
		fprintf(stderr, "After Schedule\n");
		for (i = 1; i != 0; i = _next[i]) {
			fprintf(stderr, "%d -> %d\n", i, _blocks[i]);
		}
	}
#endif

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
			ir_mem_free(scheduled);
			_xlat -= ctx->consts_count;
			ir_mem_free(_xlat);
			ir_mem_free(_next);

			ctx->prev_ref = _prev;
			ctx->flags |= IR_LINEAR;
			ir_truncate(ctx);

			return 1;
		}
	}
#endif

	ir_mem_free(_prev);

	ir_init(&new_ctx, consts_count, insns_count);
	new_ctx.insns_count = insns_count;
	new_ctx.flags = ctx->flags;
	new_ctx.spill_base = ctx->spill_base;
	new_ctx.fixed_stack_red_zone = ctx->fixed_stack_red_zone;
	new_ctx.fixed_stack_frame_size = ctx->fixed_stack_frame_size;
	new_ctx.fixed_call_stack_size = ctx->fixed_call_stack_size;
	new_ctx.fixed_regset = ctx->fixed_regset;
	new_ctx.fixed_save_regset = ctx->fixed_save_regset;

	/* Copy constants */
	if (consts_count == ctx->consts_count) {
		new_ctx.consts_count = consts_count;
		ref = 1 - consts_count;
		insn = &ctx->ir_base[ref];
		new_insn = &new_ctx.ir_base[ref];

		memcpy(new_insn, insn, sizeof(ir_insn) * (IR_TRUE - ref));
		while (ref != IR_TRUE) {
			_xlat[ref] = ref;
			if (new_insn->op == IR_FUNC || new_insn->op == IR_STR) {
				new_insn->val.addr = ir_str(&new_ctx, ir_get_str(ctx, new_insn->val.i32));
			}
			new_insn++;
			ref++;
		}
	} else {
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
				new_insn->val.addr = ir_str(&new_ctx, ir_get_str(ctx, insn->val.i32));
			} else {
				new_insn->val.u64 = insn->val.u64;
			}
		} IR_BITSET_FOREACH_END();
	}

	ir_mem_free(used);

	/* Copy instructions and count use edges */
	edges_count = 0;
	for (i = 1; i != 0; i = _next[i]) {
		insn = &ctx->ir_base[i];

		new_ref = _xlat[i];
		new_insn = &new_ctx.ir_base[new_ref];
		*new_insn = *insn;

		n = ir_input_edges_count(ctx, insn);
		for (j = n, p = insn->ops + 1, q = new_insn->ops + 1; j > 0; p++, q++, j--) {
			ref = *p;
			*q = ref = _xlat[ref];
			edges_count += (ref > 0);
		}

		flags = ir_op_flags[insn->op];
		j = IR_OPERANDS_COUNT(flags);
		if (j > n) {
			switch (j) {
				case 3:
					switch (IR_OPND_KIND(flags, 3)) {
						case IR_OPND_CONTROL_REF:
							new_insn->op3 = _xlat[insn->op3];
							break;
						case IR_OPND_STR:
							new_insn->op3 = ir_str(&new_ctx, ir_get_str(ctx, insn->op3));
							break;
						default:
							break;
					}
					if (n == 2) {
						break;
					}
					IR_FALLTHROUGH;
				case 2:
					switch (IR_OPND_KIND(flags, 2)) {
						case IR_OPND_CONTROL_REF:
							new_insn->op2 = _xlat[insn->op2];
							break;
						case IR_OPND_STR:
							new_insn->op2 = ir_str(&new_ctx, ir_get_str(ctx, insn->op2));
							break;
						default:
							break;
					}
					if (n == 1) {
						break;
					}
					IR_FALLTHROUGH;
				case 1:
					switch (IR_OPND_KIND(flags, 1)) {
						case IR_OPND_CONTROL_REF:
							new_insn->op1 = _xlat[insn->op1];
							break;
						case IR_OPND_STR:
							new_insn->op1 = ir_str(&new_ctx, ir_get_str(ctx, insn->op1));
							break;
						default:
							break;
					}
					break;
				default:
					break;
			}
		}
	}

	if (ctx->binding) {
		ir_xlat_binding(ctx, _xlat);
		new_ctx.binding = ctx->binding;
		ctx->binding = NULL;
	}

	/* Copy use lists and edges */
	new_ctx.use_lists = lists = ir_mem_malloc(insns_count * sizeof(ir_use_list));
	new_ctx.use_edges = edges = ir_mem_malloc(edges_count * sizeof(ir_ref));
	new_ctx.use_edges_count = edges_count;
	new_ctx.prev_ref = _prev = ir_mem_malloc(insns_count * sizeof(ir_ref));
	prev_ref = 0;
	edges_count = 0;
	for (i = 1; i != 0; i = _next[i]) {
		use_list = &ctx->use_lists[i];
		new_ref = _xlat[i];
		_prev[new_ref] = prev_ref;
		prev_ref = new_ref;
		new_list = &lists[new_ref];
		new_list->refs = edges_count;
		n = use_list->count;
		k = 0;
		if (n) {
			for (p = &ctx->use_edges[use_list->refs]; n > 0; n--, p++) {
				ref = *p;
				if (ir_bitset_in(scheduled, ref)) {
					*edges = _xlat[ref];
					edges++;
					k++;
				}
			}
			edges_count += k;
		}
		new_list->count = k;
	}
	IR_ASSERT(new_ctx.use_edges_count >= edges_count);

	ir_mem_free(scheduled);
	_xlat -= ctx->consts_count;
	ir_mem_free(_xlat);

	new_ctx.cfg_blocks_count = ctx->cfg_blocks_count;
	new_ctx.cfg_edges_count = ctx->cfg_edges_count;
	new_ctx.cfg_blocks = ctx->cfg_blocks;
	new_ctx.cfg_edges = ctx->cfg_edges;
	ctx->cfg_blocks = NULL;
	ctx->cfg_edges = NULL;

	ir_free(ctx);
	IR_ASSERT(new_ctx.consts_count == new_ctx.consts_limit);
	IR_ASSERT(new_ctx.insns_count == new_ctx.insns_limit);
	memcpy(ctx, &new_ctx, sizeof(ir_ctx));
	ctx->flags |= IR_LINEAR;

	ir_mem_free(_next);

	return 1;
}

void ir_build_prev_refs(ir_ctx *ctx)
{
	uint32_t b;
	ir_block *bb;
	ir_ref i, n, prev;
	ir_insn *insn;

	ctx->prev_ref = ir_mem_malloc(ctx->insns_count * sizeof(ir_ref));
	prev = 0;
	for (b = 1, bb = ctx->cfg_blocks + b; b <= ctx->cfg_blocks_count; b++, bb++) {
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		for (i = bb->start, insn = ctx->ir_base + i; i < bb->end;) {
			ctx->prev_ref[i] = prev;
			n = ir_operands_count(ctx, insn);
			n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
			prev = i;
			i += n;
			insn += n;
		}
		ctx->prev_ref[i] = prev;
	}
}
