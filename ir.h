#ifndef IR_H
#define IR_H

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>

#define IR_VERSION "0.0.1"

typedef uint8_t bool;

#ifdef IR_DEBUG
# include <assert.h>
# define IR_ASSERT(x) assert(x)
#else
# define IR_ASSERT(x)
#endif

#ifdef __has_builtin
# if __has_builtin(__builtin_expect)
#   define EXPECTED(condition)   __builtin_expect(!!(condition), 1)
#   define UNEXPECTED(condition) __builtin_expect(!!(condition), 0)
# endif
#endif
#ifndef EXPECTED
# define EXPECTED(condition)   (condition)
# define UNEXPECTED(condition) (condition)
#endif

#ifdef __has_attribute
# if __has_attribute(always_inline)
#  define IR_ALWAYS_INLINE static inline __attribute__((always_inline))
# endif
# if __has_attribute(noinline)
#  define IR_NEVER_INLINE __attribute__((noinline))
# endif
# if __has_attribute(unused)
#  define IR_UNUSED_LABEL __attribute__((unused));
# endif
#endif
#ifndef IR_ALWAYS_INLINE
# define IR_ALWAYS_INLINE static inline
#endif
#ifndef IR_NEVER_INLINE
# define IR_NEVER_INLINE
#endif
#ifndef IR_UNUSED_LABEL
# define IR_UNUSED_LABEL
#endif

#if defined(__BYTE_ORDER__)
# if defined(__ORDER_LITTLE_ENDIAN__)
#  if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#   define IR_STRUCT_LOHI(lo, hi) struct {lo; hi;}
#  endif
# endif
# if defined(__ORDER_BIG_ENDIAN__)
#  if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#   define IR_STRUCT_LOHI(lo, hi) struct {hi; lo;}
#  endif
# endif
#endif
#ifndef IR_STRUCT_LOHI
# error "Unknown byte order"
#endif

#if defined(__SIZEOF_SIZE_T__)
# if __SIZEOF_SIZE_T__ == 8
#  define IR_64 1
# elif __SIZEOF_SIZE_T__ != 4
#  error "Unknown addr size"
# endif
#else
# error "Unknown addr size"
#endif

#define ir_mem_malloc(size)       malloc(size)
#define ir_mem_calloc(n, size)    calloc(n, size)
#define ir_mem_realloc(ptr, size) realloc(ptr, size)
#define ir_mem_free(ptr)          free(ptr)

void *ir_mem_mmap(size_t size);
int ir_mem_unmap(void *ptr, size_t size);
int ir_mem_protect(void *ptr, size_t size);
int ir_mem_unprotect(void *ptr, size_t size);
int ir_mem_flush(void *ptr, size_t size);

/* IR_TYPE flags (low 4 bits are used for type size) */
#define IR_TYPE_SIGNED     (1<<4)
#define IR_TYPE_UNSIGNED   (1<<5)
#define IR_TYPE_FP         (1<<6)
#define IR_TYPE_SPECIAL    (1<<7)
#define IR_TYPE_BOOL       (IR_TYPE_SPECIAL|IR_TYPE_UNSIGNED)
#define IR_TYPE_ADDR       (IR_TYPE_SPECIAL|IR_TYPE_UNSIGNED)
#define IR_TYPE_CHAR       (IR_TYPE_SPECIAL|IR_TYPE_SIGNED)

#define IR_TYPES(_) \
	_(BOOL,   bool,      b,    "%u",        IR_TYPE_BOOL)     \
	_(U8,     uint8_t,   u8,   "%u",        IR_TYPE_UNSIGNED) \
	_(U16,    uint16_t,  u16,  "%u",        IR_TYPE_UNSIGNED) \
	_(U32,    uint32_t,  u32,  "%u",        IR_TYPE_UNSIGNED) \
	_(U64,    uint64_t,  u64,  "%" PRIu64,  IR_TYPE_UNSIGNED) \
	_(ADDR,   uintptr_t, addr, "%" PRIxPTR, IR_TYPE_ADDR)     \
	_(CHAR,   char,      c,    "'%c'",      IR_TYPE_CHAR)     \
	_(I8,     int8_t,    i8,   "%d",        IR_TYPE_SIGNED)   \
	_(I16,    int16_t,   i16,  "%d",        IR_TYPE_SIGNED)   \
	_(I32,    int32_t,   i32,  "%d",        IR_TYPE_SIGNED)   \
	_(I64,    int64_t,   i64,  "%" PRIi64,  IR_TYPE_SIGNED)   \
	_(DOUBLE, double,    d,    "%g",        IR_TYPE_FP)       \
	_(FLOAT,  float,     f,    "%f",        IR_TYPE_FP)       \

