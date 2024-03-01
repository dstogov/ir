/*
 * IR - Lightweight JIT Compilation Framework
 * (LLVM code generator)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_private.h"

#include <math.h>

#define IR_I1  IR_BOOL
#define IR_F64 IR_DOUBLE
#define IR_F32 IR_FLOAT
#define IR_PTR IR_ADDR

#define IR_I8B            (IR_LAST_TYPE + 0)
#define IR_I16B           (IR_LAST_TYPE + 1)
#define IR_I32B           (IR_LAST_TYPE + 2)
#define IR_I64B           (IR_LAST_TYPE + 3)
#define IR_F64I           (IR_LAST_TYPE + 4)
#define IR_F32I           (IR_LAST_TYPE + 5)

#define IR_LAST_LLVM_TYPE (IR_LAST_TYPE + 6)

static const char *ir_type_llvm_name[IR_LAST_LLVM_TYPE] = {
	"void",   // IR_VOID
	"i1",     // IR_BOOL
	"i8",     // IR_U8
	"i16",    // IR_U16
	"i32",    // IR_U32
	"i64",    // IR_U63
	"ptr",    // IR_ADDR
	"i8",     // IR_CHAR
	"i8",     // IR_I8
	"i16",    // IR_I16
	"i32",    // IR_I32
	"i64",    // IR_I64
	"double", // IR_DOUBLE
	"float",  // IR_FLOAT

	"{i8, i1}",      // IR_I8B
	"{i16, i1}",     // IR_I16B
	"{i32, i1}",     // IR_I32B
	"{i64, i1}",     // IR_I64B
	"{double, i32}", // IR_F64I
	"{float, i32}",  // IR_F32I
};

#ifdef IR_64
# define IR_LLVM_MEMSET_NAME  "llvm.memset.p0.i64"
# define IR_LLVM_MEMCPY_NAME  "llvm.memcpy.p0.p0.i64"
# define IR_LLVM_MEMMOVE_NAME "llvm.memmove.p0.p0.i64"
#else
# define IR_LLVM_MEMSET_NAME  "llvm.memset.p0.i32"
# define IR_LLVM_MEMCPY_NAME  "llvm.memcpy.p0.p0.i32"
# define IR_LLVM_MEMMOVE_NAME "llvm.memmove.p0.p0.i32"
#endif

#define IR_BUILTINS(_) \
	_("ceil",      CEIL_F64)     \
	_("ceilf",     CEIL_F32)     \
	_("copysign",  COPYSIGN_F64) \
	_("copysignf", COPYSIGN_F32) \
	_("cos",       COS_F64)      \
	_("cosf",      COS_F32)      \
	_("exp",       EXP_F64)      \
	_("exp10",     EXP10_F64)    \
	_("exp10f",    EXP10_F32)    \
	_("exp2",      EXP2_F64)     \
	_("exp2f",     EXP2_F32)     \
	_("expf",      EXP_F32)      \
	_("floor",     FLOOR_F64)    \
	_("floorf",    FLOOR_F32)    \
	_("isnan",     ISNAN_F64)    \
	_("isnanf",    ISNAN_F32)    \
	_("ldexp",     LDEXP_F64)    \
	_("ldexpf",    LDEXP_F32)    \
	_("log",       LOG_F64)      \
	_("log10",     LOG10_F64)    \
	_("log10f",    LOG10_F32)    \
	_("log2",      LOG2_F64)     \
	_("log2f",     LOG2_F32)     \
	_("logf",      LOG_F32)      \
	_("memcpy",    MEMCPY)       \
	_("memmove",   MEMMOVE)      \
	_("memset",    MEMSET)       \
	_("pow",       POW_F64)      \
	_("powf",      POW_F32)      \
	_("rint",      RINT_F64)    \
	_("rintf",     RINT_F32)    \
	_("round",     ROUND_F64)    \
	_("roundf",    ROUND_F32)    \
	_("sin",       SIN_F64)      \
	_("sinf",      SIN_F32)      \
	_("sqrt",      SQRT_F64)     \
	_("sqrtf",     SQRT_F32)     \
	_("trunc",     TRUNC_F64)    \
	_("truncf",    TRUNC_F32)    \

#define IR_LLVM_INTRINSICS(_) \
	_(MEMSET,       IR_LLVM_MEMSET_NAME,           VOID, 4, PTR, I8,  SSIZE_T, I1) \
	_(MEMCPY,       IR_LLVM_MEMCPY_NAME,           VOID, 4, PTR, PTR, SSIZE_T, I1) \
	_(MEMMOVE,      IR_LLVM_MEMMOVE_NAME,          VOID, 4, PTR, PTR, SSIZE_T, I1) \
	_(FRAMEADDRESS, "llvm.frameaddress.p0",        PTR,  1, I32, ___, ___, ___) \
	_(VA_START,     "llvm.va_start",               VOID, 1, PTR, ___, ___, ___) \
	_(VA_END,       "llvm.va_end",                 VOID, 1, PTR, ___, ___, ___) \
	_(VA_COPY,      "llvm.va_copy",                VOID, 2, PTR, PTR, ___, ___) \
	_(DEBUGTRAP,    "llvm.debugtrap",              VOID, 0, ___, ___, ___, ___) \
	_(SADD_OV_I8,   "llvm.sadd.with.overflow.i8",  I8B,  2, I8,  I8,  ___, ___) \
	_(SADD_OV_I16,  "llvm.sadd.with.overflow.i16", I16B, 2, I16, I16, ___, ___) \
	_(SADD_OV_I32,  "llvm.sadd.with.overflow.i32", I32B, 2, I32, I32, ___, ___) \
	_(SADD_OV_I64,  "llvm.sadd.with.overflow.i64", I64B, 2, I64, I64, ___, ___) \
	_(UADD_OV_I8,   "llvm.uadd.with.overflow.i8",  I8B,  2, I8,  I8,  ___, ___) \
	_(UADD_OV_I16,  "llvm.uadd.with.overflow.i16", I16B, 2, I16, I16, ___, ___) \
	_(UADD_OV_I32,  "llvm.uadd.with.overflow.i32", I32B, 2, I32, I32, ___, ___) \
	_(UADD_OV_I64,  "llvm.uadd.with.overflow.i64", I64B, 2, I64, I64, ___, ___) \
	_(SSUB_OV_I8,   "llvm.ssub.with.overflow.i8",  I8B,  2, I8,  I8,  ___, ___) \
	_(SSUB_OV_I16,  "llvm.ssub.with.overflow.i16", I16B, 2, I16, I16, ___, ___) \
	_(SSUB_OV_I32,  "llvm.ssub.with.overflow.i32", I32B, 2, I32, I32, ___, ___) \
	_(USUB_OV_I8,   "llvm.usub.with.overflow.i8",  I8B,  2, I8,  I8,  ___, ___) \
	_(USUB_OV_I16,  "llvm.usub.with.overflow.i16", I16B, 2, I16, I16, ___, ___) \
	_(USUB_OV_I32,  "llvm.usub.with.overflow.i32", I32B, 2, I32, I32, ___, ___) \
	_(USUB_OV_I64,  "llvm.usub.with.overflow.i64", I64B, 2, I64, I64, ___, ___) \
	_(SSUB_OV_I64,  "llvm.ssub.with.overflow.i64", I64B, 2, I64, I64, ___, ___) \
	_(SMUL_OV_I8,   "llvm.smul.with.overflow.i8",  I8B,  2, I8,  I8,  ___, ___) \
	_(SMUL_OV_I16,  "llvm.smul.with.overflow.i16", I16B, 2, I16, I16, ___, ___) \
	_(SMUL_OV_I32,  "llvm.smul.with.overflow.i32", I32B, 2, I32, I32, ___, ___) \
	_(SMUL_OV_I64,  "llvm.smul.with.overflow.i64", I64B, 2, I64, I64, ___, ___) \
	_(UMUL_OV_I8,   "llvm.umul.with.overflow.i8",  I8B,  2, I8,  I8,  ___, ___) \
	_(UMUL_OV_I16,  "llvm.umul.with.overflow.i16", I16B, 2, I16, I16, ___, ___) \
	_(UMUL_OV_I32,  "llvm.umul.with.overflow.i32", I32B, 2, I32, I32, ___, ___) \
	_(UMUL_OV_I64,  "llvm.umul.with.overflow.i64", I64B, 2, I64, I64, ___, ___) \
	_(FSHL_I8,      "llvm.fshl.i8",                I8,   3, I8,  I8,  I8,  ___) \
	_(FSHL_I16,     "llvm.fshl.i16",               I16,  3, I16, I16, I16, ___) \
	_(FSHL_I32,     "llvm.fshl.i32",               I32,  3, I32, I32, I32, ___) \
	_(FSHL_I64,     "llvm.fshl.i64",               I64,  3, I64, I64, I64, ___) \
	_(FSHR_I8,      "llvm.fshr.i8",                I8,   3, I8,  I8,  I8,  ___) \
	_(FSHR_I16,     "llvm.fshr.i16",               I16,  3, I16, I16, I16, ___) \
	_(FSHR_I32,     "llvm.fshr.i32",               I32,  3, I32, I32, I32, ___) \
	_(FSHR_I64,     "llvm.fshr.i64",               I64,  3, I64, I64, I64, ___) \
	_(BSWAP_I16,    "llvm.bswap.i16",              I16,  1, I16, ___, ___, ___) \
	_(BSWAP_I32,    "llvm.bswap.i32",              I32,  1, I32, ___, ___, ___) \
	_(BSWAP_I64,    "llvm.bswap.i64",              I64,  1, I64, ___, ___, ___) \
	_(CTPOP_I8,     "llvm.ctpop.i8",               I8,   1, I8,  ___, ___, ___) \
	_(CTPOP_I16,    "llvm.ctpop.i16",              I16,  1, I16, ___, ___, ___) \
	_(CTPOP_I32,    "llvm.ctpop.i32",              I32,  1, I32, ___, ___, ___) \
	_(CTPOP_I64,    "llvm.ctpop.i64",              I64,  1, I64, ___, ___, ___) \
	_(CTLZ_I8,      "llvm.ctlz.i8",                I8,   2, I8,  I1,  ___, ___) \
	_(CTLZ_I16,     "llvm.ctlz.i16",               I16,  2, I16, I1,  ___, ___) \
	_(CTLZ_I32,     "llvm.ctlz.i32",               I32,  2, I32, I1,  ___, ___) \
	_(CTLZ_I64,     "llvm.ctlz.i64",               I64,  2, I64, I1,  ___, ___) \
	_(CTTZ_I8,      "llvm.cttz.i8",                I8,   2, I8,  I1,  ___, ___) \
	_(CTTZ_I16,     "llvm.cttz.i16",               I16,  2, I16, I1,  ___, ___) \
	_(CTTZ_I32,     "llvm.cttz.i32",               I32,  2, I32, I1,  ___, ___) \
	_(CTTZ_I64,     "llvm.cttz.i64",               I64,  2, I64, I1,  ___, ___) \
	_(SMIN_I8,      "llvm.smin.i8",                I8,   2, I8,  I8,  ___, ___) \
	_(SMIN_I16,     "llvm.smin.i16",               I16,  2, I16, I16, ___, ___) \
	_(SMIN_I32,     "llvm.smin.i32",               I32,  2, I32, I32, ___, ___) \
	_(SMIN_I64,     "llvm.smin.i64",               I64,  2, I64, I64, ___, ___) \
	_(UMIN_I8,      "llvm.umin.i8",                I8,   2, I8,  I8,  ___, ___) \
	_(UMIN_I16,     "llvm.umin.i16",               I16,  2, I16, I16, ___, ___) \
	_(UMIN_I32,     "llvm.umin.i32",               I32,  2, I32, I32, ___, ___) \
	_(UMIN_I64,     "llvm.umin.i64",               I64,  2, I64, I64, ___, ___) \
	_(MINNUM_F64,   "llvm.minnum.f64",             F64,  2, F64, F64, ___, ___) \
	_(MINNUM_F32,   "llvm.minnum.f32",             F32,  2, F32, F32, ___, ___) \
	_(SMAX_I8,      "llvm.smax.i8",                I8,   2, I8,  I8,  ___, ___) \
	_(SMAX_I16,     "llvm.smax.i16",               I16,  2, I16, I16, ___, ___) \
	_(SMAX_I32,     "llvm.smax.i32",               I32,  2, I32, I32, ___, ___) \
	_(SMAX_I64,     "llvm.smax.i64",               I64,  2, I64, I64, ___, ___) \
	_(UMAX_I8,      "llvm.umax.i8",                I8,   2, I8,  I8,  ___, ___) \
	_(UMAX_I16,     "llvm.umax.i16",               I16,  2, I16, I16, ___, ___) \
	_(UMAX_I32,     "llvm.umax.i32",               I32,  2, I32, I32, ___, ___) \
	_(UMAX_I64,     "llvm.umax.i64",               I64,  2, I64, I64, ___, ___) \
	_(MAXNUM_F64,   "llvm.maxnum.f64",             F64,  2, F64, F64, ___, ___) \
	_(MAXNUM_F32,   "llvm.maxnum.f32",             F32,  2, F32, F32, ___, ___) \
	_(ABS_I8,       "llvm.abs.i8",                 I8,   2, I8,  I1,  ___, ___) \
	_(ABS_I16,      "llvm.abs.i16",                I16,  2, I16, I1,  ___, ___) \
	_(ABS_I32,      "llvm.abs.i32",                I32,  2, I32, I1,  ___, ___) \
	_(ABS_I64,      "llvm.abs.i64",                I64,  2, I64, I1,  ___, ___) \
	_(FABS_F64,     "llvm.fabs.f64",               F64,  1, F64, ___, ___, ___) \
	_(FABS_F32,     "llvm.fabs.f32",               F32,  1, F32, ___, ___, ___) \
	_(SQRT_F64,     "llvm.sqrt.f64",               F64,  1, F64, ___, ___, ___) \
	_(SQRT_F32,     "llvm.sqrt.f32",               F32,  1, F32, ___, ___, ___) \
	_(SIN_F64,      "llvm.sin.f64",                F64,  1, F64, ___, ___, ___) \
	_(SIN_F32,      "llvm.sin.f32",                F32,  1, F32, ___, ___, ___) \
	_(COS_F64,      "llvm.cos.f64",                F64,  1, F64, ___, ___, ___) \
	_(COS_F32,      "llvm.cos.f32",                F32,  1, F32, ___, ___, ___) \
	_(POW_F64,      "llvm.pow.f64",                F64,  2, F64, F64, ___, ___) \
	_(POW_F32,      "llvm.pow.f32",                F32,  2, F32, F32, ___, ___) \
	_(EXP_F64,      "llvm.exp.f64",                F64,  1, F64, ___, ___, ___) \
	_(EXP_F32,      "llvm.exp.f32",                F32,  1, F32, ___, ___, ___) \
	_(EXP2_F64,     "llvm.exp2.f64",               F64,  1, F64, ___, ___, ___) \
	_(EXP2_F32,     "llvm.exp2.f32",               F32,  1, F32, ___, ___, ___) \
	_(EXP10_F64,    "llvm.exp10.f64",              F64,  1, F64, ___, ___, ___) \
	_(EXP10_F32,    "llvm.exp10.f32",              F32,  1, F32, ___, ___, ___) \
	_(LDEXP_F64,    "llvm.ldexp.f64.i32",          F64,  2, F64, I32, ___, ___) \
	_(LDEXP_F32,    "llvm.ldexp.f32.i32",          F32,  2, F32, I32, ___, ___) \
	_(LOG_F64,      "llvm.log.f64",                F64,  1, F64, ___, ___, ___) \
	_(LOG_F32,      "llvm.log.f32",                F32,  1, F32, ___, ___, ___) \
	_(LOG2_F64,     "llvm.log2.f64",               F64,  1, F64, ___, ___, ___) \
	_(LOG2_F32,     "llvm.log2.f32",               F32,  1, F32, ___, ___, ___) \
	_(LOG10_F64,    "llvm.log10.f64",              F64,  1, F64, ___, ___, ___) \
	_(LOG10_F32,    "llvm.log10.f32",              F32,  1, F32, ___, ___, ___) \
	_(COPYSIGN_F64, "llvm.copysign.f64",           F32,  2, F32, F32, ___, ___) \
	_(COPYSIGN_F32, "llvm.copysign.f32",           F64,  2, F64, F64, ___, ___) \
	_(FLOOR_F64,    "llvm.floor.f64",              F64,  1, F64, ___, ___, ___) \
	_(FLOOR_F32,    "llvm.floor.f32",              F32,  1, F32, ___, ___, ___) \
	_(CEIL_F64,     "llvm.ceil.f64",               F64,  1, F64, ___, ___, ___) \
	_(CEIL_F32,     "llvm.ceil.f32",               F32,  1, F32, ___, ___, ___) \
	_(TRUNC_F64,    "llvm.trunc.f64",              F64,  1, F64, ___, ___, ___) \
	_(TRUNC_F32,    "llvm.trunc.f32",              F32,  1, F32, ___, ___, ___) \
	_(RINT_F64,     "llvm.rint.f64",               F64,  1, F64, ___, ___, ___) \
	_(RINT_F32,     "llvm.rint.f32",               F32,  1, F32, ___, ___, ___) \
	_(ROUND_F64,    "llvm.round.f64",              F64,  1, F64, ___, ___, ___) \
	_(ROUND_F32,    "llvm.round.f32",              F32,  1, F32, ___, ___, ___) \
	_(ISNAN_F64,    "fcmp uno double", /* fake */  F64I, 1, F64, ___, ___, ___) \
	_(ISNAN_F32,    "fcmp uno float",  /* fake */  F32I, 1, F32, ___, ___, ___) \

