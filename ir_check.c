/*
 * IR - Lightweight JIT Compilation Framework
 * (IR verification)
 * This file is part of the IR Project distributed under the MIT-style LICENSE.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h"

void ir_consistency_check(void)
{
	IR_ASSERT(IR_UNUSED == 0);
	IR_ASSERT(IR_NOP == 0);

	IR_ASSERT((int)IR_BOOL   == (int)IR_C_BOOL);
	IR_ASSERT((int)IR_U8     == (int)IR_C_U8);
	IR_ASSERT((int)IR_U16    == (int)IR_C_U16);
	IR_ASSERT((int)IR_U32    == (int)IR_C_U32);
	IR_ASSERT((int)IR_U64    == (int)IR_C_U64);
	IR_ASSERT((int)IR_ADDR   == (int)IR_C_ADDR);
	IR_ASSERT((int)IR_CHAR   == (int)IR_C_CHAR);
	IR_ASSERT((int)IR_I8     == (int)IR_C_I8);
	IR_ASSERT((int)IR_I16    == (int)IR_C_I16);
	IR_ASSERT((int)IR_I32    == (int)IR_C_I32);
	IR_ASSERT((int)IR_I64    == (int)IR_C_I64);
	IR_ASSERT((int)IR_DOUBLE == (int)IR_C_DOUBLE);
	IR_ASSERT((int)IR_FLOAT  == (int)IR_C_FLOAT);

	IR_ASSERT((IR_EQ ^ 1) == IR_NE);
	IR_ASSERT((IR_LT ^ 3) == IR_GT);
	IR_ASSERT((IR_GT ^ 3) == IR_LT);
	IR_ASSERT((IR_LE ^ 3) == IR_GE);
	IR_ASSERT((IR_GE ^ 3) == IR_LE);
	IR_ASSERT((IR_ULT ^ 3) == IR_UGT);
	IR_ASSERT((IR_UGT ^ 3) == IR_ULT);
	IR_ASSERT((IR_ULE ^ 3) == IR_UGE);
	IR_ASSERT((IR_UGE ^ 3) == IR_ULE);
	IR_ASSERT((IR_ORDERED ^ 1) == IR_UNORDERED);

	IR_ASSERT(IR_ADD + 1 == IR_SUB);
}

typedef struct {
	ir_arena  *arena;
	ir_bitset *use_set;
	ir_bitset *input_set;
} ir_check_ctx;

static bool ir_check_use_list(ir_check_ctx *check_ctx, const ir_ctx *ctx, ir_ref from, ir_ref to)
{
	ir_ref n, *p;
	ir_use_list *use_list = &ctx->use_lists[from];

	n = use_list->count;
	if (n > 16) {
		/* Avoid quadratic complexity by maintaining a temporary bit-set */
		ir_bitset set;

		if (!check_ctx->use_set || !(set = check_ctx->use_set[from])) {
			if (!check_ctx->arena) {
				check_ctx->arena = ir_arena_create(sizeof(ir_arena) +
					ctx->insns_count * sizeof(ir_bitset) +
					ir_bitset_len(ctx->insns_count) * sizeof(ir_bitset_base_t));
			}
			if (!check_ctx->use_set) {
				check_ctx->use_set = ir_arena_alloc(&check_ctx->arena, ctx->insns_count * sizeof(ir_bitset));
				memset(check_ctx->use_set, 0, ctx->insns_count * sizeof(ir_bitset));
			}
			check_ctx->use_set[from] = set = (ir_bitset)ir_arena_alloc(&check_ctx->arena,
				ir_bitset_len(ctx->insns_count) * sizeof(ir_bitset_base_t));
			memset(set, 0, ir_bitset_len(ctx->insns_count) * sizeof(ir_bitset_base_t));
			for (p = &ctx->use_edges[use_list->refs]; n > 0; p++, n--) {
				ir_bitset_incl(set, *p);
			}
		}
		return ir_bitset_in(set, to);
	}
	for (p = &ctx->use_edges[use_list->refs]; n > 0; p++, n--) {
		if (*p == to) {
			return 1;
		}
	}
	return 0;
}