#define IR_IS_TYPE_UNSIGNED(t) ((t) < IR_CHAR)
#define IR_IS_TYPE_SIGNED(t)   ((t) >= IR_CHAR && (t) < IR_DOUBLE)
#define IR_IS_TYPE_INT(t)      ((t) < IR_DOUBLE)
#define IR_IS_TYPE_FP(t)       ((t) >= IR_DOUBLE)

/* List of IR opcodes
 * ==================
 *
 * Each instruction is described by a type (opcode, flags, op1_type, op2_type, op3_type)
 *
 * flags
 * -----
 * v     - void
 * d     - data      IR_OP_FLAG_DATA
 * r     - ref       IR_OP_FLAG_DATA alias
 * c     - control   IR_OP_FLAG_CONTROL
 * l     - load      IR_OP_FLAG_MEM + IR_OP_FLAG_MEM_LOAD
 * s     - store     IR_OP_FLAG_MEM + IR_OP_FLAG_STORE
 * x     - call      IR_OP_FLAG_MEM + IR_OP_FLAG_CALL
 * a     - alloc     IR_OP_FLAG_MEM + IR_OP_FLAG_ALLOC
 * 0-3   - number of input edges
 * N     - number of arguments is defined in the insn->inputs_count (MERGE)
 * P     - number of arguments is defined in the op1->inputs_count (PHI)
 * X1-X3 - number of extra data ops
 *
 * operand types
 * -------------
 * ___ - unused
 * def - reference to a definition op (data-flow use-def dependency edge)
 * ref - memory reference (data-flow use-def dependency edge)
 * arg - argument referene CALL/TAILCALL/CARG->CARG
 * src - reference to a previous control region (IF, IF_TRUE, IF_FALSE, LOOP_BEGIN, LOOP_END, RETURN)
 * reg - data-control dependency on region (PHI, VAR, PARAM)
 * beg - reference to a LOOP_BEGIN region (LOOP_END, LOOP_EXIT)
 * ret - reference to a previous RETURN instruction (RETURN)
 * str - string: variable/argument name (VAR, PARAM, CALL, TAILCALL)
 * num - number: argument number (PARAM)
 *
 * The order of IR opcodes is carefully selected for efficient folding.
 * - foldable instruction go first
 * - NOP is never used (code 0 is used as ANY pattern)
 * - CONST is the most often used instruction (encode with 1 bit)
 * - equality inversion:  EQ <-> NE                         => op =^ 1
 * - comparison inversio: [U]LT <-> [U]GT, [U]LE <-> [U]GE  => op =^ 3
 */

