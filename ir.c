#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <sys/mman.h>

#include "ir.h"
#include "ir_private.h"

#include <math.h>

#ifdef HAVE_VALGRIND
# include <valgrind/valgrind.h>
#endif

#define IR_TYPE_FLAGS(name, type, field, flags) ((flags)|sizeof(type)),
#define IR_TYPE_NAME(name, type, field, flags)  #name,
#define IR_TYPE_CNAME(name, type, field, flags) #type,
#define IR_TYPE_SIZE(name, type, field, flags)  sizeof(type),
#define IR_OP_NAME(name, flags, op1, op2, op3)  #name,

const uint8_t ir_type_flags[IR_LAST_TYPE] = {
	0,
	IR_TYPES(IR_TYPE_FLAGS)
};

const char *ir_type_name[IR_LAST_TYPE] = {
	"void",
	IR_TYPES(IR_TYPE_NAME)
};

const uint8_t ir_type_size[IR_LAST_TYPE] = {
	0,
	IR_TYPES(IR_TYPE_SIZE)
};

const char *ir_type_cname[IR_LAST_TYPE] = {
	"void",
	IR_TYPES(IR_TYPE_CNAME)
};

const char *ir_op_name[IR_LAST_OP] = {
	IR_OPS(IR_OP_NAME)
#ifdef IR_PHP
	IR_PHP_OPS(IR_OP_NAME)
#endif
};

void ir_print_const(ir_ctx *ctx, ir_insn *insn, FILE *f)
{
	if (insn->op == IR_FUNC) {
		fprintf(f, "%s", ir_get_str(ctx, insn->val.addr));
		return;
	} else if (insn->op == IR_STR) {
		fprintf(f, "\"%s\"", ir_get_str(ctx, insn->val.addr));
		return;
	}
	IR_ASSERT(IR_IS_CONST_OP(insn->op));
	switch (insn->type) {
		case IR_BOOL:
			fprintf(f, "%u", insn->val.b);
			break;
		case IR_U8:
			fprintf(f, "%u", insn->val.u8);
			break;
		case IR_U16:
			fprintf(f, "%u", insn->val.u16);
			break;
		case IR_U32:
			fprintf(f, "%u", insn->val.u32);
			break;
		case IR_U64:
			fprintf(f, "%" PRIu64, insn->val.u64);
			break;
		case IR_ADDR:
			if (insn->val.addr) {
				fprintf(f, "0x%" PRIxPTR, insn->val.addr);
			} else {
				fprintf(f, "0");
			}
			break;
		case IR_CHAR:
			if (insn->val.c == '\\') {
				fprintf(f, "'\\\\'");
			} else if (insn->val.c >= ' ') {
				fprintf(f, "'%c'", insn->val.c);
			} else if (insn->val.c == '\t') {
				fprintf(f, "'\\t'");
			} else if (insn->val.c == '\r') {
				fprintf(f, "'\\r'");
			} else if (insn->val.c == '\n') {
				fprintf(f, "'\\n'");
			} else if (insn->val.c == '\0') {
				fprintf(f, "'\\0'");
			} else {
				fprintf(f, "%u", insn->val.c);
			}
			break;
		case IR_I8:
			fprintf(f, "%d", insn->val.i8);
			break;
		case IR_I16:
			fprintf(f, "%d", insn->val.i16);
			break;
		case IR_I32:
			fprintf(f, "%d", insn->val.i32);
			break;
		case IR_I64:
			fprintf(f, "%" PRIi64, insn->val.i64);
			break;
		case IR_DOUBLE:
			fprintf(f, "%g", insn->val.d);
			break;
		case IR_FLOAT:
			fprintf(f, "%f", insn->val.f);
			break;
		default:
			IR_ASSERT(0);
			break;
	}
}