#define IR____ 0
#define IR_LLVM_INTRINSIC_ID(id, name, ret, num, arg1, arg2, arg3, arg4) \
	IR_LLVM_INTR_ ## id,
#define IR_LLVM_INTRINSIC_DESC(id, name, ret, num, arg1, arg2, arg3, arg4) \
	{name, IR_ ## ret, num, {IR_ ## arg1, IR_ ## arg2, IR_ ## arg3, IR_ ## arg4}},

typedef enum _ir_llvm_intrinsic_id {
	IR_LLVM_INTRINSICS(IR_LLVM_INTRINSIC_ID)
	IR_LLVM_INTR_LAST
} ir_llvm_intrinsic_id;

#define IR_LLVM_INTRINSIC_BITSET_LEN ((IR_LLVM_INTR_LAST + (IR_BITSET_BITS - 1)) / IR_BITSET_BITS)

static const struct {
	const char *name;
	uint8_t     ret_type;
	uint8_t     params_count;
	uint8_t     param_types[4];
} ir_llvm_intrinsic_desc[] = {
	IR_LLVM_INTRINSICS(IR_LLVM_INTRINSIC_DESC)
};

#define IR_LLVM_BUILTIN_DESC(name, id) \
	{name, IR_LLVM_INTR_ ## id},

static const struct {
	const char           *name;
	ir_llvm_intrinsic_id  id;
} ir_llvm_builtin_map[] = {
	IR_BUILTINS(IR_LLVM_BUILTIN_DESC)
};

static void ir_emit_ref(ir_ctx *ctx, FILE *f, ir_ref ref)
{
	if (IR_IS_CONST_REF(ref)) {
		ir_insn *insn = &ctx->ir_base[ref];
		if (insn->op == IR_FUNC || insn->op == IR_SYM) {
			fprintf(f, "@%s", ir_get_str(ctx, insn->val.name));
		} else if (insn->op == IR_STR) {
			fprintf(f, "@.str%d", -ref);
		} else if (insn->op == IR_ADDR) {
			if (insn->val.addr == 0) {
				fprintf(f, "null");
			} else if (insn->op == IR_ADDR) {
				fprintf(f, "u0x%" PRIxPTR, insn->val.addr);
//				fprintf(f, "inttoptr(i64 u0x%" PRIxPTR " to ptr)", insn->val.addr);
			}
		} else if (IR_IS_TYPE_FP(insn->type)) {
			if (insn->type == IR_DOUBLE) {
				double d = insn->val.d;
				if (isnan(d)) {
					fprintf(f, "nan");
				} else if (d == 0.0) {
					fprintf(f, "0.0");
				} else {
					double e = log10(d);
					if (e < -4 || e >= 6) {
						fprintf(f, "%e", d);
					} else if (round(d) == d) {
						fprintf(f, "%.0f.0", d);
					} else {
						fprintf(f, "%g", d);
					}
				}
			} else {
				IR_ASSERT(insn->type == IR_FLOAT);
				double d = insn->val.f;
				if (isnan(d)) {
					fprintf(f, "nan");
				} else if (d == 0.0) {
					fprintf(f, "0.0");
				} else {
					double e = log10(d);
					if (e < -4 || e >= 6) {
						fprintf(f, "%e", d);
					} else if (round(d) == d) {
						fprintf(f, "%.0f.0", d);
					} else {
						fprintf(f, "%g", d);
					}
				}
			}
		} else {
			ir_print_const(ctx, &ctx->ir_base[ref], f, true);
		}
	} else {
		fprintf(f, "%%d%d", ref);
	}
}

static void ir_emit_def_ref(ir_ctx *ctx, FILE *f, ir_ref def)
{
	fprintf(f, "\t%%d%d = ", def);
}

static void ir_emit_phi(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, ir_block *bb)
{
	bool first = 1;
	uint32_t j, n, *p;

	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "phi %s ", ir_type_llvm_name[insn->type]);
	n = insn->inputs_count;
	p = ctx->cfg_edges + bb->predecessors;
	for (j = 2; j <= n; j++) {
		if (first) {
			first = 0;
		} else {
			fprintf(f, ", ");
		}
		fprintf(f, "[");
		ir_emit_ref(ctx, f, ir_insn_op(insn, j));
		fprintf(f, ", %%l%d", *p);
		p++;
		fprintf(f, "]");
	}
	fprintf(f, "\n");
}

static void ir_emit_unary_neg(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = insn->type;

	ir_emit_def_ref(ctx, f, def);
	if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "fneg %s ", ir_type_llvm_name[type]);
	} else {
		fprintf(f, "sub %s 0, ", ir_type_llvm_name[type]);
	}
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, "\n");
}

