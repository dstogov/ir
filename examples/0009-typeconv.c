/*
 * IR - Lightweight JIT Compilation Framework
 * (Examples package)
 * Copyright (C) 2026 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"
#include "ir_builder.h"
#include <stdlib.h>

/*
 * Tagged integer packing/unpacking using a 1-bit tag.
 *
 * int64_t pack(int32_t val) {
 *	return ((int64_t)val << 1) | 1;
 * }
 *
 * int32_t unpack(int64_t tagged) {
 *	return (int32_t)(tagged >> 1);
 * }
 */
typedef int64_t (*pack_t)(int32_t);
typedef int32_t (*unpack_t)(int64_t);

void gen_pack(ir_ctx *ctx)
{
	ir_START();
	ir_ref val = ir_PARAM(IR_I32, "val", 1);

	/* (int64_t)val */
	ir_ref wide = ir_SEXT_I64(val);
	/* wide << 1 */
	ir_ref shifted = ir_SHL_I64(wide, ir_CONST_I64(1));
	/* shifted | 1 */
	ir_ref tagged = ir_OR_I64(shifted, ir_CONST_I64(1));

	ir_RETURN(tagged);
}

void gen_unpack(ir_ctx *ctx)
{
	ir_START();
	ir_ref tagged = ir_PARAM(IR_I64, "tagged", 1);

	/* tagged >> 1 */
	ir_ref shifted = ir_SHR_I64(tagged, ir_CONST_I64(1));
	/* (int32_t)shifted */
	ir_ref val = ir_TRUNC_I32(shifted);

	ir_RETURN(val);
}

int main(int argc, char **argv)
{
	ir_ctx ctx;
	size_t size;

	ir_consistency_check();

	/* Compile pack */
	ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
	ctx.ret_type = IR_I64;
	gen_pack(&ctx);
	pack_t pack = (pack_t)ir_jit_compile(&ctx, 2, &size);

	/* Compile unpack */
	ir_ctx ctx2;
	ir_init(&ctx2, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
	ctx2.ret_type = IR_I32;
	gen_unpack(&ctx2);
	unpack_t unpack = (unpack_t)ir_jit_compile(&ctx2, 2, &size);

	if (pack && unpack) {
		int32_t values[] = {0, 1, -1, 42, -100, 2147483647};
		for (int i = 0; i < 6; i++) {
			int64_t tagged = pack(values[i]);
			int32_t restored = unpack(tagged);
			printf("pack(%d) = 0x%lx, unpack = %d %s\n",
				values[i], tagged, restored,
				(restored == values[i]) ? "(ok)" : "(MISMATCH)");
		}
	}

	ir_free(&ctx);
	ir_free(&ctx2);

	return 0;
}
