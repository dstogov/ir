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
#include <llvm-c/BitReader.h>
#include <llvm-c/IRReader.h>

#define IR_BAD_TYPE IR_LAST_TYPE

static ir_ref llvm2ir_const_expr(ir_ctx *ctx, LLVMValueRef insn);
static ir_ref llvm2ir_auto_cast(ir_ctx *ctx, ir_ref ref, ir_type type);

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
		default:
			break;
	}

	str = LLVMPrintTypeToString(type);
	fprintf(stderr, "Unsupported LLVM type: %s\n", str);
	LLVMDisposeMessage(str);
	return IR_BAD_TYPE;
}

static ir_ref llvm2ir_op(ir_ctx *ctx, LLVMValueRef op, ir_type type)
{
	ir_ref ref;
	LLVMBool lose;

	switch( LLVMGetValueKind(op)) {
		case LLVMConstantIntValueKind:
			switch (type) {
				case IR_BOOL:
					return ir_const_bool(ctx, LLVMConstIntGetZExtValue(op));
				case IR_I8:
					return ir_const_i8(ctx, LLVMConstIntGetSExtValue(op));
				case IR_I16:
					return ir_const_i16(ctx, LLVMConstIntGetSExtValue(op));
				case IR_I32:
					return ir_const_i32(ctx, LLVMConstIntGetSExtValue(op));
				case IR_I64:
					return ir_const_i64(ctx, LLVMConstIntGetSExtValue(op));
				case IR_U8:
					return ir_const_u8(ctx, LLVMConstIntGetZExtValue(op));
				case IR_U16:
					return ir_const_u16(ctx, LLVMConstIntGetZExtValue(op));
				case IR_U32:
					return ir_const_u32(ctx, LLVMConstIntGetZExtValue(op));
				case IR_U64:
					return ir_const_u64(ctx, LLVMConstIntGetZExtValue(op));
				case IR_CHAR:
					return ir_const_char(ctx, LLVMConstIntGetZExtValue(op));
				default:
					IR_ASSERT(0);
					return 0;
			}
		case LLVMConstantFPValueKind:
			if (type == IR_DOUBLE) {
				return ir_const_double(ctx, LLVMConstRealGetDouble(op, &lose));
			} else {
				return ir_const_float(ctx, (float)LLVMConstRealGetDouble(op, &lose));
			}
			break;
		case LLVMConstantPointerNullValueKind:
			return IR_NULL;
		case LLVMConstantExprValueKind:
			ref = llvm2ir_const_expr(ctx, op);
			if (ctx->ir_base[ref].type != type) {
				ref = llvm2ir_auto_cast(ctx, ref, type);
			}
			return ref;
		case LLVMArgumentValueKind:
		case LLVMInstructionValueKind:
			ref  = ir_addrtab_find(ctx->binding, (uintptr_t)op);
			IR_ASSERT(ref != (ir_ref)IR_INVALID_VAL);
			return ref;
		case LLVMFunctionValueKind:
		case LLVMGlobalVariableValueKind:
			// TODO:
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

static void llvm2ir_jmp(ir_ctx *ctx, LLVMValueRef insn)
{
	ir_ref ref;

	ref = ir_END(); // TODO: LOOP_END
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
}

static void llvm2ir_if(ir_ctx *ctx, LLVMValueRef insn)
{
	ir_ref ref;
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	ir_type type = llvm2ir_type(LLVMTypeOf(op0));

	ref = ir_IF(llvm2ir_op(ctx, op0, type));
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
}

static void llvm2ir_switch(ir_ctx *ctx, LLVMValueRef insn)
{
	ir_ref ref;
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	ir_type type = llvm2ir_type(LLVMTypeOf(op0));

	ref = ir_SWITCH(llvm2ir_op(ctx, op0, type));
	ir_addrtab_add(ctx->binding, (uintptr_t)insn, ref);
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

static ir_ref llvm2ir_binary_op(ir_ctx *ctx, LLVMOpcode opcode, LLVMValueRef expr, ir_op op)
{
	LLVMValueRef op0 = LLVMGetOperand(expr, 0);
	LLVMValueRef op1 = LLVMGetOperand(expr, 1);
	ir_type type = llvm2ir_type(LLVMTypeOf(expr));
	ir_ref ref;

	if (opcode == LLVMUDiv || opcode == LLVMURem) {
		IR_ASSERT(type >= IR_I8 && type <= IR_I64);
		type = type - IR_I8 + IR_U8;
	}
	ref = ir_fold2(ctx, IR_OPT(op, type), llvm2ir_op(ctx, op0, type), llvm2ir_op(ctx, op1, type));
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
		case LLVMRealONE: op = IR_NE;  break;
		case LLVMRealUEQ:
		case LLVMRealUNE: op = IR_NE;  break;
		case LLVMRealUGT: op = IR_UGT; break;
		case LLVMRealUGE: op = IR_UGE; break;
		case LLVMRealULT: op = IR_ULT; break;
		case LLVMRealULE: op = IR_ULE; break;
		case LLVMRealOGT: op = IR_GT;  break;
		case LLVMRealOGE: op = IR_GE;  break;
		case LLVMRealOLT: op = IR_LT;  break;
		case LLVMRealOLE: op = IR_LE;  break;
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
	ir_type type = llvm2ir_type(LLVMGetAllocatedType(insn));
	LLVMValueRef op0 = LLVMGetOperand(insn, 0);
	ir_ref ref;

	if (LLVMGetValueKind(op0) == LLVMConstantIntValueKind && LLVMConstIntGetZExtValue(op0) == 1) {
		ref = ir_VAR(type, ""); // TODO: unique name
	} else {
		ref = ir_ALLOCA(ir_MUL_A(llvm2ir_op(ctx, op0, IR_ADDR), ir_const_addr(ctx, ir_type_size[type])));
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

static ir_ref llvm2ir_const_expr(ir_ctx *ctx, LLVMValueRef expr)
{
	LLVMOpcode opcode = LLVMGetInstructionOpcode(expr);

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
		default:
			break;
	}
	fprintf(stderr, "Unsupported LLVM expr: %d\n", opcode);
	return 0;
}

static ir_ref llvm2ir_auto_cast(ir_ctx *ctx, ir_ref ref, ir_type type)
{
	// TODO:
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
			LLVMBasicBlockRef true_bb, false_bb;

			IR_ASSERT(LLVMGetNumSuccessors(insn) == 2);
			true_bb = LLVMGetSuccessor(insn, 0); /* true branch */
			false_bb = LLVMGetSuccessor(insn, 1); /* false branch */
			if (bb == true_bb) {
				ir_IF_TRUE(ref);
			} else {
				IR_ASSERT(bb == false_bb);
				ir_IF_FALSE(ref);
			}
		}
	} else if (opcode == LLVMSwitch) {
		// TODO:
		IR_ASSERT(0);
	} else {
		IR_ASSERT(0);
	}
}

static ir_ref llvm2ir_bb_end(ir_ctx *ctx, LLVMBasicBlockRef bb, LLVMBasicBlockRef pred_bb)
{
	LLVMValueRef insn = LLVMGetLastInstruction(pred_bb);
	LLVMOpcode opcode = LLVMGetInstructionOpcode(insn);
	ir_ref ref;

	if (opcode == LLVMBr) {
		ref = ir_addrtab_find(ctx->binding, (uintptr_t)insn);
		if (ref == (ir_ref)IR_INVALID_VAL) {
			ref = IR_UNUSED;
		} else if (LLVMIsConditional(insn)) {
			LLVMBasicBlockRef true_bb, false_bb;

			IR_ASSERT(LLVMGetNumSuccessors(insn) == 2);
			true_bb = LLVMGetSuccessor(insn, 0); /* true branch */
			false_bb = LLVMGetSuccessor(insn, 1); /* false branch */
			if (bb == true_bb) {
				ir_IF_TRUE(ref);
				ref = ir_END();
			} else {
				IR_ASSERT(bb == false_bb);
				ir_IF_FALSE(ref);
				ref = ir_END();
			}
		}
	} else if (opcode == LLVMSwitch) {
		// TODO:
		IR_ASSERT(0);
		ref = IR_UNUSED;
	} else {
		IR_ASSERT(0);
		ref = IR_UNUSED;
	}
	return ref;
}

static int llvm2ir_func(ir_ctx *ctx, LLVMModuleRef module, LLVMValueRef func)
{
	uint32_t i, j, count, cconv, bb_count;
	LLVMBasicBlockRef *bbs, bb;
	LLVMValueRef param, insn;
	LLVMOpcode opcode;
	ir_type type;
	ir_ref ref, max_inputs_count;
	ir_hashtab bb_hash;
	ir_use_list *predecessors;
	uint32_t *predecessor_edges;
	ir_ref *inputs, *bb_starts;

	cconv = LLVMGetFunctionCallConv(func);
	if (cconv == LLVMCCallConv) {
		/* skip */
	} else if (cconv == LLVMX86FastcallCallConv) {
		ctx->flags |= IR_FASTCALL_FUNC;
	} else {
		fprintf(stderr, "Unsupported Calling Convention: %d\n", cconv);
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

	bb_starts = ir_mem_malloc(bb_count * sizeof(ir_ref));
	for (i = 0; i < bb_count; i++) {
		bb = bbs[i];
		count = predecessors[i].count;
		if (count == 1) {
			llvm2ir_bb_start(ctx, bb, bbs[predecessor_edges[predecessors[i].refs]]);
		} else if (count > 1) {
			for (j = 0; j < count; j++) {
				inputs[j] = llvm2ir_bb_end(ctx, bb, bbs[predecessor_edges[predecessors[i].refs + j]]);
			}
			ir_MERGE_N(count, inputs); // TODO: LOOP_BEGIN
		} else if (i != 0) {
			ir_BEGIN(IR_UNUSED); /* unreachable block */
		}
		bb_starts[i] = ctx->control;

		for (insn = LLVMGetFirstInstruction(bb); insn; insn = LLVMGetNextInstruction(insn)) {
			opcode = LLVMGetInstructionOpcode(insn);
			switch (opcode) {
				case LLVMRet:
					llvm2ir_ret(ctx, insn);
					break;
				case LLVMBr:
					if (!LLVMIsConditional(insn)) {
						llvm2ir_jmp(ctx, insn);
					} else {
						llvm2ir_if(ctx, insn);
					}
					break;
				case LLVMSwitch:
					llvm2ir_switch(ctx, insn);
					break;
				case LLVMIndirectBr:
					// TODO:
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
					ref = ir_PHI_N(llvm2ir_type(LLVMTypeOf(insn)), count, inputs);
					ir_addrtab_add(ctx->binding, (uint64_t)insn, ref);
					break;
				case LLVMSelect:
					llvm2ir_cond_op(ctx, insn);
					break;
				case LLVMCall:
					// TODO:
					break;
				default:
					fprintf(stderr, "Unsupported LLVM insn: %d\n", opcode);
					return 0;
			}
		}
	}

	/* Backpatch MERGEs and PHIs */
	for (i = 0; i < bb_count; i++) {
		ir_ref merge, phi, *merge_ops;
		uint32_t k;

		bb = bbs[i];
		count = predecessors[i].count;
		if (count <= 1) continue;
		merge = bb_starts[i];
		IR_ASSERT(ctx->ir_base[merge].op == IR_MERGE || ctx->ir_base[merge].op == IR_LOOP_BEGIN);
		for (k = 0; k < count; k++) {
			merge_ops = ctx->ir_base[merge].ops + 1;
			if (!merge_ops[k]) {
				ref = llvm2ir_bb_end(ctx, bb, bbs[predecessor_edges[predecessors[i].refs + k]]);
				IR_ASSERT(ref != IR_UNUSED);
				ir_MERGE_SET_OP(merge, k + 1, ref);
			}
		}
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

	ir_mem_free(bb_starts);
	ir_addrtab_free(ctx->binding);
	ir_mem_free(ctx->binding);
	ctx->binding = NULL;
	ir_addrtab_free(&bb_hash);
	ir_mem_free(bbs);
	ir_mem_free(predecessors);
	ir_mem_free(predecessor_edges);
	ir_mem_free(inputs);

	ir_save(ctx, stderr);

	return 1;
}

static int ir_load_llvm_module(LLVMModuleRef module, uint32_t flags)
{
	ir_ctx ctx;
	LLVMValueRef func;

	for (func = LLVMGetFirstFunction(module); func; func = LLVMGetNextFunction(func)) {
		if (LLVMIsDeclaration(func)) continue;
		ir_init(&ctx, flags, 256, 1024);
		if (!llvm2ir_func(&ctx, module, func)) {
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