static bool ir_check_input_list(ir_check_ctx *check_ctx, const ir_ctx *ctx, ir_ref from, ir_ref to)
{
	ir_insn *insn = &ctx->ir_base[to];
	ir_ref n, j, *p;

	n = ir_input_edges_count(ctx, insn);
	if (n > 16) {
		/* Avoid quadratic complexity by maintaining a temporary bit-set */
		ir_bitset set;

		if (!check_ctx->input_set || !(set = check_ctx->input_set[to])) {
			if (!check_ctx->arena) {
				check_ctx->arena = ir_arena_create(sizeof(ir_arena) +
					ctx->insns_count * sizeof(ir_bitset) +
					ir_bitset_len(ctx->insns_count) * sizeof(ir_bitset_base_t));
			}
			if (!check_ctx->input_set) {
				check_ctx->input_set = ir_arena_alloc(&check_ctx->arena, ctx->insns_count * sizeof(ir_bitset));
				memset(check_ctx->input_set, 0, ctx->insns_count * sizeof(ir_bitset));
			}
			check_ctx->input_set[to] = set = (ir_bitset)ir_arena_alloc(&check_ctx->arena,
				ir_bitset_len(ctx->insns_count) * sizeof(ir_bitset_base_t));
			memset(set, 0, ir_bitset_len(ctx->insns_count) * sizeof(ir_bitset_base_t));
			for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
				if (*p > 0) ir_bitset_incl(set, *p);
			}
		}
		return ir_bitset_in(set, from);
	}
	for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
		if (*p == from) {
			return 1;
		}
	}
	return 0;
}

static bool ir_check_domination(const ir_ctx *ctx, ir_ref def, ir_ref use)
{
	uint32_t b1 = ctx->cfg_map[def];
	uint32_t b2 = ctx->cfg_map[use];
	ir_block *blocks = ctx->cfg_blocks;
	uint32_t b1_depth = blocks[b1].dom_depth;
	const ir_block *bb2 = &blocks[b2];

	if (b1 == b2) {
		return def < use;
	}
	while (bb2->dom_depth > b1_depth) {
		b2 = bb2->dom_parent;
		bb2 = &blocks[b2];
	}
	return b1 == b2;
}

static bool ir_check_types_compatible(ir_type t1, ir_type t2)
{
	if (t1 == t2) {
		return 1;
	} else if (t1 == IR_ADDR) {
		return t2 == IR_UINTPTR_T || t2 == IR_INTPTR_T;
	} else if (t2 == IR_ADDR) {
		return t1 == IR_UINTPTR_T || t1 == IR_INTPTR_T;
	}
	return 0;
}

/* vector elements of integer types may differ in signedness */
static bool ir_check_element_types_compatible(ir_type t1, ir_type t2)
{
	return ir_check_types_compatible(t1, t2)
		|| (IR_IS_TYPE_INT(t1) && IR_IS_TYPE_INT(t2) && ir_type_size[t1] == ir_type_size[t2]);
}

/* integer literal of another integer type is accepted if its value is representable in "type" */
static bool ir_check_const_fits(const ir_insn *c, ir_type type)
{
	uint64_t max;

	if (c->op != c->type || !IR_IS_TYPE_INT(c->type) || !IR_IS_TYPE_INT(type)) {
		return 0;
	} else if (type == IR_BOOL) {
		return c->val.u64 <= 1;
	}
	max = UINT64_MAX >> (64 - ir_type_size[type] * 8);
	if (IR_IS_TYPE_SIGNED(type)) {
		max >>= 1;
	}
	if (IR_IS_TYPE_SIGNED(c->type) && c->val.i64 < 0) {
		return IR_IS_TYPE_SIGNED(type) && c->val.i64 >= -(int64_t)max - 1;
	}
	return c->val.u64 <= max;
}

static bool ir_check_type_matches(const ir_ctx *ctx, ir_ref ref, ir_type type)
{
	return ir_check_types_compatible(ctx->ir_base[ref].type, type)
		|| (IR_IS_CONST_REF(ref) && ir_check_const_fits(&ctx->ir_base[ref], type));
}

static bool ir_check_is_addr(ir_type type)
{
	return ir_check_types_compatible(IR_ADDR, type);
}

static bool ir_check_same_shape(ir_type t1, ir_type t2)
{
	if (IR_IS_TYPE_VECTOR(t1)) {
		return IR_IS_TYPE_VECTOR(t2) && IR_VECTOR_LENGTH(t1) == IR_VECTOR_LENGTH(t2);
	}
	return IR_IS_TYPE_SCALAR(t2);
}

static const char *ir_check_shape_name(ir_type type)
{
	return IR_IS_TYPE_VECTOR(type) ? "a vector of the same length as result" : "a scalar";
}

