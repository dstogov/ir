#ifndef IR_H
#define IR_H

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define IR_VERSION "0.0.1"

#if defined(IR_TARGET_X86)
# define IR_TARGET "x86"
#elif defined(IR_TARGET_X64)
# define IR_TARGET "x86_64"
#elif defined(IR_TARGET_AARCH64)
# define IR_TARGET "aarch64"
#else
# error "Unknown IR target"
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

#ifdef IR_PHP
# include "ir_php.h"
#endif

/* IR Type flags (low 4 bits are used for type size) */
#define IR_TYPE_SIGNED     (1<<4)
#define IR_TYPE_UNSIGNED   (1<<5)
#define IR_TYPE_FP         (1<<6)
#define IR_TYPE_SPECIAL    (1<<7)
#define IR_TYPE_BOOL       (IR_TYPE_SPECIAL|IR_TYPE_UNSIGNED)
#define IR_TYPE_ADDR       (IR_TYPE_SPECIAL|IR_TYPE_UNSIGNED)
#define IR_TYPE_CHAR       (IR_TYPE_SPECIAL|IR_TYPE_SIGNED)

/* List of IR types */
#define IR_TYPES(_) \
	_(BOOL,   bool,      b,    IR_TYPE_BOOL)     \
	_(U8,     uint8_t,   u8,   IR_TYPE_UNSIGNED) \
	_(U16,    uint16_t,  u16,  IR_TYPE_UNSIGNED) \
	_(U32,    uint32_t,  u32,  IR_TYPE_UNSIGNED) \
	_(U64,    uint64_t,  u64,  IR_TYPE_UNSIGNED) \
	_(ADDR,   uintptr_t, addr, IR_TYPE_ADDR)     \
	_(CHAR,   char,      c,    IR_TYPE_CHAR)     \
	_(I8,     int8_t,    i8,   IR_TYPE_SIGNED)   \
	_(I16,    int16_t,   i16,  IR_TYPE_SIGNED)   \
	_(I32,    int32_t,   i32,  IR_TYPE_SIGNED)   \
	_(I64,    int64_t,   i64,  IR_TYPE_SIGNED)   \
	_(DOUBLE, double,    d,    IR_TYPE_FP)       \
	_(FLOAT,  float,     f,    IR_TYPE_FP)       \

#define IR_IS_TYPE_UNSIGNED(t) ((t) < IR_CHAR)
#define IR_IS_TYPE_SIGNED(t)   ((t) >= IR_CHAR && (t) < IR_DOUBLE)
#define IR_IS_TYPE_INT(t)      ((t) < IR_DOUBLE)
#define IR_IS_TYPE_FP(t)       ((t) >= IR_DOUBLE)

#define IR_TYPE_ENUM(name, type, field, flags) IR_ ## name,