#define ir_op_flag_v       0
#define ir_op_flag_v0X3    (0 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_d       IR_OP_FLAG_DATA
#define ir_op_flag_d0      ir_op_flag_d
#define ir_op_flag_d1      (ir_op_flag_d | 1 | (1 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_d2      (ir_op_flag_d | 2 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_d2C     (ir_op_flag_d | IR_OP_FLAG_COMMUTATIVE | 2 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_d3      (ir_op_flag_d | 3 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_dP      (ir_op_flag_d | 5 | (5 << IR_OP_FLAG_OPERANDS_SHIFT)) // PHI (number of operands encoded in op1->op1)
#define ir_op_flag_r       IR_OP_FLAG_DATA                                      // "d" and "r" are the same now
#define ir_op_flag_r0      ir_op_flag_r
#define ir_op_flag_r0X1    (ir_op_flag_r | 0 | (1 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_r1      (ir_op_flag_r | 1 | (1 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_r1X1    (ir_op_flag_r | 1 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_r1X2    (ir_op_flag_r | 1 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_r2      (ir_op_flag_r | 2 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_r3      (ir_op_flag_r | 3 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_c       IR_OP_FLAG_CONTROL
#define ir_op_flag_c3      (ir_op_flag_c | 3 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_S       (IR_OP_FLAG_CONTROL|IR_OP_FLAG_BB_START)
#define ir_op_flag_S0X2    (ir_op_flag_S | 0 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_S1      (ir_op_flag_S | 1 | (1 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_S1X1    (ir_op_flag_S | 1 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_S2      (ir_op_flag_S | 2 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_S2X1    (ir_op_flag_S | 2 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_SN      (ir_op_flag_S | 4 | (4 << IR_OP_FLAG_OPERANDS_SHIFT)) // MERGE (number of operands encoded in op1)
#define ir_op_flag_E       (IR_OP_FLAG_CONTROL|IR_OP_FLAG_BB_END)
#define ir_op_flag_E1      (ir_op_flag_E | 1 | (1 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_E1X1    (ir_op_flag_E | 1 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_E2      (ir_op_flag_E | 2 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_T       (IR_OP_FLAG_CONTROL|IR_OP_FLAG_BB_END|IR_OP_FLAG_TERMINATOR)
#define ir_op_flag_T2X1    (ir_op_flag_T | 2 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_l       (IR_OP_FLAG_CONTROL|IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_LOAD)
#define ir_op_flag_l0      ir_op_flag_l
#define ir_op_flag_l1      (ir_op_flag_l | 1 | (1 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_l1X1    (ir_op_flag_l | 1 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_l2      (ir_op_flag_l | 2 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_l2X1    (ir_op_flag_l | 2 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_l3      (ir_op_flag_l | 3 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_s       (IR_OP_FLAG_CONTROL|IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_STORE)
#define ir_op_flag_s0      ir_op_flag_s
#define ir_op_flag_s1      (ir_op_flag_s | 1 | (1 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_s2      (ir_op_flag_s | 2 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_s3      (ir_op_flag_s | 3 | (3 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_xN      (IR_OP_FLAG_CONTROL|IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_CALL | 4 | (4 << IR_OP_FLAG_OPERANDS_SHIFT))
#define ir_op_flag_a2      (IR_OP_FLAG_CONTROL|IR_OP_FLAG_MEM|IR_OP_FLAG_MEM_ALLOC | 2 | (2 << IR_OP_FLAG_OPERANDS_SHIFT))

#define ir_op_kind____     IR_OPND_UNUSED
#define ir_op_kind_def     IR_OPND_DATA
#define ir_op_kind_ref     IR_OPND_DATA
#define ir_op_kind_src     IR_OPND_CONTROL
#define ir_op_kind_reg     IR_OPND_CONTROL_DEP
#define ir_op_kind_beg     IR_OPND_CONTROL_REF
#define ir_op_kind_ret     IR_OPND_CONTROL_REF
#define ir_op_kind_ent     IR_OPND_CONTROL_REF
#define ir_op_kind_str     IR_OPND_STR
#define ir_op_kind_num     IR_OPND_NUM
#define ir_op_kind_fld     IR_OPND_STR
#define ir_op_kind_var     IR_OPND_VAR
#define ir_op_kind_prb     IR_OPND_PROB

