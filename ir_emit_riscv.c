/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V assembly text code generator)
 *
 * Design: two-phase operand preparation + post-emit alloc
 *   1. prep_int / prep_flt  : load operand into scratch (t6 / ft11)
 *                             or return its current phys reg
 *   2. emit the instruction
 *   3. alloc_reg / alloc_freg : assign a phys reg to the result
 *                              (may spill someone else, but operands
 *                               are already safe in scratch / stable regs)
 *
 * NOTE ON SCOPE: this emits RISC-V *assembly text*, not machine code into
 * an in-memory code buffer. It does not go through ir_match()/ir_ra.c's
 * register allocator. It plays the same role as ir_emit_c.c / ir_emit_llvm.c
 * (IR -> external-toolchain text), not the same role as ir_emit_aarch64.h /
 * ir_x86.h (IR -> in-memory JIT machine code). Positioned for AOT/standalone
 * `ir` tool use, not for the PHP-JIT in-memory codegen path.
 *
 * Fixes applied in this revision:
 *  - removed dead/unused prep_int() (had a nonsensical pointer expression
 *    and was never called; prep_int_full() is the real implementation).
 *  - comparison ops (EQ/NE/LT/.../UGT) now correctly detect a float
 *    operand even when op1 is a constant reference (previously only
 *    checked type when op1 was a non-const instruction ref, so
 *    "float_const < x" was mis-emitted as an integer compare).
 *  - INT2FP / FP2INT now select the correct fcvt width (w vs l) and
 *    signedness (u suffix) based on the actual source/destination
 *    integer type, instead of always assuming 64-bit signed (.l).
 */

#include "ir.h"
#include "ir_private.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_REFS  4096
#define MAX_SPILL 64
#define MAX_LOOPS 16

/* ── Physical register pools ──────────────────────────────────────────────── */
/* Scratch regs NOT in pool: t6 (int), ft11 (flt), s5/s6 (codegen scratch)  */
/* NOTE: s1-s4, s7-s8 are used for PHI regs — keep them OUT of LSRA pool */
#define NUM_INT_REGS 10
static const char *int_regs[NUM_INT_REGS] = {
    "t0","t1","t2","t3","t4","t5",
    "s9","s10","s11","a6"
};
#define NUM_FLT_REGS 18
static const char *flt_regs[NUM_FLT_REGS] = {
    "ft0","ft1","ft2","ft3","ft4","ft5","ft6","ft7","ft8","ft9","ft10",
    "fs0","fs1","fs2","fs3","fs4","fs5","fs6"
};
#define NUM_PHI_REGS 6
static const char *phi_regs[NUM_PHI_REGS] = {
    "s1","s2","s3","s4","s7","s8"
};
#define NUM_FPHI_REGS 6
static const char *fphi_regs[NUM_FPHI_REGS] = {
    "fs0","fs1","fs2","fs3","fs4","fs5"
};
static const char *param_regs[]  = {"a0","a1","a2","a3","a4","a5","a6","a7"};
static const char *fparam_regs[] = {"fa0","fa1","fa2","fa3","fa4","fa5","fa6","fa7"};

/* ── LSRA state ───────────────────────────────────────────────────────────── */
static const char *reg_map [MAX_REFS];
static const char *freg_map[MAX_REFS];
static const char *phi_reg_map[MAX_REFS];
static int phi_next;
static int fphi_next;

static int    last_use  [MAX_REFS];
static ir_ref int_owner [NUM_INT_REGS];
static ir_ref flt_owner [NUM_FLT_REGS];
static int    current_insn;

static int   spill_map[MAX_REFS];
static int   spill_count;
static FILE *g_f;
static int   frame_size;

/* ── last_use computation ─────────────────────────────────────────────────── */
static void compute_last_use(ir_ctx *ctx) {
    memset(last_use, 0, sizeof(last_use));
    for (ir_ref i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
#define UPD(r) do { \
    ir_ref _r = (r); \
    if (_r > 0 && !IR_IS_CONST_REF(_r) && _r < MAX_REFS) last_use[_r] = i; \
} while(0)
        UPD(insn->op1); UPD(insn->op2); UPD(insn->op3);
#undef UPD
    }
    /* extend last_use across loops */
    ir_ref stk[MAX_LOOPS];
    ir_ref lbegins[MAX_LOOPS], lends[MAX_LOOPS];
    int depth = 0, nloops = 0;
    for (ir_ref i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
        if (insn->op == IR_LOOP_BEGIN && depth < MAX_LOOPS)
            stk[depth++] = i;
        else if (insn->op == IR_LOOP_END && depth > 0) {
            ir_ref lb = stk[--depth];
            if (nloops < MAX_LOOPS) { lbegins[nloops]=lb; lends[nloops]=i; nloops++; }
        }
    }
    for (int l = 0; l < nloops; l++) {
        ir_ref lb = lbegins[l], le = lends[l];
        for (ir_ref i = 1; i < lb; i++)
            if (last_use[i] > lb && last_use[i] < le)
                last_use[i] = le;
    }
}

