/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V 64 CPU specific definitions)
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#ifndef IR_RISCV_H
#define IR_RISCV_H

#if defined(IR_TARGET_RISCV64)

# define IR_GP_REGS(_) \
	_(X0,   zero) \
	_(X1,   ra)   \
	_(X2,   sp)   \
	_(X3,   gp)   \
	_(X4,   tp)   \
	_(X5,   t0)   \
	_(X6,   t1)   \
	_(X7,   t2)   \
	_(X8,   s0)   \
	_(X9,   s1)   \
	_(X10,  a0)   \
	_(X11,  a1)   \
	_(X12,  a2)   \
	_(X13,  a3)   \
	_(X14,  a4)   \
	_(X15,  a5)   \
	_(X16,  a6)   \
	_(X17,  a7)   \
	_(X18,  s2)   \
	_(X19,  s3)   \
	_(X20,  s4)   \
	_(X21,  s5)   \
	_(X22,  s6)   \
	_(X23,  s7)   \
	_(X24,  s8)   \
	_(X25,  s9)   \
	_(X26,  s10)  \
	_(X27,  s11)  \
	_(X28,  t3)   \
	_(X29,  t4)   \
	_(X30,  t5)   \
	_(X31,  t6)   \

# define IR_FP_REGS(_) \
	_(F0,   ft0)  _(F1,   ft1)  _(F2,   ft2)  _(F3,   ft3)  \
	_(F4,   ft4)  _(F5,   ft5)  _(F6,   ft6)  _(F7,   ft7)  \
	_(F8,   fs0)  _(F9,   fs1)  \
	_(F10,  fa0)  _(F11,  fa1)  _(F12,  fa2)  _(F13,  fa3)  \
	_(F14,  fa4)  _(F15,  fa5)  _(F16,  fa6)  _(F17,  fa7)  \
	_(F18,  fs2)  _(F19,  fs3)  _(F20,  fs4)  _(F21,  fs5)  \
	_(F22,  fs6)  _(F23,  fs7)  _(F24,  fs8)  _(F25,  fs9)  \
	_(F26,  fs10) _(F27,  fs11) \
	_(F28,  ft8)  _(F29,  ft9)  _(F30,  ft10) _(F31,  ft11) \

#else
# error "Unsupported target architecture"
#endif

#define IR_GP_REG_ENUM(code, name) IR_REG_ ## code,
#define IR_FP_REG_ENUM(code, name) IR_REG_ ## code,

enum _ir_reg {
	_IR_REG_NONE = -1,
	IR_GP_REGS(IR_GP_REG_ENUM)
	IR_FP_REGS(IR_FP_REG_ENUM)
	IR_REG_NUM,
	IR_REG_ALL = IR_REG_NUM, /* special name for regset */
	IR_REG_SET_1,            /* special name for regset */
	IR_REG_SET_2,            /* special name for regset */
	IR_REG_SET_3,            /* special name for regset */
	IR_REG_SET_NUM,
};

#define IR_REG_GP_FIRST IR_REG_X0
#define IR_REG_FP_FIRST IR_REG_F0
#define IR_REG_GP_LAST  (IR_REG_FP_FIRST - 1)
#define IR_REG_FP_LAST  (IR_REG_NUM - 1)

/* RV64 integer registers are natively 64-bit; unlike x86-32,
 * no I64_PAIR/I64_LO/I64_HI trick is needed. */
#define IR_REGSET_64BIT 1

#define IR_REG_STACK_POINTER \
	IR_REG_X2  /* sp */
#define IR_REG_FRAME_POINTER \
	IR_REG_X8  /* s0 / fp */

/* zero, ra, sp, gp, tp are excluded from general allocation:
 * - zero (x0) is hardwired to constant 0
 * - ra (x1) is reserved for CALL/RETURN linkage (first-cut: not allocatable)
 * - sp (x2) is the stack pointer, same role as x86 RSP
 * - gp/tp (x3/x4) are ABI-reserved (global pointer / thread pointer)
 * Revisit ra's fixed status once CALL support beyond leaf functions is added. */