#define _IR_OP_FLAGS(name, flags, op1, op2, op3) \
	IR_OP_FLAGS(ir_op_flag_ ## flags, ir_op_kind_ ## op1, ir_op_kind_ ## op2, ir_op_kind_ ## op3),

const uint32_t ir_op_flags[IR_LAST_OP] = {
	IR_OPS(_IR_OP_FLAGS)
#ifdef IR_PHP
	IR_PHP_OPS(_IR_OP_FLAGS)
#endif
};

static void ir_grow_bottom(ir_ctx *ctx)
{
	ir_insn *buf = ctx->ir_base - ctx->consts_limit;
	ir_ref old_consts_limit = ctx->consts_limit;

	if (ctx->consts_limit < 1024 * 4) {
		ctx->consts_limit *= 2;
	} else if (ctx->insns_limit < 1024 * 4 * 2) {
		ctx->consts_limit = 1024 * 4 * 2;
	} else {
		ctx->consts_limit += 1024 * 4;
	}
	buf = ir_mem_realloc(buf, (ctx->consts_limit + ctx->insns_limit) * sizeof(ir_insn));
	memmove(buf + ctx->consts_limit - ctx->consts_count,
		buf + old_consts_limit - ctx->consts_count,
		(old_consts_limit + ctx->insns_limit) * sizeof(ir_insn));
	ctx->ir_base = buf + ctx->consts_limit;
}

static ir_ref ir_next_const(ir_ctx *ctx)
{
	ir_ref ref = ctx->consts_count;

	if (UNEXPECTED(ref >= ctx->consts_limit)) {
		ir_grow_bottom(ctx);
	}
	ctx->consts_count = ref + 1;
	return -ref;
}

static void ir_grow_top(ir_ctx *ctx)
{
	ir_insn *buf = ctx->ir_base - ctx->consts_limit;

	if (ctx->insns_limit < 1024 * 4) {
		ctx->insns_limit *= 2;
	} else if (ctx->insns_limit < 1024 * 4 * 2) {
		ctx->insns_limit = 1024 * 4 * 2;
	} else {
		ctx->insns_limit += 1024 * 4;
	}
	buf = ir_mem_realloc(buf, (ctx->consts_limit + ctx->insns_limit) * sizeof(ir_insn));
	ctx->ir_base = buf + ctx->consts_limit;
}

static ir_ref ir_next_insn(ir_ctx *ctx)
{
	ir_ref ref = ctx->insns_count;

	if (UNEXPECTED(ref >= ctx->insns_limit)) {
		ir_grow_top(ctx);
	}
	ctx->insns_count = ref + 1;
	return ref;
}

void ir_truncate(ir_ctx *ctx)
{
	ir_insn *buf = ir_mem_malloc((ctx->consts_count + ctx->insns_count) * sizeof(ir_insn));

	memcpy(buf, ctx->ir_base - ctx->consts_count, (ctx->consts_count + ctx->insns_count) * sizeof(ir_insn));
	ir_mem_free(ctx->ir_base - ctx->consts_limit);
	ctx->insns_limit = ctx->insns_count;
	ctx->consts_limit = ctx->consts_count;
	ctx->ir_base = buf + ctx->consts_limit;
}

void ir_init(ir_ctx *ctx, ir_ref consts_limit, ir_ref insns_limit)
{
	ir_insn *buf;

	IR_ASSERT(consts_limit >= -(IR_TRUE - 1));
	IR_ASSERT(insns_limit >= IR_UNUSED + 1);

	ctx->insns_count = IR_UNUSED + 1;
	ctx->insns_limit = insns_limit;
	ctx->consts_count = -(IR_TRUE - 1);
	ctx->consts_limit = consts_limit;
	ctx->fold_cse_limit = IR_UNUSED + 1;
	ctx->flags = 0;

	ctx->use_lists = NULL;
	ctx->use_edges = NULL;

	ctx->cfg_blocks_count = 0;
	ctx->cfg_edges_count = 0;
	ctx->cfg_blocks = NULL;
	ctx->cfg_edges = NULL;
	ctx->cfg_map = NULL;
	ctx->rules = NULL;
	ctx->vregs_count = 0;
	ctx->vregs = NULL;
	ctx->live_intervals = NULL;
	ctx->regs = NULL;
	ctx->prev_insn_len = NULL;
	ctx->data = NULL;

	ctx->code_buffer = NULL;
	ctx->code_buffer_size = 0;

	ir_strtab_init(&ctx->strtab, 64, 4096);

	buf = ir_mem_malloc((consts_limit + insns_limit) * sizeof(ir_insn));
	ctx->ir_base = buf + consts_limit;

	ctx->ir_base[IR_UNUSED].optx = IR_NOP;
	ctx->ir_base[IR_NULL].optx = IR_OPT(IR_C_ADDR, IR_ADDR);
	ctx->ir_base[IR_NULL].val.u64 = 0;
	ctx->ir_base[IR_FALSE].optx = IR_OPT(IR_C_BOOL, IR_BOOL);
	ctx->ir_base[IR_FALSE].val.u64 = 0;
	ctx->ir_base[IR_TRUE].optx = IR_OPT(IR_C_BOOL, IR_BOOL);
	ctx->ir_base[IR_TRUE].val.u64 = 1;

	memset(ctx->prev_insn_chain, 0, sizeof(ctx->prev_insn_chain));
	memset(ctx->prev_const_chain, 0, sizeof(ctx->prev_const_chain));
}

void ir_free(ir_ctx *ctx)
{
	ir_insn *buf = ctx->ir_base - ctx->consts_limit;
	ir_mem_free(buf);
	ir_strtab_free(&ctx->strtab);
	if (ctx->use_lists) {
		ir_mem_free(ctx->use_lists);
	}
	if (ctx->use_edges) {
		ir_mem_free(ctx->use_edges);
	}
	if (ctx->cfg_blocks) {
		ir_mem_free(ctx->cfg_blocks);
	}
	if (ctx->cfg_edges) {
		ir_mem_free(ctx->cfg_edges);
	}
	if (ctx->cfg_map) {
		ir_mem_free(ctx->cfg_map);
	}
	if (ctx->rules) {
		ir_mem_free(ctx->rules);
	}
	if (ctx->vregs) {
		ir_mem_free(ctx->vregs);
	}
	if (ctx->live_intervals) {
		ir_free_live_intervals(ctx->live_intervals, ctx->vregs_count);
	}
	if (ctx->regs) {
		ir_mem_free(ctx->regs);
	}
	if (ctx->prev_insn_len) {
		ir_mem_free(ctx->prev_insn_len);
	}
}

ir_ref ir_const(ir_ctx *ctx, ir_val val, uint8_t type)
{
	ir_insn *insn;
	ir_ref ref;

	if (type == IR_BOOL) {
		return val.u64 ? IR_TRUE : IR_FALSE;
	} else if (type == IR_ADDR && val.u64 == 0) {
		return IR_NULL;
	}
	ref = ctx->prev_const_chain[type];
	while (ref) {
		insn = &ctx->ir_base[ref];
		if (insn->val.u64 == val.u64) {
			return ref;
		}
		ref = insn->prev_const;
	}

	ref = ir_next_const(ctx);
	insn = &ctx->ir_base[ref];

	insn->prev_const = ctx->prev_const_chain[type];
	ctx->prev_const_chain[type] = ref;

	insn->optx = IR_OPT(type, type);
	insn->val.u64 = val.u64;

	return ref;
}

ir_ref ir_const_i8(ir_ctx *ctx, int8_t c)
{
	ir_val val;
	val.i64 = c;
	return ir_const(ctx, val, IR_I8);
}

ir_ref ir_const_i16(ir_ctx *ctx, int16_t c)
{
	ir_val val;
	val.i64 = c;
	return ir_const(ctx, val, IR_I16);
}

ir_ref ir_const_i32(ir_ctx *ctx, int32_t c)
{
	ir_val val;
	val.i64 = c;
	return ir_const(ctx, val, IR_I32);
}

ir_ref ir_const_i64(ir_ctx *ctx, int64_t c)
{
	ir_val val;
	val.i64 = c;
	return ir_const(ctx, val, IR_I64);
}

ir_ref ir_const_u8(ir_ctx *ctx, uint8_t c)
{
	ir_val val;
	val.u64 = c;
	return ir_const(ctx, val, IR_U8);
}

ir_ref ir_const_u16(ir_ctx *ctx, uint16_t c)
{
	ir_val val;
	val.u64 = c;
	return ir_const(ctx, val, IR_U16);
}

ir_ref ir_const_u32(ir_ctx *ctx, uint32_t c)
{
	ir_val val;
	val.u64 = c;
	return ir_const(ctx, val, IR_U32);
}

ir_ref ir_const_u64(ir_ctx *ctx, uint64_t c)
{
	ir_val val;
	val.u64 = c;
	return ir_const(ctx, val, IR_U64);
}

ir_ref ir_const_bool(ir_ctx *ctx, bool c)
{
	return (c) ? IR_TRUE : IR_FALSE;
}

ir_ref ir_const_char(ir_ctx *ctx, char c)
{
	ir_val val;
	val.i64 = c;
	return ir_const(ctx, val, IR_CHAR);
}

ir_ref ir_const_float(ir_ctx *ctx, float c)
{
	ir_val val;
	val.u32_hi = 0;
	val.f = c;
	return ir_const(ctx, val, IR_FLOAT);
}

ir_ref ir_const_double(ir_ctx *ctx, double c)
{
	ir_val val;
	val.d = c;
	return ir_const(ctx, val, IR_DOUBLE);
}

ir_ref ir_const_addr(ir_ctx *ctx, uintptr_t c)
{
	if (c == 0) {
		return IR_NULL;
	}
	ir_val val;
	val.u64 = c;
	return ir_const(ctx, val, IR_ADDR);
}

ir_ref ir_const_func(ir_ctx *ctx, ir_ref str)
{
	ir_ref ref = ir_next_const(ctx);
	ir_insn *insn = &ctx->ir_base[ref];

	insn->optx = IR_OPT(IR_FUNC, IR_ADDR);
	insn->val.addr = str;

	return ref;
}

ir_ref ir_const_str(ir_ctx *ctx, ir_ref str)
{
	ir_ref ref = ir_next_const(ctx);
	ir_insn *insn = &ctx->ir_base[ref];

	insn->optx = IR_OPT(IR_STR, IR_ADDR);
	insn->val.addr = str;

	return ref;
}

ir_ref ir_str(ir_ctx *ctx, const char *s)
{
	return ir_strtab_lookup(&ctx->strtab, s, strlen(s), ir_strtab_count(&ctx->strtab) + 1);
}

ir_ref ir_strl(ir_ctx *ctx, const char *s, size_t len)
{
	return ir_strtab_lookup(&ctx->strtab, s, len, ir_strtab_count(&ctx->strtab) + 1);
}

const char *ir_get_str(ir_ctx *ctx, ir_ref idx)
{
	return ir_strtab_str(&ctx->strtab, idx - 1);
}

/* IR construction */
ir_ref ir_emit(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3)
{
	ir_ref   ref = ir_next_insn(ctx);
	ir_insn *insn = &ctx->ir_base[ref];

	insn->optx = opt;
	insn->op1 = op1;
	insn->op2 = op2;
	insn->op3 = op3;

	return ref;
}

static ir_ref _ir_fold_cse(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3)
{
	ir_ref ref = ctx->prev_insn_chain[opt & IR_OPT_OP_MASK];
	ir_insn *insn;

	if (ref) {
		ir_ref limit = ctx->fold_cse_limit;

		if (op1 > limit) {
			limit = op1;
		}
		if (op2 > limit) {
			limit = op2;
		}
		if (op3 > limit) {
			limit = op3;
		}
		while (ref >= limit) {
			insn = &ctx->ir_base[ref];
			if (insn->opt == opt && insn->op1 == op1 && insn->op2 == op2 && insn->op3 == op3) {
				return ref;
			}
			if (!insn->prev_insn_offset) {
				break;
			}
			ref = ref - (ir_ref)(uint32_t)insn->prev_insn_offset;
		}
	}

	return IR_UNUSED;
}

#define IR_FOLD(X)        IR_FOLD1(X, __LINE__)
#define IR_FOLD1(X, Y)    IR_FOLD2(X, Y)
#define IR_FOLD2(X, Y)    case IR_RULE_ ## Y:

#define IR_FOLD_ERROR(msg) do { \
		IR_ASSERT(0 && (msg)); \
		goto ir_fold_emit; \
	} while (0)