typedef enum _ir_type {
	IR_VOID,
	IR_TYPES(IR_TYPE_ENUM)
	IR_LAST_TYPE
} ir_type;

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
 * S     - control   IR_OP_FLAG_CONTROL + IR_OP_FLAG_BB_START
 * E     - control   IR_OP_FLAG_CONTROL + IR_OP_FLAG_BB_END
 * T     - control   IR_OP_FLAG_CONTROL + IR_OP_FLAG_BB_END + IR_OP_FLAG_TERMINATOR
 * l     - load      IR_OP_FLAG_MEM + IR_OP_FLAG_MEM_LOAD
 * s     - store     IR_OP_FLAG_MEM + IR_OP_FLAG_STORE
 * x     - call      IR_OP_FLAG_MEM + IR_OP_FLAG_CALL
 * a     - alloc     IR_OP_FLAG_MEM + IR_OP_FLAG_ALLOC
 * 0-3   - number of input edges
 * N     - number of arguments is defined in the insn->inputs_count (MERGE)
 * P     - number of arguments is defined in the op1->inputs_count (PHI)
 * X1-X3 - number of extra data ops
 * C     - commutative operation ("d2C" => IR_OP_FLAG_DATA + IR_OP_FLAG_COMMUTATIVE)
 *
 * operand types
 * -------------
 * ___ - unused
 * def - reference to a definition op (data-flow use-def dependency edge)
 * ref - memory reference (data-flow use-def dependency edge)
 * var - variable reference (data-flow use-def dependency edge)
 * arg - argument referene CALL/TAILCALL/CARG->CARG
 * src - reference to a previous control region (IF, IF_TRUE, IF_FALSE, MERGE, LOOP_BEGIN, LOOP_END, RETURN)
 * reg - data-control dependency on region (PHI, VAR, PARAM)
 * beg - reference to a LOOP_BEGIN region (LOOP_END)
 * ret - reference to a previous RETURN instruction (RETURN)
 * ent - reference to a previous ENTRY instruction (ENTRY)
 * str - string: variable/argument name (VAR, PARAM, CALL, TAILCALL)
 * num - number: argument number (PARAM)
 * prb - branch probability 1-99 (0 - unspecified): (IF_TRUE, IF_FALSE, CASE_VAL, CASE_DEFAULT)
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
	_(EQ,           d2C,  def, def, ___) /* equal                       */ \
	_(NE,           d2C,  def, def, ___) /* not equal                   */ \
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
	_(ADD,          d2C,  def, def, ___) /* addition                    */ \
	_(SUB,          d2,   def, def, ___) /* subtraction (must be ADD+1) */ \
	_(MUL,          d2C,  def, def, ___) /* multiplication              */ \
	_(DIV,          d2,   def, def, ___) /* division                    */ \
	_(MOD,          d2,   def, def, ___) /* modulo                      */ \
	_(NEG,          d1,   def, ___, ___) /* change sign                 */ \
	_(ABS,          d1,   def, ___, ___) /* absolute value              */ \
	/* (LDEXP, MIN, MAX, FPMATH)                                        */ \
	\
	/* type conversion ops                                              */ \
	_(SEXT,         d1,   def, ___, ___) /* sign extension              */ \
	_(ZEXT,         d1,   def, ___, ___) /* zero extension              */ \
	_(TRUNC,        d1,   def, ___, ___) /* truncates to int type       */ \
	_(BITCAST,      d1,   def, ___, ___) /* binary representation       */ \
	_(INT2FP,       d1,   def, ___, ___) /* int to float conversion     */ \
	_(FP2INT,       d1,   def, ___, ___) /* float to int conversion     */ \
	_(FP2FP,        d1,   def, ___, ___) /* float to float conversion   */ \
	\
	/* overflow-check                                                   */ \
	_(ADD_OV,       d2C,  def, def, ___) /* addition                    */ \
	_(SUB_OV,       d2,   def, def, ___) /* subtraction                 */ \
	_(MUL_OV,       d2C,  def, def, ___) /* multiplication              */ \
	_(OVERFLOW,     d1,   def, ___, ___) /* overflow check add/sub/mul  */ \
	\
	/* bitwise and shift ops                                            */ \
	_(NOT,          d1,   def, ___, ___) /* bitwise NOT                 */ \
	_(OR,           d2C,  def, def, ___) /* bitwise OR                  */ \
	_(AND,          d2C,  def, def, ___) /* bitwise AND                 */ \
	_(XOR,          d2C,  def, def, ___) /* bitwise XOR                 */ \
	_(SHL,	        d2,   def, def, ___) /* logic shift left            */ \
	_(SHR,	        d2,   def, def, ___) /* logic shift right           */ \
	_(SAR,	        d2,   def, def, ___) /* arithmetic shift right      */ \
	_(ROL,	        d2,   def, def, ___) /* rotate left                 */ \
	_(ROR,	        d2,   def, def, ___) /* rotate right                */ \
	_(BSWAP,        d1,   def, ___, ___) /* byte swap                   */ \
	\
	/* branch-less conditional ops                                      */ \
	_(MIN,	        d2C,  def, def, ___) /* min(op1, op2)               */ \
	_(MAX,	        d2C,  def, def, ___) /* max(op1, op2)               */ \
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
	_(FUNC_ADDR,    r0,   ___, ___, ___) /* constant func ref           */ \
	_(FUNC,         r0,   ___, ___, ___) /* constant func ref           */ \
	_(STR,          r0,   ___, ___, ___) /* constant str ref            */ \
	\
	/* call ops                                                         */ \
	_(CALL,         xN,   src, def, def) /* CALL(src, func, args...)    */ \
	_(TAILCALL,     xN,   src, def, def) /* CALL+RETURN                 */ \
	\
	/* memory reference and load/store ops                              */ \
	_(ALLOCA,       a2,   src, def, ___) /* alloca(def)                 */ \
	_(VADDR,        d1,   var, ___, ___) /* load address of local var   */ \
	_(VLOAD,        l2,   src, var, ___) /* load value of local var     */ \
	_(VSTORE,       s3,   src, var, def) /* store value to local var    */ \
	_(RLOAD,        l1X1, src, num, ___) /* load value from register    */ \
	_(RSTORE,       l2X1, src, def, num) /* store value into register   */ \
	_(LOAD,         l2,   src, ref, ___) /* load from memory            */ \
	_(STORE,        s3,   src, ref, def) /* store to memory             */ \
	_(TLS,          l1X2, src, num, num) /* thread local variable       */ \
	_(TRAP,         x1,   src, ___, ___) /* DebugBreak                  */ \
	/* memory reference ops (A, H, U, S, TMP, STR, NEW, X, V) ???       */ \
	\
	/* control-flow nodes                                               */ \
	_(START,        S0X2, ret, ent, ___) /* function start              */ \
	_(RETURN,       T2X1, src, def, ret) /* function return             */ \
	_(UNREACHABLE,  T2X1, src, def, ret) /* unreachable (tailcall, etc) */ \
	_(BEGIN,        S1,   src, ___, ___) /* block start                 */ \
	_(END,          E1,   src, ___, ___) /* block end                   */ \
	_(IF,           E2,   src, def, ___) /* conditional control split   */ \
	_(IF_TRUE,      S1X1, src, prb, ___) /* IF TRUE proj.               */ \
	_(IF_FALSE,     S1X1, src, prb, ___) /* IF FALSE proj.              */ \
	_(SWITCH,       E2,   src, def, ___) /* multi-way control split     */ \
	_(CASE_VAL,     S2X1, src, def, prb) /* switch proj.                */ \
	_(CASE_DEFAULT, S1X1, src, prb, ___) /* switch proj.                */ \
	_(MERGE,        SN,   src, src, src) /* control merge               */ \
	_(LOOP_BEGIN,   SN,   src, src, src) /* loop start                  */ \
	_(LOOP_END,     E1X1, src, beg, ___) /* loop end                    */ \
	_(IJMP,         T2X1, src, def, ret) /* computed goto               */ \
	_(ENTRY,        S0X2, num, ent, ___) /* code entry (op3 keeps addr) */ \
	\
	/* guards (floating or not) ???                                     */ \
	_(GUARD,        c3,   src, def, def) /* IF without second successor */ \
	_(GUARD_NOT  ,  c3,   src, def, def) /* IF without second successor */ \
	\
	/* tracing JIT helpers                                              */ \
	_(EXITCALL,     x2,   src, def, ___) /* save all CPU registers      */ \
	_(EXITGROUP,    c1X2, src, num, num) /* code to push exit number    */ \


