#ifndef IR_X86_H
#define IR_X86_H

#if defined(__x86_64__) || defined(_WIN64)
# define IR_GP_REGS(_) \
	_(R0,   rax,   eax,   ax,   al, ah) \
	_(R1,   rcx,   ecx,   cx,   cl, ch) \
	_(R2,   rdx,   edx,   dx,   dl, dh) \
	_(R3,   rbx,   ebx,   bx,   bl, bh) \
	_(R4,   rsp,   esp,   __,   __, __) \
	_(R5,   rbp,   ebp,   bp,  r5b, __) \
	_(R6,   rsi,   esi,   si,  r6b, __) \
	_(R7,   rdi,   edi,   di,  r7b, __) \
	_(R8,   r8,    r8d,  r8w,  r8b, __) \
	_(R9,   r9,    r9d,  r9w,  r9b, __) \
	_(R10,  r10,  r10d, r10w, r10b, __) \
	_(R11,  r11,  r11d, r11w, r11b, __) \
	_(R12,  r12,  r12d, r12w, r12b, __) \
	_(R13,  r13,  r13d, r13w, r13b, __) \
	_(R14,  r14,  r14d, r14w, r14b, __) \
	_(R15,  r15,  r15d, r15w, r15b, __) \

# define IR_FP_REGS(_) \
	_(XMM0,  xmm0) \
	_(XMM1,  xmm1) \
	_(XMM2,  xmm2) \
	_(XMM3,  xmm3) \
	_(XMM4,  xmm4) \
	_(XMM5,  xmm5) \
	_(XMM6,  xmm6) \
	_(XMM7,  xmm7) \
	_(XMM8,  xmm8) \
	_(XMM9,  xmm9) \
	_(XMM10, xmm10) \
	_(XMM11, xmm11) \
	_(XMM12, xmm12) \
	_(XMM13, xmm13) \
	_(XMM14, xmm14) \
	_(XMM15, xmm15) \

#else

# define IR_GP_REGS(_) \
	_(R0,   ___,   eax,   ax,   al, ah) \
	_(R1,   ___,   ecx,   cx,   cl, ch) \
	_(R2,   ___,   edx,   dx,   dl, dh) \
	_(R3,   ___,   ebx,   bx,   bl, bh) \
	_(R4,   ___,   esp,   __,   __, __) \
	_(R5,   ___,   ebp,   bp,   __, __) \
	_(R6,   ___,   esi,   si,   __, __) \
	_(R7,   ___,   edi,   di,   __, __) \

# define IR_FP_REGS(_) \
	_(XMM0,  xmm0) \
	_(XMM1,  xmm1) \
	_(XMM2,  xmm2) \
	_(XMM3,  xmm3) \
	_(XMM4,  xmm4) \
	_(XMM5,  xmm5) \
	_(XMM6,  xmm6) \
	_(XMM7,  xmm7) \

#endif

#define IR_GP_REG_ENUM(code, name64, name32, name16, name8, name8h) \
	IR_REG_ ## code,

#define IR_FP_REG_ENUM(code, name) \
	IR_REG_ ## code,

typedef enum _ir_reg {
	IR_REG_NONE = -1,
	IR_GP_REGS(IR_GP_REG_ENUM)
	IR_FP_REGS(IR_FP_REG_ENUM)
	IR_REG_NUM,
} ir_reg;

/* TODO: separate common code */
#if (IR_REG_NUM <= 32)
# define IR_REGSET_64BIT 0
#else
# define IR_REGSET_64BIT 1
#endif

#if IR_REGSET_64BIT
typedef uint64_t ir_regset;
#else
typedef uint32_t ir_regset;
#endif

#define IR_REGSET_EMPTY 0

#define IR_REGSET_IS_EMPTY(regset) \
	(regset == IR_REGSET_EMPTY)

#define IR_REGSET_IS_SINGLETON(regset) \
	(regset && !(regset & (regset - 1)))

#if (!IR_REGSET_64BIT)
#define IR_REGSET(reg) \
	(1u << (reg))
#else
#define IR_REGSET(reg) \
	(1ull << (reg))
#endif