#define IR_FOLD_CONST_U(_val) do { \
		val.u64 = (_val); \
		goto ir_fold_const; \
	} while (0)

#define IR_FOLD_CONST_I(_val) do { \
		val.i64 = (_val); \
		goto ir_fold_const; \
	} while (0)

#define IR_FOLD_CONST_D(_val) do { \
		val.d = (_val); \
		goto ir_fold_const; \
	} while (0)

#define IR_FOLD_CONST_F(_val) do { \
		val.f = (_val); \
		goto ir_fold_const; \
	} while (0)

#define IR_FOLD_COPY(op)  do { \
		ref = (op); \
		goto ir_fold_copy; \
	} while (0)

#define IR_FOLD_BOOL(cond) \
	IR_FOLD_COPY((cond) ? IR_TRUE : IR_FALSE)

#define IR_FOLD_NAMED(name)    ir_fold_ ## name:
#define IR_FOLD_DO_NAMED(name) goto ir_fold_ ## name
#define IR_FOLD_RESTART        goto ir_fold_restart
#define IR_FOLD_CSE            goto ir_fold_cse
#define IR_FOLD_EMIT           goto ir_fold_emit
#define IR_FOLD_NEXT           break

#include "ir_fold_hash.h"

#define IR_FOLD_RULE(x) ((x) >> 21)
#define IR_FOLD_KEY(x)  ((x) & 0x1fffff)