#define IR_OP_ENUM(name, flags, op1, op2, op3) IR_ ## name,

typedef enum _ir_op {
	IR_OPS(IR_OP_ENUM)
#ifdef IR_PHP
	IR_PHP_OPS(IR_OP_ENUM)
#endif
	IR_LAST_OP
} ir_op;

/* IR Opcode and Type Union */
#define IR_OPT_OP_MASK       0x00ff
#define IR_OPT_TYPE_MASK     0xff00
#define IR_OPT_TYPE_SHIFT    8

#define IR_OPT(op, type)     ((uint16_t)(op) | ((uint16_t)(type) << IR_OPT_TYPE_SHIFT))
#define IR_OPT_TYPE(opt)     (((opt) & IR_OPT_TYPE_MASK) >> IR_OPT_TYPE_SHIFT)

/* IR References */
typedef int32_t ir_ref;

#define IR_IS_CONST_REF(ref) ((ref) < 0)

#define IR_UNUSED            0
#define IR_NULL              (-1)
#define IR_FALSE             (-2)
#define IR_TRUE              (-3)
#define IR_LAST_FOLDABLE_OP  IR_COPY

/* IR Constant Value */
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

/* IR constant flags */
#define IR_CONST_EMIT          (1<<0)
#define IR_CONST_FASTCALL_FUNC (1<<1)

/* IR Instruction */
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
					uint16_t           const_flags;        /* flag to emit constant in rodat section */
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

/* IR Hash Tables API (private) */
typedef struct _ir_hashtab ir_hashtab;

/* IR String Tables API (implementation in ir_strtab.c) */
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

#define ir_strtab_count(strtab) (strtab)->count

typedef void (*ir_strtab_apply_t)(const char *str, uint32_t len, ir_ref val);