/* ── Spill helpers ────────────────────────────────────────────────────────── */
static int spill_slot(ir_ref ref) {
    if (spill_map[ref] < 0)
        spill_map[ref] = (spill_count < MAX_SPILL) ? spill_count++ : MAX_SPILL-1;
    return spill_map[ref];
}
static int spill_off(ir_ref ref) { return 56 + spill_slot(ref) * 8; }

/* ── Allocators ───────────────────────────────────────────────────────────── */
static const char *alloc_reg(ir_ref ref) {
    if (ref <= 0 || ref >= MAX_REFS) return "t0";
    if (reg_map[ref]) return reg_map[ref];
    for (int k = 0; k < NUM_INT_REGS; k++) {
        if (int_owner[k] == 0) {
            int_owner[k] = ref; reg_map[ref] = int_regs[k]; return int_regs[k];
        }
    }
    for (int k = 0; k < NUM_INT_REGS; k++) {
        if (last_use[int_owner[k]] < current_insn) {
            reg_map[int_owner[k]] = NULL;
            int_owner[k] = ref; reg_map[ref] = int_regs[k]; return int_regs[k];
        }
    }
    /* spill: evict earliest */
    int best = 0;
    for (int k = 1; k < NUM_INT_REGS; k++)
        if (last_use[int_owner[k]] < last_use[int_owner[best]]) best = k;
    ir_ref ev = int_owner[best];
    fprintf(g_f, "\tsd\t%s, %d(sp)\n", int_regs[best], spill_off(ev));
    reg_map[ev] = NULL;
    int_owner[best] = ref; reg_map[ref] = int_regs[best]; return int_regs[best];
}

static const char *alloc_freg(ir_ref ref) {
    if (ref <= 0 || ref >= MAX_REFS) return "ft0";
    if (freg_map[ref]) return freg_map[ref];
    for (int k = 0; k < NUM_FLT_REGS; k++) {
        if (flt_owner[k] == 0) {
            flt_owner[k] = ref; freg_map[ref] = flt_regs[k]; return flt_regs[k];
        }
    }
    for (int k = 0; k < NUM_FLT_REGS; k++) {
        if (last_use[flt_owner[k]] < current_insn) {
            freg_map[flt_owner[k]] = NULL;
            flt_owner[k] = ref; freg_map[ref] = flt_regs[k]; return flt_regs[k];
        }
    }
    int best = 0;
    for (int k = 1; k < NUM_FLT_REGS; k++)
        if (last_use[flt_owner[k]] < last_use[flt_owner[best]]) best = k;
    ir_ref ev = flt_owner[best];
    fprintf(g_f, "\tfsd\t%s, %d(sp)\n", flt_regs[best], spill_off(ev));
    freg_map[ev] = NULL;
    flt_owner[best] = ref; freg_map[ref] = flt_regs[best]; return flt_regs[best];
}

static const char *get_reg (ir_ref ref) {
    if (ref<=0||ref>=MAX_REFS) return "zero";
    return reg_map[ref] ? reg_map[ref] : "zero";
}
static const char *get_freg(ir_ref ref) {
    if (ref<=0||ref>=MAX_REFS) return "ft0";
    return freg_map[ref] ? freg_map[ref] : "ft0";
}

/* ── Operand preparation ──────────────────────────────────────────────────── */
/*
 * prep_int_full(ref):
 *   - const  → li t6, imm  ; return "t6"
 *   - in reg → return reg_map[ref]
 *   - spilled → ld t6, off(sp) ; return "t6"
 *   - lost   → return "zero"  (should not happen with enough regs)
 *
 * After calling prep_int_full(op1), if result is "t6", caller MUST copy to
 * "s5" before calling prep_int_full(op2), because op2 may also use "t6".
 */
static const char *prep_int_full(ir_ctx *ctx, ir_ref ref) {
    if (IR_IS_CONST_REF(ref)) {
        fprintf(g_f, "\tli\tt6, %lld\n", (long long)ctx->ir_base[ref].val.i64);
        return "t6";
    }
    if (ref <= 0 || ref >= MAX_REFS) return "zero";
    if (reg_map[ref]) return reg_map[ref];
    if (spill_map[ref] >= 0) {
        fprintf(g_f, "\tld\tt6, %d(sp)\n", spill_off(ref));
        return "t6";
    }
    return "zero";
}