/*
 * key = insn->op | (insn->op1->op << 7) | (insn->op1->op << 14)
 *
 * ANY and UNUSED ops are represented by 0
 */

ir_ref ir_folding(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3, ir_insn *op1_insn, ir_insn *op2_insn, ir_insn *op3_insn)
{
	uint8_t op;
	ir_ref ref;
	ir_val val;
	uint32_t key, any;

restart:
	key = (opt & IR_OPT_OP_MASK) + ((uint32_t)op1_insn->op << 7) + ((uint32_t)op2_insn->op << 14);
	any = 0x1fffff;
	do {
		uint32_t k = key & any;
		uint32_t h = _ir_fold_hashkey(k);
		uint32_t fh = _ir_fold_hash[h];
		if (IR_FOLD_KEY(fh) == k /*|| (fh = _ir_fold_hash[h+1], (fh & 0x1fffff) == k)*/) {
			switch (IR_FOLD_RULE(fh)) {
#include "ir_fold.h"
				default:
					break;
			}
		}
		if (any == 0x7f) {
			/* All parrerns ar checked. Pass on to CSE. */
			goto ir_fold_cse;
		}
		/* op2/op1/op  op2/_/op    _/op1/op    _/_/op
		 * 0x1fffff -> 0x1fc07f -> 0x003fff -> 0x00007f
		 * from masks to bis: 11 -> 10 -> 01 -> 00
		 *
		 * a b  => x y
		 * 1 1     1 0
		 * 1 0     0 1
		 * 0 1     0 0
		 *
		 * x = a & b; y = !b
		 */
		any = ((any & (any << 7)) & 0x1fc000) | (~any & 0x3f80) | 0x7f;
	} while (1);

ir_fold_restart:
	if (!(ctx->flags & IR_OPT_IN_SCCP)) {
		op1_insn = ctx->ir_base + op1;
		op2_insn = ctx->ir_base + op2;
		op3_insn = ctx->ir_base + op3;
		goto restart;
	} else {
		ctx->fold_insn.optx = opt;
		ctx->fold_insn.op1 = op1;
		ctx->fold_insn.op2 = op2;
		ctx->fold_insn.op3 = op3;
		return IR_FOLD_DO_RESTART;
	}
ir_fold_cse:
	if (!(ctx->flags & IR_OPT_IN_SCCP)) {
		/* Local CSE */
		ref = _ir_fold_cse(ctx, opt, op1, op2, op3);
		if (ref) {
			return ref;
		}

		ref = ir_emit(ctx, opt, op1, op2, op3);

		/* Update local CSE chain */
		op = opt & IR_OPT_OP_MASK;
		ir_ref prev = ctx->prev_insn_chain[op];
		ir_insn *insn = ctx->ir_base + ref;
		if (!prev || ref - prev > 0xffff) {
			/* can't fit into 16-bit */
			insn->prev_insn_offset = 0;
		} else {
			insn->prev_insn_offset = ref - prev;
		}
		ctx->prev_insn_chain[op] = ref;

		return ref;
	} else {
		return IR_FOLD_DO_CSE;
	}
ir_fold_emit:
	if (!(ctx->flags & IR_OPT_IN_SCCP)) {
		return ir_emit(ctx, opt, op1, op2, op3);
	} else {
		return IR_FOLD_DO_EMIT;
	}
ir_fold_copy:
	if (!(ctx->flags & IR_OPT_IN_SCCP)) {
		return ref;
	} else {
		ctx->fold_insn.op1 = ref;
		return IR_FOLD_DO_COPY;
	}
ir_fold_const:
	if (!(ctx->flags & IR_OPT_IN_SCCP)) {
		return ir_const(ctx, val, IR_OPT_TYPE(opt));
	} else {
		ctx->fold_insn.type = IR_OPT_TYPE(opt);
		ctx->fold_insn.val.u64 = val.u64;
		return IR_FOLD_DO_CONST;
	}
}