#define IR_OPS(_) \
	/* special op (must be the first !!!)                               */ \
	_(NOP,          v,    ___, ___, ___) /* empty instruction           */ \
	\
	/* constants reference                                              */ \
	_(C_BOOL,       r0,   ___, ___, ___) /* constant                    */ \
	_(C_U8,         r0,   ___, ___, ___) /* constant                    */ \
	_(C_U16,        r0,   ___, ___, ___) /* constant                    */ \
	_(C_U32,        r0,   ___, ___, ___) /* constant                    */ \
	_(C_U64,        r0,   ___, ___, ___) /* constant                    */ \
	_(C_ADDR,       r0,   ___, ___, ___) /* constant                    */ \
	_(C_CHAR,       r0,   ___, ___, ___) /* constant                    */ \
	_(C_I8,         r0,   ___, ___, ___) /* constant                    */ \
	_(C_I16,        r0,   ___, ___, ___) /* constant                    */ \
	_(C_I32,        r0,   ___, ___, ___) /* constant                    */ \
	_(C_I64,        r0,   ___, ___, ___) /* constant                    */ \
	_(C_DOUBLE,     r0,   ___, ___, ___) /* constant                    */ \
	_(C_FLOAT,      r0,   ___, ___, ___) /* constant                    */ \
	\
	/* equality ops  */                                                    \
	_(EQ,           d2,   def, def, ___) /* equal                       */ \
	_(NE,           d2,   def, def, ___) /* not equal                   */ \
	\
	/* comparison ops (order matters, LT must be a modulo of 4 !!!)     */ \
	_(LT,           d2,   def, def, ___) /* less                        */ \
	_(GE,           d2,   def, def, ___) /* greater or equal            */ \
	_(LE,           d2,   def, def, ___) /* less or equal               */ \
	_(GT,           d2,   def, def, ___) /* greater                     */ \
	_(ULT,          d2,   def, def, ___) /* unsigned less               */ \
	_(UGE,          d2,   def, def, ___) /* unsigned greater or equal   */ \
	_(ULE,          d2,   def, def, ___) /* unsigned less or equal      */ \
	_(UGT,          d2,   def, def, ___) /* unsigned greater            */ \
	\
	/* arithmetic ops                                                   */ \
	_(ADD,          d2,   def, def, ___) /* addition                    */ \
	_(SUB,          d2,   def, def, ___) /* subtraction (must be ADD+1) */ \
	_(MUL,          d2,   def, def, ___) /* multiplication              */ \
	_(DIV,          d2,   def, def, ___) /* division                    */ \
	_(MOD,          d2,   def, def, ___) /* modulo                      */ \
	_(POW,          d2,   def, def, ___) /* power                       */ \
	_(NEG,          d1,   def, ___, ___) /* change sign                 */ \
	_(ABS,          d1,   def, ___, ___) /* absolute value              */ \
	/* (LDEXP, MIN, MAX, FPMATH)                                        */ \
	\
	/* type conversion ops ???                                          */ \
	_(CAST,         d1,   def, ___, ___) /* type conversion             */ \
	\
	/* overflow-check ???                                               */ \
	_(ADD_OV,       d2,   def, def, ___) /* addition                    */ \
	_(SUB_OV,       d2,   def, def, ___) /* subtraction                 */ \
	_(MUL_OV,       d2,   def, def, ___) /* multiplication              */ \
	_(OVERFLOW,     d1,   def, ___, ___) /* overflow check add/sub/mul  */ \
	\
	/* bitwise and shift ops                                            */ \
	_(NOT,          d1,   def, ___, ___) /* bitwise NOT                 */ \
	_(OR,           d2,   def, def, ___) /* bitwise OR                  */ \
	_(AND,          d2,   def, def, ___) /* bitwise AND                 */ \
	_(XOR,          d2,   def, def, ___) /* bitwise XOR                 */ \
	_(SHL,	        d2,   def, def, ___) /* logic shift left            */ \
	_(SHR,	        d2,   def, def, ___) /* logic shift right           */ \
	_(SAR,	        d2,   def, def, ___) /* arithmetic shift right      */ \
	_(ROL,	        d2,   def, def, ___) /* rotate left                 */ \
	_(ROR,	        d2,   def, def, ___) /* rotate right                */ \
	_(BSWAP,        d1,   def, ___, ___) /* byte swap                   */ \
	\
	/* branch-less conditional ops                                      */ \
	_(MIN,	        d2,   def, def, ___) /* min(op1, op2)               */ \
	_(MAX,	        d2,   def, def, ___) /* max(op1, op2)               */ \
	_(COND,	        d3,   def, def, def) /* op1 ? op2 : op3             */ \
	\
	/* data-flow and miscellaneous ops                                  */ \
	_(PHI,          dP,   reg, def, def) /* SSA Phi function            */ \
	_(COPY,         d1,   def, ___, ___) /* COPY (last foldable op)     */ \
	_(PI,           d2,   reg, def, ___) /* e-SSA Pi constraint ???     */ \
	/* (USE, RENAME)                                                    */ \
	\
	/* data ops                                                         */ \
	_(PARAM,        r1X2, reg, str, num) /* incoming parameter proj.    */ \
	_(VAR,	        r1X1, reg, str, ___) /* local variable              */ \
	_(FUNC,         r0,   ___, ___, ___) /* constant func ref           */ \
	_(STR,          r0,   ___, ___, ___) /* constant str ref            */ \
	\
	/* call ops                                                         */ \
	_(CALL,         xN,   src, def, def) /* CALL(src, func, args...)    */ \
	_(TAILCALL,     xN,   src, def, def) /* CALL+RETURN                 */ \
	\
	/* memory reference and load/store ops                              */ \
	_(ALLOCA,       a1X1, src, num, ___) /* alloca(num)                 */ \
	_(VLOAD,        l2,   src, ref, ___) /* load value of local var     */ \
	_(VSTORE,       s3,   src, ref, def) /* store value to local var    */ \
    _(LOAD,         l2,   src, ref, ___) /* load from memory            */ \
    _(STORE,        s3,   src, ref, def) /* store to memory             */ \
	/* memory reference ops (A, H, U, S, TMP, STR, NEW, X, V) ???       */ \
	\
	/* control-flow nodes                                               */ \
	_(START,        c0X1, ret, ___, ___) /* function start              */ \
	_(RETURN,       c2X1, src, def, ret) /* function return             */ \
	_(UNREACHABLE,  c2X1, src, def, ret) /* unreachable (tailcall, etc) */ \
	_(BEGIN,        c1,   src, ___, ___) /* block start                 */ \
	_(END,          c1,   src, ___, ___) /* block end                   */ \
	_(IF,           c2,   src, def, ___) /* conditional control split   */ \
	_(IF_TRUE,      c1,   src, ___, ___) /* IF TRUE proj.               */ \
	_(IF_FALSE,     c1,   src, ___, ___) /* IF FALSE proj.              */ \
	_(SWITCH,       c2,   src, def, ___) /* multi-way control split     */ \
	_(CASE_VAL,     c2,   src, def, ___) /* switch proj.                */ \
	_(CASE_DEFAULT, c1,   src, ___, ___) /* switch proj.                */ \
	_(MERGE,        cN,   src, src, src) /* control merge               */ \
	_(LOOP_BEGIN,   c2,   src, src, ___) /* loop start                  */ \
	_(LOOP_END,     c1X1, src, beg, ___) /* loop end                    */ \
	_(LOOP_EXIT,    c1X1, src, beg, ___) /* loop exit                   */ \
	/* (IJMP)                                                           */ \
	\
	/* guards (floating or not) ???                                     */ \
	_(GUARD_TRUE,   c2,   src, def, ___) /* IF without second successor */ \
	_(GUARD_FALSE,  c2,   src, def, ___) /* IF without second successor */ \

