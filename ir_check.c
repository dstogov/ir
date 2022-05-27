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

	IR_ASSERT(IR_ADD + 1 == IR_SUB);
}

void ir_check(ir_ctx *ctx)
{
	//TODO:
}