void ir_strtab_init(ir_strtab *strtab, uint32_t count, uint32_t buf_size);
ir_ref ir_strtab_lookup(ir_strtab *strtab, const char *str, uint32_t len, ir_ref val);
ir_ref ir_strtab_find(ir_strtab *strtab, const char *str, uint32_t len);
ir_ref ir_strtab_update(ir_strtab *strtab, const char *str, uint32_t len, ir_ref val);
const char *ir_strtab_str(ir_strtab *strtab, ir_ref idx);
void ir_strtab_apply(ir_strtab *strtab, ir_strtab_apply_t func);
void ir_strtab_free(ir_strtab *strtab);

/* IR Context Flags */
#define IR_FUNCTION           (1<<0)
#define IR_FASTCALL_FUNC      (1<<1)
#define IR_SKIP_PROLOGUE      (1<<2)
#define IR_USE_FRAME_POINTER  (1<<3)
#define IR_PREALLOCATED_STACK (1<<4)
#define IR_HAS_ALLOCA         (1<<5)
#define IR_HAS_CALLS          (1<<6)
#define IR_NO_STACK_COMBINE   (1<<7)

#define IR_IRREDUCIBLE_CFG    (1<<8)

#define IR_OPT_FOLDING        (1<<16)
#define IR_OPT_CODEGEN        (1<<17)
#define IR_OPT_IN_SCCP        (1<<18)
#define IR_LINEAR             (1<<19)
#define IR_GEN_NATIVE         (1<<20)
#define IR_GEN_C              (1<<22)

/* x86 related */
#define IR_AVX                (1<<24)

/* debug relted */
#ifdef IR_DEBUG
# define IR_DEBUG_SCCP        (1<<27)
# define IR_DEBUG_GCM         (1<<28)
# define IR_DEBUG_SCHEDULE    (1<<29)
# define IR_DEBUG_RA          (1<<30)
#endif

typedef struct _ir_use_list      ir_use_list;
typedef struct _ir_block         ir_block;
typedef struct _ir_live_interval ir_live_interval;
typedef int8_t ir_regs[4];

typedef struct _ir_ctx {
	ir_insn           *ir_base;           /* two directional array - instructions grow down, constants grow up */
	ir_ref             insns_count;
	ir_ref             insns_limit;
	ir_ref             consts_count;
	ir_ref             consts_limit;
	uint32_t           flags;
	ir_ref             fold_cse_limit;
	ir_insn            fold_insn;
	ir_hashtab        *binding;
	ir_use_list       *use_lists;         /* def->use lists for each instruction */
	ir_ref            *use_edges;
	uint32_t           use_edges_count;
	uint32_t           cfg_blocks_count;
	uint32_t           cfg_edges_count;
	ir_block          *cfg_blocks;        /* list of Basic Blocks (starts from 1) */
	uint32_t          *cfg_edges;
	uint32_t          *cfg_map;           /* map of instructions to Basic Block number */
	uint32_t          *rules;
	uint32_t          *vregs;
	uint32_t           vregs_count;
	int32_t            spill_base;        /* base register for special spill area (e.g. PHP VM frame pointer) */
	int32_t            fixed_stack_red_zone;
	int32_t            fixed_stack_frame_size;
	uint64_t           fixed_regset;
	uint64_t           fixed_save_regset;
	ir_live_interval **live_intervals;
	ir_regs           *regs;
	uint32_t          *prev_insn_len;
	void              *data;
	uint32_t           rodata_offset;
	uint32_t           jmp_table_offset;
	void              *code_buffer;
	size_t             code_buffer_size;
	ir_strtab          strtab;
	ir_ref             prev_insn_chain[IR_LAST_FOLDABLE_OP + 1];
	ir_ref             prev_const_chain[IR_LAST_TYPE];
} ir_ctx;

/* Basic IR Construction API (implementation in ir.c) */
void ir_init(ir_ctx *ctx, ir_ref consts_limit, ir_ref insns_limit);
void ir_free(ir_ctx *ctx);
void ir_truncate(ir_ctx *ctx);

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
ir_ref ir_const_func_addr(ir_ctx *ctx, uintptr_t c, uint16_t flags);

ir_ref ir_const_func(ir_ctx *ctx, ir_ref str, uint16_t flags);
ir_ref ir_const_str(ir_ctx *ctx, ir_ref str);

void ir_print_const(ir_ctx *ctx, ir_insn *insn, FILE *f);

ir_ref ir_str(ir_ctx *ctx, const char *s);
ir_ref ir_strl(ir_ctx *ctx, const char *s, size_t len);
const char *ir_get_str(ir_ctx *ctx, ir_ref idx);

ir_ref ir_emit(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3);