static void ir_emit_unary_not(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	if (insn->type == IR_BOOL) {
		if (IR_IS_TYPE_FP(type)) {
			fprintf(f, "fcmp oeq %s ", ir_type_llvm_name[type]);
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, ", 0.0\n");
		} else {
			fprintf(f, "icmp eq %s ", ir_type_llvm_name[type]);
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, ", 0\n");
		}
	} else {
		IR_ASSERT(IR_IS_TYPE_INT(type) && type == insn->type);
		fprintf(f, "xor %s ", ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", -1\n");
	}
}

static void ir_emit_binary_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op, const char *uop, const char *fop)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	if (insn->type == IR_ADDR) {
		type = sizeof(void*) == 8 ? IR_U64 : IR_U32;
		if (!IR_IS_CONST_REF(insn->op1) && ctx->ir_base[insn->op1].type != IR_I64 && ctx->ir_base[insn->op1].type != IR_U64) {
			fprintf(f, "\t%%t%d_1 = ptrtoint ptr ", def);
			ir_emit_ref(ctx, f, insn->op1);
			fprintf(f, " to i64\n");
		}
		if (!IR_IS_CONST_REF(insn->op2) && ctx->ir_base[insn->op2].type != IR_I64 && ctx->ir_base[insn->op2].type != IR_U64) {
			fprintf(f, "\t%%t%d_2 = ptrtoint ptr ", def);
			ir_emit_ref(ctx, f, insn->op2);
			fprintf(f, " to i64\n");
		}
		fprintf(f, "\t%%t%d = %s %s ", def, uop ? uop : op, ir_type_llvm_name[type]);
		if (IR_IS_CONST_REF(insn->op1)) {
			fprintf(f, "%" PRIu64 ", ", ctx->ir_base[insn->op1].val.u64);
		} else if (ctx->ir_base[insn->op1].type == IR_I64 || ctx->ir_base[insn->op1].type == IR_U64) {
			fprintf(f, "%%d%d, ", insn->op1);
		} else {
			fprintf(f, "%%t%d_1, ", def);
		}
		if (IR_IS_CONST_REF(insn->op2)) {
			fprintf(f, "%" PRIu64 "\n", ctx->ir_base[insn->op2].val.u64);
		} else if (ctx->ir_base[insn->op2].type == IR_I64 || ctx->ir_base[insn->op2].type == IR_U64) {
			fprintf(f, "%%d%d\n", insn->op2);
		} else {
			fprintf(f, "%%t%d_2\n", def);
		}
		fprintf(f, "\t%%d%d = inttoptr i64 %%t%d to ptr\n", def, def);
		return;
	}
	ir_emit_def_ref(ctx, f, def);
	if (fop && IR_IS_TYPE_FP(type)) {
		fprintf(f, "%s ", fop);
	} else {
		IR_ASSERT(IR_IS_TYPE_INT(type));
		if (uop && IR_IS_TYPE_UNSIGNED(type)) {
			fprintf(f, "%s ", uop);
		} else {
			fprintf(f, "%s ", op);
		}
	}
	fprintf(f, "%s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", ");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, "\n");
}

