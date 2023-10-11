/*
 * IR - Lightweight JIT Compilation Framework
 * (LLVM loader)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_builder.h"
#include "ir_private.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/IRReader.h>

#define IR_BAD_TYPE IR_LAST_TYPE

static ir_ref llvm2ir_const_expr(ir_ctx *ctx, LLVMValueRef expr);
static ir_ref llvm2ir_auto_cast(ir_ctx *ctx, ir_ref ref, ir_type src_type, ir_type type);

static ir_type llvm2ir_type(LLVMTypeRef type)
{
	char *str;

	switch (LLVMGetTypeKind(type)) {
		case LLVMVoidTypeKind:
			return IR_VOID;
		case LLVMIntegerTypeKind:
			switch (LLVMGetIntTypeWidth(type)) {
				case 1:  return IR_BOOL;
				case 8:  return IR_I8;
				case 16: return IR_I16;
				case 32: return IR_I32;
				case 64: return IR_I64;
				default:
					break;
			}
			break;
		case LLVMFloatTypeKind:
			return IR_FLOAT;
		case LLVMDoubleTypeKind:
			return IR_DOUBLE;
		case LLVMPointerTypeKind:
		case LLVMFunctionTypeKind:
		case LLVMLabelTypeKind:
			return IR_ADDR;
		case LLVMVectorTypeKind:
			IR_ASSERT(0 && "NIY LLVMVectorTypeKind use -fno-vectorize -fno-slp-vectorize");
		default:
			break;
	}

	str = LLVMPrintTypeToString(type);
	fprintf(stderr, "Unsupported LLVM type: %s\n", str);
	IR_ASSERT(0);
	LLVMDisposeMessage(str);
	return IR_BAD_TYPE;
}

static ir_type llvm2ir_unsigned_type(ir_type type)
{
	if (IR_IS_TYPE_SIGNED(type)) {
		IR_ASSERT(type >= IR_I8 && type <= IR_I64);
		type = type - (IR_I8 - IR_U8);
	}
	return type;
}

static ir_type llvm2ir_signed_type(ir_type type)
{
	if (!IR_IS_TYPE_SIGNED(type)) {
		IR_ASSERT(type >= IR_U8 && type <= IR_U64);
		type = type + (IR_I8 - IR_U8);
	}
	return type;
}

static ir_ref llvm2ir_op(ir_ctx *ctx, LLVMValueRef op, ir_type type)
{
	ir_ref ref;
	LLVMBool lose;
	const char *name;
	size_t name_len;
	uint32_t cconv, flags;
	ir_val val;

	switch (LLVMGetValueKind(op)) {
		case LLVMConstantIntValueKind:
			IR_ASSERT(IR_IS_TYPE_INT(type));
			if (IR_IS_TYPE_SIGNED(type)) {
				val.i64 = LLVMConstIntGetSExtValue(op);
			} else {
				val.i64 = LLVMConstIntGetZExtValue(op);
			}
			return ir_const(ctx, val, type);
		case LLVMConstantFPValueKind:
			if (type == IR_DOUBLE) {
				val.d = LLVMConstRealGetDouble(op, &lose);
			} else {
				val.f = (float)LLVMConstRealGetDouble(op, &lose);
			}
			return ir_const(ctx, val, type);
		case LLVMConstantPointerNullValueKind:
			return IR_NULL;
		case LLVMConstantExprValueKind:
			ref = llvm2ir_const_expr(ctx, op);
			if (ctx->ir_base[ref].type != type) {
				ref = llvm2ir_auto_cast(ctx, ref, ctx->ir_base[ref].type, type);
			}
			return ref;
		case LLVMArgumentValueKind:
		case LLVMInstructionValueKind:
			ref = ir_addrtab_find(ctx->binding, (uintptr_t)op);
			IR_ASSERT(ref != (ir_ref)IR_INVALID_VAL);
			if (ctx->ir_base[ref].type != type) {
				ref = llvm2ir_auto_cast(ctx, ref, ctx->ir_base[ref].type, type);
			}
			return ref;
		case LLVMGlobalVariableValueKind:
			// TODO: resolve variable address
			return IR_NULL;
		case LLVMFunctionValueKind:
			// TODO: function prototype
			// TODO: resolve function address
			flags = 0;
			cconv = LLVMGetFunctionCallConv(op);
			if (cconv == LLVMCCallConv || cconv == LLVMFastCallConv) {
				/* skip */
			} else if (cconv == LLVMX86FastcallCallConv) {
				flags |= IR_CONST_FASTCALL_FUNC;
			} else {
				fprintf(stderr, "Unsupported Calling Convention: %d\n", cconv);
				IR_ASSERT(0);
				return 0;
			}
			if (LLVMIsFunctionVarArg(LLVMTypeOf(op))) {
				flags |= IR_CONST_VARARG_FUNC;
			}
			name = LLVMGetValueName2(op, &name_len);
			return ir_const_func(ctx, ir_strl(ctx, name, name_len), flags);
		case LLVMUndefValueValueKind:
			val.u64 = 0;
			return ir_const(ctx, val, type);
		default:
			IR_ASSERT(0);
			return 0;
	}

	return 0;
}

static void llvm2ir_ret(ir_ctx *ctx, LLVMValueRef insn)
{
	ir_ref ref;

	if (LLVMGetNumOperands(insn) == 0) {
		ref = IR_UNUSED;
	} else {
		LLVMValueRef op0 = LLVMGetOperand(insn, 0);
		ir_type type = llvm2ir_type(LLVMTypeOf(op0));

		ref = llvm2ir_op(ctx, op0, type);
	}
	ir_RETURN(ref);
}

static ir_ref llvm2ir_jmp(ir_ctx *ctx, LLVMValueRef insn)
{
	ir_ref ref;

	ref = ir_END(); /* END may be converted to LOOP_END later */
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
	return ref;
}

static ir_ref llvm2ir_if(ir_ctx *ctx, LLVMValueRef insn)
{
	ir_ref ref;
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	ir_type type = llvm2ir_type(LLVMTypeOf(op0));

	ref = ir_IF(llvm2ir_op(ctx, op0, type));
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
	return ref;
}

static ir_ref llvm2ir_switch(ir_ctx *ctx, LLVMValueRef insn)
{
	ir_ref ref;
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	ir_type type = llvm2ir_type(LLVMTypeOf(op0));

	ref = ir_SWITCH(llvm2ir_op(ctx, op0, type));
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
	return ref;
}

static ir_ref llvm2ir_unary_op(ir_ctx *ctx, LLVMValueRef expr, ir_op op)
{
	LLVMValueRef op0 = LLVMGetOperand(expr, 0);
	ir_type type = llvm2ir_type(LLVMTypeOf(expr));
	ir_ref ref;

	ref = ir_fold1(ctx, IR_OPT(op, type), llvm2ir_op(ctx, op0, type));
	ir_addrtab_add(ctx->binding, (uintptr_t)expr, ref);
	return ref;
}

static ir_ref llvm2ir_binary_expr(ir_ctx *ctx, ir_op op, ir_type type, LLVMValueRef expr)
{
	return ir_fold2(ctx, IR_OPT(op, type),
		llvm2ir_op(ctx, LLVMGetOperand(expr, 0), type),
		llvm2ir_op(ctx, LLVMGetOperand(expr, 1), type));
}