#define IR_OP_FLAG_OPERANS_SHIFT 3

#define IR_OP_FLAG_EDGES_MSK     0x07
#define IR_OP_FLAG_OPERANS_MSK   0x38
#define IR_OP_FLAG_MEM_MASK      ((1<<6)|(1<<7))

#define IR_OP_FLAG_DATA          (1<<8)
#define IR_OP_FLAG_CONTROL       (1<<9)
#define IR_OP_FLAG_MEM           (1<<10)

#define IR_OP_FLAG_MEM_LOAD      ((0<<6)|(0<<7))
#define IR_OP_FLAG_MEM_STORE     ((0<<6)|(1<<7))
#define IR_OP_FLAG_MEM_CALL      ((1<<6)|(0<<7))
#define IR_OP_FLAG_MEM_ALLOC     ((1<<6)|(1<<7))

#define IR_OPND_UNUSED      0x0
#define IR_OPND_DATA        0x1
#define IR_OPND_CONTROL     0x2
#define IR_OPND_CONTROL_DEP 0x3
#define IR_OPND_CONTROL_REF 0x4
#define IR_OPND_STR         0x5
#define IR_OPND_NUM         0x6

#define IR_OP_FLAGS(op_flags, op1_flags, op2_flags, op3_flags) \
	((op_flags) | ((op1_flags) << 20) | ((op2_flags) << 24) | ((op3_flags) << 28))

#define IR_INPUT_EDGES_COUNT(flags) (flags & IR_OP_FLAG_EDGES_MSK)
#define IR_OPERANDS_COUNT(flags)    ((flags & IR_OP_FLAG_OPERANS_MSK) >> IR_OP_FLAG_OPERANS_SHIFT)

#define IR_OPND_KIND(flags, i) \
	(((flags) >> (16 + (4 * (((i) > 3) ? 3 : (i))))) & 0xf)

#define IR_TYPE_ENUM(name, type, field, format, flags)  IR_ ## name,
#define IR_OP_ENUM(name, flags, op1, op2, op3) IR_ ## name,

typedef enum _ir_type {
	IR_VOID,
	IR_TYPES(IR_TYPE_ENUM)
	IR_LAST_TYPE
} ir_type;