static void ir_emit_binary_overflow_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn,
                                       ir_llvm_intrinsic_id id, ir_bitset used_intrinsics)
{
	ir_type type = insn->type;

	IR_ASSERT(IR_IS_TYPE_INT(type));
	if (IR_IS_TYPE_UNSIGNED(type)) id += 4;
	switch (ir_type_size[type]) {
		case 1: break;
		case 2: id += 1; break;
		case 4: id += 2; break;
		case 8: id += 3; break;
	}
	ir_bitset_incl(used_intrinsics, id);
	fprintf(f, "\t%%t%d = call {%s, i1} @%s(%s ", def, ir_type_llvm_name[type],
		ir_llvm_intrinsic_desc[id].name, ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ")\n");

	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "extractvalue {%s, i1} %%t%d, 0\n", ir_type_llvm_name[type], def);
}

static void ir_emit_rol_ror(ir_ctx *ctx, FILE *f, int def, ir_insn *insn,
                            ir_llvm_intrinsic_id id, ir_bitset used_intrinsics)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	switch (ir_type_size[type]) {
		case 1: break;
		case 2: id += 1; break;
		case 4: id += 2; break;
		case 8: id += 3; break;
	}
	ir_bitset_incl(used_intrinsics, id);
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "call %s @%s(%s ",
		ir_type_llvm_name[type], ir_llvm_intrinsic_desc[id].name, ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ")\n");
}

static void ir_emit_bitop(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, ir_llvm_intrinsic_id id,
                          bool poison, ir_bitset used_intrinsics)
{
	ir_type type = insn->type;

	if (id == IR_LLVM_INTR_BSWAP_I16) {
		id -= 1;
	}
	switch (ir_type_size[type]) {
		case 1: break;
		case 2: id += 1; break;
		case 4: id += 2; break;
		case 8: id += 3; break;
	}
	ir_bitset_incl(used_intrinsics, id);
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "call %s @%s(%s ",
		ir_type_llvm_name[type], ir_llvm_intrinsic_desc[id].name, ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	if (poison) {
		fprintf(f, ", 0");
	}
	fprintf(f, ")\n");
}

static void ir_emit_conv(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, const char *op)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "%s %s ", op, ir_type_llvm_name[ctx->ir_base[insn->op1].type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " to %s\n", ir_type_llvm_name[insn->type]);
}

static void ir_emit_minmax_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn,
                              ir_llvm_intrinsic_id id, ir_bitset used_intrinsics)
{
	ir_type type = insn->type;

	if (IR_IS_TYPE_FP(type)) {
		id += (type == IR_DOUBLE) ? 8 : 9;
	} else {
		if (IR_IS_TYPE_UNSIGNED(type)) {
			id += 4;
		}
		switch (ir_type_size[type]) {
			case 2: id += 1; break;
			case 4: id += 2; break;
			case 8: id += 3; break;
		}
	}
	ir_bitset_incl(used_intrinsics, id);
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "call %s @%s(%s ", ir_type_llvm_name[type],
		ir_llvm_intrinsic_desc[id].name, ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, ", %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ")\n");
}

static void ir_emit_conditional_op(ir_ctx *ctx, FILE *f, int def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op1].type;

	ir_emit_def_ref(ctx, f, def);
	if (type == IR_BOOL) {
		fprintf(f, "select i1 ");
		ir_emit_ref(ctx, f, insn->op1);
	} else if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "\t%%t%d = fcmp une %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", 0.0\n");
		fprintf(f, "select i1 %%t%d", def);
	} else {
		fprintf(f, "\t%%t%d = icmp ne %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", 0\n");
		fprintf(f, "select i1 %%t%d", def);
	}

	fprintf(f, ", %s ", ir_type_llvm_name[insn->type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ", %s ", ir_type_llvm_name[insn->type]);
	ir_emit_ref(ctx, f, insn->op3);
	fprintf(f, "\n");
}

static void ir_emit_abs(ir_ctx *ctx, FILE *f, int def, ir_insn *insn, ir_bitset used_intrinsics)
{
	ir_type type = ctx->ir_base[insn->op1].type;
	ir_llvm_intrinsic_id id;

	ir_emit_def_ref(ctx, f, def);
	if (IR_IS_TYPE_FP(type)) {
		id = type == IR_DOUBLE ? IR_LLVM_INTR_FABS_F64 : IR_LLVM_INTR_FABS_F32;
		ir_bitset_incl(used_intrinsics, id);
		fprintf(f, "call %s @%s(%s ",
			ir_type_llvm_name[type], ir_llvm_intrinsic_desc[id].name, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ")\n");
	} else {
		id = IR_LLVM_INTR_ABS_I8;
		switch (ir_type_size[type]) {
			case 2: id += 1; break;
			case 4: id += 2; break;
			case 8: id += 3; break;
		}
		ir_bitset_incl(used_intrinsics, id);
		fprintf(f, "call %s @%s(%s ",
			ir_type_llvm_name[type], ir_llvm_intrinsic_desc[id].name, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op1);
		fprintf(f, ", i1 0)\n");
	}
}

static void ir_emit_if(ir_ctx *ctx, FILE *f, uint32_t b, ir_ref def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op2].type;
	uint32_t true_block = 0, false_block = 0;

	ir_get_true_false_blocks(ctx, b, &true_block, &false_block);

	// TODO: i1 @llvm.expect.i1(i1 <val>, i1 <expected_val>) ???

	if (type == IR_BOOL) {
		fprintf(f, "\tbr i1 ");
		ir_emit_ref(ctx, f, insn->op2);
	} else if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "\t%%t%d = fcmp une %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", 0.0\n");
		fprintf(f, "\tbr i1 %%t%d", def);
	} else {
		fprintf(f, "\t%%t%d = icmp ne %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", 0\n");
		fprintf(f, "\tbr i1 %%t%d", def);
	}
	fprintf(f, ", label %%l%d, label %%l%d\n", true_block, false_block);
}