static ir_ref llvm2ir_binary_op(ir_ctx *ctx, LLVMOpcode opcode, LLVMValueRef expr, ir_op op)
{
	ir_type type = llvm2ir_type(LLVMTypeOf(expr));
	ir_ref ref;

	if (opcode == LLVMUDiv || opcode == LLVMURem) {
		type = llvm2ir_unsigned_type(type);
	} else if (opcode == LLVMSDiv || opcode == LLVMSRem) {
		type = llvm2ir_signed_type(type);
	}
	ref = llvm2ir_binary_expr(ctx, op, type, expr);
	ir_addrtab_add(ctx->binding, (uintptr_t)expr, ref);
	return ref;
}

static ir_ref llvm2ir_cast_op(ir_ctx *ctx, LLVMValueRef expr, ir_op op)
{
	LLVMValueRef op0 = LLVMGetOperand(expr, 0);
	ir_type type;
	ir_ref ref;

	type = llvm2ir_type(LLVMTypeOf(op0));
	ref = llvm2ir_op(ctx, op0, type);
	type = llvm2ir_type(LLVMTypeOf(expr));
	ref = ir_fold1(ctx, IR_OPT(op, type), ref);
	ir_addrtab_add(ctx->binding, (uintptr_t)expr, ref);
	return ref;
}

static ir_ref llvm2ir_icmp_op(ir_ctx *ctx, LLVMValueRef expr)
{
	LLVMValueRef op0 = LLVMGetOperand(expr, 0);
	LLVMValueRef op1 = LLVMGetOperand(expr, 1);
	ir_type type = llvm2ir_type(LLVMTypeOf(op0));
	ir_ref ref;
	ir_op op;

	switch (LLVMGetICmpPredicate(expr)) {
		case LLVMIntEQ:  op = IR_EQ;  break;
		case LLVMIntNE:  op = IR_NE;  break;
		case LLVMIntUGT: op = IR_UGT; break;
		case LLVMIntUGE: op = IR_UGE; break;
		case LLVMIntULT: op = IR_ULT; break;
		case LLVMIntULE: op = IR_ULE; break;
		case LLVMIntSGT: op = IR_GT;  break;
		case LLVMIntSGE: op = IR_GE;  break;
		case LLVMIntSLT: op = IR_LT;  break;
		case LLVMIntSLE: op = IR_LE;  break;
		default: IR_ASSERT(0); return 0;
	}
	ref = ir_fold2(ctx, IR_OPT(op, IR_BOOL), llvm2ir_op(ctx, op0, type), llvm2ir_op(ctx, op1, type));
	ir_addrtab_add(ctx->binding, (uintptr_t)expr, ref);
	return ref;
}

static ir_ref llvm2ir_fcmp_op(ir_ctx *ctx, LLVMValueRef expr)
{
	LLVMValueRef op0 = LLVMGetOperand(expr, 0);
	LLVMValueRef op1 = LLVMGetOperand(expr, 1);
	ir_type type = llvm2ir_type(LLVMTypeOf(op0));
	ir_ref ref;
	ir_op op;

	switch (LLVMGetFCmpPredicate(expr)) {
		case LLVMRealOEQ:
		case LLVMRealUEQ: op = IR_EQ;  break;
		case LLVMRealONE:
		case LLVMRealUNE: op = IR_NE;  break;
		case LLVMRealUGT: op = IR_UGT; break;
		case LLVMRealUGE: op = IR_UGE; break;
		case LLVMRealULT: op = IR_ULT; break;
		case LLVMRealULE: op = IR_ULE; break;
		case LLVMRealOGT: op = IR_GT;  break;
		case LLVMRealOGE: op = IR_GE;  break;
		case LLVMRealOLT: op = IR_LT;  break;
		case LLVMRealOLE: op = IR_LE;  break;
		case LLVMRealUNO: op = IR_NE;  break; // TODO: isnan() upport. IR_NE is invalid ???
		default: IR_ASSERT(0); return 0;
	}
	ref = ir_fold2(ctx, IR_OPT(op, IR_BOOL), llvm2ir_op(ctx, op0, type), llvm2ir_op(ctx, op1, type));
	ir_addrtab_add(ctx->binding, (uintptr_t)expr, ref);
	return ref;
}

static ir_ref llvm2ir_cond_op(ir_ctx *ctx, LLVMValueRef expr)
{
	LLVMValueRef op0 = LLVMGetOperand(expr, 0);
	LLVMValueRef op1 = LLVMGetOperand(expr, 1);
	LLVMValueRef op2 = LLVMGetOperand(expr, 2);
	ir_type type = llvm2ir_type(LLVMTypeOf(expr));
	ir_ref ref;

	ref = ir_fold3(ctx, IR_OPT(IR_COND, type), llvm2ir_op(ctx, op0, IR_BOOL), llvm2ir_op(ctx, op1, type), llvm2ir_op(ctx, op2, type));
	ir_addrtab_add(ctx->binding, (uintptr_t)expr, ref);
	return ref;
}

static void llvm2ir_alloca(ir_ctx *ctx, LLVMValueRef insn)
{
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	ir_ref ref;

	if (LLVMGetValueKind(op0) == LLVMConstantIntValueKind && LLVMConstIntGetZExtValue(op0) == 1) {
		LLVMTypeKind type_kind = LLVMGetTypeKind(LLVMGetAllocatedType(insn));

		if (type_kind == LLVMIntegerTypeKind
		 || type_kind == LLVMFloatTypeKind
		 || type_kind == LLVMDoubleTypeKind
		 || type_kind == LLVMPointerTypeKind
		 || type_kind == LLVMFunctionTypeKind
		 || type_kind == LLVMLabelTypeKind) {
			ir_type type = llvm2ir_type(LLVMGetAllocatedType(insn));

			ref = ir_VAR(type, ""); // TODO: unique name
		} else {
			ref = ir_ALLOCA(ir_MUL_A(llvm2ir_op(ctx, op0, IR_ADDR),
				ir_const_addr(ctx, LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, LLVMGetAllocatedType(insn)))));
		}
	} else {
		ref = ir_ALLOCA(ir_MUL_A(llvm2ir_op(ctx, op0, IR_ADDR),
			ir_const_addr(ctx, LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, LLVMGetAllocatedType(insn)))));
	}
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
}

static void llvm2ir_load(ir_ctx *ctx, LLVMValueRef insn)
{
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	ir_type type = llvm2ir_type(LLVMTypeOf(insn));
	ir_ref ref;

	ref = llvm2ir_op(ctx, op0, IR_ADDR);
	if (ctx->ir_base[ref].op == IR_VAR) {
		ref = ir_VLOAD(type, ref);
	} else {
		ref = ir_LOAD(type, ref);
	}
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
}

static void llvm2ir_store(ir_ctx *ctx, LLVMValueRef insn)
{
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	LLVMValueRef op1 = LLVMGetOperand(insn, 1);
	ir_type type = llvm2ir_type(LLVMTypeOf(op0));
	ir_ref ref, val;

	ref = llvm2ir_op(ctx, op1, IR_ADDR);
	val = llvm2ir_op(ctx, op0, type);
	if (ctx->ir_base[ref].op == IR_VAR) {
		ir_VSTORE(ref, val);
	} else {
		ir_STORE(ref, val);
	}
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ctx->control);
}