static const char *prep_flt_full(ir_ctx *ctx, ir_ref ref, int is_d) {
    if (IR_IS_CONST_REF(ref)) {
        ir_insn *c = &ctx->ir_base[ref];
        if (is_d) {
            fprintf(g_f, "\tli\ts6, %llu\n", (unsigned long long)c->val.u64);
            fprintf(g_f, "\tsd\ts6, -8(sp)\n");
            fprintf(g_f, "\tfld\tft11, -8(sp)\n");
        } else {
            fprintf(g_f, "\tli\ts6, %u\n", c->val.u32);
            fprintf(g_f, "\tsw\ts6, -4(sp)\n");
            fprintf(g_f, "\tflw\tft11, -4(sp)\n");
        }
        return "ft11";
    }
    if (ref <= 0 || ref >= MAX_REFS) return "ft0";
    if (freg_map[ref]) return freg_map[ref];
    if (spill_map[ref] >= 0) {
        fprintf(g_f, "\t%s\tft11, %d(sp)\n", is_d ? "fld" : "flw", spill_off(ref));
        return "ft11";
    }
    return "ft0";
}

/*
 * prep2_int: prepare two integer operands safely.
 * op1 → r1buf,  op2 → r2buf
 * If r1 = "t6", copy to "s5" before computing r2.
 */
static void prep2_int(ir_ctx *ctx, ir_ref op1, ir_ref op2,
                      char *r1buf, char *r2buf) {
    const char *r1 = prep_int_full(ctx, op1);
    if (strcmp(r1, "t6") == 0) {
        fprintf(g_f, "\tmv\ts5, t6\n");
        r1 = "s5";
    }
    strcpy(r1buf, r1);
    const char *r2 = prep_int_full(ctx, op2);
    strcpy(r2buf, r2);
}

static void prep2_flt(ir_ctx *ctx, ir_ref op1, ir_ref op2, int is_d,
                      char *r1buf, char *r2buf) {
    const char *r1 = prep_flt_full(ctx, op1, is_d);
    if (strcmp(r1, "ft11") == 0) {
        fprintf(g_f, "\t%s\tfs6, ft11\n", is_d ? "fmv.d" : "fmv.s");
        r1 = "fs6";
    }
    strcpy(r1buf, r1);
    const char *r2 = prep_flt_full(ctx, op2, is_d);
    strcpy(r2buf, r2);
}

/* ── emit_ref / emit_fref (for PHI init/update) ─────────────────────────── */
static void emit_ref(ir_ref ref, const char *dst, ir_ctx *ctx) {
    if (IR_IS_CONST_REF(ref)) {
        fprintf(g_f, "\tli\t%s, %lld\n", dst, (long long)ctx->ir_base[ref].val.i64);
    } else {
        const char *src = get_reg(ref);
        if (strcmp(dst, src) != 0)
            fprintf(g_f, "\tmv\t%s, %s\n", dst, src);
    }
}
static void emit_fref(ir_ref ref, const char *dst, int is_d, ir_ctx *ctx) {
    if (IR_IS_CONST_REF(ref)) {
        ir_insn *c = &ctx->ir_base[ref];
        if (is_d) {
            fprintf(g_f, "\tli\ts6, %llu\n", (unsigned long long)c->val.u64);
            fprintf(g_f, "\tsd\ts6, -8(sp)\n");
            fprintf(g_f, "\tfld\t%s, -8(sp)\n", dst);
        } else {
            fprintf(g_f, "\tli\ts6, %u\n", c->val.u32);
            fprintf(g_f, "\tsw\ts6, -4(sp)\n");
            fprintf(g_f, "\tflw\t%s, -4(sp)\n", dst);
        }
    } else {
        const char *src = get_freg(ref);
        if (strcmp(dst, src) != 0)
            fprintf(g_f, "\t%s\t%s, %s\n", is_d ? "fmv.d" : "fmv.s", dst, src);
    }
}