#define IR_REGSET_FIXED \
	IR_REGSET_INTERVAL(IR_REG_X0, IR_REG_X4)

#define IR_REGSET_GP \
	IR_REGSET_DIFFERENCE(IR_REGSET_INTERVAL(IR_REG_GP_FIRST, IR_REG_GP_LAST), IR_REGSET_FIXED)
#define IR_REGSET_FP \
	IR_REGSET_DIFFERENCE(IR_REGSET_INTERVAL(IR_REG_FP_FIRST, IR_REG_FP_LAST), IR_REGSET_FIXED)

/* ABI name aliases (mirrors x86's IR_REG_RAX = IR_REG_R0 pattern) */

/* integer registers */
#define IR_REG_ZERO IR_REG_X0
#define IR_REG_RA   IR_REG_X1
#define IR_REG_SP   IR_REG_X2
#define IR_REG_GP   IR_REG_X3
#define IR_REG_TP   IR_REG_X4
#define IR_REG_T0   IR_REG_X5
#define IR_REG_T1   IR_REG_X6
#define IR_REG_T2   IR_REG_X7
#define IR_REG_S0   IR_REG_X8   /* also used as frame pointer (fp) */
#define IR_REG_FP   IR_REG_X8
#define IR_REG_S1   IR_REG_X9
#define IR_REG_A0   IR_REG_X10
#define IR_REG_A1   IR_REG_X11
#define IR_REG_A2   IR_REG_X12
#define IR_REG_A3   IR_REG_X13
#define IR_REG_A4   IR_REG_X14
#define IR_REG_A5   IR_REG_X15
#define IR_REG_A6   IR_REG_X16
#define IR_REG_A7   IR_REG_X17
#define IR_REG_S2   IR_REG_X18
#define IR_REG_S3   IR_REG_X19
#define IR_REG_S4   IR_REG_X20
#define IR_REG_S5   IR_REG_X21
#define IR_REG_S6   IR_REG_X22
#define IR_REG_S7   IR_REG_X23
#define IR_REG_S8   IR_REG_X24
#define IR_REG_S9   IR_REG_X25
#define IR_REG_S10  IR_REG_X26
#define IR_REG_S11  IR_REG_X27
#define IR_REG_T3   IR_REG_X28
#define IR_REG_T4   IR_REG_X29
#define IR_REG_T5   IR_REG_X30
#define IR_REG_T6   IR_REG_X31

/* floating-point registers */
#define IR_REG_FT0  IR_REG_F0
#define IR_REG_FT1  IR_REG_F1
#define IR_REG_FT2  IR_REG_F2
#define IR_REG_FT3  IR_REG_F3
#define IR_REG_FT4  IR_REG_F4
#define IR_REG_FT5  IR_REG_F5
#define IR_REG_FT6  IR_REG_F6
#define IR_REG_FT7  IR_REG_F7
#define IR_REG_FS0  IR_REG_F8
#define IR_REG_FS1  IR_REG_F9
#define IR_REG_FA0  IR_REG_F10
#define IR_REG_FA1  IR_REG_F11
#define IR_REG_FA2  IR_REG_F12
#define IR_REG_FA3  IR_REG_F13
#define IR_REG_FA4  IR_REG_F14
#define IR_REG_FA5  IR_REG_F15
#define IR_REG_FA6  IR_REG_F16
#define IR_REG_FA7  IR_REG_F17
#define IR_REG_FS2  IR_REG_F18
#define IR_REG_FS3  IR_REG_F19
#define IR_REG_FS4  IR_REG_F20
#define IR_REG_FS5  IR_REG_F21
#define IR_REG_FS6  IR_REG_F22
#define IR_REG_FS7  IR_REG_F23
#define IR_REG_FS8  IR_REG_F24
#define IR_REG_FS9  IR_REG_F25
#define IR_REG_FS10 IR_REG_F26
#define IR_REG_FS11 IR_REG_F27
#define IR_REG_FT8  IR_REG_F28
#define IR_REG_FT9  IR_REG_F29
#define IR_REG_FT10 IR_REG_F30
#define IR_REG_FT11 IR_REG_F31

#endif /* IR_RISCV_H */