ir_ref ir_fold(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3)
{
	if (UNEXPECTED(!(ctx->flags & IR_OPT_FOLDING))) {
		return ir_emit(ctx, opt, op1, op2, op3);
	}
	return ir_folding(ctx, opt, op1, op2, op3, ctx->ir_base + op1, ctx->ir_base + op2, ctx->ir_base + op3);
}

ir_ref ir_emit_N(ir_ctx *ctx, uint32_t opt, uint32_t count)
{
	int i;
	ir_ref ref = ctx->insns_count;
	ir_insn *insn;

	while (UNEXPECTED(ref + count/4 >= ctx->insns_limit)) {
		ir_grow_top(ctx);
	}
	ctx->insns_count = ref + 1 + count/4;

	insn = &ctx->ir_base[ref];
	insn->optx = opt;
	if ((opt & IR_OPT_OP_MASK) != IR_PHI) {
		insn->inputs_count = count;
	}
	for (i = 1; i <= (count|3); i++) {
		insn->ops[i] = IR_UNUSED;
	}

	return ref;
}

void ir_set_op(ir_ctx *ctx, ir_ref ref, uint32_t n, ir_ref val)
{
	ir_insn *insn = &ctx->ir_base[ref];

	if (n > 3) {
		uint32_t count = 3;

		if (insn->op == IR_MERGE) {
			count = insn->inputs_count;
			if (count == 0) {
				count = 2;
			}
		} else if (insn->op == IR_CALL || insn->op == IR_TAILCALL) {
			count = insn->inputs_count;
			if (count == 0) {
				count = 2;
			}
		} else if (insn->op == IR_PHI) {
			count = ctx->ir_base[insn->op1].inputs_count + 1;
			if (count == 1) {
				count = 3;
			}
		} else {
			IR_ASSERT(0);
		}
		IR_ASSERT(n <= count);
	}
	insn->ops[n] = val;
}