static ir_type llvm2ir_overflow_type(LLVMTypeRef stype)
{
	ir_type type;

	IR_ASSERT(LLVMGetTypeKind(stype) == LLVMStructTypeKind);
	IR_ASSERT(LLVMCountStructElementTypes(stype) == 2);
	IR_ASSERT(llvm2ir_type(LLVMStructGetTypeAtIndex(stype, 1)) == IR_BOOL);
	stype = LLVMStructGetTypeAtIndex(stype, 0);
	type = llvm2ir_type(stype);
	IR_ASSERT(IR_IS_TYPE_INT(type));
	return type;
}

#define STR_START(name, name_len, str) (name_len >= strlen(str) && memcmp(name, str, strlen(str)) == 0)
#define STR_EQUAL(name, name_len, str) (name_len == strlen(str) && memcmp(name, str, strlen(str)) == 0)

static ir_ref llvm2ir_intrinsic(ir_ctx *ctx, LLVMValueRef insn, LLVMTypeRef ftype, uint32_t count, const char *name, size_t name_len)
{
	ir_type type;

	if (STR_START(name, name_len, "llvm.lifetime.")) {
		/* skip */
	} else if (STR_START(name, name_len, "llvm.dbg.")) {
		/* skip */
	} else if (STR_EQUAL(name, name_len, "llvm.assume")) {
		/* skip */
	} else if (STR_START(name, name_len, "llvm.smax.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		type = llvm2ir_signed_type(type);
		return llvm2ir_binary_expr(ctx, IR_MAX, type, insn);
	} else if (STR_START(name, name_len, "llvm.umax.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		type = llvm2ir_unsigned_type(type);
		return llvm2ir_binary_expr(ctx, IR_MAX, type, insn);
	} else if (STR_START(name, name_len, "llvm.maxnum.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_FP(type));
		return llvm2ir_binary_expr(ctx, IR_MAX, type, insn);
	} else if (STR_START(name, name_len, "llvm.smin.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		type = llvm2ir_signed_type(type);
		return llvm2ir_binary_expr(ctx, IR_MIN, type, insn);
	} else if (STR_START(name, name_len, "llvm.umin.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		type = llvm2ir_unsigned_type(type);
		return llvm2ir_binary_expr(ctx, IR_MIN, type, insn);
	} else if (STR_START(name, name_len, "llvm.minnum.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_FP(type));
		return llvm2ir_binary_expr(ctx, IR_MIN, type, insn);
	} else if (STR_START(name, name_len, "llvm.sadd.with.overflow.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_overflow_type(LLVMGetReturnType(ftype));
		type = llvm2ir_signed_type(type);
		return llvm2ir_binary_expr(ctx, IR_ADD_OV, type, insn);
	} else if (STR_START(name, name_len, "llvm.uadd.with.overflow.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_overflow_type(LLVMGetReturnType(ftype));
		type = llvm2ir_unsigned_type(type);
		return llvm2ir_binary_expr(ctx, IR_ADD_OV, type, insn);
	} else if (STR_START(name, name_len, "llvm.ssub.with.overflow.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_overflow_type(LLVMGetReturnType(ftype));
		type = llvm2ir_signed_type(type);
		return llvm2ir_binary_expr(ctx, IR_SUB_OV, type, insn);
	} else if (STR_START(name, name_len, "llvm.usub.with.overflow.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_overflow_type(LLVMGetReturnType(ftype));
		type = llvm2ir_unsigned_type(type);
		return llvm2ir_binary_expr(ctx, IR_SUB_OV, type, insn);
	} else if (STR_START(name, name_len, "llvm.smul.with.overflow.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_overflow_type(LLVMGetReturnType(ftype));
		type = llvm2ir_signed_type(type);
		return llvm2ir_binary_expr(ctx, IR_MUL_OV, type, insn);
	} else if (STR_START(name, name_len, "llvm.umul.with.overflow.")) {
		IR_ASSERT(count == 2);
		type = llvm2ir_overflow_type(LLVMGetReturnType(ftype));
		type = llvm2ir_unsigned_type(type);
		return llvm2ir_binary_expr(ctx, IR_MUL_OV, type, insn);
	} else if (STR_START(name, name_len, "llvm.sadd.sat.")) {
		ir_ref ref, overflow;
		ir_val val;

		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		type = llvm2ir_signed_type(type);
		ref = llvm2ir_binary_expr(ctx, IR_ADD_OV, type, insn);
		overflow = ir_OVERFLOW(ref);
		// TODO: support for overflow in case of two negative values ???
		switch (ir_type_size[type]) {
			case 1: val.u64 = 0x7f; break;
			case 2: val.u64 = 0x7fff; break;
			case 4: val.u64 = 0x7fffffff; break;
			case 8: val.u64 = 0x7fffffffffffffff; break;
			default: IR_ASSERT(0);
		}
		return ir_COND(type, overflow, ir_const(ctx, val, type), ref);
	} else if (STR_START(name, name_len, "llvm.uadd.sat.")) {
		ir_ref ref, overflow;
		ir_val val;

		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		type = llvm2ir_unsigned_type(type);
		ref = llvm2ir_binary_expr(ctx, IR_ADD_OV, type, insn);
		overflow = ir_OVERFLOW(ref);
		switch (ir_type_size[type]) {
			case 1: val.u64 = 0xff; break;
			case 2: val.u64 = 0xffff; break;
			case 4: val.u64 = 0xffffffff; break;
			case 8: val.u64 = 0xffffffffffffffff; break;
			default: IR_ASSERT(0);
		}
		return ir_COND(type, overflow, ir_const(ctx, val, type), ref);
	} else if (STR_START(name, name_len, "llvm.ssub.sat.")) {
		ir_ref ref, overflow;
		ir_val val;

		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		type = llvm2ir_signed_type(type);
		ref = llvm2ir_binary_expr(ctx, IR_SUB_OV, type, insn);
		overflow = ir_OVERFLOW(ref);
		// TODO: support for overflow in case of two negative values ???
		switch (ir_type_size[type]) {
			case 1: val.u64 = 0x80; break;
			case 2: val.u64 = 0x8000; break;
			case 4: val.u64 = 0x80000000; break;
			case 8: val.u64 = 0x8000000000000000; break;
			default: IR_ASSERT(0);
		}
		return ir_COND(type, overflow, ir_const(ctx, val, type), ref);
	} else if (STR_START(name, name_len, "llvm.usub.sat.")) {
		ir_ref ref, overflow;
		ir_val val;

		IR_ASSERT(count == 2);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		type = llvm2ir_unsigned_type(type);
		ref = llvm2ir_binary_expr(ctx, IR_SUB_OV, type, insn);
		overflow = ir_OVERFLOW(ref);
		val.u64 = 0;
		return ir_COND(type, overflow, ir_const(ctx, val, type), ref);
	} else if (STR_START(name, name_len, "llvm.abs.")) {
		IR_ASSERT(count == 1);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		return ir_ABS(type, llvm2ir_op(ctx, LLVMGetOperand(insn, 0), type));
	} else if (STR_START(name, name_len, "llvm.fabs.")) {
		IR_ASSERT(count == 1);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_FP(type));
		return ir_ABS(type, llvm2ir_op(ctx, LLVMGetOperand(insn, 0), type));
	} else if (STR_START(name, name_len, "llvm.bswap.")) {
		IR_ASSERT(count == 1);
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		return ir_BSWAP(type, llvm2ir_op(ctx, LLVMGetOperand(insn, 0), type));
	} else if (STR_START(name, name_len, "llvm.fshl.")
			&& count == 3
			&& LLVMGetOperand(insn, 0) == LLVMGetOperand(insn, 1)) {
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		return ir_ROL(type,
			llvm2ir_op(ctx, LLVMGetOperand(insn, 0), type),
			llvm2ir_op(ctx, LLVMGetOperand(insn, 2), type));
	} else if (STR_START(name, name_len, "llvm.fshr.")
			&& count == 3
			&& LLVMGetOperand(insn, 0) == LLVMGetOperand(insn, 1)) {
		type = llvm2ir_type(LLVMGetReturnType(ftype));
		IR_ASSERT(IR_IS_TYPE_INT(type));
		return ir_ROR(type,
			llvm2ir_op(ctx, LLVMGetOperand(insn, 0), type),
			llvm2ir_op(ctx, LLVMGetOperand(insn, 2), type));
	} else if (STR_START(name, name_len, "llvm.ctpop.")) {
		// TODO:
	} else if (STR_START(name, name_len, "llvm.memset.")) {
		// TODO:
	} else if (STR_START(name, name_len, "llvm.memcpy.")) {
		// TODO:
	} else if (STR_START(name, name_len, "llvm.memmove.")) {
		// TODO:
	} else if (STR_START(name, name_len, "llvm.frameaddress.")) {
		// TODO:
	} else if (STR_EQUAL(name, name_len, "llvm.debugtrap")) {
		ir_TRAP();
		return ctx->control;
	} else {
		fprintf(stderr, "Unsupported LLVM intrinsic: %s\n", name);
		IR_ASSERT(0);
	}
	return IR_UNUSED;
}

static void llvm2ir_call(ir_ctx *ctx, LLVMValueRef insn)
{
	LLVMValueRef arg, func = LLVMGetCalledValue(insn);
	LLVMTypeRef ftype = LLVMGetCalledFunctionType(insn);
//	uint32_t cconv = LLVMGetInstructionCallConv(insn);
//	LLVMBool tail = LLVMIsTailCall(insn);
	ir_type type;
	uint32_t i, count = LLVMGetNumArgOperands(insn);
	ir_ref ref;
	ir_ref *args = alloca(sizeof(ref) * count);

	if (LLVMGetValueKind(func) == LLVMFunctionValueKind) {
		size_t name_len;
		const char *name;

		name = LLVMGetValueName2(func, &name_len);
		if (STR_START(name, name_len, "llvm.")) {
			ref = llvm2ir_intrinsic(ctx, insn, ftype, count, name, name_len);
			if (ref) {
				ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
				return;
			}
		}
	}

	args = alloca(sizeof(ref) * count);
	for (i = 0; i < count; i++) {
        arg = LLVMGetOperand(insn, i);
        type = llvm2ir_type(LLVMTypeOf(arg));
        args[i] = llvm2ir_op(ctx, arg, type);
	}
	ref = ir_CALL_N(llvm2ir_type(LLVMGetReturnType(ftype)), llvm2ir_op(ctx, func, IR_ADDR), count, args);
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
}

static bool llvm2ir_extract(ir_ctx *ctx, LLVMValueRef expr)
{
	LLVMValueRef op0;
	ir_ref ref;
	uint32_t n;

	IR_ASSERT(LLVMGetNumOperands(expr) == 1 && LLVMGetNumIndices(expr) == 1);
	op0 = LLVMGetOperand(expr, 0);
	ref = ir_addrtab_find(ctx->binding, (uintptr_t)op0);
	IR_ASSERT(ref != (ir_ref)IR_INVALID_VAL);
	if (ctx->ir_base[ref].op != IR_ADD_OV
	 && ctx->ir_base[ref].op != IR_SUB_OV
	 && ctx->ir_base[ref].op != IR_MUL_OV
	 && ctx->ir_base[ref].op != IR_PHI) {
		return 0;
	}
	n = *LLVMGetIndices(expr);
	if (n == 0) {
		/* use the same result */
	} else if (n == 1) {
		ref = ir_OVERFLOW(ref);
	} else {
		return 0;
	}
	ir_addrtab_add(ctx->binding, (uintptr_t)expr, ref);
	return 1;
}

static void llvm2ir_freeze(ir_ctx *ctx, LLVMValueRef expr)
{
	LLVMValueRef op0;
	ir_ref ref;

	IR_ASSERT(LLVMGetNumOperands(expr) == 1);
	op0 = LLVMGetOperand(expr, 0);
	ref = ir_addrtab_find(ctx->binding, (uintptr_t)op0);
	IR_ASSERT(ref != (ir_ref)IR_INVALID_VAL);
	ir_addrtab_add(ctx->binding, (uintptr_t)expr, ref);
}

static ir_ref llvm2ir_const_element_ptr(ir_ctx *ctx, LLVMValueRef expr)
{
	LLVMValueRef op;
	LLVMValueRef op0 = LLVMGetOperand(expr, 0);
	LLVMTypeRef type = LLVMTypeOf(op0);
	LLVMTypeKind type_kind;
	uint32_t i, count;
	uintptr_t index, offset = 0;
	ir_ref ref;

	type_kind = LLVMGetTypeKind(type);
	IR_ASSERT(type_kind == LLVMPointerTypeKind);
	if (LLVMGetValueKind(op0) == LLVMGlobalVariableValueKind) {
		type = LLVMGlobalGetValueType(op0);
		type_kind = LLVMGetTypeKind(type);
	}

	op = LLVMGetOperand(expr, 1);
	IR_ASSERT(LLVMGetValueKind(op) == LLVMConstantIntValueKind);
	index = LLVMConstIntGetSExtValue(op);
	offset += index * LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, type);

	count = LLVMGetNumOperands(expr);
	for (i = 2; i < count; i++) {
		op = LLVMGetOperand(expr, i);
		switch (type_kind) {
			case LLVMStructTypeKind:
				IR_ASSERT(LLVMGetValueKind(op) == LLVMConstantIntValueKind);
				index = LLVMConstIntGetSExtValue(op);
				offset += LLVMOffsetOfElement((LLVMTargetDataRef)ctx->rules, type, index);
				type = LLVMStructGetTypeAtIndex(type, index);
				break;
			case LLVMPointerTypeKind:
				IR_ASSERT(!LLVMPointerTypeIsOpaque(type));
				IR_FALLTHROUGH;
			case LLVMArrayTypeKind:
				IR_ASSERT(LLVMGetValueKind(op) == LLVMConstantIntValueKind);
				index = LLVMConstIntGetSExtValue(op);
				type = LLVMGetElementType(type);
				offset += index * LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, type);
				break;
			default:
				IR_ASSERT(0);
				return 0;
		}
		type_kind = LLVMGetTypeKind(type);
    }
    ref = llvm2ir_op(ctx, op0, IR_ADDR);
    if (offset) {
		ref = ir_ADD_A(ref, ir_const_addr(ctx, (uintptr_t)offset));
    }
	return ref;
}

static void llvm2ir_element_ptr(ir_ctx *ctx, LLVMValueRef insn)
{
	LLVMValueRef op;
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	LLVMTypeRef type = LLVMTypeOf(op0);
	LLVMTypeKind type_kind;
	uint32_t i, count;
	uintptr_t index, offset = 0, size;
	ir_ref ref = llvm2ir_op(ctx, op0, IR_ADDR);

	type_kind = LLVMGetTypeKind(type);
	IR_ASSERT(type_kind == LLVMPointerTypeKind);
	if (LLVMGetValueKind(op0) == LLVMGlobalVariableValueKind) {
		type = LLVMGlobalGetValueType(op0);
		type_kind = LLVMGetTypeKind(type);
	} else {
		type = LLVMGetGEPSourceElementType(insn);
		type_kind = LLVMGetTypeKind(type);
	}

	op = LLVMGetOperand(insn, 1);
	if (LLVMGetValueKind(op) == LLVMConstantIntValueKind) {
		index = LLVMConstIntGetSExtValue(op);
		offset += index * LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, type);
	} else {
		size = LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, type);
		if (size == 1) {
			ref = ir_ADD_A(ref, llvm2ir_op(ctx, op, IR_ADDR));
		} else {
			ref = ir_ADD_A(ref, ir_MUL_A(
				llvm2ir_op(ctx, op, IR_ADDR),
				ir_const_addr(ctx, LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, type))));
		}
	}

	count = LLVMGetNumOperands(insn);
	for (i = 2; i < count; i++) {
		op = LLVMGetOperand(insn, i);
		switch (type_kind) {
			case LLVMStructTypeKind:
				IR_ASSERT(LLVMGetValueKind(op) == LLVMConstantIntValueKind);
				index = LLVMConstIntGetSExtValue(op);
				offset += LLVMOffsetOfElement((LLVMTargetDataRef)ctx->rules, type, index);
				type = LLVMStructGetTypeAtIndex(type, index);
				break;
			case LLVMPointerTypeKind:
				IR_ASSERT(!LLVMPointerTypeIsOpaque(type));
				IR_FALLTHROUGH;
			case LLVMArrayTypeKind:
				type = LLVMGetElementType(type);
				if (LLVMGetValueKind(op) == LLVMConstantIntValueKind) {
					index = LLVMConstIntGetSExtValue(op);
					offset += index * LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, type);
				} else {
					if (offset) {
						ref = ir_ADD_A(ref, ir_const_addr(ctx, (uintptr_t)offset));
						offset = 0;
					}
					size = LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, type);
					if (size == 1) {
						ref = ir_ADD_A(ref, llvm2ir_op(ctx, op, IR_ADDR));
					} else {
						ref = ir_ADD_A(ref, ir_MUL_A(
							llvm2ir_op(ctx, op, IR_ADDR),
							ir_const_addr(ctx, LLVMABISizeOfType((LLVMTargetDataRef)ctx->rules, type))));
					}
				}
				break;
			default:
				IR_ASSERT(0);
				return;
		}
		type_kind = LLVMGetTypeKind(type);
	}
	if (offset) {
		ref = ir_ADD_A(ref, ir_const_addr(ctx, (uintptr_t)offset));
	}
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
}

static ir_ref llvm2ir_const_expr(ir_ctx *ctx, LLVMValueRef expr)
{
	LLVMOpcode opcode = LLVMGetConstOpcode(expr);

	switch (opcode) {
		case LLVMFNeg:
			return llvm2ir_unary_op(ctx, expr, IR_NEG);
		case LLVMAdd:
		case LLVMFAdd:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_ADD);
		case LLVMSub:
		case LLVMFSub:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_SUB);
		case LLVMMul:
		case LLVMFMul:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_MUL);
		case LLVMUDiv:
		case LLVMSDiv:
		case LLVMFDiv:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_DIV);
		case LLVMURem:
		case LLVMSRem:
			break;
		case LLVMShl:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_SHL);
		case LLVMLShr:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_SHR);
		case LLVMAShr:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_SAR);
		case LLVMAnd:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_AND);
		case LLVMOr:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_OR);
		case LLVMXor:
			return llvm2ir_binary_op(ctx, opcode, expr, IR_XOR);
		case LLVMTrunc:
			return llvm2ir_cast_op(ctx, expr, IR_TRUNC);
		case LLVMZExt:
			return llvm2ir_cast_op(ctx, expr, IR_ZEXT);
		case LLVMSExt:
			return llvm2ir_cast_op(ctx, expr, IR_SEXT);
		case LLVMFPTrunc:
		case LLVMFPExt:
			return llvm2ir_cast_op(ctx, expr, IR_FP2FP);
		case LLVMFPToUI:
		case LLVMFPToSI:
			return llvm2ir_cast_op(ctx, expr, IR_FP2INT);
		case LLVMUIToFP:
		case LLVMSIToFP:
			return llvm2ir_cast_op(ctx, expr, IR_INT2FP);
		case LLVMPtrToInt:
		case LLVMIntToPtr:
		case LLVMBitCast:
			return llvm2ir_cast_op(ctx, expr, IR_BITCAST);
		case LLVMICmp:
			return llvm2ir_icmp_op(ctx, expr);
		case LLVMFCmp:
			return llvm2ir_fcmp_op(ctx, expr);
		case LLVMSelect:
			return llvm2ir_cond_op(ctx, expr);
		case LLVMGetElementPtr:
			return llvm2ir_const_element_ptr(ctx, expr);
		default:
			break;
	}
	fprintf(stderr, "Unsupported LLVM expr: %d\n", opcode);
	IR_ASSERT(0);
	return 0;
}