static const char *ir_check_conversion(ir_op op, ir_type type, ir_type use_type)
{
	switch (op) {
		case IR_SEXT:
		case IR_ZEXT:
			return (IR_IS_TYPE_INT(use_type) && ir_type_size[use_type] < ir_type_size[type])
				? NULL : "an integer smaller than result";
		case IR_TRUNC:
			return (IR_IS_TYPE_INT(use_type) && ir_type_size[use_type] > ir_type_size[type])
				? NULL : "an integer larger than result";
		case IR_INT2FP:
			return IR_IS_TYPE_INT(use_type) ? NULL : "an integer";
		default:
			IR_ASSERT(op == IR_FP2INT || op == IR_FP2FP);
			return IR_IS_TYPE_FP(use_type) ? NULL : "a floating-point value";
	}
}

/* returns the description of the expected input, or NULL if the input is valid */
static const char *ir_check_input_type(const ir_ctx *ctx, const ir_insn *insn, int j, ir_ref use)
{
	ir_type type = insn->type;
	ir_type use_type = ctx->ir_base[use].type;
	ir_type base = IR_VECTOR_BASE_TYPE(type);
	ir_type use_base = IR_VECTOR_BASE_TYPE(use_type);

	switch (insn->op) {
		case IR_SHL:
		case IR_SHR:
		case IR_SAR:
		case IR_ROL:
		case IR_ROR:
			if (j == 2 && IR_IS_TYPE_INT(use_type)) {
				/* shift amount may have any integer type */
				return NULL;
			}
			return ir_check_type_matches(ctx, use, type) ? NULL : "the same type as result";
		case IR_NOT:
			if (type == IR_BOOL && IR_IS_TYPE_INT(use_type)) {
				/* boolean NOT */
				return NULL;
			}
			return ir_check_type_matches(ctx, use, type) ? NULL : "the same type as result";
		case IR_COND:
			if (j == 1) {
				return IR_IS_TYPE_SCALAR(use_type) ? NULL : "a scalar";
			}
			IR_FALLTHROUGH;
		case IR_ADD:
		case IR_SUB:
		case IR_MUL:
		case IR_DIV:
		case IR_MOD:
		case IR_NEG:
		case IR_ABS:
		case IR_ADD_OV:
		case IR_SUB_OV:
		case IR_MUL_OV:
		case IR_OR:
		case IR_AND:
		case IR_XOR:
		case IR_BSWAP:
		case IR_MIN:
		case IR_MAX:
		case IR_PHI:
		case IR_COPY:
		case IR_PI:
			return ir_check_type_matches(ctx, use, type) ? NULL : "the same type as result";
		case IR_EQ:
		case IR_NE:
		case IR_LT:
		case IR_GE:
		case IR_LE:
		case IR_GT:
		case IR_ULT:
		case IR_UGE:
		case IR_ULE:
		case IR_UGT:
		case IR_ORDERED:
		case IR_UNORDERED:
			if (!ir_check_same_shape(type, use_type)) {
				return ir_check_shape_name(type);
			} else if ((insn->op == IR_ORDERED || insn->op == IR_UNORDERED) && !IR_IS_TYPE_FP(use_base)) {
				return "a floating-point value";
			} else if (j == 2
			 && !ir_check_type_matches(ctx, use, ctx->ir_base[insn->op1].type)
			 && !ir_check_type_matches(ctx, insn->op1, use_type)) {
				return "the same type as ops[1]";
			}
			return NULL;
		case IR_SEXT:
		case IR_ZEXT:
		case IR_TRUNC:
		case IR_INT2FP:
		case IR_FP2INT:
		case IR_FP2FP:
			if (!ir_check_same_shape(type, use_type)) {
				return ir_check_shape_name(type);
			}
			return ir_check_conversion(insn->op, base, use_base);
		case IR_BITCAST:
			return ir_get_type_size(use_type) == ir_get_type_size(type) ? NULL : "of the same size as result";
		case IR_OVERFLOW:
			switch (ctx->ir_base[use].op) {
				case IR_ADD_OV:
				case IR_SUB_OV:
				case IR_MUL_OV:
					return NULL;
				default:
					return "ADD_OV, SUB_OV or MUL_OV";
			}
		case IR_EXTRACT:
			if (j == 2) {
				return IR_IS_TYPE_INT(use_type) ? NULL : "an integer";
			}
			return (IR_IS_TYPE_VECTOR(use_type) && ir_check_element_types_compatible(use_base, type))
				? NULL : "a vector of result elements";
		case IR_REPLACE:
			if (j == 1) {
				return use_type == type ? NULL : "the same type as result";
			} else if (j == 2) {
				return IR_IS_TYPE_INT(use_type) ? NULL : "an integer";
			}
			IR_FALLTHROUGH;
		case IR_SPLAT:
			return ir_check_element_types_compatible(base, use_type) ? NULL : "a result element";
		case IR_SHUFFLE:
			if (j == 3) {
				return (IR_IS_TYPE_VECTOR(use_type) && IR_IS_TYPE_INT(use_base)) ? NULL : "an integer vector";
			}
			return (IR_IS_TYPE_VECTOR(use_type) && ir_check_element_types_compatible(base, use_base))
				? NULL : "a vector of result elements";
		case IR_CALL:
		case IR_TAILCALL:
			if (j != 2) {
				return NULL;
			}
			IR_FALLTHROUGH;
		case IR_PROTO:
		case IR_ARGVAL:
		case IR_BLOCK_END:
		case IR_VA_START:
		case IR_VA_END:
		case IR_VA_COPY:
		case IR_VA_ARG:
		case IR_IJMP:
		case IR_IGOTO:
		case IR_EXITCALL:
			return ir_check_is_addr(use_type) ? NULL : "an address";
		case IR_GUARD:
		case IR_GUARD_NOT:
			if (j == 3) {
				return ir_check_is_addr(use_type) ? NULL : "an address";
			}
			IR_FALLTHROUGH;
		case IR_IF:
			return IR_IS_TYPE_SCALAR(use_type) ? NULL : "a scalar";
		case IR_CTPOP:
		case IR_CTLZ:
		case IR_CTTZ:
		case IR_ALLOCA:
		case IR_AFREE:
		case IR_SWITCH:
			return IR_IS_TYPE_INT(use_type) ? NULL : "an integer";
		case IR_CASE_VAL:
		case IR_CASE_RANGE:
			if (ctx->ir_base[insn->op1].op == IR_SWITCH) {
				ir_type cond_type = ctx->ir_base[ctx->ir_base[insn->op1].op2].type;

				return ir_check_type_matches(ctx, use, cond_type) ? NULL : "the same type as SWITCH condition";
			}
			return IR_IS_TYPE_INT(use_type) ? NULL : "an integer";
		case IR_VADDR:
			return ctx->ir_base[use].op == IR_VAR ? NULL : "VAR";
		case IR_VSTORE:
		case IR_VSTORE_v:
			if (j == 3 && ctx->ir_base[insn->op2].op == IR_VAR) {
				return ir_check_type_matches(ctx, use, ctx->ir_base[insn->op2].type) ? NULL : "the same type as VAR";
			}
			return NULL;
		default:
			return NULL;
	}
}