typedef enum _ir_op {
	IR_OPS(IR_OP_ENUM)
	IR_LAST_OP
} ir_op;

typedef int32_t ir_ref;

#define IR_IS_CONST_REF(ref) ((ref) < 0)

#define IR_UNUSED            0
#define IR_NULL              (-1)
#define IR_FALSE             (-2)
#define IR_TRUE              (-3)
#define IR_LAST_FOLDABLE     IR_COPY

#define IR_IS_CONST(op)      ((op) > IR_NOP && (op) <= IR_C_FLOAT)
#define IR_IS_FOLDABLE(op)   ((op) <= IR_LAST_FOLDABLE)

#define IR_OPT_OP_MASK       0x00ff
#define IR_OPT_TYPE_MASK     0xff00
#define IR_OPT_TYPE_SHIFT    8

#define IR_OPT(op, type)     ((uint16_t)(op) | ((uint16_t)(type) << IR_OPT_TYPE_SHIFT))

#define IR_OPT_TYPE(opt)     (((opt) & IR_OPT_TYPE_MASK) >> IR_OPT_TYPE_SHIFT)

typedef union _ir_val {
	double                             d;
	uint64_t                           u64;
	int64_t                            i64;
#ifdef IR_64
	uintptr_t                          addr;
#endif
	IR_STRUCT_LOHI(
		union {
			uint32_t                   u32;
			int32_t                    i32;
			float                      f;
#ifndef IR_64
            uintptr_t                  addr;
#endif
			IR_STRUCT_LOHI(
				union {
					uint16_t           u16;
					int16_t            i16;
					IR_STRUCT_LOHI(
						union {
							uint8_t    u8;
							int8_t     i8;
							bool       b;
							char       c;
						},
						uint8_t        u8_hi
					);
				},
				uint16_t               u16_hi
			);
		},
		uint32_t                       u32_hi
	);
} ir_val;

typedef struct _ir_insn {
	IR_STRUCT_LOHI(
		union {
			IR_STRUCT_LOHI(
				union {
					IR_STRUCT_LOHI(
						uint8_t        op,
						uint8_t        type;
					);
					uint16_t           opt;
				},
				union {
					uint16_t           inputs_count;       /* number of input control edges for MERGE, CALL, TAILCALL */
					uint16_t           prev_insn_offset;   /* 16-bit backward offset from current instruction for CSE */
					uint16_t           emit_const;         /* flag to emit constant in rodat section */
				}
			);
			uint32_t                   optx;
			ir_ref                     ops[1];
		},
		union {
			ir_ref                     op1;
			ir_ref                     prev_const;
		}
	);
	union {
		IR_STRUCT_LOHI(
			ir_ref                     op2,
			ir_ref                     op3
		);
		ir_val                         val;
	};
} ir_insn;

typedef struct _ir_strtab {
	void       *data;
	uint32_t    mask;
	uint32_t    size;
	uint32_t    count;
	uint32_t    pos;
	char       *buf;
	uint32_t    buf_size;
	uint32_t    buf_top;
} ir_strtab;

#define IR_IS_BB_START(op) \
	((op) == IR_START || (op) == IR_BEGIN || (op) == IR_MERGE || (op) == IR_LOOP_BEGIN || \
	 (op) == IR_IF_TRUE || (op) == IR_IF_FALSE || (op) == IR_CASE_VAL || (op) == IR_CASE_DEFAULT)

#define IR_IS_BB_MERGE(op) \
	((op) == IR_MERGE || (op) == IR_LOOP_BEGIN)

#define IR_IS_BB_END(op) \
	((op) == IR_RETURN || (op) == IR_END || (op) == IR_LOOP_END || (op) == IR_IF || (op) == IR_SWITCH)

#define IR_BB_UNREACHABLE      (1<<0)
#define IR_BB_LOOP_HEADER      (1<<1)
#define IR_BB_IRREDUCIBLE_LOOP (1<<2)
#define IR_BB_DESSA_MOVES      (1<<3) /* translation out of SSA requires MOVEs      */
#define IR_BB_MAY_SKIP         (1<<4) /* empty BB                                   */