static ir_ref llvm2ir_auto_cast(ir_ctx *ctx, ir_ref ref, ir_type src_type, ir_type type)
{
	if (type == IR_ADDR && ctx->ir_base[ref].op == IR_VAR) {
		return ir_VADDR(ref);
	} else if (IR_IS_TYPE_INT(type)) {
		if (IR_IS_TYPE_INT(src_type)) {
			if (ir_type_size[type] == ir_type_size[src_type]) {
				if (type == IR_ADDR) {
					return ref;
				} else {
					return ir_BITCAST(type, ref);
				}
			} else if (ir_type_size[type] > ir_type_size[src_type]) {
				if (IR_IS_TYPE_SIGNED(src_type)) {
					return ir_SEXT(type, ref);
				} else {
					return ir_ZEXT(type, ref);
				}
			} else if (ir_type_size[type] < ir_type_size[src_type]) {
				return ir_TRUNC(type, ref);
			}
		} else {
			// TODO: FP to INT conversion
		}
	} else if (IR_IS_TYPE_FP(type)) {
		if (IR_IS_TYPE_FP(src_type)) {
			return ir_FP2FP(type, ref);
		} else {
			return ir_INT2FP(type, ref);
		}
	}
	IR_ASSERT(0);
	return ref;
}

static void llvm2ir_bb_start(ir_ctx *ctx, LLVMBasicBlockRef bb, LLVMBasicBlockRef pred_bb)
{
	LLVMValueRef insn = LLVMGetLastInstruction(pred_bb);
	LLVMOpcode opcode = LLVMGetInstructionOpcode(insn);

	if (opcode == LLVMBr) {
		ir_ref ref = ir_addrtab_find(ctx->binding, (uintptr_t)insn);

		IR_ASSERT(ref != (ir_ref)IR_INVALID_VAL);
		if (!LLVMIsConditional(insn)) {
			ir_BEGIN(ref);
		} else {
			LLVMBasicBlockRef true_bb;

			IR_ASSERT(LLVMGetNumSuccessors(insn) == 2);
			true_bb = LLVMGetSuccessor(insn, 0); /* true branch */
			if (bb == true_bb) {
				IR_ASSERT(bb != LLVMGetSuccessor(insn, 1)); /* false branch */
				ir_IF_TRUE(ref);
			} else {
				IR_ASSERT(bb == LLVMGetSuccessor(insn, 1)); /* false branch */
				ir_IF_FALSE(ref);
			}
		}
	} else if (opcode == LLVMSwitch) {
		ir_ref ref = ir_addrtab_find(ctx->binding, (uintptr_t)insn);

		IR_ASSERT(ref != (ir_ref)IR_INVALID_VAL);
		if (LLVMGetSwitchDefaultDest(insn) == bb) {
			ir_CASE_DEFAULT(ref);
		} else  {
			uint32_t i;
			uint32_t count = LLVMGetNumOperands(insn);

			for (i = 2; i < count; i += 2) {
				if (LLVMValueAsBasicBlock(LLVMGetOperand(insn, i + 1)) == bb) {
					LLVMValueRef val = LLVMGetOperand(insn, i);
					ir_type type = llvm2ir_type(LLVMTypeOf(val));
					ir_CASE_VAL(ref, llvm2ir_op(ctx, val, type));
					break;
				}
			}
		}
	} else {
		IR_ASSERT(0);
	}
}