static void ir_emit_guard(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op2].type;

	// TODO: i1 @llvm.expect.i1(i1 <val>, i1 <expected_val>) ???
	if (type == IR_BOOL) {
		fprintf(f, "\tbr i1 ");
		ir_emit_ref(ctx, f, insn->op2);
	} else if (IR_IS_TYPE_FP(type)) {
		fprintf(f, "\t%%t%d = fcmp une %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", 0.0\n");
		fprintf(f, "\tbr i1 %%t%d", def);
	} else {
		fprintf(f, "\t%%t%d = icmp ne %s ", def, ir_type_llvm_name[type]);
		ir_emit_ref(ctx, f, insn->op2);
		fprintf(f, ", 0\n");
		fprintf(f, "\tbr i1 %%t%d", def);
	}
	fprintf(f, ", label %%l%d_true, label %%l%d_false\n", def, def);
	fprintf(f, "l%d_%s:\n", def, insn->op == IR_GUARD ? "false" : "true");
	fprintf(f, "\tindirectbr ptr ");
	if (IR_IS_CONST_REF(insn->op3) && ctx->ir_base[insn->op3].op == IR_ADDR) {
		fprintf(f, "inttoptr(i64 u0x%" PRIxPTR " to ptr)", ctx->ir_base[insn->op3].val.addr);
	} else {
		ir_emit_ref(ctx, f, insn->op3);
	}
	fprintf(f, ", []\n");
	fprintf(f, "l%d_%s:\n", def, insn->op == IR_GUARD ? "true" : "false");
}

static void ir_emit_switch(ir_ctx *ctx, FILE *f, uint32_t b, ir_ref def, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op2].type;
	ir_block *bb;
	uint32_t n, *p, use_block;
	ir_insn *use_insn;

	// TODO: i1 @llvm.expect.i1(i1 <val>, i1 <expected_val>) ???

	fprintf(f, "\tswitch %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op2);

	bb = &ctx->cfg_blocks[b];

	p = &ctx->cfg_edges[bb->successors];
	for (n = bb->successors_count; n != 0; p++, n--) {
		use_block = *p;
		use_insn = &ctx->ir_base[ctx->cfg_blocks[use_block].start];
		if (use_insn->op == IR_CASE_DEFAULT) {
			fprintf(f, ", label %%l%d", ir_skip_empty_target_blocks(ctx, use_block));
			break;
		} else {
			IR_ASSERT(use_insn->op == IR_CASE_VAL);
		}
	}
	fprintf(f, " [\n");
	p = &ctx->cfg_edges[bb->successors];
	for (n = bb->successors_count; n != 0; p++, n--) {
		use_block = *p;
		use_insn = &ctx->ir_base[ctx->cfg_blocks[use_block].start];
		if (use_insn->op == IR_CASE_VAL) {
			fprintf(f, "\t\t%s ", ir_type_llvm_name[type]);
			ir_emit_ref(ctx, f, use_insn->op2);
			fprintf(f, ", label %%l%d\n", ir_skip_empty_target_blocks(ctx, use_block));
		} else {
			IR_ASSERT(use_insn->op == IR_CASE_DEFAULT);
		}
	}
	fprintf(f, "\t]\n");
}

static int ir_builtin_func(const char *name)
{
	int l = 0;
	int r = sizeof(ir_llvm_builtin_map) / sizeof(ir_llvm_builtin_map[0]);

	while (l <= r) {
		int n = (l + r) / 2;
		int ret = strcmp(name, ir_llvm_builtin_map[n].name);

		if (ret > 0) {
			l = n + 1;
		} else if (ret < 0) {
			r = n - 1;
		} else {
			return n;
		}
	}
	return -1;
}

static void ir_emit_call(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn, ir_bitset used_intrinsics)
{
	int j, k, n;
	ir_ref last_arg = IR_UNUSED;
	const char *name = NULL;

	if (IR_IS_CONST_REF(insn->op2)) {
		const ir_insn *func = &ctx->ir_base[insn->op2];

		IR_ASSERT(func->op == IR_FUNC);
		name = ir_get_str(ctx, func->val.name);
		if (func->proto) {
			const ir_proto_t *proto = (const ir_proto_t *)ir_get_str(ctx, func->proto);

			if (proto->flags & IR_BUILTIN_FUNC) {
				int n = ir_builtin_func(name);
				if (n >= 0) {
					ir_llvm_intrinsic_id id = ir_llvm_builtin_map[n].id;

					if (id == IR_LLVM_INTR_MEMSET
					 || id == IR_LLVM_INTR_MEMCPY
					 || id == IR_LLVM_INTR_MEMMOVE) {
						last_arg = IR_FALSE;
				    } else if (id == IR_LLVM_INTR_ISNAN_F64
				     || id == IR_LLVM_INTR_ISNAN_F32) {
						ir_emit_def_ref(ctx, f, def);
						fprintf(f, "%s ", ir_llvm_intrinsic_desc[id].name);
						ir_emit_ref(ctx, f, insn->op3);
						fprintf(f, ", 0.0\n");
						return;
				    }
					ir_bitset_incl(used_intrinsics, id);
					name = ir_llvm_intrinsic_desc[id].name;
			    }
			}
		}
	}

	if (insn->type != IR_VOID) {
		ir_emit_def_ref(ctx, f, def);
	} else {
		fprintf(f, "\t");
	}
	if (insn->op == IR_TAILCALL) {
		fprintf(f, "tail ");
	}
	fprintf(f, "call ");
	if (ir_is_fastcall(ctx, insn)) {
		fprintf(f, "x86_fastcallcc ");
	}
	fprintf(f, "%s ", ir_type_llvm_name[insn->type]);
	if (ir_is_vararg(ctx, insn)) {
		fprintf(f, "(...) ");
	}

	// TODO: function prototype ???

	if (IR_IS_CONST_REF(insn->op2)) {
		fprintf(f, "@%s", name);
	} else {
		ir_emit_ref(ctx, f, insn->op2);
	}
	fprintf(f, "(");
	n = insn->inputs_count;
	for (j = 3; j <= n; j++) {
		if (j != 3) {
			fprintf(f, ", ");
		}
		k = ir_insn_op(insn, j);
		fprintf(f, "%s ", ir_type_llvm_name[ctx->ir_base[k].type]);
		ir_emit_ref(ctx, f, k);
	}
	if (last_arg) {
		fprintf(f, ", ");
		fprintf(f, "%s ", ir_type_llvm_name[ctx->ir_base[last_arg].type]);
		ir_emit_ref(ctx, f, last_arg);
	}
	fprintf(f, ")\n");
	if (insn->op == IR_TAILCALL) {
		if (insn->type != IR_VOID) {
			fprintf(f, "\tret %s", ir_type_llvm_name[insn->type]);
			fprintf(f, " ");
			ir_emit_ref(ctx, f, def);
			fprintf(f, "\n");
		} else {
			fprintf(f, "\tunreachable\n");
		}
	}
}