typedef struct _ir_block {
	uint32_t flags;
	ir_ref   start;              /* index of first instruction                 */
	ir_ref   end;                /* index of last instruction                  */
	uint32_t successors;         /* index in ir_ctx->cfg_edges[] array         */
	uint32_t successors_count;
	uint32_t predecessors;       /* index in ir_ctx->cfg_edges[] array         */
	uint32_t predecessors_count;
	union {
		int  dom_parent;         /* immediate dominator block                  */
		int  idom;               /* immediate dominator block                  */
	};
	union {
		int  dom_depth;          /* depth from the root of the dominators tree */
		int  postnum;            /* used temporary during tree constructon     */
	};
	int      dom_child;          /* first dominated blocks                     */
	int      dom_next_child;     /* next dominated block (linked list)         */
	int      loop_header;
	int      loop_depth;
} ir_block;

typedef struct _ir_use_list {
	ir_ref        refs;          /* index in ir_cts->use_edges[] array         */
	ir_ref        count;
} ir_use_list;

#define IR_FUNCTION          (1<<0)
#define IR_USE_FRAME_POINTER (1<<1)
#define IR_IRREDUCIBLE_CFG   (1<<2)

#define IR_OPT_FOLDING       (1<<16)
#define IR_OPT_IN_SCCP       (1<<17)
#define IR_LINEAR            (1<<18)
#define IR_GEN_NATIVE        (1<<19)
#define IR_GEN_C             (1<<20)

/* x86 related */
#define IR_AVX               (1<<24)

typedef enum _ir_fold_action {
	IR_FOLD_DO_RESTART,
	IR_FOLD_DO_CSE,
	IR_FOLD_DO_EMIT,
	IR_FOLD_DO_COPY,
	IR_FOLD_DO_CONST
} ir_fold_action;

typedef struct _ir_use_pos       ir_use_pos;
typedef struct _ir_live_range    ir_live_range;
typedef struct _ir_live_interval ir_live_interval;

struct _ir_use_pos {
	uint16_t       op_num;
	int8_t         hint;
	int8_t         reg;
	ir_ref         pos;
	ir_use_pos    *next;
};

struct _ir_live_range {
	ir_ref         start;
	ir_ref         end;
	ir_live_range *next;
};

struct _ir_live_interval {
	uint8_t        type;
	int8_t         reg;
	int32_t        stack_spill_pos;
	ir_live_range  range;
	ir_use_pos    *use_pos;
};

typedef struct _ir_ctx {
	ir_insn           *ir_base;
	ir_ref             insns_count;
    ir_ref             insns_limit;
    ir_ref             consts_count;
    ir_ref             consts_limit;
	uint32_t           flags;
    ir_ref             fold_cse_limit;
	ir_insn            fold_insn;
    ir_use_list       *use_lists;
    ir_ref            *use_edges;
    uint32_t           cfg_blocks_count;
    uint32_t           cfg_edges_count;
    ir_block          *cfg_blocks;
    uint32_t          *cfg_edges;
    int               *gcm_blocks;
    uint32_t          *rules;
    uint32_t           vregs_count;
    uint32_t          *vregs;
    ir_live_interval **live_intervals;
    ir_live_range     *unused_live_ranges;
    union {
	    uint32_t      *prev_insn_len;
	    uint32_t	  *bb_num;
	};
    void              *data;
    ir_strtab          strtab;
    ir_ref             prev_insn_chain[IR_LAST_FOLDABLE + 1];
    ir_ref             prev_const_chain[IR_LAST_TYPE];
} ir_ctx;

extern const uint8_t ir_type_flags[IR_LAST_TYPE];
extern const char *ir_type_name[IR_LAST_TYPE];
extern const char *ir_type_cname[IR_LAST_TYPE];
extern const uint8_t ir_type_size[IR_LAST_TYPE];
extern const uint32_t ir_op_flags[IR_LAST_OP];
extern const char *ir_op_name[IR_LAST_OP];

IR_ALWAYS_INLINE ir_ref ir_variable_inputs_count(ir_insn *insn)
{
	uint32_t n = insn->inputs_count;
	if (n == 0) {
		n = 2;
	}
	return n;
}

IR_ALWAYS_INLINE ir_ref ir_operands_count(ir_ctx *ctx, ir_insn *insn)
{
	uint32_t flags = ir_op_flags[insn->op];
	uint32_t n = IR_OPERANDS_COUNT(flags);
	if (n == 4) {
		/* MERGE or CALL */
		n = ir_variable_inputs_count(insn);
		if (n == 0) {
			n = 2;
		}
	} else if (n == 5) {
		/* PHI */
		n = ir_variable_inputs_count(&ctx->ir_base[insn->op1]) + 1;
	}
	return n;
}