static void llvm2ir_patch_merge(ir_ctx *ctx, ir_ref merge, uint32_t n, ir_ref ref)
{
	IR_ASSERT(ctx->ir_base[merge].op == IR_MERGE || ctx->ir_base[merge].op == IR_LOOP_BEGIN);
	ctx->ir_base[merge].op = IR_LOOP_BEGIN;
	ir_MERGE_SET_OP(merge, n + 1, ref);
}

static uint32_t llvm2ir_compute_post_order(
		ir_hashtab        *bb_hash,
		LLVMBasicBlockRef *bbs,
		uint32_t           bb_count,
		uint32_t           start,
		uint32_t          *post_order)
{
	uint32_t b, succ_b, j, n, count = 0;
	LLVMBasicBlockRef bb, succ_bb;
	LLVMValueRef insn;
	LLVMOpcode opcode;
	ir_worklist worklist;

	ir_worklist_init(&worklist, bb_count);
	ir_worklist_push(&worklist, start);

	while (ir_worklist_len(&worklist) != 0) {
next:
		b = ir_worklist_peek(&worklist);
		bb = bbs[b];
		insn = LLVMGetLastInstruction(bb);
		opcode = LLVMGetInstructionOpcode(insn);
		if (opcode == LLVMBr || opcode == LLVMSwitch) {
			n = LLVMGetNumSuccessors(insn);
			for (j = 0; j < n; j++) {
				succ_bb = LLVMGetSuccessor(insn, j);
				succ_b = ir_addrtab_find(bb_hash, (uintptr_t)succ_bb);
				IR_ASSERT(succ_b < bb_count);
				if (ir_worklist_push(&worklist, succ_b)) {
					goto next;
				}
			}
		}
		ir_worklist_pop(&worklist);
		post_order[count++] = b;
	}
	ir_worklist_free(&worklist);
	return count;
}