#if (!IR_REGSET_64BIT)
#define IR_REGSET_INTERVAL(reg1, reg2) \
	(((1u << ((reg2) - (reg1) + 1)) - 1) << (reg1))
#else
#define IR_REGSET_INTERVAL(reg1, reg2) \
	(((1ull << ((reg2) - (reg1) + 1)) - 1) << (reg1))
#endif

#define IR_REGSET_IN(regset, reg) \
	(((regset) & IR_REGSET(reg)) != 0)

#define IR_REGSET_INCL(regset, reg) \
	(regset) |= IR_REGSET(reg)

#define IR_REGSET_EXCL(regset, reg) \
	(regset) &= ~IR_REGSET(reg)

#define IR_REGSET_UNION(set1, set2) \
	((set1) | (set2))

#define IR_REGSET_INTERSECTION(set1, set2) \
	((set1) & (set2))

#define IR_REGSET_DIFFERENCE(set1, set2) \
	((set1) & ~(set2))

#if !defined(_WIN32)
# if (!IR_REGSET_64BIT)
#  define IR_REGSET_FIRST(set) ((ir_reg)__builtin_ctz(set))
#  define IR_REGSET_LAST(set)  ((ir_reg)(__builtin_clz(set)^31))
# else
#  define IR_REGSET_FIRST(set) ((ir_reg)__builtin_ctzll(set))
#  define ir_REGSET_LAST(set)  ((ir_reg)(__builtin_clzll(set)^63))
# endif
#else
# include <intrin.h>
uint32_t __inline __ir_ctz(uint32_t value) {
	DWORD trailing_zero = 0;
	if (_BitScanForward(&trailing_zero, value)) {
		return trailing_zero;
	}
	return 32;
}
uint32_t __inline __ir_clz(uint32_t value) {
	DWORD leading_zero = 0;
	if (_BitScanReverse(&leading_zero, value)) {
		return 31 - leading_zero;
	}
	return 32;
}
# define IR_REGSET_FIRST(set) ((ir_reg)__ir_ctz(set))
# define IR_REGSET_LAST(set)  ((ir_reg)(__ir_clz(set)^31))
#endif

#define IR_REGSET_FOREACH(set, reg) \
	do { \
		ir_regset _tmp = (set); \
		while (!IR_REGSET_IS_EMPTY(_tmp)) { \
			ir_reg _reg = IR_REGSET_FIRST(_tmp); \
			IR_REGSET_EXCL(_tmp, _reg); \
			reg = _reg; \

#define IR_REGSET_FOREACH_END() \
		} \
	} while (0)


#define IR_REG_RAX IR_REG_R0
#define IR_REG_RCX IR_REG_R1
#define IR_REG_RDX IR_REG_R2
#define IR_REG_RBX IR_REG_R3
#define IR_REG_RSP IR_REG_R4
#define IR_REG_RBP IR_REG_R5
#define IR_REG_RSI IR_REG_R6
#define IR_REG_RDI IR_REG_R7

#define IR_REG_GP_FIRST IR_REG_R0
#define IR_REG_FP_FIRST IR_REG_XMM0
#define IR_REG_GP_LAST  (IR_REG_FP_FIRST - 1)
#define IR_REG_FP_LAST  (IR_REG_NUM - 1)

#define IR_REG_STACK_POINTER \
	IR_REG_RSP
#define IR_REG_FRAME_POINTER \
	IR_REG_RBP
#define IR_REGSET_FIXED \
	(IR_REGSET(IR_REG_RSP))
#define IR_REGSET_GP \
	IR_REGSET_DIFFERENCE(IR_REGSET_INTERVAL(IR_REG_GP_FIRST, IR_REG_GP_LAST), IR_REGSET_FIXED)
#define IR_REGSET_FP \
	IR_REGSET_DIFFERENCE(IR_REGSET_INTERVAL(IR_REG_FP_FIRST, IR_REG_FP_LAST), IR_REGSET_FIXED)

/* Calling Convention */
#ifdef _WIN64