IR_ALWAYS_INLINE ir_ref ir_input_edges_count(ir_ctx *ctx, ir_insn *insn)
{
	uint32_t flags = ir_op_flags[insn->op];
	uint32_t n = IR_INPUT_EDGES_COUNT(flags);
	if (n == 4) {
		/* MERGE or CALL */
		n = ir_variable_inputs_count(insn);
	} else if (n == 5) {
		/* PHI */
		n = ir_variable_inputs_count(&ctx->ir_base[insn->op1]) + 1;
	}
	return n;
}

/* Basic IR Construction API (implementation in ir.c) */
void ir_init(ir_ctx *ctx, ir_ref consts_limit, ir_ref insns_limit);

ir_ref ir_const(ir_ctx *ctx, ir_val val, uint8_t type);
ir_ref ir_const_i8(ir_ctx *ctx, int8_t c);
ir_ref ir_const_i16(ir_ctx *ctx, int16_t c);
ir_ref ir_const_i32(ir_ctx *ctx, int32_t c);
ir_ref ir_const_i64(ir_ctx *ctx, int64_t c);
ir_ref ir_const_u8(ir_ctx *ctx, uint8_t c);
ir_ref ir_const_u16(ir_ctx *ctx, uint16_t c);
ir_ref ir_const_u32(ir_ctx *ctx, uint32_t c);
ir_ref ir_const_u64(ir_ctx *ctx, uint64_t c);
ir_ref ir_const_bool(ir_ctx *ctx, bool c);
ir_ref ir_const_char(ir_ctx *ctx, char c);
ir_ref ir_const_float(ir_ctx *ctx, float c);
ir_ref ir_const_double(ir_ctx *ctx, double c);
ir_ref ir_const_addr(ir_ctx *ctx, uintptr_t c);

ir_ref ir_const_func(ir_ctx *ctx, ir_ref str);
ir_ref ir_const_str(ir_ctx *ctx, ir_ref str);

ir_ref ir_str(ir_ctx *ctx, const char *s);
ir_ref ir_strl(ir_ctx *ctx, const char *s, size_t len);
const char *ir_get_str(ir_ctx *ctx, ir_ref idx);

ir_ref ir_emit0(ir_ctx *ctx, uint32_t opt);
ir_ref ir_emit1(ir_ctx *ctx, uint32_t opt, ir_ref op1);
ir_ref ir_emit2(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2);
ir_ref ir_emit3(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3);

ir_ref ir_emit_N(ir_ctx *ctx, uint32_t opt, uint32_t count);
void   ir_set_op(ir_ctx *ctx, ir_ref ref, uint32_t n, ir_ref val);

ir_ref ir_fold(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3, ir_insn *op1_insn, ir_insn *op2_insn, ir_insn *op3_insn);

ir_ref ir_fold0(ir_ctx *ctx, uint32_t opt);
ir_ref ir_fold1(ir_ctx *ctx, uint32_t opt, ir_ref op1);
ir_ref ir_fold2(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2);
ir_ref ir_fold3(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3);

void ir_bind(ir_ctx *ctx, ir_ref var, ir_ref def);

#define ir_param(ctx, type, region, name, pos) \
	ir_emit3((ctx), IR_OPT(IR_PARAM, (type)), (region), ir_str((ctx), (name)), (pos))
#define ir_var(ctx, type, region, name) \
	ir_emit2((ctx), IR_OPT(IR_VAR, (type)), (region), ir_str((ctx), (name)))

#define ir_set_op1(ctx, ins, val) do { \
		(ctx)->ir_base[(ins)].op1 = (val); \
	} while (0)
#define ir_set_op2(ctx, ins, val) do { \
		(ctx)->ir_base[(ins)].op2 = (val); \
	} while (0)
#define ir_set_op3(ctx, ins, val) do { \
		(ctx)->ir_base[(ins)].op3 = (val); \
	} while (0)

void ir_truncate(ir_ctx *ctx);
void ir_free(ir_ctx *ctx);

void ir_print_const(ir_ctx *ctx, ir_insn *insn, FILE *f);

/* Def -> Use lists */
void ir_build_def_use_lists(ir_ctx *ctx);

