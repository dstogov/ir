/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V assembly text code generator)
 */

#include "ir.h"
#include "ir_private.h"
#include <string.h>

/* Allocatable temp registers (caller-saved) */
#define RISCV_NUM_REGS 7
static const char *riscv_tmp_regs[] = {
    "t0", "t1", "t2", "t3", "t4", "t5", "t6"
};

/* Map from IR ref -> register name (simple linear scan) */
#define MAX_REFS 4096
static const char *reg_map[MAX_REFS];
static int reg_next;

static const char *alloc_reg(ir_ref ref) {
    if (ref < 0 || ref >= MAX_REFS) return "t0";
    if (reg_map[ref]) return reg_map[ref];
    const char *r = riscv_tmp_regs[reg_next % RISCV_NUM_REGS];
    reg_next++;
    reg_map[ref] = r;
    return r;
}

static const char *get_reg(ir_ref ref) {
    if (ref < 0 || ref >= MAX_REFS) return "t0";
    return reg_map[ref] ? reg_map[ref] : "t0";
}

static void emit_load_const(FILE *f, const char *dst, ir_insn *insn) {
    int64_t v = insn->val.i64;
    if (v >= -2048 && v <= 2047) {
        fprintf(f, "\tli\t%s, %lld\n", dst, (long long)v);
    } else {
        fprintf(f, "\tli\t%s, %lld\n", dst, (long long)v);
    }
}