/* ── PHI helpers ──────────────────────────────────────────────────────────── */
static void emit_phi_init(ir_ctx *ctx, ir_ref loop_begin) {
    for (ir_ref i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
        if (insn->op != IR_PHI || insn->op1 != loop_begin) continue;
        int is_float = (insn->type == IR_FLOAT || insn->type == IR_DOUBLE);
        int is_d = (insn->type == IR_DOUBLE);
        if (is_float) {
            if (!phi_reg_map[i]) {
                phi_reg_map[i] = fphi_regs[fphi_next % NUM_FPHI_REGS];
                fphi_next++;
            }
            freg_map[i] = phi_reg_map[i];
            emit_fref(insn->op2, phi_reg_map[i], is_d, ctx);
        } else {
            if (!phi_reg_map[i]) {
                phi_reg_map[i] = phi_regs[phi_next % NUM_PHI_REGS];
                phi_next++;
            }
            reg_map[i] = phi_reg_map[i];
            emit_ref(insn->op2, phi_reg_map[i], ctx);
        }
    }
}

static void emit_phi_update(ir_ctx *ctx, ir_ref loop_begin) {
    static const char *scratch[]  = {"s5","s6","a5","a4","a3","a2"};
    static const char *fscratch[] = {"ft8","ft9","ft10","fs6","fs5","fs4"};
    int sc = 0, fsc = 0;
    /* first pass: load back-edge values into scratch */
    for (ir_ref i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
        if (insn->op != IR_PHI || insn->op1 != loop_begin) continue;
        int is_float = (insn->type == IR_FLOAT || insn->type == IR_DOUBLE);
        int is_d = (insn->type == IR_DOUBLE);
        if (is_float) {
            emit_fref(insn->op3, fscratch[fsc % 6], is_d, ctx);
            fsc++;
        } else {
            emit_ref(insn->op3, scratch[sc % 6], ctx);
            sc++;
        }
    }
    /* second pass: move scratch to phi regs */
    sc = 0; fsc = 0;
    for (ir_ref i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
        if (insn->op != IR_PHI || insn->op1 != loop_begin) continue;
        int is_float = (insn->type == IR_FLOAT || insn->type == IR_DOUBLE);
        int is_d = (insn->type == IR_DOUBLE);
        if (is_float) {
            const char *dst = phi_reg_map[i] ? phi_reg_map[i] : get_freg(i);
            const char *s   = fscratch[fsc % 6];
            if (strcmp(dst, s) != 0)
                fprintf(g_f, "\t%s\t%s, %s\n", is_d ? "fmv.d":"fmv.s", dst, s);
            fsc++;
        } else {
            const char *dst = phi_reg_map[i] ? phi_reg_map[i] : get_reg(i);
            const char *s   = scratch[sc % 6];
            if (strcmp(dst, s) != 0)
                fprintf(g_f, "\tmv\t%s, %s\n", dst, s);
            sc++;
        }
    }
}