static int llvm2ir_func(ir_ctx *ctx, LLVMModuleRef module, LLVMValueRef func)
{
	uint32_t i, j, b, count, cconv, bb_count;
	LLVMBasicBlockRef *bbs, bb;
	LLVMValueRef param, insn;
	LLVMOpcode opcode;
	ir_type type;
	ir_ref ref, max_inputs_count;
	ir_hashtab bb_hash;
	ir_use_list *predecessors;
	uint32_t *predecessor_edges, *predecessor_refs_count;
	ir_ref *inputs, *bb_starts, *predecessor_refs;

	// TODO: function prototype
	cconv = LLVMGetFunctionCallConv(func);
	if (cconv == LLVMCCallConv || cconv == LLVMFastCallConv) {
		/* skip */
	} else if (cconv == LLVMX86FastcallCallConv) {
		ctx->flags |= IR_FASTCALL_FUNC;
	} else {
		fprintf(stderr, "Unsupported Calling Convention: %d\n", cconv);
		IR_ASSERT(0);
		return 0;
	}
	if (LLVMIsFunctionVarArg(LLVMTypeOf(func))) {
		// TODO:
	}

	/* Reuse "binding" for LLVMValueRef -> ir_ref hash */
	ctx->binding = ir_mem_malloc(sizeof(ir_hashtab));
	ir_addrtab_init(ctx->binding, 256);

	ir_START();
	for (i = 0; i < LLVMCountParams(func); i++) {
		param = LLVMGetParam(func, i);
		type = llvm2ir_type(LLVMTypeOf(param));
		if (type == IR_BAD_TYPE) {
			return 0;
		}
		ref = ir_PARAM(type, "", i + 1); // TODO: unique name
		ir_addrtab_add(ctx->binding, (uintptr_t)param, ref);
	}

	/* Find LLVM BasicBlocks Predecessors */
	bb_count = LLVMCountBasicBlocks(func);
	bbs = ir_mem_malloc(bb_count * sizeof(LLVMBasicBlockRef));
	predecessors = ir_mem_calloc(bb_count, sizeof(ir_use_list));
	LLVMGetBasicBlocks(func, bbs);
	ir_addrtab_init(&bb_hash, bb_count);
	for (i = 0; i < bb_count; i++) {
		bb = bbs[i];
		ir_addrtab_add(&bb_hash, (uintptr_t)bb, i);
	}
	for (i = 0; i < bb_count; i++) {
		bb = bbs[i];
		insn = LLVMGetLastInstruction(bb);
		opcode = LLVMGetInstructionOpcode(insn);
		if (opcode == LLVMBr || opcode == LLVMSwitch) {
			count = LLVMGetNumSuccessors(insn);
			for (j = 0; j < count; j++) {
				uint32_t b = ir_addrtab_find(&bb_hash, (uintptr_t)LLVMGetSuccessor(insn, j));

				IR_ASSERT(b < bb_count);
				predecessors[b].count++;
			}
		}
	}
	count = 0;
	max_inputs_count = 0;
	for (i = 0; i < bb_count; i++) {
		predecessors[i].refs = count;
		count += predecessors[i].count;
		max_inputs_count = IR_MAX(max_inputs_count, predecessors[i].count);
		predecessors[i].count = 0;
	}
	predecessor_edges = ir_mem_malloc(sizeof(uint32_t) * count);
	predecessor_refs = ir_mem_calloc(sizeof(ir_ref), count);
	predecessor_refs_count = ir_mem_calloc(sizeof(uint32_t), bb_count);
	inputs = ir_mem_malloc(sizeof(ir_ref) * max_inputs_count);
	for (i = 0; i < bb_count; i++) {
		bb = bbs[i];
		insn = LLVMGetLastInstruction(bb);
		opcode = LLVMGetInstructionOpcode(insn);
		if (opcode == LLVMBr || opcode == LLVMSwitch) {
			count = LLVMGetNumSuccessors(insn);
			for (j = 0; j < count; j++) {
				uint32_t b = ir_addrtab_find(&bb_hash, (uintptr_t)LLVMGetSuccessor(insn, j));

				IR_ASSERT(b < bb_count);
				predecessor_edges[predecessors[b].refs + predecessors[b].count++] = i;
			}
		}
	}

	/* Find proper basic blocks order */
	bb = LLVMGetEntryBasicBlock(func);
	b = ir_addrtab_find(&bb_hash, (uintptr_t)bb);
	IR_ASSERT(b < bb_count);
	uint32_t *post_order = ir_mem_malloc(sizeof(uint32_t) * bb_count);
	uint32_t post_order_count = llvm2ir_compute_post_order(&bb_hash, bbs, bb_count, b, post_order);

	/* Process all reachable basic blocks in proper order */
	ir_bitset visited = ir_bitset_malloc(bb_count);
	bb_starts = ir_mem_malloc(bb_count * sizeof(ir_ref));
	while (post_order_count) {
		i = post_order[--post_order_count];
		bb = bbs[i];
		count = predecessors[i].count;
		if (count == 1) {
			llvm2ir_bb_start(ctx, bb, bbs[predecessor_edges[predecessors[i].refs]]);
		} else if (count > 1) {
			ir_MERGE_N(count, predecessor_refs + predecessors[i].refs); /* MERGE may be converted to LOOP_BEGIN later */
		} else if (i != 0) {
			ir_BEGIN(IR_UNUSED); /* unreachable block */
		}
		bb_starts[i] = ctx->control;
		ir_bitset_incl(visited, i);

		for (insn = LLVMGetFirstInstruction(bb); insn; insn = LLVMGetNextInstruction(insn)) {
			opcode = LLVMGetInstructionOpcode(insn);
			switch (opcode) {
				case LLVMRet:
					llvm2ir_ret(ctx, insn);
					break;
				case LLVMBr:
					if (!LLVMIsConditional(insn)) {
						ref = llvm2ir_jmp(ctx, insn);
						b = ir_addrtab_find(&bb_hash, (uintptr_t)LLVMGetSuccessor(insn, 0));
						IR_ASSERT(b < bb_count);
						if (predecessors[b].count > 1) {
							if (ir_bitset_in(visited, b)) {
								llvm2ir_patch_merge(ctx, bb_starts[b], predecessor_refs_count[b], ref);
								ctx->ir_base[ref].op = IR_LOOP_END;
							} else {
								predecessor_refs[predecessors[b].refs + predecessor_refs_count[b]] = ref;
							}
							predecessor_refs_count[b]++;
						}
					} else {
						ref = llvm2ir_if(ctx, insn);
						b = ir_addrtab_find(&bb_hash, (uintptr_t)LLVMGetSuccessor(insn, 0)); /* true branch */
						IR_ASSERT(b < bb_count);
						if (predecessors[b].count > 1) {
							ir_IF_TRUE(ref);
							if (ir_bitset_in(visited, b)) {
								llvm2ir_patch_merge(ctx, bb_starts[b], predecessor_refs_count[b], ir_LOOP_END());
							} else {
								predecessor_refs[predecessors[b].refs + predecessor_refs_count[b]] = ir_END();;
							}
							predecessor_refs_count[b]++;
						} else {
							IR_ASSERT(!ir_bitset_in(visited, b));
						}
						b = ir_addrtab_find(&bb_hash, (uintptr_t)LLVMGetSuccessor(insn, 1)); /* false branch */
						IR_ASSERT(b < bb_count);
						if (predecessors[b].count > 1) {
							ir_IF_FALSE(ref);
							if (ir_bitset_in(visited, b)) {
								llvm2ir_patch_merge(ctx, bb_starts[b], predecessor_refs_count[b], ir_LOOP_END());
							} else {
								predecessor_refs[predecessors[b].refs + predecessor_refs_count[b]] = ir_END();;
							}
							predecessor_refs_count[b]++;
						} else {
							IR_ASSERT(!ir_bitset_in(visited, b));
						}
					}
					break;
				case LLVMSwitch:
					ref = llvm2ir_switch(ctx, insn);
					b = ir_addrtab_find(&bb_hash, (uintptr_t)LLVMGetSwitchDefaultDest(insn));
					IR_ASSERT(b < bb_count);
					if (predecessors[b].count > 1) {
						ir_CASE_DEFAULT(ref);
						if (ir_bitset_in(visited, b)) {
							llvm2ir_patch_merge(ctx, bb_starts[b], predecessor_refs_count[b], ir_LOOP_END());
						} else {
							predecessor_refs[predecessors[b].refs + predecessor_refs_count[b]] = ir_END();
						}
						predecessor_refs_count[b]++;
					} else {
						IR_ASSERT(!ir_bitset_in(visited, b));
					}
					count = LLVMGetNumOperands(insn);
					for (j = 2; j < count; j += 2) {
						b = ir_addrtab_find(&bb_hash, (uintptr_t)LLVMValueAsBasicBlock(LLVMGetOperand(insn, j + 1)));
						IR_ASSERT(b < bb_count);
						if (predecessors[b].count > 1) {
							LLVMValueRef val = LLVMGetOperand(insn, j);
							ir_type type = llvm2ir_type(LLVMTypeOf(val));
							ir_CASE_VAL(ref, llvm2ir_op(ctx, val, type));
							if (ir_bitset_in(visited, b)) {
								llvm2ir_patch_merge(ctx, bb_starts[b], predecessor_refs_count[b], ir_LOOP_END());
							} else {
								predecessor_refs[predecessors[b].refs + predecessor_refs_count[b]] = ir_END();
							}
							predecessor_refs_count[b]++;
						} else {
							IR_ASSERT(!ir_bitset_in(visited, b));
						}
					}
					break;
				case LLVMIndirectBr:
					// TODO:
					IR_ASSERT(0 && "NIY LLVMIndirectBr");
					break;
				case LLVMUnreachable:
					ir_UNREACHABLE();
					break;
				case LLVMFNeg:
					llvm2ir_unary_op(ctx, insn, IR_NEG);
					break;
				case LLVMAdd:
				case LLVMFAdd:
					llvm2ir_binary_op(ctx, opcode, insn, IR_ADD);
					break;
				case LLVMSub:
				case LLVMFSub:
					llvm2ir_binary_op(ctx, opcode, insn, IR_SUB);
					break;
				case LLVMMul:
				case LLVMFMul:
					llvm2ir_binary_op(ctx, opcode, insn, IR_MUL);
					break;
				case LLVMUDiv:
				case LLVMSDiv:
				case LLVMFDiv:
					llvm2ir_binary_op(ctx, opcode, insn, IR_DIV);
					break;
				case LLVMURem:
				case LLVMSRem:
					llvm2ir_binary_op(ctx, opcode, insn, IR_MOD);
					break;
				case LLVMShl:
					llvm2ir_binary_op(ctx, opcode, insn, IR_SHL);
					break;
				case LLVMLShr:
					llvm2ir_binary_op(ctx, opcode, insn, IR_SHR);
					break;
				case LLVMAShr:
					llvm2ir_binary_op(ctx, opcode, insn, IR_SAR);
					break;
				case LLVMAnd:
					llvm2ir_binary_op(ctx, opcode, insn, IR_AND);
					break;
				case LLVMOr:
					llvm2ir_binary_op(ctx, opcode, insn, IR_OR);
					break;
				case LLVMXor:
					llvm2ir_binary_op(ctx, opcode, insn, IR_XOR);
					break;
				case LLVMAlloca:
					llvm2ir_alloca(ctx, insn);
					break;
				case LLVMLoad:
					llvm2ir_load(ctx, insn);
					break;
				case LLVMStore:
					llvm2ir_store(ctx, insn);
					break;
				case LLVMTrunc:
					llvm2ir_cast_op(ctx, insn, IR_TRUNC);
					break;
				case LLVMZExt:
					llvm2ir_cast_op(ctx, insn, IR_ZEXT);
					break;
				case LLVMSExt:
					llvm2ir_cast_op(ctx, insn, IR_SEXT);
					break;
				case LLVMFPTrunc:
				case LLVMFPExt:
					llvm2ir_cast_op(ctx, insn, IR_FP2FP);
					break;
				case LLVMFPToUI:
				case LLVMFPToSI:
					llvm2ir_cast_op(ctx, insn, IR_FP2INT);
					break;
				case LLVMUIToFP:
				case LLVMSIToFP:
					llvm2ir_cast_op(ctx, insn, IR_INT2FP);
					break;
				case LLVMPtrToInt:
				case LLVMIntToPtr:
				case LLVMBitCast:
					llvm2ir_cast_op(ctx, insn, IR_BITCAST);
					break;
				case LLVMICmp:
					llvm2ir_icmp_op(ctx, insn);
					break;
				case LLVMFCmp:
					llvm2ir_fcmp_op(ctx, insn);
					break;
				case LLVMPHI:
					count = LLVMCountIncoming(insn);
					memset(inputs, 0, count * sizeof(ir_ref));
					if (LLVMGetTypeKind(LLVMTypeOf(insn)) == LLVMStructTypeKind) {
						type = llvm2ir_overflow_type(LLVMTypeOf(insn));
					} else {
						type = llvm2ir_type(LLVMTypeOf(insn));
					}
					ref = ir_PHI_N(type, count, inputs);
					ir_addrtab_add(ctx->binding, (uint64_t)insn, ref);
					break;
				case LLVMSelect:
					llvm2ir_cond_op(ctx, insn);
					break;
				case LLVMCall:
					llvm2ir_call(ctx, insn);
					break;
				case LLVMGetElementPtr:
					llvm2ir_element_ptr(ctx, insn);
					break;
				case LLVMFreeze:
					llvm2ir_freeze(ctx, insn);
					break;
				case LLVMExtractValue:
					if (llvm2ir_extract(ctx, insn)) {
						break;
					}
					IR_FALLTHROUGH;
				default:
					fprintf(stderr, "Unsupported LLVM insn: %d\n", opcode);
					IR_ASSERT(0);
					return 0;
			}
		}
	}

	/* Backpatch PHIs */
	for (i = 0; i < bb_count; i++) {
		ir_ref phi;
		uint32_t k;

		bb = bbs[i];
		count = predecessors[i].count;
		if (count <= 1) continue;
		IR_ASSERT(ctx->ir_base[bb_starts[i]].op == IR_MERGE || ctx->ir_base[bb_starts[i]].op == IR_LOOP_BEGIN);
		for (insn = LLVMGetFirstInstruction(bb);
				insn && LLVMGetInstructionOpcode(insn) == LLVMPHI;
				insn = LLVMGetNextInstruction(insn)) {
			phi = ir_addrtab_find(ctx->binding, (uint64_t)insn);
			IR_ASSERT(phi != (ir_ref)IR_INVALID_VAL);
			IR_ASSERT(ctx->ir_base[phi].op == IR_PHI);
			IR_ASSERT(LLVMCountIncoming(insn) == count);
			for (j = 0; j < count; j++) {
				LLVMValueRef op = LLVMGetIncomingValue(insn, j);
				uint32_t b = ir_addrtab_find(&bb_hash, (uintptr_t)LLVMGetIncomingBlock(insn, j));

				IR_ASSERT(b < bb_count);
				ref = llvm2ir_op(ctx, op, ctx->ir_base[phi].type);
				for (k = 0; k < count; k++) {
					if (predecessor_edges[predecessors[i].refs + k] == b) {
						ir_PHI_SET_OP(phi, k + 1, ref);
					}
				}
			}
		}
	}

	ir_mem_free(visited);
	ir_mem_free(post_order);
	ir_mem_free(predecessor_refs);
	ir_mem_free(predecessor_refs_count);
	ir_mem_free(bb_starts);
	ir_addrtab_free(ctx->binding);
	ir_mem_free(ctx->binding);
	ctx->binding = NULL;
	ir_addrtab_free(&bb_hash);
	ir_mem_free(bbs);
	ir_mem_free(predecessors);
	ir_mem_free(predecessor_edges);
	ir_mem_free(inputs);

	return 1;
}