static void ir_emit_ijmp(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	fprintf(f, "\tindirectbr ptr ");
	if (IR_IS_CONST_REF(insn->op2) && ctx->ir_base[insn->op2].op == IR_ADDR) {
		fprintf(f, "inttoptr(i64 u0x%" PRIxPTR " to ptr)", ctx->ir_base[insn->op2].val.addr);
	} else {
		ir_emit_ref(ctx, f, insn->op2);
	}
	fprintf(f, ", []\n");
}

static void ir_emit_alloca(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "alloca i8, %s ", ir_type_llvm_name[ctx->ir_base[insn->op2].type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, ", align 16\n");
}

static void ir_emit_vaddr(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "bitcast ptr ");
	ir_emit_ref(ctx, f, insn->op1);
	fprintf(f, " to ptr\n");
}

static void ir_emit_load(ir_ctx *ctx, FILE *f, ir_ref def, ir_insn *insn)
{
	ir_emit_def_ref(ctx, f, def);
	fprintf(f, "load %s, ptr ", ir_type_llvm_name[insn->type]);
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, "\n");
}

static void ir_emit_store(ir_ctx *ctx, FILE *f, ir_insn *insn)
{
	ir_type type = ctx->ir_base[insn->op3].type;

	fprintf(f, "\tstore %s ", ir_type_llvm_name[type]);
	ir_emit_ref(ctx, f, insn->op3);
	fprintf(f, ", ptr ");
	ir_emit_ref(ctx, f, insn->op2);
	fprintf(f, "\n");
}

static bool may_be_used_by_phi(ir_ctx *ctx, ir_block *bb)
{
	if (bb->successors_count == 1) {
		bb = &ctx->cfg_blocks[ctx->cfg_edges[bb->successors]];
		if (bb->predecessors_count > 1) {
			ir_use_list *use_list = &ctx->use_lists[bb->start];
			ir_ref i, n, *p;

			n = use_list->count;
			if (n > 1) {
				for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
					if (ctx->ir_base[*p].op == IR_PHI) {
						return 1;
					}
				}
			}
		}
	}
	return 0;
}

