/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V assembly text code generator)
 */

#include "ir.h"
#include "ir_private.h"
#include <string.h>
#include <stdlib.h>

#define MAX_REFS 4096
#define NUM_TMP_REGS 6
#define NUM_FTMP_REGS 6

static const char *tmp_regs[NUM_TMP_REGS] = {
    "t0", "t1", "t2", "t3", "t4", "t5"
};
static const char *ftmp_regs[NUM_FTMP_REGS] = {
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5"
};
static const char *param_regs[] = {
    "a0","a1","a2","a3","a4","a5","a6","a7"
};

static const char *reg_map[MAX_REFS];
static const char *freg_map[MAX_REFS];
static int reg_next;
static int freg_next;

static const char *alloc_reg(ir_ref ref) {
    if (ref <= 0 || ref >= MAX_REFS) return "t0";
    if (reg_map[ref]) return reg_map[ref];
    const char *r = tmp_regs[reg_next % NUM_TMP_REGS];
    reg_next++;
    reg_map[ref] = r;
    return r;
}

static const char *alloc_freg(ir_ref ref) {
    if (ref <= 0 || ref >= MAX_REFS) return "ft0";
    if (freg_map[ref]) return freg_map[ref];
    const char *r = ftmp_regs[freg_next % NUM_FTMP_REGS];
    freg_next++;
    freg_map[ref] = r;
    return r;
}

static const char *get_reg(ir_ref ref) {
    if (ref <= 0 || ref >= MAX_REFS) return "zero";
    return reg_map[ref] ? reg_map[ref] : "zero";
}

static const char *get_freg(ir_ref ref) {
    if (ref <= 0 || ref >= MAX_REFS) return "ft0";
    return freg_map[ref] ? freg_map[ref] : "ft0";
}

static void emit_ref(FILE *f, ir_ctx *ctx, const char *dst, ir_ref ref) {
    if (IR_IS_CONST_REF(ref)) {
        ir_insn *c = &ctx->ir_base[ref];
        fprintf(f, "\tli\t%s, %lld\n", dst, (long long)c->val.i64);
    } else {
        const char *src = get_reg(ref);
        if (strcmp(dst, src) != 0)
            fprintf(f, "\tmv\t%s, %s\n", dst, src);
    }
}

static void emit_fref(FILE *f, ir_ctx *ctx, const char *dst, ir_ref ref, int is_double) {
    if (IR_IS_CONST_REF(ref)) {
        ir_insn *c = &ctx->ir_base[ref];
        // 把浮點常數存到 stack 再 load
        if (is_double) {
            uint64_t bits = c->val.u64;
            fprintf(f, "\tli\ts5, %llu\n", (unsigned long long)bits);
            fprintf(f, "\tsd\ts5, -8(sp)\n");
            fprintf(f, "\tfld\t%s, -8(sp)\n", dst);
        } else {
            uint32_t bits = c->val.u32;
            fprintf(f, "\tli\ts5, %u\n", bits);
            fprintf(f, "\tsw\ts5, -4(sp)\n");
            fprintf(f, "\tflw\t%s, -4(sp)\n", dst);
        }
    } else {
        const char *src = get_freg(ref);
        if (strcmp(dst, src) != 0)
            fprintf(f, "\t%s\t%s, %s\n", is_double ? "fmv.d" : "fmv.s", dst, src);
    }
}

/* emit PHI initial values before loop entry */
static void emit_phi_init(ir_ctx *ctx, FILE *f, ir_ref loop_begin) {
    ir_ref i;
    for (i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
        if (insn->op == IR_PHI && insn->op1 == loop_begin) {
            const char *dst = alloc_reg(i);
            emit_ref(f, ctx, dst, insn->op2);
        }
    }
}

/* emit PHI back-edge updates before loop end */
static void emit_phi_update(ir_ctx *ctx, FILE *f, ir_ref loop_begin) {
    ir_ref i;
    /* use scratch space to avoid clobber */
    /* simple: copy to scratch first, then to phi regs */
    static const char *scratch[] = {"s1","s2","s3","s4","s5","s6"};
    int sc = 0;
    /* first pass: load back-edge values into scratch */
    for (i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
        if (insn->op == IR_PHI && insn->op1 == loop_begin) {
            const char *s = scratch[sc % 6];
            emit_ref(f, ctx, s, insn->op3);
            sc++;
        }
    }
    /* second pass: move scratch to phi regs */
    sc = 0;
    for (i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
        if (insn->op == IR_PHI && insn->op1 == loop_begin) {
            const char *dst = get_reg(i);
            const char *s = scratch[sc % 6];
            if (strcmp(dst, s) != 0)
                fprintf(f, "\tmv\t%s, %s\n", dst, s);
            sc++;
        }
    }
}