/* CFG - Control Flow Graph (implementation in ir_cfg.c) */
int ir_build_cfg(ir_ctx *ctx);
int ir_build_dominators_tree(ir_ctx *ctx);
int ir_find_loops(ir_ctx *ctx);

/* SCCP - Sparse Conditional Constant Propagation (implementation in ir_sccp.c) */
int ir_sccp(ir_ctx *ctx);

/* GCM - Global Code Motion (implementation in ir_gcm.c) */
int ir_gcm(ir_ctx *ctx);
int ir_schedule(ir_ctx *ctx);

/* Liveness & Register Allocation (implementation in ir_ra.c) */
typedef int (*emit_copy_t)(ir_ctx *ctx, uint8_t type, int from, int to);

int ir_assign_virtual_registers(ir_ctx *ctx);
int ir_compute_live_ranges(ir_ctx *ctx);
int ir_coalesce(ir_ctx *ctx);
int ir_reg_alloc(ir_ctx *ctx);
int ir_compute_dessa_moves(ir_ctx *ctx);
int ir_gen_dessa_moves(ir_ctx *ctx, int b, emit_copy_t emit_copy);
void ir_free_live_ranges(ir_live_range *live_range);
void ir_free_live_intervals(ir_live_interval **live_intervals, int count);

/* Target CPU instruction selection and code geneartion (see ir_x86.c) */
int ir_match(ir_ctx *ctx);
void *ir_emit(ir_ctx *ctx, size_t *size);

const char *ir_reg_name(int8_t reg, ir_type type);

/* Target CPU disassembler (implementation in ir_disasm.c) */
int ir_disasm_init(void);
void ir_disasm_free(void);
void ir_disasm_add_symbol(const char *name, uint64_t addr, uint64_t size);
int ir_disasm(const char *name, const void *start, size_t size);

/* Linux perf interface (implementation in ir_perf.c) */
void ir_perf_jitdump_open(void);
void ir_perf_jitdump_close(void);
void ir_perf_jitdump_register(const char *name, const void *start, size_t size);
void ir_perf_map_register(const char *name, const void *start, size_t size);

/* GDB JIT interface (implementation in ir_gdb.c) */
int ir_gdb_register(const char    *name,
                    const void    *start,
                    size_t         size,
                    uint32_t       sp_offset,
                    uint32_t       sp_adjustment);
void ir_gdb_unregister_all(void);
bool ir_gdb_present(void);

/* IR load API (implementation in ir_load.c) */
void ir_loader_init(void);
void ir_loader_free(void);
int ir_load(ir_ctx *ctx, FILE *f);

/* IR save API (implementation in ir_save.c) */
void ir_save(ir_ctx *ctx, FILE *f);

/* IR debug dump API (implementation in ir_dump.c) */
void ir_dump(ir_ctx *ctx, FILE *f);
void ir_dump_dot(ir_ctx *ctx, FILE *f);
void ir_dump_use_lists(ir_ctx *ctx, FILE *f);
void ir_dump_cfg(ir_ctx *ctx, FILE *f);
void ir_dump_gcm(ir_ctx *ctx, FILE *f);
void ir_dump_live_ranges(ir_ctx *ctx, FILE *f);

/* IR to C conversion (implementation in ir_emit_c.c) */
int ir_emit_c(ir_ctx *ctx, FILE *f);

/* IR verification API (implementation in ir_check.c) */
void ir_check(ir_ctx *ctx);

/* String Tables API (implementation in ir_strtab.c) */
#define ir_strtab_count(strtab) (strtab)->count

typedef void (*ir_strtab_apply_t)(const char *str, uint32_t len, ir_ref val);

void ir_strtab_init(ir_strtab *strtab, uint32_t count, uint32_t buf_size);
ir_ref ir_strtab_lookup(ir_strtab *strtab, const char *str, uint32_t len, ir_ref val);
ir_ref ir_strtab_find(ir_strtab *strtab, const char *str, uint32_t len);
ir_ref ir_strtab_update(ir_strtab *strtab, const char *str, uint32_t len, ir_ref val);
const char *ir_strtab_str(ir_strtab *strtab, ir_ref idx);
void ir_strtab_apply(ir_strtab *strtab, ir_strtab_apply_t func);
void ir_strtab_free(ir_strtab *strtab);

#endif /* IR_H */