void ir_bind(ir_ctx *ctx, ir_ref var, ir_ref def)
{
	// TODO: node to VAR binding is not implemented yet
}

/* Batch construction of def->use edges */
void ir_build_def_use_lists(ir_ctx *ctx)
{
	ir_ref n, i, j, *p, ref, def;
	ir_insn *insn;
	ir_bitset worklist1 = ir_bitset_malloc(ctx->insns_count);
	ir_bitset worklist2 = ir_bitset_malloc(ctx->insns_count);
	uint32_t edges_count = 0;
	ir_use_list *lists = ir_mem_calloc(ctx->insns_count, sizeof(ir_use_list));
	ir_ref *edges;

	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count;) {
		n = ir_input_edges_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			ref = *p;
			if (ref > 0) {
				ir_bitset_incl(worklist2, i);
				ir_bitset_incl(worklist1, ref);
				lists[ref].count++;
				edges_count++;
			}
		}
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		i += n;
		insn += n;
	}

	n = 0;
	IR_BITSET_FOREACH(worklist1, ir_bitset_len(ctx->insns_count), ref) {
		lists[ref].refs = n;
		n += lists[ref].count;
		lists[ref].count = 0;
	} IR_BITSET_FOREACH_END();
	IR_ASSERT(n == edges_count);

	edges = ir_mem_malloc(edges_count * sizeof(ir_ref));
	IR_BITSET_FOREACH(worklist2, ir_bitset_len(ctx->insns_count), ref) {
		insn = &ctx->ir_base[ref];
		n = ir_input_edges_count(ctx, insn);
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			def = *p;
			if (def > 0) {
				edges[lists[def].refs + lists[def].count++] = ref;
			}
		}
	} IR_BITSET_FOREACH_END();

	ir_mem_free(worklist2);
	ir_mem_free(worklist1);
	ctx->use_edges = edges;
	ctx->use_lists = lists;
}