/* ── Main emit function ───────────────────────────────────────────────────── */
int ir_emit_riscv(ir_ctx *ctx, const char *name, FILE *f)
{
    g_f = f;
    memset(reg_map,      0, sizeof(reg_map));
    memset(freg_map,     0, sizeof(freg_map));
    memset(phi_reg_map,  0, sizeof(phi_reg_map));
    memset(last_use,     0, sizeof(last_use));
    memset(int_owner,    0, sizeof(int_owner));
    memset(flt_owner,    0, sizeof(flt_owner));
    memset(spill_map,   -1, sizeof(spill_map));
    phi_next  = 0;
    fphi_next = 0;
    spill_count = 0;
    current_insn = 0;
    compute_last_use(ctx);

    frame_size = 56 + MAX_SPILL * 8;   /* 56 = ra+s0..s4 area; rest = spill */

    fprintf(f, "\t.text\n");
    fprintf(f, "\t.globl %s\n", name);
    fprintf(f, "\t.type %s, @function\n", name);
    fprintf(f, "%s:\n", name);
    fprintf(f, "\taddi\tsp, sp, -%d\n", frame_size);
    fprintf(f, "\tsd\tra, %d(sp)\n", frame_size - 8);
    fprintf(f, "\tsd\ts0, %d(sp)\n", frame_size - 16);
    fprintf(f, "\tsd\ts1, %d(sp)\n", frame_size - 24);
    fprintf(f, "\tsd\ts2, %d(sp)\n", frame_size - 32);
    fprintf(f, "\tsd\ts3, %d(sp)\n", frame_size - 40);
    fprintf(f, "\tsd\ts4, %d(sp)\n", frame_size - 48);
    fprintf(f, "\taddi\ts0, sp, %d\n", frame_size);

    /* assign params */
    int param_idx = 0, fparam_idx = 0;
    for (ir_ref i = 1; i < ctx->insns_count; i++) {
        ir_insn *insn = &ctx->ir_base[i];
        if (insn->op == IR_PARAM) {
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                if (fparam_idx < 8) freg_map[i] = fparam_regs[fparam_idx++];
            } else {
                if (param_idx < 8) reg_map[i] = param_regs[param_idx++];
            }
        }
    }

    /* main emit loop */
    for (ir_ref i = 1; i < ctx->insns_count; i++) {
        current_insn = i;
        ir_insn *insn = &ctx->ir_base[i];
        ir_ref op1 = insn->op1;
        ir_ref op2 = insn->op2;
        const char *dst;
        char r1buf[16], r2buf[16];

        switch (insn->op) {

        case IR_NOP:
        case IR_START:
        case IR_PARAM:
            break;

        case IR_END:
            /* if inside if-true branch → jump over false branch */
            {
                ir_insn *parent = (op1 > 0) ? &ctx->ir_base[op1] : NULL;
                if (parent && parent->op == IR_IF_TRUE) {
                    ir_ref if_ref = parent->op1;
                    for (ir_ref m = i+1; m < ctx->insns_count; m++) {
                        if (ctx->ir_base[m].op == IR_MERGE) {
                            fprintf(f, "\tj\t.Lend_%d\n", if_ref);
                            break;
                        }
                    }
                }
                /* if/else PHI update for this branch */
                int is_if_true  = parent && parent->op == IR_IF_TRUE;
                int is_if_false = parent && parent->op == IR_IF_FALSE;
                if (is_if_true || is_if_false) {
                    for (ir_ref m = i+1; m < ctx->insns_count; m++) {
                        ir_insn *mi = &ctx->ir_base[m];
                        if (mi->op == IR_MERGE) {
                            for (ir_ref p = m+1; p < ctx->insns_count; p++) {
                                ir_insn *pi = &ctx->ir_base[p];
                                if (pi->op != IR_PHI || pi->op1 != m) break;
                                ir_ref val = is_if_true ? pi->op2 : pi->op3;
                                if (pi->type == IR_FLOAT || pi->type == IR_DOUBLE) {
                                    int is_d = (pi->type == IR_DOUBLE);
                                    const char *pdst = alloc_freg(p);
                                    emit_fref(val, pdst, is_d, ctx);
                                } else {
                                    const char *pdst = alloc_reg(p);
                                    emit_ref(val, pdst, ctx);
                                }
                            }
                            break;
                        }
                    }
                }
            }
            break;

        case IR_LOOP_BEGIN:
            emit_phi_init(ctx, i);
            fprintf(f, ".Lloop_%d:\n", i);
            break;

        case IR_LOOP_END: {
            /* find corresponding LOOP_BEGIN via its op2 back-edge */
            ir_ref lb = 0;
            for (ir_ref m = 1; m < ctx->insns_count; m++) {
                if (ctx->ir_base[m].op == IR_LOOP_BEGIN && ctx->ir_base[m].op2 == i) {
                    lb = m; break;
                }
            }
            if (!lb) { lb = op1; while (lb>0 && ctx->ir_base[lb].op!=IR_LOOP_BEGIN) lb=ctx->ir_base[lb].op1; }
            emit_phi_update(ctx, lb);
            fprintf(f, "\tj\t.Lloop_%d\n", lb);
            break;
        }

        case IR_PHI:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                if (!phi_reg_map[i]) {
                    phi_reg_map[i] = fphi_regs[fphi_next % NUM_FPHI_REGS];
                    fphi_next++;
                }
                freg_map[i] = phi_reg_map[i];
            } else {
                if (!phi_reg_map[i]) {
                    phi_reg_map[i] = phi_regs[phi_next % NUM_PHI_REGS];
                    phi_next++;
                }
                reg_map[i] = phi_reg_map[i];
            }
            break;

        case IR_BEGIN:
        case IR_MERGE:
            fprintf(f, ".Lbb_%d:\n", i);
            {
                ir_insn *p1 = (insn->op1>0) ? &ctx->ir_base[insn->op1] : NULL;
                ir_insn *p2 = (insn->op2>0) ? &ctx->ir_base[insn->op2] : NULL;
                ir_ref if_ref = 0;
                if (p1 && p1->op == IR_IF_FALSE) if_ref = p1->op1;
                else if (p2 && p2->op == IR_IF_FALSE) if_ref = p2->op1;
                /* also check END→IF_FALSE */
                if (!if_ref) {
                    if (p1 && p1->op == IR_END && p1->op1>0 && ctx->ir_base[p1->op1].op == IR_IF_FALSE)
                        if_ref = ctx->ir_base[p1->op1].op1;
                    else if (p2 && p2->op == IR_END && p2->op1>0 && ctx->ir_base[p2->op1].op == IR_IF_FALSE)
                        if_ref = ctx->ir_base[p2->op1].op1;
                }
                if (if_ref) fprintf(f, ".Lend_%d:\n", if_ref);
            }
            break;

        case IR_COPY:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                const char *r1 = prep_flt_full(ctx, op1, is_d);
                dst = alloc_freg(i);
                if (strcmp(r1, dst) != 0)
                    fprintf(f, "\t%s\t%s, %s\n", is_d?"fmv.d":"fmv.s", dst, r1);
            } else {
                const char *r1 = prep_int_full(ctx, op1);
                dst = alloc_reg(i);
                if (strcmp(r1, dst) != 0)
                    fprintf(f, "\tmv\t%s, %s\n", dst, r1);
            }
            break;

        case IR_ADD:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                prep2_flt(ctx, op1, op2, is_d, r1buf, r2buf);
                dst = alloc_freg(i);
                fprintf(f, "\t%s\t%s, %s, %s\n", is_d?"fadd.d":"fadd.s", dst, r1buf, r2buf);
            } else {
                prep2_int(ctx, op1, op2, r1buf, r2buf);
                dst = alloc_reg(i);
                fprintf(f, "\tadd\t%s, %s, %s\n", dst, r1buf, r2buf);
            }
            break;

        case IR_SUB:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                prep2_flt(ctx, op1, op2, is_d, r1buf, r2buf);
                dst = alloc_freg(i);
                fprintf(f, "\t%s\t%s, %s, %s\n", is_d?"fsub.d":"fsub.s", dst, r1buf, r2buf);
            } else {
                prep2_int(ctx, op1, op2, r1buf, r2buf);
                dst = alloc_reg(i);
                fprintf(f, "\tsub\t%s, %s, %s\n", dst, r1buf, r2buf);
            }
            break;

        case IR_MUL:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                prep2_flt(ctx, op1, op2, is_d, r1buf, r2buf);
                dst = alloc_freg(i);
                fprintf(f, "\t%s\t%s, %s, %s\n", is_d?"fmul.d":"fmul.s", dst, r1buf, r2buf);
            } else {
                prep2_int(ctx, op1, op2, r1buf, r2buf);
                dst = alloc_reg(i);
                fprintf(f, "\tmul\t%s, %s, %s\n", dst, r1buf, r2buf);
            }
            break;

        case IR_DIV:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                prep2_flt(ctx, op1, op2, is_d, r1buf, r2buf);
                dst = alloc_freg(i);
                fprintf(f, "\t%s\t%s, %s, %s\n", is_d?"fdiv.d":"fdiv.s", dst, r1buf, r2buf);
            } else {
                prep2_int(ctx, op1, op2, r1buf, r2buf);
                dst = alloc_reg(i);
                fprintf(f, "\tdiv\t%s, %s, %s\n", dst, r1buf, r2buf);
            }
            break;

        case IR_EQ: case IR_NE: case IR_LT:
        case IR_GE: case IR_LE: case IR_GT:
        case IR_ULT: case IR_UGE: case IR_ULE: case IR_UGT: {
            /* FIX: previously only inspected op1's type when op1 was a
             * non-const ref, so a float *constant* on the left-hand side
             * (op1) was silently treated as an integer compare. ir_base[]
             * is valid for negative (constant) refs too — the context
             * comment in ir.h describes it as a two-directional array —
             * so we can safely dereference op1 whenever it isn't
             * IR_UNUSED, const or not. */
            ir_insn *op1_insn = (op1 != IR_UNUSED) ? &ctx->ir_base[op1] : NULL;
            int is_float = op1_insn && (op1_insn->type==IR_FLOAT||op1_insn->type==IR_DOUBLE);
            int is_d     = op1_insn && (op1_insn->type==IR_DOUBLE);
            if (is_float) {
                prep2_flt(ctx, op1, op2, is_d, r1buf, r2buf);
                dst = alloc_reg(i);
                const char *suf = is_d ? "d" : "s";
                switch(insn->op) {
                case IR_EQ: fprintf(f,"\tfeq.%s\t%s, %s, %s\n",suf,dst,r1buf,r2buf); break;
                case IR_NE: fprintf(f,"\tfeq.%s\t%s, %s, %s\n\txori\t%s,%s,1\n",suf,dst,r1buf,r2buf,dst,dst); break;
                case IR_LT: fprintf(f,"\tflt.%s\t%s, %s, %s\n",suf,dst,r1buf,r2buf); break;
                case IR_LE: fprintf(f,"\tfle.%s\t%s, %s, %s\n",suf,dst,r1buf,r2buf); break;
                case IR_GT: fprintf(f,"\tflt.%s\t%s, %s, %s\n",suf,dst,r2buf,r1buf); break;
                case IR_GE: fprintf(f,"\tfle.%s\t%s, %s, %s\n",suf,dst,r2buf,r1buf); break;
                default: break;
                }
            } else {
                prep2_int(ctx, op1, op2, r1buf, r2buf);
                dst = alloc_reg(i);
                switch(insn->op) {
                case IR_EQ:  fprintf(f,"\tsub\t%s,%s,%s\n\tseqz\t%s,%s\n",dst,r1buf,r2buf,dst,dst); break;
                case IR_NE:  fprintf(f,"\tsub\t%s,%s,%s\n\tsnez\t%s,%s\n",dst,r1buf,r2buf,dst,dst); break;
                case IR_LT: case IR_ULT: fprintf(f,"\tslt\t%s,%s,%s\n",dst,r1buf,r2buf); break;
                case IR_GE: case IR_UGE: fprintf(f,"\tslt\t%s,%s,%s\n\txori\t%s,%s,1\n",dst,r1buf,r2buf,dst,dst); break;
                case IR_LE: case IR_ULE: fprintf(f,"\tslt\t%s,%s,%s\n\txori\t%s,%s,1\n",dst,r2buf,r1buf,dst,dst); break;
                case IR_GT: case IR_UGT: fprintf(f,"\tslt\t%s,%s,%s\n",dst,r2buf,r1buf); break;
                default: break;
                }
            }
            break;
        }

        case IR_IF: {
            const char *cond = prep_int_full(ctx, op2);
            fprintf(f, "\tbeqz\t%s, .Lfalse_%d\n", cond, i);
            break;
        }

        case IR_IF_TRUE:
            break;

        case IR_IF_FALSE:
            fprintf(f, ".Lfalse_%d:\n", op1);
            break;

        case IR_RETURN:
            if (op2 > 0) {
                ir_insn *ret = &ctx->ir_base[op2];
                if (ret->type == IR_FLOAT || ret->type == IR_DOUBLE) {
                    int is_d = (ret->type == IR_DOUBLE);
                    const char *r = prep_flt_full(ctx, op2, is_d);
                    if (strcmp(r, "fa0") != 0)
                        fprintf(f, "\t%s\tfa0, %s\n", is_d?"fmv.d":"fmv.s", r);
                } else {
                    const char *r = prep_int_full(ctx, op2);
                    if (strcmp(r, "a0") != 0)
                        fprintf(f, "\tmv\ta0, %s\n", r);
                }
            }
            fprintf(f, "\tld\ts4, %d(sp)\n", frame_size - 48);
            fprintf(f, "\tld\ts3, %d(sp)\n", frame_size - 40);
            fprintf(f, "\tld\ts2, %d(sp)\n", frame_size - 32);
            fprintf(f, "\tld\ts1, %d(sp)\n", frame_size - 24);
            fprintf(f, "\tld\ts0, %d(sp)\n", frame_size - 16);
            fprintf(f, "\tld\tra, %d(sp)\n", frame_size - 8);
            fprintf(f, "\taddi\tsp, sp, %d\n", frame_size);
            fprintf(f, "\tret\n");
            break;

        /* Type conversion */
        case IR_ZEXT:
        case IR_SEXT:
        case IR_TRUNC: {
            const char *r1 = prep_int_full(ctx, op1);
            dst = alloc_reg(i);
            if (strcmp(r1, dst) != 0) fprintf(f, "\tmv\t%s, %s\n", dst, r1);
            break;
        }

        case IR_INT2FP: {
            /* FIX: pick fcvt width (w/l) and signedness (u suffix) from
             * the *source* integer type instead of always assuming a
             * 64-bit signed source (previously hardcoded fcvt.?.l). */
            int is_d = (insn->type == IR_DOUBLE);
            ir_insn *si = &ctx->ir_base[op1];
            int src_is_64 = (ir_type_size[si->type] == 8);
            int src_unsigned = IR_IS_TYPE_UNSIGNED(si->type);
            const char *width = src_is_64 ? "l" : "w";
            const char *usuf  = src_unsigned ? "u" : "";
            const char *r1 = prep_int_full(ctx, op1);
            dst = alloc_freg(i);
            fprintf(f, "\tfcvt.%s.%s%s\t%s, %s\n", is_d?"d":"s", width, usuf, dst, r1);
            break;
        }

        case IR_FP2INT: {
            /* FIX: pick fcvt width (w/l) and signedness (u suffix) from
             * the *destination* integer type instead of always assuming
             * a 64-bit signed destination (previously hardcoded fcvt.l.?). */
            ir_insn *si = (op1>0&&!IR_IS_CONST_REF(op1)) ? &ctx->ir_base[op1] : NULL;
            int src_is_d = si && (si->type == IR_DOUBLE);
            int dst_is_64 = (ir_type_size[insn->type] == 8);
            int dst_unsigned = IR_IS_TYPE_UNSIGNED(insn->type);
            const char *width = dst_is_64 ? "l" : "w";
            const char *usuf  = dst_unsigned ? "u" : "";
            const char *r1 = prep_flt_full(ctx, op1, src_is_d);
            dst = alloc_reg(i);
            fprintf(f, "\tfcvt.%s%s.%s\t%s, %s, rtz\n", width, usuf, src_is_d?"d":"s", dst, r1);
            break;
        }

        case IR_FP2FP: {
            int dst_is_d = (insn->type == IR_DOUBLE);
            ir_insn *si = (op1>0&&!IR_IS_CONST_REF(op1)) ? &ctx->ir_base[op1] : NULL;
            int src_is_d = si && (si->type == IR_DOUBLE);
            const char *r1 = prep_flt_full(ctx, op1, src_is_d);
            dst = alloc_freg(i);
            if (dst_is_d && !src_is_d)
                fprintf(f, "\tfcvt.d.s\t%s, %s\n", dst, r1);
            else if (!dst_is_d && src_is_d)
                fprintf(f, "\tfcvt.s.d\t%s, %s\n", dst, r1);
            else if (strcmp(r1, dst) != 0)
                fprintf(f, "\t%s\t%s, %s\n", dst_is_d?"fmv.d":"fmv.s", dst, r1);
            break;
        }

        case IR_LOAD: {
            const char *addr = prep_int_full(ctx, insn->op2);
            /* addr might be t6; if alloc_reg evicts something, addr is safe
             * because it's in t6 (scratch, not in pool) */
            if (insn->type == IR_FLOAT) {
                dst = alloc_freg(i);
                fprintf(f, "\tflw\t%s, 0(%s)\n", dst, addr);
            } else if (insn->type == IR_DOUBLE) {
                dst = alloc_freg(i);
                fprintf(f, "\tfld\t%s, 0(%s)\n", dst, addr);
            } else {
                dst = alloc_reg(i);
                fprintf(f, "\tld\t%s, 0(%s)\n", dst, addr);
            }
            break;
        }

        case IR_STORE: {
            /* prepare addr first, save to s5 since prep_flt may use t6 */
            const char *addr = prep_int_full(ctx, insn->op2);
            if (strcmp(addr, "t6") == 0) { fprintf(f, "\tmv\ts5, t6\n"); addr = "s5"; }
            char addrbuf[16]; strcpy(addrbuf, addr);
            ir_ref val_ref = insn->op3;
            ir_insn *vi = (val_ref>0&&!IR_IS_CONST_REF(val_ref)) ? &ctx->ir_base[val_ref] : NULL;
            int is_float = vi && (vi->type==IR_FLOAT||vi->type==IR_DOUBLE);
            if (is_float) {
                int is_d = vi->type == IR_DOUBLE;
                const char *vr = prep_flt_full(ctx, val_ref, is_d);
                fprintf(f, "\t%s\t%s, 0(%s)\n", is_d?"fsd":"fsw", vr, addrbuf);
            } else {
                const char *vr = prep_int_full(ctx, val_ref);
                fprintf(f, "\tsd\t%s, 0(%s)\n", vr, addrbuf);
            }
            break;
        }

        /* Comparison ops already handled above in IR_EQ..IR_UGT */

        /* ABS */
        case IR_ABS:
            if (insn->type == IR_FLOAT || insn->type == IR_DOUBLE) {
                int is_d = (insn->type == IR_DOUBLE);
                const char *r1 = prep_flt_full(ctx, op1, is_d);
                dst = alloc_freg(i);
                fprintf(f, "\t%s\t%s, %s\n", is_d?"fabs.d":"fabs.s", dst, r1);
            } else {
                /* integer abs: neg + max */
                const char *r1 = prep_int_full(ctx, op1);
                dst = alloc_reg(i);
                fprintf(f, "\tneg\tt6, %s\n", r1);
                fprintf(f, "\tmax\t%s, %s, t6\n", dst, r1);
            }
            break;

        case IR_VAR:
        case IR_VSTORE:
        case IR_VLOAD:
            break;

        default:
            fprintf(f, "\t# skip op=%d ref=%d\n", insn->op, i);
            break;
        }
    }

    fprintf(f, "\t.size %s, .-%s\n", name, name);
    return 1;
}