# define IR_REG_INT_RET1 IR_REG_RAX
# define IR_REG_FP_RET1  IR_REG_XMM0
# define IR_REG_INT_ARGS 4
# define IR_REG_FP_ARGS  4
# define IR_REG_INT_ARG1 IR_REG_RCX
# define IR_REG_INT_ARG2 IR_REG_RDX
# define IR_REG_INT_ARG3 IR_REG_R8
# define IR_REG_INT_ARG4 IR_REG_R9
# define IR_REG_FP_ARG1  IR_REG_XMM0
# define IR_REG_FP_ARG2  IR_REG_XMM1
# define IR_REG_FP_ARG3  IR_REG_XMM2
# define IR_REG_FP_ARG4  IR_REG_XMM3

# define IR_REGSET_SCRATCH \
	(IR_REGSET_INTERVAL(IR_REG_RAX, IR_REG_RDX) \
	| IR_REGSET_INTERVAL(IR_REG_R8, IR_REG_R11) \
	| IR_REGSET_FP)

# define IR_REGSET_PRESERVED \
	(IR_REGSET(IR_REG_RBX) \
	| IR_REGSET_INTERVAL(IR_REG_RBP, IR_REG_RDI) \
	| IR_REGSET_INTERVAL(IR_REG_R12, IR_REG_R15))

#elif defined(__x86_64__)

# define IR_REG_INT_RET1 IR_REG_RAX
# define IR_REG_FP_RET1  IR_REG_XMM0
# define IR_REG_INT_ARGS 6
# define IR_REG_FP_ARGS  8
# define IR_REG_INT_ARG1 IR_REG_RDI
# define IR_REG_INT_ARG2 IR_REG_RSI
# define IR_REG_INT_ARG3 IR_REG_RDX
# define IR_REG_INT_ARG4 IR_REG_RCX
# define IR_REG_INT_ARG5 IR_REG_R8
# define IR_REG_INT_ARG6 IR_REG_R9
# define IR_REG_FP_ARG1  IR_REG_XMM0
# define IR_REG_FP_ARG2  IR_REG_XMM1
# define IR_REG_FP_ARG3  IR_REG_XMM2
# define IR_REG_FP_ARG4  IR_REG_XMM3
# define IR_REG_FP_ARG5  IR_REG_XMM4
# define IR_REG_FP_ARG6  IR_REG_XMM5
# define IR_REG_FP_ARG7  IR_REG_XMM6
# define IR_REG_FP_ARG8  IR_REG_XMM7

# define IR_REGSET_SCRATCH \
	(IR_REGSET_INTERVAL(IR_REG_RAX, IR_REG_RDX) \
	| IR_REGSET_INTERVAL(IR_REG_RSI, IR_REG_RDI) \
	| IR_REGSET_INTERVAL(IR_REG_R8, IR_REG_R11) \
	| IR_REGSET_FP)

# define IR_REGSET_PRESERVED \
	(IR_REGSET(IR_REG_RBX) \
	| IR_REGSET(IR_REG_RBP) \
	| IR_REGSET_INTERVAL(IR_REG_R12, IR_REG_R15))

#else

# define IR_REG_INT_RET1   IR_REG_RAX
# define IR_REG_INT_ARGS   0
# define IR_REG_FP_ARGS    0

# define IR_REG_INT_FCARGS 2
# define IR_REG_FP_FCARGS  0
# define IR_REG_INT_FCARG1 IR_REG_RCX
# define IR_REG_INT_FCARG2 IR_REG_RDX

# define IR_REGSET_SCRATCH \
	(IR_REGSET_INTERVAL(IR_REG_RAX, IR_REG_RDX) | IR_REGSET_FP)

# define IR_REGSET_PRESERVED \
	(IR_REGSET(IR_REG_RBX) \
	| IR_REGSET(IR_REG_RBP) \
	| IR_REGSET_INTRAVLA(IR_REG_RSI, IR_REG_RDI))

#endif

bool ir_needs_vreg(ir_ctx *ctx, ir_ref ref);

/* Registers modified by the given instruction */
ir_regset ir_get_scratch_regset(ir_ctx *ctx, ir_ref ref);
ir_reg ir_uses_fixed_reg(ir_ctx *ctx, ir_ref ref, int op_num);
bool ir_result_reuses_op1_reg(ir_ctx *ctx, ir_ref ref);
uint8_t ir_get_use_flags(ir_ctx *ctx, ir_ref ref, int op_num);

#endif /* IR_X86_H */