int ir_emit_riscv(ir_ctx *ctx, const char *name, FILE *f)
{
    ir_ref i;
    ir_insn *insn;

    memset(reg_map, 0, sizeof(reg_map));
    reg_next = 0;

    /* Function header */
    fprintf(f, "\t.text\n");
    fprintf(f, "\t.globl %s\n", name);
    fprintf(f, "\t.type %s, @function\n", name);
    fprintf(f, "%s:\n", name);

    /* Prologue: save ra and s0 */
    fprintf(f, "\taddi\tsp, sp, -16\n");
    fprintf(f, "\tsd\tra, 8(sp)\n");
    fprintf(f, "\tsd\ts0, 0(sp)\n");
    fprintf(f, "\taddi\ts0, sp, 16\n");

    /* Map PARAM to a0, a1, ... */
    int param_idx = 0;
    static const char *param_regs[] = {"a0","a1","a2","a3","a4","a5","a6","a7"};

    /* First pass: assign param registers */
    for (i = 1 - ctx->consts_count; i < ctx->insns_count; i++) {
        insn = &ctx->ir_base[i];
        if (insn->op == IR_PARAM) {
            if (param_idx < 8 && i >= 0 && i < MAX_REFS) {
                reg_map[i] = param_regs[param_idx++];
            }
        }
    }

    /* Second pass: emit instructions */
    for (i = 1; i < ctx->insns_count; i++) {
        insn = &ctx->ir_base[i];
        ir_ref op1 = insn->op1;
        ir_ref op2 = insn->op2;
        ir_ref op3 = insn->op3;
        const char *dst;

        switch (insn->op) {
        case IR_NOP:
        case IR_START:
        case IR_END:
        case IR_BEGIN:
        case IR_MERGE:
            break;

        case IR_PARAM:
            /* already handled */
            break;

        case IR_VAR:
            break;

        case IR_COPY:
            if (op1 > 0 && op1 < ctx->insns_count) {
                dst = alloc_reg(i);
                if (IR_IS_CONST_REF(op1)) {
                    emit_load_const(f, dst, &ctx->ir_base[op1]);
                } else {
                    fprintf(f, "\tmv\t%s, %s\n", dst, get_reg(op1));
                }
            } else if (IR_IS_CONST_REF(op1)) {
                dst = alloc_reg(i);
                emit_load_const(f, dst, &ctx->ir_base[op1]);
            }
            break;

        case IR_ADD:
            dst = alloc_reg(i);
            if (IR_IS_CONST_REF(op1)) {
                const char *r2 = (op2 > 0) ? get_reg(op2) : "zero";
                int64_t v = ctx->ir_base[op1].val.i64;
                if (v >= -2048 && v <= 2047)
                    fprintf(f, "\taddi\t%s, %s, %lld\n", dst, r2, (long long)v);
                else {
                    fprintf(f, "\tli\tt6, %lld\n", (long long)v);
                    fprintf(f, "\tadd\t%s, %s, t6\n", dst, r2);
                }
            } else if (IR_IS_CONST_REF(op2)) {
                const char *r1 = (op1 > 0) ? get_reg(op1) : "zero";
                int64_t v = ctx->ir_base[op2].val.i64;
                if (v >= -2048 && v <= 2047)
                    fprintf(f, "\taddi\t%s, %s, %lld\n", dst, r1, (long long)v);
                else {
                    fprintf(f, "\tli\tt6, %lld\n", (long long)v);
                    fprintf(f, "\tadd\t%s, %s, t6\n", dst, r1);
                }
            } else {
                const char *r1 = (op1 > 0) ? get_reg(op1) : "zero";
                const char *r2 = (op2 > 0) ? get_reg(op2) : "zero";
                fprintf(f, "\tadd\t%s, %s, %s\n", dst, r1, r2);
            }
            break;

        case IR_SUB:
            dst = alloc_reg(i);
            if (IR_IS_CONST_REF(op2)) {
                const char *r1 = (op1 > 0) ? get_reg(op1) : "zero";
                int64_t v = ctx->ir_base[op2].val.i64;
                fprintf(f, "\taddi\t%s, %s, %lld\n", dst, r1, (long long)-v);
            } else {
                const char *r1 = (op1 > 0) ? get_reg(op1) : "zero";
                const char *r2 = (op2 > 0) ? get_reg(op2) : "zero";
                fprintf(f, "\tsub\t%s, %s, %s\n", dst, r1, r2);
            }
            break;

        case IR_MUL:
            dst = alloc_reg(i);
            {
                const char *r1 = (op1 > 0) ? get_reg(op1) : "zero";
                const char *r2 = (op2 > 0) ? get_reg(op2) : "zero";
                if (IR_IS_CONST_REF(op1)) {
                    fprintf(f, "\tli\t%s, %lld\n", dst, (long long)ctx->ir_base[op1].val.i64);
                    r1 = dst;
                }
                if (IR_IS_CONST_REF(op2)) {
                    /* use t6 as scratch if dst == r1 */
                    fprintf(f, "\tli\tt6, %lld\n", (long long)ctx->ir_base[op2].val.i64);
                    r2 = "t6";
                }
                fprintf(f, "\tmul\t%s, %s, %s\n", dst, r1, r2);
            }
            break;

        case IR_EQ:
        case IR_NE:
        case IR_LT:
        case IR_GE:
        case IR_LE:
        case IR_GT:
            /* comparison: produce 0/1 in dst */
            dst = alloc_reg(i);
            {
                const char *r1 = IR_IS_CONST_REF(op1) ? "t5" : ((op1>0)?get_reg(op1):"zero");
                const char *r2 = IR_IS_CONST_REF(op2) ? "t6" : ((op2>0)?get_reg(op2):"zero");
                if (IR_IS_CONST_REF(op1))
                    fprintf(f, "\tli\tt5, %lld\n", (long long)ctx->ir_base[op1].val.i64);
                if (IR_IS_CONST_REF(op2))
                    fprintf(f, "\tli\tt6, %lld\n", (long long)ctx->ir_base[op2].val.i64);
                switch (insn->op) {
                case IR_EQ: fprintf(f, "\tsub\t%s, %s, %s\n\tseqz\t%s, %s\n", dst,r1,r2,dst,dst); break;
                case IR_NE: fprintf(f, "\tsub\t%s, %s, %s\n\tsnez\t%s, %s\n", dst,r1,r2,dst,dst); break;
                case IR_LT: fprintf(f, "\tslt\t%s, %s, %s\n", dst,r1,r2); break;
                case IR_GE: fprintf(f, "\tslt\t%s, %s, %s\n\txori\t%s, %s, 1\n", dst,r1,r2,dst,dst); break;
                case IR_LE: fprintf(f, "\tslt\t%s, %s, %s\n\txori\t%s, %s, 1\n", dst,r2,r1,dst,dst); break;
                case IR_GT: fprintf(f, "\tslt\t%s, %s, %s\n", dst,r2,r1); break;
                default: break;
                }
            }
            break;

        case IR_IF: {
            /* conditional branch: op1=control, op2=cond */
            const char *cond = IR_IS_CONST_REF(op2) ? "t0" : ((op2>0)?get_reg(op2):"zero");
            if (IR_IS_CONST_REF(op2))
                fprintf(f, "\tli\tt0, %lld\n", (long long)ctx->ir_base[op2].val.i64);
            fprintf(f, "\tbeqz\t%s, .Lfalse_%d\n", cond, i);
            fprintf(f, ".Ltrue_%d:\n", i);
            break;
        }

        case IR_IF_TRUE:
            /* falls through from IF true branch */
            break;

        case IR_IF_FALSE:
            fprintf(f, ".Lfalse_%d:\n", op1);
            break;

        case IR_LOOP_BEGIN:
            fprintf(f, ".Lloop_%d:\n", i);
            break;

        case IR_LOOP_END: {
            /* find the LOOP_BEGIN by walking up the control chain */
            ir_ref ctrl = op1;
            while (ctrl > 0 && ctx->ir_base[ctrl].op != IR_LOOP_BEGIN) {
                ctrl = ctx->ir_base[ctrl].op1;
            }
            fprintf(f, "\tj\t.Lloop_%d\n", ctrl);
            break;
        }

        case IR_PHI:
            /* PHI handled by register allocation; skip */
            dst = alloc_reg(i);
            break;

        case IR_RETURN:
            /* op2 = return value */
            if (op2 > 0 && !IR_IS_CONST_REF(op2)) {
                const char *rv = get_reg(op2);
                if (strcmp(rv, "a0") != 0)
                    fprintf(f, "\tmv\ta0, %s\n", rv);
            } else if (IR_IS_CONST_REF(op2)) {
                fprintf(f, "\tli\ta0, %lld\n", (long long)ctx->ir_base[op2].val.i64);
            }
            fprintf(f, "\tld\tra, 8(sp)\n");
            fprintf(f, "\tld\ts0, 0(sp)\n");
            fprintf(f, "\taddi\tsp, sp, 16\n");
            fprintf(f, "\tret\n");
            break;

        default:
            fprintf(f, "\t# unhandled op %d (ref %d)\n", insn->op, i);
            break;
        }
    }

    fprintf(f, "\t.size %s, .-%s\n", name, name);
    return 1;
}