int ir_emit_riscv(ir_ctx *ctx, const char *name, FILE *f)
{
    ir_ref i;
    ir_insn *insn;

    memset(reg_map, 0, sizeof(reg_map));
    memset(freg_map, 0, sizeof(freg_map));
    reg_next = 0;
    freg_next = 0;

    fprintf(f, "\t.text\n");
    fprintf(f, "\t.globl %s\n", name);
    fprintf(f, "\t.type %s, @function\n", name);
    fprintf(f, "%s:\n", name);
    fprintf(f, "\taddi\tsp, sp, -48\n");
    fprintf(f, "\tsd\tra, 40(sp)\n");
    fprintf(f, "\tsd\ts0, 32(sp)\n");
    fprintf(f, "\tsd\ts1, 24(sp)\n");
    fprintf(f, "\tsd\ts2, 16(sp)\n");
    fprintf(f, "\tsd\ts3, 8(sp)\n");
    fprintf(f, "\tsd\ts4, 0(sp)\n");
    fprintf(f, "\taddi\ts0, sp, 48\n");

    /* assign params */
    int param_idx = 0;
    int fparam_idx = 0;
    static const char *fparam_regs[] = {"fa0","fa1","fa2","fa3","fa4","fa5","fa6","fa7"};
    for (i = 1; i < ctx->insns_count; i++) {
        insn = &ctx->ir_base[i];
        if (insn->op == IR_PARAM) {
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                if (fparam_idx < 8)
                    freg_map[i] = fparam_regs[fparam_idx++];
            } else {
                if (param_idx < 8)
                    reg_map[i] = param_regs[param_idx++];
            }
        }
    }

    /* main emit loop */
    for (i = 1; i < ctx->insns_count; i++) {
        insn = &ctx->ir_base[i];
        ir_ref op1 = insn->op1;
        ir_ref op2 = insn->op2;
        const char *dst;

        switch (insn->op) {
        case IR_NOP:
        case IR_START:
        case IR_PARAM:
            break;

        case IR_END:
            break;

        case IR_LOOP_BEGIN:
            emit_phi_init(ctx, f, i);
            fprintf(f, ".Lloop_%d:\n", i);
            break;

        case IR_LOOP_END: {
            /* find LOOP_BEGIN */
            ir_ref lb = op1;
            while (lb > 0 && ctx->ir_base[lb].op != IR_LOOP_BEGIN)
                lb = ctx->ir_base[lb].op1;
            emit_phi_update(ctx, f, lb);
            fprintf(f, "\tj\t.Lloop_%d\n", lb);
            break;
        }

        case IR_PHI:
            /* handled by emit_phi_init/emit_phi_update */
            alloc_reg(i); /* ensure reg is allocated */
            break;

        case IR_BEGIN:
        case IR_MERGE:
            fprintf(f, ".Lbb_%d:\n", i);
            break;

        case IR_COPY:
            dst = alloc_reg(i);
            if (IR_IS_CONST_REF(op1))
                fprintf(f, "\tli\t%s, %lld\n", dst, (long long)ctx->ir_base[op1].val.i64);
            else if (op1 > 0)
                fprintf(f, "\tmv\t%s, %s\n", dst, get_reg(op1));
            break;

        case IR_ADD:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                dst = alloc_freg(i);
                const char *r1 = alloc_freg(op1); emit_fref(f, ctx, r1, op1, is_d);
                const char *r2 = alloc_freg(op2); emit_fref(f, ctx, r2, op2, is_d);
                fprintf(f, "\t%s\t%s, %s, %s\n", is_d ? "fadd.d" : "fadd.s", dst, r1, r2);
                break;
            }
            dst = alloc_reg(i);
            if (IR_IS_CONST_REF(op2)) {
                int64_t v = ctx->ir_base[op2].val.i64;
                const char *r1 = op1 > 0 ? get_reg(op1) : "zero";
                if (v >= -2048 && v <= 2047)
                    fprintf(f, "\taddi\t%s, %s, %lld\n", dst, r1, (long long)v);
                else {
                    fprintf(f, "\tli\ts5, %lld\n", (long long)v);
                    fprintf(f, "\tadd\t%s, %s, s5\n", dst, r1);
                }
            } else if (IR_IS_CONST_REF(op1)) {
                int64_t v = ctx->ir_base[op1].val.i64;
                const char *r2 = op2 > 0 ? get_reg(op2) : "zero";
                if (v >= -2048 && v <= 2047)
                    fprintf(f, "\taddi\t%s, %s, %lld\n", dst, r2, (long long)v);
                else {
                    fprintf(f, "\tli\ts5, %lld\n", (long long)v);
                    fprintf(f, "\tadd\t%s, %s, s5\n", dst, r2);
                }
            } else {
                fprintf(f, "\tadd\t%s, %s, %s\n", dst, get_reg(op1), get_reg(op2));
            }
            break;

        case IR_SUB:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                dst = alloc_freg(i);
                const char *r1 = alloc_freg(op1); emit_fref(f, ctx, r1, op1, is_d);
                const char *r2 = alloc_freg(op2); emit_fref(f, ctx, r2, op2, is_d);
                fprintf(f, "\t%s\t%s, %s, %s\n", is_d ? "fsub.d" : "fsub.s", dst, r1, r2);
                break;
            }
            dst = alloc_reg(i);
            if (IR_IS_CONST_REF(op2)) {
                int64_t v = ctx->ir_base[op2].val.i64;
                fprintf(f, "\taddi\t%s, %s, %lld\n", dst, get_reg(op1), (long long)-v);
            } else if (IR_IS_CONST_REF(op1)) {
                fprintf(f, "\tli\ts5, %lld\n", (long long)ctx->ir_base[op1].val.i64);
                fprintf(f, "\tsub\t%s, s5, %s\n", dst, get_reg(op2));
            } else {
                fprintf(f, "\tsub\t%s, %s, %s\n", dst, get_reg(op1), get_reg(op2));
            }
            break;

        case IR_MUL:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                dst = alloc_freg(i);
                const char *r1 = alloc_freg(op1); emit_fref(f, ctx, r1, op1, is_d);
                const char *r2 = alloc_freg(op2); emit_fref(f, ctx, r2, op2, is_d);
                fprintf(f, "\t%s\t%s, %s, %s\n", is_d ? "fmul.d" : "fmul.s", dst, r1, r2);
                break;
            }
            dst = alloc_reg(i);
            {
                const char *r1, *r2;
                if (IR_IS_CONST_REF(op1)) {
                    fprintf(f, "\tli\ts5, %lld\n", (long long)ctx->ir_base[op1].val.i64);
                    r1 = "s5";
                } else r1 = get_reg(op1);
                if (IR_IS_CONST_REF(op2)) {
                    fprintf(f, "\tli\ts6, %lld\n", (long long)ctx->ir_base[op2].val.i64);
                    r2 = "s6";
                } else r2 = get_reg(op2);
                fprintf(f, "\tmul\t%s, %s, %s\n", dst, r1, r2);
            }
            break;

        case IR_DIV:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                dst = alloc_freg(i);
                const char *r1 = alloc_freg(op1); emit_fref(f, ctx, r1, op1, is_d);
                const char *r2 = alloc_freg(op2); emit_fref(f, ctx, r2, op2, is_d);
                fprintf(f, "\t%s\t%s, %s, %s\n", is_d ? "fdiv.d" : "fdiv.s", dst, r1, r2);
                break;
            }
            dst = alloc_reg(i);
            {
                const char *r1, *r2;
                if (IR_IS_CONST_REF(op1)) {
                    fprintf(f, "\tli\ts5, %lld\n", (long long)ctx->ir_base[op1].val.i64);
                    r1 = "s5";
                } else r1 = get_reg(op1);
                if (IR_IS_CONST_REF(op2)) {
                    fprintf(f, "\tli\ts6, %lld\n", (long long)ctx->ir_base[op2].val.i64);
                    r2 = "s6";
                } else r2 = get_reg(op2);
                fprintf(f, "\tdiv\t%s, %s, %s\n", dst, r1, r2);
            }
            break;

        case IR_EQ: case IR_NE: case IR_LT:
        case IR_GE: case IR_LE: case IR_GT:
        case IR_ULT: case IR_UGE: case IR_ULE: case IR_UGT:
            dst = alloc_reg(i);
            {
                /* 浮點比較：operand 的 type 看 op1 的 insn type */
                ir_insn *op1_insn = (op1 > 0 && !IR_IS_CONST_REF(op1)) ? &ctx->ir_base[op1] : NULL;
                int is_float = op1_insn && (op1_insn->type == IR_FLOAT || op1_insn->type == IR_DOUBLE);
                int is_d     = op1_insn && (op1_insn->type == IR_DOUBLE);

                if (is_float) {
                    /* 把兩個 operand 載入 float 暫存器 */
                    const char *fr1 = alloc_freg(op1); emit_fref(f, ctx, fr1, op1, is_d);
                    const char *fr2 = alloc_freg(op2); emit_fref(f, ctx, fr2, op2, is_d);
                    const char *suf = is_d ? "d" : "s";
                    switch (insn->op) {
                    /* feq/flt/fle 直接有對應指令 */
                    case IR_EQ:
                        fprintf(f, "\tfeq.%s\t%s, %s, %s\n", suf, dst, fr1, fr2);
                        break;
                    case IR_NE:
                        fprintf(f, "\tfeq.%s\t%s, %s, %s\n", suf, dst, fr1, fr2);
                        fprintf(f, "\txori\t%s, %s, 1\n", dst, dst);
                        break;
                    case IR_LT:
                        fprintf(f, "\tflt.%s\t%s, %s, %s\n", suf, dst, fr1, fr2);
                        break;
                    case IR_LE:
                        fprintf(f, "\tfle.%s\t%s, %s, %s\n", suf, dst, fr1, fr2);
                        break;
                    case IR_GT:
                        /* a > b  ≡  b < a */
                        fprintf(f, "\tflt.%s\t%s, %s, %s\n", suf, dst, fr2, fr1);
                        break;
                    case IR_GE:
                        /* a >= b  ≡  b <= a */
                        fprintf(f, "\tfle.%s\t%s, %s, %s\n", suf, dst, fr2, fr1);
                        break;
                    default:
                        fprintf(f, "\t# unsupported float cmp op=%d\n", insn->op);
                        break;
                    }
                } else {
                    /* 原整數比較邏輯 */
                    const char *r1, *r2;
                    if (IR_IS_CONST_REF(op1)) {
                        fprintf(f, "\tli\ts5, %lld\n", (long long)ctx->ir_base[op1].val.i64);
                        r1 = "s5";
                    } else r1 = op1 > 0 ? get_reg(op1) : "zero";
                    if (IR_IS_CONST_REF(op2)) {
                        fprintf(f, "\tli\ts6, %lld\n", (long long)ctx->ir_base[op2].val.i64);
                        r2 = "s6";
                    } else r2 = op2 > 0 ? get_reg(op2) : "zero";
                    switch (insn->op) {
                    case IR_EQ:  fprintf(f,"\tsub\t%s,%s,%s\n\tseqz\t%s,%s\n",dst,r1,r2,dst,dst); break;
                    case IR_NE:  fprintf(f,"\tsub\t%s,%s,%s\n\tsnez\t%s,%s\n",dst,r1,r2,dst,dst); break;
                    case IR_LT:  case IR_ULT: fprintf(f,"\tslt\t%s,%s,%s\n",dst,r1,r2); break;
                    case IR_GE:  case IR_UGE: fprintf(f,"\tslt\t%s,%s,%s\n\txori\t%s,%s,1\n",dst,r1,r2,dst,dst); break;
                    case IR_LE:  case IR_ULE: fprintf(f,"\tslt\t%s,%s,%s\n\txori\t%s,%s,1\n",dst,r2,r1,dst,dst); break;
                    case IR_GT:  case IR_UGT: fprintf(f,"\tslt\t%s,%s,%s\n",dst,r2,r1); break;
                    default: break;
                    }
                }
            }
            break;

        case IR_IF: {
            const char *cond;
            if (IR_IS_CONST_REF(op2)) {
                fprintf(f, "\tli\ts5, %lld\n", (long long)ctx->ir_base[op2].val.i64);
                cond = "s5";
            } else cond = get_reg(op2);
            fprintf(f, "\tbeqz\t%s, .Lfalse_%d\n", cond, i);
            break;
        }

        case IR_IF_TRUE:
            /* fall-through */
            break;

        case IR_IF_FALSE:
            fprintf(f, ".Lfalse_%d:\n", op1);
            break;

        case IR_RETURN:
            if (op2 > 0) {
                ir_insn *ret_insn = &ctx->ir_base[op2];
                if (ret_insn->type == IR_FLOAT || ret_insn->type == IR_DOUBLE) {
                    emit_fref(f, ctx, "fa0", op2, ret_insn->type == IR_DOUBLE);
                } else {
                    emit_ref(f, ctx, "a0", op2);
                }
            }
            fprintf(f, "\tld\ts4, 0(sp)\n");
            fprintf(f, "\tld\ts3, 8(sp)\n");
            fprintf(f, "\tld\ts2, 16(sp)\n");
            fprintf(f, "\tld\ts1, 24(sp)\n");
            fprintf(f, "\tld\ts0, 32(sp)\n");
            fprintf(f, "\tld\tra, 40(sp)\n");
            fprintf(f, "\taddi\tsp, sp, 48\n");
            fprintf(f, "\tret\n");
            break;

        case IR_INT2FP: {
            /* int → float/double */
            int is_d = (insn->type == IR_DOUBLE);
            dst = alloc_freg(i);  /* dst 是 float reg */
            const char *src;
            if (IR_IS_CONST_REF(op1)) {
                fprintf(f, "\tli\ts5, %lld\n", (long long)ctx->ir_base[op1].val.i64);
                src = "s5";
            } else src = get_reg(op1);
            /* fcvt.d.l = int64→double, fcvt.s.l = int64→float */
            fprintf(f, "\t%s\t%s, %s\n", is_d ? "fcvt.d.l" : "fcvt.s.l", dst, src);
            break;
        }

        case IR_FP2INT: {
            /* float/double → int */
            ir_insn *src_insn = (op1 > 0 && !IR_IS_CONST_REF(op1)) ? &ctx->ir_base[op1] : NULL;
            int src_is_d = src_insn && (src_insn->type == IR_DOUBLE);
            dst = alloc_reg(i);  /* dst 是 int reg */
            const char *fr1 = alloc_freg(op1); emit_fref(f, ctx, fr1, op1, src_is_d);
            /* fcvt.l.d = double→int64 (round to zero), fcvt.l.s = float→int64 */
            fprintf(f, "\t%s\t%s, %s, rtz\n", src_is_d ? "fcvt.l.d" : "fcvt.l.s", dst, fr1);
            break;
        }

        case IR_FP2FP: {
            /* f32 ↔ f64 */
            int dst_is_d = (insn->type == IR_DOUBLE);
            dst = alloc_freg(i);
            ir_insn *src_insn = (op1 > 0 && !IR_IS_CONST_REF(op1)) ? &ctx->ir_base[op1] : NULL;
            int src_is_d = src_insn && (src_insn->type == IR_DOUBLE);
            const char *fr1 = alloc_freg(op1); emit_fref(f, ctx, fr1, op1, src_is_d);
            if (dst_is_d && !src_is_d)
                fprintf(f, "\tfcvt.d.s\t%s, %s\n", dst, fr1);  /* f32 → f64 */
            else if (!dst_is_d && src_is_d)
                fprintf(f, "\tfcvt.s.d\t%s, %s\n", dst, fr1);  /* f64 → f32 */
            else
                fprintf(f, "\t%s\t%s, %s\n", dst_is_d ? "fmv.d" : "fmv.s", dst, fr1);
            break;
        }

        case IR_ZEXT:
        case IR_SEXT: {
            dst = alloc_reg(i);
            /* bool/int → wider int，直接 mv 就夠（RISC-V register 本來就是 64-bit） */
            if (IR_IS_CONST_REF(op1))
                fprintf(f, "\tli\t%s, %lld\n", dst, (long long)ctx->ir_base[op1].val.i64);
            else
                fprintf(f, "\tmv\t%s, %s\n", dst, get_reg(op1));
            break;
        }

        case IR_TRUNC: {
            dst = alloc_reg(i);
            if (IR_IS_CONST_REF(op1))
                fprintf(f, "\tli\t%s, %lld\n", dst, (long long)ctx->ir_base[op1].val.i64);
            else
                fprintf(f, "\tmv\t%s, %s\n", dst, get_reg(op1));
            break;
        }

        case IR_VAR:
        case IR_VSTORE:
        case IR_VLOAD:
            /* skip virtual vars for now */
            break;

        default:
            fprintf(f, "\t# skip op=%d ref=%d\n", insn->op, i);
            break;
        }
    }

    fprintf(f, "\t.size %s, .-%s\n", name, name);
    return 1;
}