static int ir_emit_func(ir_ctx *ctx, const char *name, FILE *f)
{
	ir_ref i, n, *p, use;
	ir_insn *insn;
	ir_use_list *use_list;
	bool first;
	uint32_t _b, b, target;
	ir_block *bb;
	ir_bitset_base_t used_intrinsics[IR_LLVM_INTRINSIC_BITSET_LEN];

	memset(used_intrinsics, 0, sizeof(used_intrinsics));

	/* Emit function prototype */
	fprintf(f, "define ");
	if (ctx->flags & IR_STATIC) {
		fprintf(f, "internal ");
	}
	if (ctx->flags & IR_FASTCALL_FUNC) {
		fprintf(f, "x86_fastcallcc ");
	}
	fprintf(f, "%s", ir_type_llvm_name[ctx->ret_type != (ir_type)-1 ? ctx->ret_type : IR_VOID]);
	fprintf(f, " @%s(", name);
	use_list = &ctx->use_lists[1];
	n = use_list->count;
	first = 1;
	for (i = 0, p = &ctx->use_edges[use_list->refs]; i < n; i++, p++) {
		use = *p;
		insn = &ctx->ir_base[use];
		if (insn->op == IR_PARAM) {
			if (first) {
				first = 0;
			} else {
				fprintf(f, ", ");
			}
			fprintf(f, "%s %%d%d", ir_type_llvm_name[insn->type], use);
		}
	}
	if (ctx->flags & IR_VARARG_FUNC) {
		if (first) {
			first = 0;
		} else {
			fprintf(f, ", ");
		}
		fprintf(f, "...");
	}
	fprintf(f, ")\n{\n");

	for (_b = 1; _b <= ctx->cfg_blocks_count; _b++) {
		if (ctx->cfg_schedule) {
			b = ctx->cfg_schedule[_b];
		} else {
			b = _b;
		}
		bb = &ctx->cfg_blocks[b];
		IR_ASSERT(!(bb->flags & IR_BB_UNREACHABLE));
		if ((bb->flags & (IR_BB_START|IR_BB_ENTRY|IR_BB_EMPTY)) == IR_BB_EMPTY) {
			continue;
		}
		if (bb->predecessors_count > 0 || may_be_used_by_phi(ctx, bb)) {
			fprintf(f, "l%d:\n", b);
		}
		for (i = bb->start, insn = ctx->ir_base + i; i <= bb->end;) {
			switch (insn->op) {
				case IR_START:
				case IR_BEGIN:
				case IR_IF_TRUE:
				case IR_IF_FALSE:
				case IR_CASE_VAL:
				case IR_CASE_DEFAULT:
				case IR_MERGE:
				case IR_LOOP_BEGIN:
				case IR_PARAM:
					/* skip */
					break;
				case IR_PHI:
					ir_emit_phi(ctx, f, i, insn, bb);
					break;
				case IR_VAR:
					use_list = &ctx->use_lists[i];
					if (use_list->count > 0) {
						fprintf(f, "\t%%d%d = alloca %s\n", i, ir_type_llvm_name[insn->type]);
					}
					break;
				case IR_EQ:
					ir_emit_binary_op(ctx, f, i, insn, "icmp eq", NULL, "fcmp oeq");
					break;
				case IR_NE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp ne", NULL, "fcmp une");
					break;
				case IR_LT:
					ir_emit_binary_op(ctx, f, i, insn, "icmp slt", "icmp ult", "fcmp olt");
					break;
				case IR_GE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp sge", "icmp uge", "fcmp oge");
					break;
				case IR_LE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp sle", "icmp ule", "fcmp ole");
					break;
				case IR_GT:
					ir_emit_binary_op(ctx, f, i, insn, "icmp sgt", "icmp ugt", "fcmp ogt");
					break;
				case IR_ULT:
					ir_emit_binary_op(ctx, f, i, insn, "icmp ult", NULL, "fcmp ult");
					break;
				case IR_UGE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp uge", NULL, "fcmp uge");
					break;
				case IR_ULE:
					ir_emit_binary_op(ctx, f, i, insn, "icmp ule", NULL, "fcmp ule");
					break;
				case IR_UGT:
					ir_emit_binary_op(ctx, f, i, insn, "icmp ugt", NULL, "fcmp ugt");
					break;
				case IR_ADD:
					ir_emit_binary_op(ctx, f, i, insn, "add", NULL, "fadd");
					break;
				case IR_SUB:
					ir_emit_binary_op(ctx, f, i, insn, "sub", NULL, "fsub");
					break;
				case IR_MUL:
					ir_emit_binary_op(ctx, f, i, insn, "mul", NULL, "fmul");
					break;
				case IR_DIV:
					ir_emit_binary_op(ctx, f, i, insn, "sdiv", "udiv", "fdiv");
					break;
				case IR_MOD:
					ir_emit_binary_op(ctx, f, i, insn, "srem", "urem", NULL);
					break;
				case IR_NEG:
					ir_emit_unary_neg(ctx, f, i, insn);
					break;
				case IR_NOT:
					ir_emit_unary_not(ctx, f, i, insn);
					break;
				case IR_OR:
					ir_emit_binary_op(ctx, f, i, insn, "or", NULL, NULL);
					break;
				case IR_AND:
					ir_emit_binary_op(ctx, f, i, insn, "and", NULL, NULL);
					break;
				case IR_XOR:
					ir_emit_binary_op(ctx, f, i, insn, "xor", NULL, NULL);
					break;
				case IR_MIN:
					ir_emit_minmax_op(ctx, f, i, insn, IR_LLVM_INTR_SMIN_I8, used_intrinsics);
					break;
				case IR_MAX:
					ir_emit_minmax_op(ctx, f, i, insn, IR_LLVM_INTR_SMAX_I8, used_intrinsics);
					break;
				case IR_COND:
					ir_emit_conditional_op(ctx, f, i, insn);
					break;
				case IR_ABS:
					ir_emit_abs(ctx, f, i, insn, used_intrinsics);
					break;
				case IR_SHL:
					ir_emit_binary_op(ctx, f, i, insn, "shl", NULL, NULL);
					break;
				case IR_SHR:
					ir_emit_binary_op(ctx, f, i, insn, "lshr", NULL, NULL);
					break;
				case IR_SAR:
					ir_emit_binary_op(ctx, f, i, insn, "ashr", NULL, NULL);
					break;
				case IR_ROL:
					ir_emit_rol_ror(ctx, f, i, insn, IR_LLVM_INTR_FSHL_I8, used_intrinsics);
					break;
				case IR_ROR:
					ir_emit_rol_ror(ctx, f, i, insn, IR_LLVM_INTR_FSHR_I8, used_intrinsics);
					break;
				case IR_BSWAP:
					ir_emit_bitop(ctx, f, i, insn, IR_LLVM_INTR_BSWAP_I16, 0, used_intrinsics);
					break;
				case IR_CTPOP:
					ir_emit_bitop(ctx, f, i, insn, IR_LLVM_INTR_CTPOP_I8, 0, used_intrinsics);
					break;
				case IR_CTLZ:
					ir_emit_bitop(ctx, f, i, insn, IR_LLVM_INTR_CTLZ_I8, 1, used_intrinsics);
					break;
				case IR_CTTZ:
					ir_emit_bitop(ctx, f, i, insn, IR_LLVM_INTR_CTTZ_I8, 1, used_intrinsics);
					break;
				case IR_SEXT:
					IR_ASSERT(IR_IS_TYPE_INT(insn->type));
					IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
					IR_ASSERT(ir_type_size[insn->type] > ir_type_size[ctx->ir_base[insn->op1].type]);
					ir_emit_conv(ctx, f, i, insn, "sext");
					break;
				case IR_ZEXT:
					IR_ASSERT(IR_IS_TYPE_INT(insn->type));
					IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
					IR_ASSERT(ir_type_size[insn->type] > ir_type_size[ctx->ir_base[insn->op1].type]);
					ir_emit_conv(ctx, f, i, insn, "zext");
					break;
				case IR_TRUNC:
					IR_ASSERT(IR_IS_TYPE_INT(insn->type));
					IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
					IR_ASSERT(ir_type_size[insn->type] < ir_type_size[ctx->ir_base[insn->op1].type]);
					ir_emit_conv(ctx, f, i, insn, "trunc");
					break;
				case IR_BITCAST:
					if (insn->type == IR_ADDR
					 && IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type)
					 && ctx->ir_base[insn->op1].type != IR_ADDR) {
						ir_emit_conv(ctx, f, i, insn, "inttoptr");
					} else if (ctx->ir_base[insn->op1].type == IR_ADDR
					 && IR_IS_TYPE_INT(insn->type)
					 && insn->type != IR_ADDR) {
						ir_emit_conv(ctx, f, i, insn, "ptrtoint");
					} else {
						ir_emit_conv(ctx, f, i, insn, "bitcast");
					}
					break;
				case IR_PROTO:
					ir_emit_conv(ctx, f, i, insn, "bitcast");
					break;
				case IR_INT2FP:
					IR_ASSERT(IR_IS_TYPE_FP(insn->type));
					IR_ASSERT(IR_IS_TYPE_INT(ctx->ir_base[insn->op1].type));
					ir_emit_conv(ctx, f, i, insn, IR_IS_TYPE_UNSIGNED(ctx->ir_base[insn->op1].type) ? "uitofp" : "sitofp");
					break;
				case IR_FP2INT:
					IR_ASSERT(IR_IS_TYPE_INT(insn->type));
					IR_ASSERT(IR_IS_TYPE_FP(ctx->ir_base[insn->op1].type));
					ir_emit_conv(ctx, f, i, insn, IR_IS_TYPE_UNSIGNED(insn->type) ? "fptoui" : "fptosi");
					break;
				case IR_FP2FP:
					IR_ASSERT(IR_IS_TYPE_FP(insn->type));
					IR_ASSERT(IR_IS_TYPE_FP(ctx->ir_base[insn->op1].type));
					ir_emit_conv(ctx, f, i, insn, insn->type == IR_DOUBLE ? "fpext" : "fptrunc");
					break;
				case IR_COPY:
					ir_emit_conv(ctx, f, i, insn, "bitcast");
					break;
				case IR_ADD_OV:
					ir_emit_binary_overflow_op(ctx, f, i, insn, IR_LLVM_INTR_SADD_OV_I8, used_intrinsics);
					break;
				case IR_SUB_OV:
					ir_emit_binary_overflow_op(ctx, f, i, insn, IR_LLVM_INTR_SSUB_OV_I8, used_intrinsics);
					break;
				case IR_MUL_OV:
					ir_emit_binary_overflow_op(ctx, f, i, insn, IR_LLVM_INTR_SMUL_OV_I8, used_intrinsics);
					break;
				case IR_OVERFLOW:
					ir_emit_def_ref(ctx, f, i);
					fprintf(f, "extractvalue {%s, i1} %%t%d, 1\n", ir_type_llvm_name[ctx->ir_base[insn->op1].type], insn->op1);
					break;
				case IR_RETURN:
					IR_ASSERT(bb->successors_count == 0);
					if (!insn->op2) {
						fprintf(f, "\tret void\n");
					} else {
						fprintf(f, "\tret %s ", ir_type_llvm_name[ctx->ir_base[insn->op2].type]);
						ir_emit_ref(ctx, f, insn->op2);
						fprintf(f, "\n");
					}
					break;
				case IR_END:
				case IR_LOOP_END:
					IR_ASSERT(bb->successors_count == 1);
					target = ir_skip_empty_target_blocks(ctx, ctx->cfg_edges[bb->successors]);
					fprintf(f, "\tbr label %%l%d\n", target);
					break;
				case IR_IF:
					ir_emit_if(ctx, f, b, i, insn);
					break;
				case IR_SWITCH:
					ir_emit_switch(ctx, f, b, i, insn);
					break;
				case IR_CALL:
				case IR_TAILCALL:
					ir_emit_call(ctx, f, i, insn, used_intrinsics);
					if (insn->op == IR_TAILCALL) {
						i = bb->end + 1;
						continue;
					}
					break;
				case IR_IJMP:
					ir_emit_ijmp(ctx, f, insn);
					break;
				case IR_ALLOCA:
					ir_emit_alloca(ctx, f, i, insn);
					break;
				case IR_VADDR:
					ir_emit_vaddr(ctx, f, i, insn);
					break;
				case IR_LOAD:
				case IR_VLOAD:
					ir_emit_load(ctx, f, i, insn);
					break;
				case IR_STORE:
				case IR_VSTORE:
					ir_emit_store(ctx, f, insn);
					break;
				case IR_FRAME_ADDR:
					ir_emit_def_ref(ctx, f, i);
					ir_bitset_incl(used_intrinsics, IR_LLVM_INTR_FRAMEADDRESS);
					fprintf(f, "\tcall ptr @llvm.frameaddress.p0(i32 0)\n");
					break;
				case IR_VA_START:
					ir_bitset_incl(used_intrinsics, IR_LLVM_INTR_VA_START);
					fprintf(f, "\tcall void @llvm.va_start(ptr ");
					ir_emit_ref(ctx, f, insn->op2);
					fprintf(f, ")\n");
					break;
				case IR_VA_END:
					ir_bitset_incl(used_intrinsics, IR_LLVM_INTR_VA_END);
					fprintf(f, "\tcall void @llvm.va_end(ptr ");
					ir_emit_ref(ctx, f, insn->op2);
					fprintf(f, ")\n");
					break;
				case IR_VA_COPY:
					ir_bitset_incl(used_intrinsics, IR_LLVM_INTR_VA_COPY);
					fprintf(f, "\tcall void @llvm.va_copy(ptr");
					ir_emit_ref(ctx, f, insn->op2);
					fprintf(f, ", ptr ");
					ir_emit_ref(ctx, f, insn->op3);
					fprintf(f, ")\n");
					break;
				case IR_VA_ARG:
					ir_emit_def_ref(ctx, f, i);
					fprintf(f, "va_arg ptr ");
					ir_emit_ref(ctx, f, insn->op2);
					fprintf(f, ", %s\n", ir_type_cname[insn->type]);
					break;
				case IR_TRAP:
					ir_bitset_incl(used_intrinsics, IR_LLVM_INTR_DEBUGTRAP);
					fprintf(f, "\tcall void @llvm.debugtrap()\n");
					break;
				case IR_GUARD:
				case IR_GUARD_NOT:
					ir_emit_guard(ctx, f, i, insn);
					break;
				case IR_UNREACHABLE:
					fprintf(f, "\tunreachable\n");
					break;
				case IR_RLOAD:
				case IR_RSTORE:
				case IR_TLS:
				default:
					IR_ASSERT(0 && "NIY instruction");
					ctx->status = IR_ERROR_UNSUPPORTED_CODE_RULE;
					return 0;
			}
			n = ir_insn_len(insn);
			i += n;
			insn += n;
		}
	}

	fprintf(f, "}\n");

	IR_BITSET_FOREACH(used_intrinsics, IR_LLVM_INTRINSIC_BITSET_LEN, i) {
		const char *name = ir_llvm_intrinsic_desc[i].name;
		if (!ctx->loader || !ctx->loader->has_sym || !ctx->loader->has_sym(ctx->loader, name)) {
			ir_emit_llvm_func_decl(name, 0, ir_llvm_intrinsic_desc[i].ret_type,
				ir_llvm_intrinsic_desc[i].params_count, ir_llvm_intrinsic_desc[i].param_types, f);
			if (ctx->loader->add_sym) {
				ctx->loader->add_sym(ctx->loader, name, NULL);
			}
		}
	} IR_BITSET_FOREACH_END();

	for (i = IR_UNUSED + 1, insn = ctx->ir_base - i; i < ctx->consts_count; i++, insn--) {
		if (insn->op == IR_FUNC) {
			const char *name;

			name = ir_get_str(ctx, insn->val.name);
			if (insn->proto) {
				const ir_proto_t *proto = (const ir_proto_t *)ir_get_str(ctx, insn->proto);

				if (proto->flags & IR_BUILTIN_FUNC) {
					n = ir_builtin_func(name);
					if (n >= 0) {
						ir_llvm_intrinsic_id id = ir_llvm_builtin_map[n].id;
						if (id == IR_LLVM_INTR_ISNAN_F64
						 || id == IR_LLVM_INTR_ISNAN_F32) {
							continue;
						}
						name = ir_llvm_intrinsic_desc[id].name;
					}
				}
				if (!ctx->loader || !ctx->loader->has_sym || !ctx->loader->has_sym(ctx->loader, name)) {
					ir_emit_llvm_func_decl(name, proto->flags, proto->ret_type, proto->params_count, proto->param_types, f);
					if (ctx->loader->add_sym) {
						ctx->loader->add_sym(ctx->loader, name, NULL);
					}
				}
			} else {
				fprintf(f, "declare void @%s()\n", name);
			}
		} else if (insn->op == IR_SYM) {
			const char *name;

			name = ir_get_str(ctx, insn->val.name);
			if (!ctx->loader || !ctx->loader->has_sym || !ctx->loader->has_sym(ctx->loader, name)) {
				// TODO: symbol "global" or "constant" ???
				// TODO: symbol type ???
				fprintf(f, "@%s = external global ptr\n", name);
				if (ctx->loader->add_sym) {
					ctx->loader->add_sym(ctx->loader, name, NULL);
				}
			}
		} else if (insn->op == IR_STR) {
			const char *str = ir_get_str(ctx, insn->val.str);
			// TODO: strlen != size ???
			int len = strlen(str);
			int j;

			fprintf(f, "@.str%d = private unnamed_addr constant [%d x i8] c\"", i, len + 1);
			for (j = 0; j < len; j++) {
				char c = str[j];

				if (c < ' ' || c >= 127) {
					char c1 = c >> 8;
					char c2 = c & 15;
					c1 = (c1 < 10) ? (c1 + '0') : (c1 - 10 + 'A');
					c2 = (c2 < 10) ? (c2 + '0') : (c2 - 10 + 'A');
					fprintf(f, "\\%c%c", c1, c2);
				} else {
					fprintf(f, "%c", c);
				}
			}
			fprintf(f, "\\00\"\n");
		}
	}

	return 1;
}