ir_ref ir_emit0(ir_ctx *ctx, uint32_t opt);
ir_ref ir_emit1(ir_ctx *ctx, uint32_t opt, ir_ref op1);
ir_ref ir_emit2(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2);
ir_ref ir_emit3(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3);

void   ir_set_op1(ir_ctx *ctx, ir_ref ref, ir_ref val);
void   ir_set_op2(ir_ctx *ctx, ir_ref ref, ir_ref val);
void   ir_set_op3(ir_ctx *ctx, ir_ref ref, ir_ref val);

ir_ref ir_emit_N(ir_ctx *ctx, uint32_t opt, uint32_t count);
void   ir_set_op(ir_ctx *ctx, ir_ref ref, uint32_t n, ir_ref val);

ir_ref ir_fold(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3);

ir_ref ir_fold0(ir_ctx *ctx, uint32_t opt);
ir_ref ir_fold1(ir_ctx *ctx, uint32_t opt, ir_ref op1);
ir_ref ir_fold2(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2);
ir_ref ir_fold3(ir_ctx *ctx, uint32_t opt, ir_ref op1, ir_ref op2, ir_ref op3);

ir_ref ir_param(ir_ctx *ctx, ir_type type, ir_ref region, const char *name, int pos);
ir_ref ir_var(ir_ctx *ctx, ir_type type, ir_ref region, const char *name);
void   ir_bind(ir_ctx *ctx, ir_ref var, ir_ref def);

/* Def -> Use lists */
void ir_build_def_use_lists(ir_ctx *ctx);

/* CFG - Control Flow Graph (implementation in ir_cfg.c) */
int ir_build_cfg(ir_ctx *ctx);
int ir_build_dominators_tree(ir_ctx *ctx);
int ir_find_loops(ir_ctx *ctx);
int ir_schedule_blocks(ir_ctx *ctx);

/* SCCP - Sparse Conditional Constant Propagation (implementation in ir_sccp.c) */
int ir_sccp(ir_ctx *ctx);

/* GCM - Global Code Motion and scheduling (implementation in ir_gcm.c) */
int ir_gcm(ir_ctx *ctx);
int ir_schedule(ir_ctx *ctx);

/* Liveness & Register Allocation (implementation in ir_ra.c) */
int ir_assign_virtual_registers(ir_ctx *ctx);
int ir_compute_live_ranges(ir_ctx *ctx);
int ir_coalesce(ir_ctx *ctx);
int ir_compute_dessa_moves(ir_ctx *ctx);
int ir_reg_alloc(ir_ctx *ctx);

/* Target CPU instruction selection and code geneartion (see ir_x86.c) */
int ir_match(ir_ctx *ctx);
void *ir_emit_code(ir_ctx *ctx, size_t *size);

/* Target CPU disassembler (implementation in ir_disasm.c) */
int  ir_disasm_init(void);
void ir_disasm_free(void);
void ir_disasm_add_symbol(const char *name, uint64_t addr, uint64_t size);
int  ir_disasm(const char *name,
               const void *start,
               size_t      size,
               bool        asm_addr,
               ir_ctx     *ctx,
               FILE       *f);

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
void ir_dump_cfg_map(ir_ctx *ctx, FILE *f);
void ir_dump_live_ranges(ir_ctx *ctx, FILE *f);

/* IR to C conversion (implementation in ir_emit_c.c) */
int ir_emit_c(ir_ctx *ctx, FILE *f);

/* IR verification API (implementation in ir_check.c) */
bool ir_check(ir_ctx *ctx);
void ir_consistency_check(void);

/* IR Memmory Allocation */
#ifndef ir_mem_malloc
# define ir_mem_malloc   malloc
#endif
#ifndef ir_mem_calloc
# define ir_mem_calloc   calloc
#endif
#ifndef ir_mem_realloc
# define ir_mem_realloc  realloc
#endif
#ifndef ir_mem_free
# define ir_mem_free     free
#endif

#ifndef ir_mem_pmalloc
# define ir_mem_pmalloc  malloc
#endif
#ifndef ir_mem_pcalloc
# define ir_mem_pcalloc  calloc
#endif
#ifndef ir_mem_prealloc
# define ir_mem_prealloc realloc
#endif
#ifndef ir_mem_pfree
# define ir_mem_pfree    free
#endif

void *ir_mem_mmap(size_t size);
int ir_mem_unmap(void *ptr, size_t size);
int ir_mem_protect(void *ptr, size_t size);
int ir_mem_unprotect(void *ptr, size_t size);
int ir_mem_flush(void *ptr, size_t size);

#endif /* IR_H */