/* Helper Data Types */
void ir_array_grow(ir_array *a, uint32_t size)
{
	IR_ASSERT(size > a->size);
	a->refs = ir_mem_realloc(a->refs, size * sizeof(ir_ref));
	memset(a->refs + a->size, 0, (size - a->size) * sizeof(ir_ref));
	a->size = size;
}

void ir_array_insert(ir_array *a, uint32_t i, ir_ref val)
{
	IR_ASSERT(i < a->size);
	if (a->refs[a->size - 1]) {
		ir_array_grow(a, a->size + 1);
	}
	memmove(a->refs + i + 1, a->refs + i, (a->size - i - 1) * sizeof(ir_ref));
	a->refs[i] = val;
}

void ir_array_remove(ir_array *a, uint32_t i)
{
	IR_ASSERT(i < a->size);
	memmove(a->refs + i, a->refs + i + 1, (a->size - i - 1) * sizeof(ir_ref));
	a->refs[a->size - 1] = IR_UNUSED;
}

void ir_list_insert(ir_list *l, uint32_t i, ir_ref val)
{
	IR_ASSERT(i < l->len);
	if (l->len >= l->a.size) {
		ir_array_grow(&l->a, l->a.size + 1);
	}
	memmove(l->a.refs + i + 1, l->a.refs + i, (l->len - i) * sizeof(ir_ref));
	l->a.refs[i] = val;
	l->len++;
}

void ir_list_remove(ir_list *l, uint32_t i)
{
	IR_ASSERT(i < l->len);
	memmove(l->a.refs + i, l->a.refs + i + 1, (l->len - i) * sizeof(ir_ref));
	l->len--;
}

bool ir_list_contains(ir_list *l, ir_ref val)
{
	uint32_t i;

	for (i = 0; i < l->len; i++) {
		if (ir_array_at(&l->a, i) == val) {
			return 1;
		}
	}
	return 0;
}

void *ir_mem_mmap(size_t size)
{
	return mmap(NULL, size, PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

int ir_mem_unmap(void *ptr, size_t size)
{
	munmap(ptr, size);
	return 1;
}

int ir_mem_protect(void *ptr, size_t size)
{
	mprotect(ptr, size, PROT_READ | PROT_EXEC);
	return 1;
}

int ir_mem_unprotect(void *ptr, size_t size)
{
	mprotect(ptr, size, PROT_READ | PROT_WRITE);
	return 1;
}

int ir_mem_flush(void *ptr, size_t size)
{
#if ((defined(__GNUC__) && ZEND_GCC_VERSION >= 4003) || __has_builtin(__builtin___clear_cache))
	__builtin___clear_cache((char*)(ptr), (char*)(ptr) + size);
#endif
#ifdef HAVE_VALGRIND
	VALGRIND_DISCARD_TRANSLATIONS(ptr, size);
#endif
	return 1;
}