static bool ir_check_input(const ir_ctx *ctx, ir_ref i, const ir_insn *insn, int j, ir_ref use)
{
	const char *expected = ir_check_input_type(ctx, insn, j, use);

	if (expected) {
		fprintf(stderr, "ir_base[%d].ops[%d] (%d) must be %s (instruction %s)\n",
			i, j, use, expected, ir_op_name[insn->op]);
		return 0;
	}
	return 1;
}

bool ir_check(const ir_ctx *ctx)
{
	ir_ref i, j, n, *p, use;
	ir_insn *insn, *use_insn;
	ir_type type;
	uint32_t flags;
	bool ok = 1;
	ir_check_ctx check_ctx;

	if (ctx->insns_count < 1 || ctx->ir_base[1].op != IR_START) {
		fprintf(stderr, "ir_base[1].op invalid opcode (%d)\n",
			(ctx->insns_count < 1) ? IR_NOP : ctx->ir_base[0].op);
		ok = 0;
	}

	check_ctx.arena = NULL;
	check_ctx.use_set = NULL;
	check_ctx.input_set = NULL;

	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count;) {
		if (insn->op >= IR_LAST_OP) {
			fprintf(stderr, "ir_base[%d].op invalid opcode (%d)\n", i, insn->op);
			ok = 0;
			break;
		}
		flags = ir_op_flags[insn->op];

		if (flags & IR_OP_FLAG_DATA) {
			IR_ASSERT((flags & (IR_OP_FLAG_MEM|IR_OP_FLAG_CONTROL)) == 0);
			if (insn->type == IR_VOID) {
				fprintf(stderr, "ir_base[%d].type must be set (instruction %s)\n", i, ir_op_name[insn->op]);
				ok = 0;
			} else {
				switch (insn->op) {
					case IR_EQ:
					case IR_NE:
					case IR_LT:
					case IR_GE:
					case IR_LE:
					case IR_GT:
					case IR_ULT:
					case IR_UGE:
					case IR_ULE:
					case IR_UGT:
					case IR_ORDERED:
					case IR_UNORDERED:
						if (insn->type != IR_BOOL && !IR_IS_TYPE_VECTOR(insn->type)) {
							fprintf(stderr, "ir_base[%d].type must be bool or vector (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_SEXT:
					case IR_ZEXT:
					case IR_TRUNC:
					case IR_FP2INT:
						if (!IR_IS_TYPE_INT(insn->type)
						 && (!IR_IS_TYPE_VECTOR(insn->type) || !IR_IS_TYPE_INT(IR_VECTOR_BASE_TYPE(insn->type)))) {
							fprintf(stderr, "ir_base[%d].type must be integer or vector (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_INT2FP:
					case IR_FP2FP:
						if (!IR_IS_TYPE_FP(insn->type)
						 && (!IR_IS_TYPE_VECTOR(insn->type) || !IR_IS_TYPE_FP(IR_VECTOR_BASE_TYPE(insn->type)))) {
							fprintf(stderr, "ir_base[%d].type must be real number or vector (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_PROTO:
					case IR_VADDR:
					case IR_FRAME_ADDR:
					case IR_ARGVAL:
						if (insn->type != IR_ADDR) {
							fprintf(stderr, "ir_base[%d].type must be uintptr_t (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_ADD_OV:
					case IR_SUB_OV:
					case IR_MUL_OV:
						if (!IR_IS_TYPE_INT(insn->type)) {
							fprintf(stderr, "ir_base[%d].type must be integer (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_OVERFLOW:
						if (insn->type != IR_BOOL) {
							fprintf(stderr, "ir_base[%d].type must be bool (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_NOT:
						if (insn->type != IR_BOOL && !IR_IS_TYPE_INT(insn->type) && !IR_IS_TYPE_VECTOR(insn->type)) {
							fprintf(stderr, "ir_base[%d].type must be bool, integer or vector (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_OR:
					case IR_AND:
					case IR_XOR:
					case IR_SHL:
					case IR_SHR:
					case IR_SAR:
						if (!IR_IS_TYPE_INT(insn->type)
						 && (!IR_IS_TYPE_VECTOR(insn->type) || !IR_IS_TYPE_INT(IR_VECTOR_BASE_TYPE(insn->type)))) {
							fprintf(stderr, "ir_base[%d].type must be integer or vector (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_ROL:
					case IR_ROR:
					case IR_BSWAP:
						if (!IR_IS_TYPE_INT(insn->type)) {
							fprintf(stderr, "ir_base[%d].type must be integer (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_CTPOP:
					case IR_CTLZ:
					case IR_CTTZ:
						if (!IR_IS_TYPE_INT(insn->type)) {
							fprintf(stderr, "ir_base[%d].type must be integer (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						} else if (ir_type_size[insn->type] > sizeof(void*)) {
							fprintf(stderr, "ir_base[%d].type size must fit into a %d-bit integer (instruction %s)\n", i, (int)sizeof(void*) * 8, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_EXTRACT:
						if (!IR_IS_TYPE_INT(insn->type) && !IR_IS_TYPE_FP(insn->type)) {
							fprintf(stderr, "ir_base[%d].type must be integer or real (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					case IR_REPLACE:
					case IR_SPLAT:
					case IR_SHUFFLE:
						if (!IR_IS_TYPE_VECTOR(insn->type)) {
							fprintf(stderr, "ir_base[%d].type must be vector (instruction %s)\n", i, ir_op_name[insn->op]);
							ok = 0;
						}
						break;
					default:
						break;
				}
			}
		} else if (flags & IR_OP_FLAG_MEM) {
			IR_ASSERT(flags & IR_OP_FLAG_CONTROL);
			if ((flags & IR_OP_FLAG_MEM_MASK) == IR_OP_FLAG_MEM_LOAD) {
				if (insn->type == IR_VOID) {
					fprintf(stderr, "ir_base[%d].type must be set (instruction %s)\n", i, ir_op_name[insn->op]);
					ok = 0;
				}
			} else if ((flags & IR_OP_FLAG_MEM_MASK) == IR_OP_FLAG_MEM_ALLOC) {
				if (insn->type == IR_VOID) {
					fprintf(stderr, "ir_base[%d].type must be set (instruction %s)\n", i, ir_op_name[insn->op]);
					ok = 0;
				} else if (insn->type != IR_ADDR) {
					fprintf(stderr, "ir_base[%d].type must be uintptr_t (instruction %s)\n", i, ir_op_name[insn->op]);
					ok = 0;
				}
			} else if ((flags & IR_OP_FLAG_MEM_MASK) == IR_OP_FLAG_MEM_STORE) {
				if (insn->type != IR_VOID) {
					fprintf(stderr, "ir_base[%d].type must be empty (instruction %s)\n", i, ir_op_name[insn->op]);
					ok = 0;
				}
			} else {
				IR_ASSERT((flags & IR_OP_FLAG_MEM_MASK) == IR_OP_FLAG_MEM_CALL);
			}
		} else if (flags & IR_OP_FLAG_CONTROL) {
			if (insn->type != IR_VOID) {
				fprintf(stderr, "ir_base[%d].type must be empty (instruction %s)\n", i, ir_op_name[insn->op]);
				ok = 0;
			}
		}

		n = ir_input_edges_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			use = *p;
			if (use != IR_UNUSED) {
				if (IR_IS_CONST_REF(use)) {
					if (IR_OPND_KIND(flags, j) != IR_OPND_DATA) {
						fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must not be constant\n", i, j, use);
						ok = 0;
					} else if (-use >= ctx->consts_count) {
						fprintf(stderr, "ir_base[%d].ops[%d] constant reference (%d) is out of range\n", i, j, use);
						ok = 0;
					} else if (!ir_check_input(ctx, i, insn, j, use)) {
						ok = 0;
					}
				} else {
					if (use >= ctx->insns_count) {
						fprintf(stderr, "ir_base[%d].ops[%d] insn reference (%d) is out of range\n", i, j, use);
						ok = 0;
						continue;
					}
					use_insn = &ctx->ir_base[use];
					switch (IR_OPND_KIND(flags, j)) {
						case IR_OPND_DATA:
							if (!(ir_op_flags[use_insn->op] & IR_OP_FLAG_DATA)) {
								if (!(ir_op_flags[use_insn->op] & IR_OP_FLAG_MEM)
								 || use_insn->type == IR_VOID) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be DATA\n", i, j, use);
									ok = 0;
								}
							}
							if ((ctx->flags2 & IR_LINEAR)
							 && use >= i
							 && !(insn->op == IR_PHI && ctx->ir_base[insn->op1].op == IR_LOOP_BEGIN)) {
								fprintf(stderr, "ir_base[%d].ops[%d] invalid forward reference (%d)\n", i, j, use);
								ok = 0;
							}
							if (!ir_check_input(ctx, i, insn, j, use)) {
								ok = 0;
							}
							if ((ctx->flags2 & IR_LINEAR)
							 && ctx->cfg_map
							 && insn->op != IR_PHI
							 && !ir_check_domination(ctx, use, i)) {
								fprintf(stderr, "ir_base[%d].ops[%d] -> %d, %d doesn't dominate %d\n", i, j, use, use, i);
								ok = 0;
							}
							break;
						case IR_OPND_CONTROL:
							if (flags & IR_OP_FLAG_BB_START) {
								if (!(ir_op_flags[use_insn->op] & IR_OP_FLAG_BB_END)) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be BB_END\n", i, j, use);
									ok = 0;
								}
							} else {
								if (ir_op_flags[use_insn->op] & IR_OP_FLAG_BB_END) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must not be BB_END\n", i, j, use);
									ok = 0;
								}
							}
							if ((ctx->flags2 & IR_LINEAR)
							 && use >= i
							 && !(insn->op == IR_LOOP_BEGIN)) {
								fprintf(stderr, "ir_base[%d].ops[%d] invalid forward reference (%d)\n", i, j, use);
								ok = 0;
							}
							break;
						case IR_OPND_CONTROL_DEP:
							if ((ctx->flags2 & IR_LINEAR)
							 && use >= i) {
								fprintf(stderr, "ir_base[%d].ops[%d] invalid forward reference (%d)\n", i, j, use);
								ok = 0;
							} else if (insn->op == IR_PHI) {
								ir_insn *merge_insn = &ctx->ir_base[insn->op1];
								if (merge_insn->op != IR_MERGE && merge_insn->op != IR_LOOP_BEGIN) {
									fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be MERGE or LOOP_BEGIN\n", i, j, use);
									ok = 0;
								}
							}
							break;
						case IR_OPND_CONTROL_REF:
							if (!(ir_op_flags[use_insn->op] & IR_OP_FLAG_CONTROL)) {
								fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be CONTROL\n", i, j, use);
								ok = 0;
							}
							break;
						case IR_OPND_CONTROL_GUARD:
							if (!(ir_op_flags[use_insn->op] & IR_OP_FLAG_BB_START)
							 && use_insn->op != IR_GUARD
							 && use_insn->op != IR_GUARD_NOT) {
								fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) must be BB_START or GUARD\n", i, j, use);
								ok = 0;
							}
							break;
						default:
							fprintf(stderr, "ir_base[%d].ops[%d] reference (%d) of unsupported kind\n", i, j, use);
							ok = 0;
					}
				}
			} else if ((insn->op == IR_RETURN || insn->op == IR_UNREACHABLE) && j == 2) {
				/* pass (function returns void) */
			} else if (insn->op == IR_BEGIN && j == 1) {
				/* pass (start of unreachable basic block) */
			} else if (IR_OPND_KIND(flags, j) == IR_OPND_CONTROL_GUARD) {
				/* reference to control guard is optional */
			} else if (IR_OPND_KIND(flags, j) != IR_OPND_CONTROL_REF
					&& (insn->op != IR_SNAPSHOT || j == 1)) {
				fprintf(stderr, "ir_base[%d].ops[%d] missing reference (%d)\n", i, j, use);
				ok = 0;
			}
			if (ctx->use_lists
			 && use > 0
			 && !ir_check_use_list(&check_ctx, ctx, use, i)) {
				fprintf(stderr, "ir_base[%d].ops[%d] is not in use list (%d)\n", i, j, use);
				ok = 0;
			}
		}

		switch (insn->op) {
			case IR_PHI:
				if (insn->inputs_count != ctx->ir_base[insn->op1].inputs_count + 1) {
					fprintf(stderr, "ir_base[%d] inconsistent PHI inputs_count (%d != %d)\n",
						i, insn->inputs_count, ctx->ir_base[insn->op1].inputs_count + 1);
					ok = 0;
				}
				break;
			case IR_LOAD:
			case IR_LOAD_v:
			case IR_STORE:
			case IR_STORE_v:
				type = ctx->ir_base[insn->op2].type;
				if (type != IR_ADDR
				 && (!IR_IS_TYPE_INT(type) || ir_type_size[type] != ir_type_size[IR_ADDR])) {
					fprintf(stderr, "ir_base[%d].op2 must have ADDR type (%s)\n",
						i, IR_IS_TYPE_VECTOR(type) ? "VECTOR" : ir_type_name[type]);
					ok = 0;
				}
				break;
			case IR_VLOAD:
			case IR_VLOAD_v:
			case IR_VSTORE:
			case IR_VSTORE_v:
				if (ctx->ir_base[insn->op2].op != IR_VAR) {
					fprintf(stderr, "ir_base[%d].op2 must be 'VAR' (%s)\n",
						i, ir_op_name[ctx->ir_base[insn->op2].op]);
					ok = 0;
				}
				break;
			case IR_RETURN:
				if (ctx->ret_type != (insn->op2 ? ctx->ir_base[insn->op2].type : IR_VOID)) {
					fprintf(stderr, "ir_base[%d].type incompatible return type\n", i);
					ok = 0;
				}
				break;
			case IR_TAILCALL:
				if (ctx->ret_type != insn->type) {
					fprintf(stderr, "ir_base[%d].type incompatible return type\n", i);
					ok = 0;
				}
				break;
			case IR_PARAM:
				if (i > 2 && ctx->ir_base[i - 1].op != IR_PARAM) {
					fprintf(stderr, "ir_base[%d].op PARAMs must be used only right after START\n", i);
					ok = 0;
				}
				break;
		}

		if (ctx->use_lists) {
			ir_use_list *use_list = &ctx->use_lists[i];
			ir_ref count, n = use_list->count;

			for (p = &ctx->use_edges[use_list->refs]; n > 0; p++, n--) {
				use = *p;
				if (!ir_check_input_list(&check_ctx, ctx, i, use)) {
					fprintf(stderr, "ir_base[%d] is in use list of ir_base[%d]\n", use, i);
					ok = 0;
				}
			}

			if ((flags & IR_OP_FLAG_CONTROL) && !(flags & IR_OP_FLAG_MEM)) {
				switch (insn->op) {
					case IR_SWITCH:
						/* may have many successors */
						if (use_list->count < 1) {
							fprintf(stderr, "ir_base[%d].op (SWITCH) must have at least 1 successor (%d)\n", i, use_list->count);
							ok = 0;
						}
						break;
					case IR_IF:
						if (use_list->count != 2) {
							fprintf(stderr, "ir_base[%d].op (IF) must have 2 successors (%d)\n", i, use_list->count);
							ok = 0;
						}
						break;
					case IR_UNREACHABLE:
					case IR_RETURN:
						if (use_list->count == 1) {
							/* UNREACHABLE and RETURN may be linked with the following ENTRY by a fake edge */
							if (ctx->ir_base[ctx->use_edges[use_list->refs]].op == IR_ENTRY) {
								break;
							}
						}
						IR_FALLTHROUGH;
					case IR_IJMP:
						if (use_list->count != 0) {
							fprintf(stderr, "ir_base[%d].op (%s) must not have successors (%d)\n",
								i, ir_op_name[insn->op], use_list->count);
							ok = 0;
						}
						break;
					case IR_IGOTO:
					case IR_ASM_GOTO:
						break;
					default:
						/* skip data references */
						count = n = use_list->count;
						for (p = &ctx->use_edges[use_list->refs]; n > 0; p++, n--) {
							use = *p;
							if (!(ir_op_flags[ctx->ir_base[use].op] & IR_OP_FLAG_CONTROL)) {
								count--;
							}
						}
						if (count != 1) {
							if (insn->op == IR_CALL && count == 2) {
								/* result of CALL may be used as data in control instruction */
								break;
							}
							if ((insn->op == IR_LOOP_END || insn->op == IR_END) && count == 2) {
								/* LOOP_END/END may be linked with the following ENTRY by a fake edge */
								if (ctx->ir_base[ctx->use_edges[use_list->refs]].op == IR_ENTRY) {
									count--;
								}
								if (ctx->ir_base[ctx->use_edges[use_list->refs + 1]].op == IR_ENTRY) {
									count--;
								}
								if (count == 1) {
									break;
								}
							}
							if (count == 0 && (insn->op == IR_END || insn->op == IR_LOOP_END)) {
								/* Dead block */
								break;
							}
							fprintf(stderr, "ir_base[%d].op (%s) must have 1 successor (%d)\n",
								i, ir_op_name[insn->op], count);
							ok = 0;
						}
						break;
				}
			}
		}
		n = ir_insn_inputs_to_len(n);
		i += n;
		insn += n;
	}

	if (check_ctx.arena) {
		ir_arena_free(check_ctx.arena);
	}

//	if (!ok) {
//		ir_dump_codegen(ctx, stderr);
//	}

#ifndef IR_CHECK_NO_ABORT
	IR_ASSERT(ok);
#endif

	return ok;
}

bool ir_check_prototype(const ir_ctx *ctx, uint32_t flags, uint8_t ret_type, uint32_t params_count, uint8_t *param_types)
{
	bool ok = 1;
	ir_ref ref = 2;
	uint32_t n = 0;

	while (ref < ctx->insns_count && ctx->ir_base[ref].op == IR_PARAM) {
		if (n >= params_count) {
			fprintf(stderr, "parameter count doesn't match function signature\n");
			ok = 0;
			break;
		} else if (ctx->ir_base[ref].type != param_types[n]) {
			fprintf(stderr, "parameter %d type doesn't match function signature\n", n);
			ok = 0;
		}
		ref++;
		n++;
	}

	if (n < params_count) {
		fprintf(stderr, "parameter count doesn't match function signature\n");
		ok = 0;
	}
	if ((flags & IR_VARARG_FUNC) != (ctx->flags & IR_VARARG_FUNC)) {
		fprintf(stderr, "IR_VARARG_FUNC flag doesn't match function signature\n");
		ok = 0;
	}
	if (ret_type != ctx->ret_type) {
		fprintf(stderr, "return type doesn't match function signature\n");
		ok = 0;
	}
	if ((flags & IR_CALL_CONV_MASK) != (ctx->flags & IR_CALL_CONV_MASK)) {
		fprintf(stderr, "calling convention doesn't match function signature\n");
		ok = 0;
	}

	return ok;
}