static int ir_load_llvm_module(LLVMModuleRef module, uint32_t flags)
{
	ir_ctx ctx;
	LLVMValueRef func;
	LLVMTargetDataRef target_data = LLVMGetModuleDataLayout(module);

	for (func = LLVMGetFirstFunction(module); func; func = LLVMGetNextFunction(func)) {
		if (LLVMIsDeclaration(func)) continue;
		ir_init(&ctx, flags, 256, 1024);
		ctx.rules = (void*)target_data;
		if (!llvm2ir_func(&ctx, module, func)) {
			ctx.rules = NULL;
			ir_free(&ctx);
			return 0;
		}
		ctx.rules = NULL;

		// TODO:
		fprintf(stderr, "%s:\n", LLVMGetValueName(func));
		ir_save(&ctx, stderr);
		if (!ir_check(&ctx)) {
			ir_free(&ctx);
			return 0;
		}

		ir_free(&ctx);
	}

	return 1;
}

int ir_load_llvm_bitcode(const char *filename, uint32_t flags)
{
	LLVMMemoryBufferRef memory_buffer;
	LLVMModuleRef module;
	LLVMBool ret;
	char *message;

	if (LLVMCreateMemoryBufferWithContentsOfFile(filename, &memory_buffer, &message)) {
		fprintf(stderr, "Cannot open '%s': %s\n", filename, message);
		free(message);
		return 0;
	}
	ret = LLVMParseBitcodeInContext2(LLVMGetGlobalContext(), memory_buffer, &module);
	LLVMDisposeMemoryBuffer(memory_buffer);
	if (ret) {
		fprintf(stderr, "Cannot parse LLVM bitcode\n");
		return 0;
	}
	if (!ir_load_llvm_module(module, flags)) {
		LLVMDisposeModule(module);
		fprintf(stderr, "Cannot convert LLVM to IR\n");
		return 0;
	}
	LLVMDisposeModule(module);

	return 1;
}

int ir_load_llvm_asm(const char *filename, uint32_t flags)
{
	LLVMMemoryBufferRef memory_buffer;
	LLVMModuleRef module;
	LLVMBool ret;
	char *message;

	if (LLVMCreateMemoryBufferWithContentsOfFile(filename, &memory_buffer, &message)) {
		fprintf(stderr, "Cannot open '%s': %s\n", filename, message);
		free(message);
		return 0;
	}
	ret = LLVMParseIRInContext(LLVMGetGlobalContext(), memory_buffer, &module, &message);
	if (ret) {
		fprintf(stderr, "Cannot parse LLVM file: %s\n", message);
		free(message);
		return 0;
	}
	if (!ir_load_llvm_module(module, flags)) {
		LLVMDisposeModule(module);
		fprintf(stderr, "Cannot convert LLVM to IR\n");
		return 0;
	}
	LLVMDisposeModule(module);

	return 1;
}