int ir_emit_llvm(ir_ctx *ctx, const char *name, FILE *f)
{
	return ir_emit_func(ctx, name, f);
}

void ir_emit_llvm_func_decl(const char *name, uint32_t flags, ir_type ret_type, uint32_t params_count, const uint8_t *param_types, FILE *f)
{
	fprintf(f, "declare %s%s @%s(",
		(flags & IR_FASTCALL_FUNC) ? "x86_fastcallcc ": "",
		ir_type_llvm_name[ret_type], name);
	if (params_count) {
		const uint8_t *p = param_types;

		fprintf(f, "%s", ir_type_llvm_name[*p]);
		p++;
		while (--params_count) {
			fprintf(f, ", %s", ir_type_llvm_name[*p]);
			p++;
		}
		if (flags & IR_VARARG_FUNC) {
			fprintf(f, ", ...");
		}
	} else if (flags & IR_VARARG_FUNC) {
		fprintf(f, "...");
	}
	fprintf(f, ")\n");
}

void ir_emit_llvm_sym_decl(const char *name, uint32_t flags, bool has_data, FILE *f)
{
	fprintf(f, "@%s = ", name);
	if (flags & IR_EXTERN) {
		fprintf(f, "external ");
	} else if (flags & IR_STATIC) {
		fprintf(f, "private ");
	}
	if (flags & IR_CONST) {
		fprintf(f, "constant ");
	} else {
		fprintf(f, "global ");
	}
	// TODO: type
	fprintf(f, "ptr");
	if (!(flags & IR_EXTERN)) {
		// TODO: init
		fprintf(f, " zeroinitializer");
	}
	fprintf(f, "\n");
}
