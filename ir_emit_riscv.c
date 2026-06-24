/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V assembly code generator)
 */

#include "ir.h"
#include "ir_private.h"

/* RISC-V 32 general-purpose registers */
static const char *riscv_reg_names[] = {
    "zero", "ra", "sp", "gp", "tp",
    "t0", "t1", "t2",
    "s0", "s1",
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
    "t3", "t4", "t5", "t6"
};

int ir_emit_riscv(ir_ctx *ctx, const char *name, FILE *f)
{
    fprintf(f, "\t.text\n");
    fprintf(f, "\t.globl %s\n", name);
    fprintf(f, "%s:\n", name);
    /* TODO: emit prologue, basic blocks, epilogue */
    fprintf(f, "\tret\n");
    return 1;
}
