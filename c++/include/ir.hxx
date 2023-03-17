/*
 * IR - Lightweight JIT Compilation Framework
 * (C++ wrapper for IR library)
 * Copyright (C) 2023 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#ifndef IR_HXX
#define IR_HXX

#include "ir.h"
#include "ir_builder.h"

namespace ir
{
	const auto consistency_check = ir_consistency_check;
	const auto regs_number = ir_regs_number;
	const auto reg_is_int = ir_reg_is_int;
	const auto reg_name = ir_reg_name;

	const auto disasm_init = ir_disasm_init;
	const auto disasm_free = ir_disasm_free;
	const auto disasm_add_symbol = ir_disasm_add_symbol;
	const auto disasm = ir_disasm;

	const auto perf_jitdump_open = ir_perf_jitdump_open;
	const auto perf_jitdump_close = ir_perf_jitdump_close;
	const auto perf_jitdump_register = ir_perf_jitdump_register;
	const auto perf_map_register = ir_perf_map_register;

	const auto gdb_register = ir_gdb_register;
	const auto gdb_unregister_all = ir_gdb_unregister_all;
	const auto gdb_present = ir_gdb_present;

	// XXX Implement loader API. Looks like the implementation is not yet OOP suitable?

	const auto mem_mmap = ir_mem_mmap;
	const auto mem_unmap = ir_mem_unmap;
	const auto mem_protect = ir_mem_protect;
	const auto mem_unprotect = ir_mem_unprotect;
	const auto mem_flush = ir_mem_flush;

	class ref {
	protected:
		ir_ref r;
	public:
		ref() : r(0) {}
		ref(ir_ref _r) : r(_r) {}
		ref(const ref& _ro) { r = _ro.r; }
		operator ir_ref() const { return r; }
	};

	class type {
	protected:
		ir_type t;
	public:
		type(ir_type _t) : t(_t) {}
		type() : t(IR_VOID) {}
		operator ir_type() const { return t; }
	};

	// i8, i16, i32, i64, u8, u16, u32, u64, addr, c, b, d, f
#define IR_TYPE_CLS(name, cxx_type, field, flags) \
	class field : public type, public ref { \
	protected: \
		cxx_type v; \
	public: \
		static const field t; \
		field() : v(0), type(IR_ ## name), ref(0) {} \
		field(cxx_type _v) : v(_v), type(IR_ ## name), ref(0) {} \
		field(const ref& _r) : v(0), type(IR_ ## name), ref(_r) {} \
		operator cxx_type() const { return v; } \
		operator ref() const { return r; } \
		operator type() const { return t; } \
	}; \
	const field field::t = field{ IR_ ## name };
	IR_TYPES(IR_TYPE_CLS)
#undef IR_TYPE_CLS
	class none : public type {
	public:
		static const none t;
		none() : type(IR_VOID) {}
	};
	const none none::t = none{};

	class engine {
	protected:
		ir_ctx ctx;
	public:
		engine() { ir_init(&ctx, 0, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN); }
		engine(uint32_t flags, int32_t consts_limit = IR_CONSTS_LIMIT_MIN, int32_t insns_limit = IR_INSNS_LIMIT_MIN) {
			ir_init(&ctx, flags, consts_limit, insns_limit);
			uint64_t debug_regset = 0xffffffffffffffff;
			ctx.fixed_regset = ~debug_regset;
		}
		~engine() {
			ir_free(&ctx);
		}

		bool check() { return ir_check(&ctx); }
		void save() { ir_save(&ctx, stderr); }
		void truncate() { ir_truncate(&ctx); }

		void build_def_use_lists() { ir_build_def_use_lists(&ctx); }
		int sccp() { return ir_sccp(&ctx); }
		void build_cfg() { ir_build_cfg(&ctx); }
		void remove_unreachable_blocks() { ir_remove_unreachable_blocks(&ctx); }
		void build_dominators_tree() { ir_build_dominators_tree(&ctx); }
		void find_loops() { ir_find_loops(&ctx); }
		void gcm() { ir_gcm(&ctx); }
		void schedule() { ir_schedule(&ctx); }
		void match() { ir_match(&ctx); }
		void assign_virtual_registers() { ir_assign_virtual_registers(&ctx); }
		void compute_live_ranges() { ir_compute_live_ranges(&ctx); }
		void coalesce() { ir_coalesce(&ctx); }
		void reg_alloc() { ir_reg_alloc(&ctx); }
		void schedule_blocks() { ir_schedule_blocks(&ctx); }
		void compute_dessa_moves() { ir_compute_dessa_moves(&ctx); }

		void* emit_code() { size_t sz; return ir_emit_code(&ctx, &sz); }
	};

	class builder : public engine {
	protected:
		int32_t param_pos_cnt = 0;
	public:
		builder() : engine() {}
		builder(uint32_t flags, int32_t consts_limit = IR_CONSTS_LIMIT_MIN, int32_t insns_limit = IR_INSNS_LIMIT_MIN)
			: engine(flags, consts_limit, insns_limit) {}

		i8 constval(i8 v) { return ref(ir_const_i8(&ctx, v)); }
		i16 constval(i16 v) { return ref(ir_const_i16(&ctx, v)); }
		i32 constval(i32 v) { return ref(ir_const_i32(&ctx, v)); }
		i64 constval(i64 v) { return ref(ir_const_i64(&ctx, v)); }
		u8 constval(u8 v) { return ref(ir_const_u8(&ctx, v)); }
		u16 constval(u16 v) { return ref(ir_const_u16(&ctx, v)); }
		u32 constval(u32 v) { return ref(ir_const_u32(&ctx, v)); }
		u64 constval(u64 v) { return ref(ir_const_u64(&ctx, v)); }
		b constval(b v) { return ref(ir_const_bool(&ctx, v)); }
		c constval(c v) { return ref(ir_const_char(&ctx, v)); }
		f constval(f v) { return ref(ir_const_float(&ctx, v)); }
		f constval(float v) { return ref(ir_const_float(&ctx, v)); }
		d constval(d v) { return ref(ir_const_double(&ctx, v)); }
		d constval(double v) { return ref(ir_const_double(&ctx, v)); }
		addr constval(addr v) { return ref(ir_const_addr(&ctx, v)); }
		ref constval(ref v) { return ir_const_str(&ctx, v); } // XXX have type str?
		addr constval(addr v, uint16_t flags) { return ref(ir_const_func_addr(&ctx, v, flags)); }
		ref constval(ref v, uint16_t flags) { return ir_const_func(&ctx, v, flags); } // XXX have type func?

		b eq(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_EQ, IR_BOOL), ref(op1), ref(op2))); }
		b ne(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_NE, IR_BOOL), ref(op1), ref(op2))); }

		b gt(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_GT, IR_BOOL), ref(op1), ref(op2))); }
		b lt(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_LT, IR_BOOL), ref(op1), ref(op2))); }
		b ge(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_GE, IR_BOOL), ref(op1), ref(op2))); }
		b le(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_LE, IR_BOOL), ref(op1), ref(op2))); }

		b ugt(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_UGT, IR_BOOL), ref(op1), ref(op2))); }
		b ult(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ULT, IR_BOOL), ref(op1), ref(op2))); }
		b uge(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_UGE, IR_BOOL), ref(op1), ref(op2))); }
		b ule(ref op1, ref op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ULE, IR_BOOL), ref(op1), ref(op2))); }

		i8 sub(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_I8), ref(op1), ref(op2))); }
		i16 sub(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_I16), ref(op1), ref(op2))); }
		i32 sub(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_I32), ref(op1), ref(op2))); }
		i64 sub(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_I64), ref(op1), ref(op2))); }
		u8 sub(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_U8), ref(op1), ref(op2))); }
		u16 sub(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_U16), ref(op1), ref(op2))); }
		u32 sub(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_U32), ref(op1), ref(op2))); }
		u64 sub(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_U64), ref(op1), ref(op2))); }
		addr sub(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_ADDR), ref(op1), ref(op2))); }
		c sub(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_CHAR), ref(op1), ref(op2))); }
		f sub(f op1, f op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_FLOAT), ref(op1), ref(op2))); }
		d sub(d op1, d op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB, IR_DOUBLE), ref(op1), ref(op2))); }

		i8 add(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_I8), ref(op1), ref(op2))); }
		i16 add(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_I16), ref(op1), ref(op2))); }
		i32 add(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_I32), ref(op1), ref(op2))); }
		i64 add(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_I64), ref(op1), ref(op2))); }
		u8 add(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_U8), ref(op1), ref(op2))); }
		u16 add(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_U16), ref(op1), ref(op2))); }
		u32 add(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_U32), ref(op1), ref(op2))); }
		u64 add(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_U64), ref(op1), ref(op2))); }
		addr add(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_ADDR), ref(op1), ref(op2))); }
		c add(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_CHAR), ref(op1), ref(op2))); }
		f add(f op1, f op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_FLOAT), ref(op1), ref(op2))); }
		d add(d op1, d op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD, IR_DOUBLE), ref(op1), ref(op2))); }

		i8 mul(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_I8), ref(op1), ref(op2))); }
		i16 mul(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_I16), ref(op1), ref(op2))); }
		i32 mul(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_I32), ref(op1), ref(op2))); }
		i64 mul(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_I64), ref(op1), ref(op2))); }
		u8 mul(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_U8), ref(op1), ref(op2))); }
		u16 mul(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_U16), ref(op1), ref(op2))); }
		u32 mul(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_U32), ref(op1), ref(op2))); }
		u64 mul(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_U64), ref(op1), ref(op2))); }
		addr mul(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_ADDR), ref(op1), ref(op2))); }
		c mul(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_CHAR), ref(op1), ref(op2))); }
		f mul(f op1, f op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_FLOAT), ref(op1), ref(op2))); }
		d mul(d op1, d op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL, IR_DOUBLE), ref(op1), ref(op2))); }

		i8 div(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_I8), ref(op1), ref(op2))); }
		i16 div(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_I16), ref(op1), ref(op2))); }
		i32 div(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_I32), ref(op1), ref(op2))); }
		i64 div(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_I64), ref(op1), ref(op2))); }
		u8 div(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_U8), ref(op1), ref(op2))); }
		u16 div(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_U16), ref(op1), ref(op2))); }
		u32 div(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_U32), ref(op1), ref(op2))); }
		u64 div(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_U64), ref(op1), ref(op2))); }
		addr div(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_ADDR), ref(op1), ref(op2))); }
		c div(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_CHAR), ref(op1), ref(op2))); }
		f div(f op1, f op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_FLOAT), ref(op1), ref(op2))); }
		d div(d op1, d op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_DIV, IR_DOUBLE), ref(op1), ref(op2))); }

		i8 mod(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_I8), ref(op1), ref(op2))); }
		i16 mod(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_I16), ref(op1), ref(op2))); }
		i32 mod(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_I32), ref(op1), ref(op2))); }
		i64 mod(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_I64), ref(op1), ref(op2))); }
		u8 mod(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_U8), ref(op1), ref(op2))); }
		u16 mod(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_U16), ref(op1), ref(op2))); }
		u32 mod(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_U32), ref(op1), ref(op2))); }
		u64 mod(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_U64), ref(op1), ref(op2))); }
		addr mod(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_ADDR), ref(op1), ref(op2))); }
		c mod(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MOD, IR_CHAR), ref(op1), ref(op2))); }

		i8 neg(i8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NEG, IR_I8), ref(op1))); }
		i16 neg(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NEG, IR_I16), ref(op1))); }
		i32 neg(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NEG, IR_I32), ref(op1))); }
		i64 neg(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NEG, IR_I64), ref(op1))); }
		c neg(c op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NEG, IR_CHAR), ref(op1))); }
		f neg(f op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NEG, IR_FLOAT), ref(op1))); }
		d neg(d op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NEG, IR_DOUBLE), ref(op1))); }

		i8 abs(i8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ABS, IR_I8), ref(op1))); }
		i16 abs(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ABS, IR_I16), ref(op1))); }
		i32 abs(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ABS, IR_I32), ref(op1))); }
		i64 abs(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ABS, IR_I64), ref(op1))); }
		c abs(c op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ABS, IR_CHAR), ref(op1))); }
		f abs(f op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ABS, IR_FLOAT), ref(op1))); }
		d abs(d op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ABS, IR_DOUBLE), ref(op1))); }

		i8 sext(i8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_I8), ref(op1))); }
		i16 sext(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_I16), ref(op1))); }
		i32 sext(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_I32), ref(op1))); }
		i64 sext(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_I64), ref(op1))); }
		c sext(c op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_CHAR), ref(op1))); }
		u8 sext(u8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_U8), ref(op1))); }
		u16 sext(u16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_U16), ref(op1))); }
		u32 sext(u32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_U32), ref(op1))); }
		u64 sext(u64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_U64), ref(op1))); }
		addr sext(addr op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_SEXT, IR_ADDR), ref(op1))); }

		i8 zext(i8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_I8), ref(op1))); }
		i16 zext(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_I16), ref(op1))); }
		i32 zext(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_I32), ref(op1))); }
		i64 zext(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_I64), ref(op1))); }
		c zext(c op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_CHAR), ref(op1))); }
		u8 zext(u8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_U8), ref(op1))); }
		u16 zext(u16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_U16), ref(op1))); }
		u32 zext(u32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_U32), ref(op1))); }
		u64 zext(u64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_U64), ref(op1))); }
		addr zext(addr op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_ZEXT, IR_ADDR), ref(op1))); }

		i8 trunc(i8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_I8), ref(op1))); }
		i16 trunc(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_I16), ref(op1))); }
		i32 trunc(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_I32), ref(op1))); }
		i64 trunc(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_I64), ref(op1))); }
		c trunc(c op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_CHAR), ref(op1))); }
		u8 trunc(u8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_U8), ref(op1))); }
		u16 trunc(u16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_U16), ref(op1))); }
		u32 trunc(u32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_U32), ref(op1))); }
		u64 trunc(u64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_U64), ref(op1))); }
		addr trunc(addr op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_TRUNC, IR_ADDR), ref(op1))); }

		i8 bitcast(i8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_I8), ref(op1))); }
		i16 bitcast(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_I16), ref(op1))); }
		i32 bitcast(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_I32), ref(op1))); }
		i64 bitcast(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_I64), ref(op1))); }
		u8 bitcast(u8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_U8), ref(op1))); }
		u16 bitcast(u16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_U16), ref(op1))); }
		u32 bitcast(u32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_U32), ref(op1))); }
		u64 bitcast(u64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_U64), ref(op1))); }
		addr bitcast(addr op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_ADDR), ref(op1))); }
		c bitcast(c op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_CHAR), ref(op1))); }
		f bitcast(f op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_FLOAT), ref(op1))); }
		d bitcast(d op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BITCAST, IR_DOUBLE), ref(op1))); }

		// TODO int2d, fp2**, f2d, d2f

		i8 add_ov(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_I8), ref(op1), ref(op2))); }
		i16 add_ov(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_I16), ref(op1), ref(op2))); }
		i32 add_ov(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_I32), ref(op1), ref(op2))); }
		i64 add_ov(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_I64), ref(op1), ref(op2))); }
		u8 add_ov(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_U8), ref(op1), ref(op2))); }
		u16 add_ov(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_U16), ref(op1), ref(op2))); }
		u32 add_ov(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_U32), ref(op1), ref(op2))); }
		u64 add_ov(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_U64), ref(op1), ref(op2))); }
		addr add_ov(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_ADDR), ref(op1), ref(op2))); }
		c add_ov(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ADD_OV, IR_CHAR), ref(op1), ref(op2))); }

		i8 sub_ov(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_I8), ref(op1), ref(op2))); }
		i16 sub_ov(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_I16), ref(op1), ref(op2))); }
		i32 sub_ov(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_I32), ref(op1), ref(op2))); }
		i64 sub_ov(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_I64), ref(op1), ref(op2))); }
		u8 sub_ov(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_U8), ref(op1), ref(op2))); }
		u16 sub_ov(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_U16), ref(op1), ref(op2))); }
		u32 sub_ov(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_U32), ref(op1), ref(op2))); }
		u64 sub_ov(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_U64), ref(op1), ref(op2))); }
		addr sub_ov(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_ADDR), ref(op1), ref(op2))); }
		c sub_ov(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SUB_OV, IR_CHAR), ref(op1), ref(op2))); }

		i8 mul_ov(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_I8), ref(op1), ref(op2))); }
		i16 mul_ov(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_I16), ref(op1), ref(op2))); }
		i32 mul_ov(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_I32), ref(op1), ref(op2))); }
		i64 mul_ov(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_I64), ref(op1), ref(op2))); }
		u8 mul_ov(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_U8), ref(op1), ref(op2))); }
		u16 mul_ov(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_U16), ref(op1), ref(op2))); }
		u32 mul_ov(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_U32), ref(op1), ref(op2))); }
		u64 mul_ov(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_U64), ref(op1), ref(op2))); }
		addr mul_ov(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_ADDR), ref(op1), ref(op2))); }
		c mul_ov(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MUL_OV, IR_CHAR), ref(op1), ref(op2))); }

		b overflow(ref op1) { return ir_fold1(&ctx, IR_OPT(IR_OVERFLOW, IR_BOOL), op1); }

		b not_cond(b op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_BOOL), ref(op1))); }
		i8 not_cond(i8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_I8), ref(op1))); }
		i16 not_cond(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_I16), ref(op1))); }
		i32 not_cond(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_I32), ref(op1))); }
		i64 not_cond(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_I64), ref(op1))); }
		u8 not_cond(u8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_U8), ref(op1))); }
		u16 not_cond(u16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_U16), ref(op1))); }
		u32 not_cond(u32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_U32), ref(op1))); }
		u64 not_cond(u64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_U64), ref(op1))); }
		addr not_cond(addr op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_ADDR), ref(op1))); }
		c not_cond(c op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_NOT, IR_CHAR), ref(op1))); }

		b or_cond(b op1, b op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_BOOL), ref(op1), ref(op2))); }
		i8 or_cond(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_I8), ref(op1), ref(op2))); }
		i16 or_cond(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_I16), ref(op1), ref(op2))); }
		i32 or_cond(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_I32), ref(op1), ref(op2))); }
		i64 or_cond(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_I64), ref(op1), ref(op2))); }
		u8 or_cond(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_U8), ref(op1), ref(op2))); }
		u16 or_cond(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_U16), ref(op1), ref(op2))); }
		u32 or_cond(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_U32), ref(op1), ref(op2))); }
		u64 or_cond(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_U64), ref(op1), ref(op2))); }
		addr or_cond(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_ADDR), ref(op1), ref(op2))); }
		c or_cond(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_OR, IR_CHAR), ref(op1), ref(op2))); }

		b and_cond(b op1, b op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_BOOL), ref(op1), ref(op2))); }
		i8 and_cond(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_I8), ref(op1), ref(op2))); }
		i16 and_cond(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_I16), ref(op1), ref(op2))); }
		i32 and_cond(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_I32), ref(op1), ref(op2))); }
		i64 and_cond(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_I64), ref(op1), ref(op2))); }
		u8 and_cond(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_U8), ref(op1), ref(op2))); }
		u16 and_cond(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_U16), ref(op1), ref(op2))); }
		u32 and_cond(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_U32), ref(op1), ref(op2))); }
		u64 and_cond(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_U64), ref(op1), ref(op2))); }
		addr and_cond(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_ADDR), ref(op1), ref(op2))); }
		c and_cond(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_AND, IR_CHAR), ref(op1), ref(op2))); }

		b xor_cond(b op1, b op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_BOOL), ref(op1), ref(op2))); }
		i8 xor_cond(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_I8), ref(op1), ref(op2))); }
		i16 xor_cond(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_I16), ref(op1), ref(op2))); }
		i32 xor_cond(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_I32), ref(op1), ref(op2))); }
		i64 xor_cond(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_I64), ref(op1), ref(op2))); }
		u8 xor_cond(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_U8), ref(op1), ref(op2))); }
		u16 xor_cond(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_U16), ref(op1), ref(op2))); }
		u32 xor_cond(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_U32), ref(op1), ref(op2))); }
		u64 xor_cond(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_U64), ref(op1), ref(op2))); }
		addr xor_cond(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_ADDR), ref(op1), ref(op2))); }
		c xor_cond(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_XOR, IR_CHAR), ref(op1), ref(op2))); }

		i8 shl(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_I8), ref(op1), ref(op2))); }
		i16 shl(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_I16), ref(op1), ref(op2))); }
		i32 shl(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_I32), ref(op1), ref(op2))); }
		i64 shl(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_I64), ref(op1), ref(op2))); }
		u8 shl(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_U8), ref(op1), ref(op2))); }
		u16 shl(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_U16), ref(op1), ref(op2))); }
		u32 shl(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_U32), ref(op1), ref(op2))); }
		u64 shl(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_U64), ref(op1), ref(op2))); }
		addr shl(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_ADDR), ref(op1), ref(op2))); }
		c shl(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHL, IR_CHAR), ref(op1), ref(op2))); }

		i8 shr(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_I8), ref(op1), ref(op2))); }
		i16 shr(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_I16), ref(op1), ref(op2))); }
		i32 shr(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_I32), ref(op1), ref(op2))); }
		i64 shr(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_I64), ref(op1), ref(op2))); }
		u8 shr(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_U8), ref(op1), ref(op2))); }
		u16 shr(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_U16), ref(op1), ref(op2))); }
		u32 shr(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_U32), ref(op1), ref(op2))); }
		u64 shr(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_U64), ref(op1), ref(op2))); }
		addr shr(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_ADDR), ref(op1), ref(op2))); }
		c shr(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SHR, IR_CHAR), ref(op1), ref(op2))); }

		i8 sar(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_I8), ref(op1), ref(op2))); }
		i16 sar(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_I16), ref(op1), ref(op2))); }
		i32 sar(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_I32), ref(op1), ref(op2))); }
		i64 sar(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_I64), ref(op1), ref(op2))); }
		u8 sar(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_U8), ref(op1), ref(op2))); }
		u16 sar(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_U16), ref(op1), ref(op2))); }
		u32 sar(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_U32), ref(op1), ref(op2))); }
		u64 sar(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_U64), ref(op1), ref(op2))); }
		addr sar(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_ADDR), ref(op1), ref(op2))); }
		c sar(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_SAR, IR_CHAR), ref(op1), ref(op2))); }

		i8 rol(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_I8), ref(op1), ref(op2))); }
		i16 rol(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_I16), ref(op1), ref(op2))); }
		i32 rol(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_I32), ref(op1), ref(op2))); }
		i64 rol(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_I64), ref(op1), ref(op2))); }
		u8 rol(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_U8), ref(op1), ref(op2))); }
		u16 rol(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_U16), ref(op1), ref(op2))); }
		u32 rol(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_U32), ref(op1), ref(op2))); }
		u64 rol(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_U64), ref(op1), ref(op2))); }
		addr rol(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_ADDR), ref(op1), ref(op2))); }
		c rol(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROL, IR_CHAR), ref(op1), ref(op2))); }

		i8 ror(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_I8), ref(op1), ref(op2))); }
		i16 ror(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_I16), ref(op1), ref(op2))); }
		i32 ror(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_I32), ref(op1), ref(op2))); }
		i64 ror(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_I64), ref(op1), ref(op2))); }
		u8 ror(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_U8), ref(op1), ref(op2))); }
		u16 ror(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_U16), ref(op1), ref(op2))); }
		u32 ror(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_U32), ref(op1), ref(op2))); }
		u64 ror(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_U64), ref(op1), ref(op2))); }
		addr ror(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_ADDR), ref(op1), ref(op2))); }
		c ror(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_ROR, IR_CHAR), ref(op1), ref(op2))); }

		i16 bswap(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BSWAP, IR_I16), ref(op1))); }
		i32 bswap(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BSWAP, IR_I32), ref(op1))); }
		i64 bswap(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BSWAP, IR_I64), ref(op1))); }
		u16 bswap(u16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BSWAP, IR_U16), ref(op1))); }
		u32 bswap(u32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BSWAP, IR_U32), ref(op1))); }
		u64 bswap(u64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BSWAP, IR_U64), ref(op1))); }
		addr bswap(addr op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_BSWAP, IR_ADDR), ref(op1))); }

		i8 min(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_I8), ref(op1), ref(op2))); }
		i16 min(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_I16), ref(op1), ref(op2))); }
		i32 min(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_I32), ref(op1), ref(op2))); }
		i64 min(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_I64), ref(op1), ref(op2))); }
		u8 min(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_U8), ref(op1), ref(op2))); }
		u16 min(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_U16), ref(op1), ref(op2))); }
		u32 min(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_U32), ref(op1), ref(op2))); }
		u64 min(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_U64), ref(op1), ref(op2))); }
		addr min(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_ADDR), ref(op1), ref(op2))); }
		c min(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_CHAR), ref(op1), ref(op2))); }
		d min(d op1, d op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_DOUBLE), ref(op1), ref(op2))); }
		f min(f op1, f op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MIN, IR_FLOAT), ref(op1), ref(op2))); }

		i8 max(i8 op1, i8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_I8), ref(op1), ref(op2))); }
		i16 max(i16 op1, i16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_I16), ref(op1), ref(op2))); }
		i32 max(i32 op1, i32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_I32), ref(op1), ref(op2))); }
		i64 max(i64 op1, i64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_I64), ref(op1), ref(op2))); }
		u8 max(u8 op1, u8 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_U8), ref(op1), ref(op2))); }
		u16 max(u16 op1, u16 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_U16), ref(op1), ref(op2))); }
		u32 max(u32 op1, u32 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_U32), ref(op1), ref(op2))); }
		u64 max(u64 op1, u64 op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_U64), ref(op1), ref(op2))); }
		addr max(addr op1, addr op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_ADDR), ref(op1), ref(op2))); }
		c max(c op1, c op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_CHAR), ref(op1), ref(op2))); }
		d max(d op1, d op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_DOUBLE), ref(op1), ref(op2))); }
		f max(f op1, f op2) { return ref(ir_fold2(&ctx, IR_OPT(IR_MAX, IR_FLOAT), ref(op1), ref(op2))); }

		i8 cond(i8 op1, i8 op2, i8 op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_I8), ref(op1), ref(op2), ref(op3))); }
		i16 cond(i16 op1, i16 op2, i16 op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_I16), ref(op1), ref(op2), ref(op3))); }
		i32 cond(i32 op1, i32 op2, i32 op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_I32), ref(op1), ref(op2), ref(op3))); }
		i64 cond(i64 op1, i64 op2, i64 op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_I64), ref(op1), ref(op2), ref(op3))); }
		u8 cond(u8 op1, u8 op2, u8 op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_U8), ref(op1), ref(op2), ref(op3))); }
		u16 cond(u16 op1, u16 op2, u16 op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_U16), ref(op1), ref(op2), ref(op3))); }
		u32 cond(u32 op1, u32 op2, u32 op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_U32), ref(op1), ref(op2), ref(op3))); }
		u64 cond(u64 op1, u64 op2, u64 op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_U64), ref(op1), ref(op2), ref(op3))); }
		addr cond(addr op1, addr op2, addr op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_ADDR), ref(op1), ref(op2), ref(op3))); }
		c cond(c op1, c op2, c op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_CHAR), ref(op1), ref(op2), ref(op3))); }
		d cond(d op1, d op2, d op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_DOUBLE), ref(op1), ref(op2), ref(op3))); }
		f cond(f op1, f op2, f op3) { return ref(ir_fold3(&ctx, IR_OPT(IR_COND, IR_FLOAT), ref(op1), ref(op2), ref(op3))); }

		ref phi(ref src1) { return _ir_PHI_2(&ctx, src1, IR_UNUSED); }
		ref phi(ref src1, ref src2) { return _ir_PHI_2(&ctx, src1, src2); }
		void phi_set_op(ref phi, ref pos, ref src) { _ir_PHI_SET_OP(&ctx, phi, pos, src); }

		//addr add_addr(addr addr, addr offset) { return _ir_ADD_OFFSET(&ctx, addr, offset);  }

		i8 copy(i8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_I8), ref(op1))); }
		i16 copy(i16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_I16), ref(op1))); }
		i32 copy(i32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_I32), ref(op1))); }
		i64 copy(i64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_I64), ref(op1))); }
		u8 copy(u8 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_U8), ref(op1))); }
		u16 copy(u16 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_U16), ref(op1))); }
		u32 copy(u32 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_U32), ref(op1))); }
		u64 copy(u64 op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_U64), ref(op1))); }
		addr copy(addr op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_ADDR), ref(op1))); }
		c copy(c op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_CHAR), ref(op1))); }
		b copy(b op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_BOOL), ref(op1))); }
		f copy(f op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_FLOAT), ref(op1))); }
		d copy(d op1) { return ref(ir_fold1(&ctx, IR_OPT(IR_COPY, IR_DOUBLE), ref(op1))); }

		i8 hard_copy(i8 op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_I8), ref(op1), 1)); }
		i16 hard_copy(i16 op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_I16), ref(op1), 1)); }
		i32 hard_copy(i32 op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_I32), ref(op1), 1)); }
		i64 hard_copy(i64 op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_I64), ref(op1), 1)); }
		u8 hard_copy(u8 op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_U8), ref(op1), 1)); }
		u16 hard_copy(u16 op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_U16), ref(op1), 1)); }
		u32 hard_copy(u32 op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_U32), ref(op1), 1)); }
		u64 hard_copy(u64 op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_U64), ref(op1), 1)); }
		addr hard_copy(addr op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_ADDR), ref(op1), 1)); }
		c hard_copy(c op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_CHAR), ref(op1), 1)); }
		b hard_copy(b op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_BOOL), ref(op1), 1)); }
		f hard_copy(f op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_FLOAT), ref(op1), 1)); }
		d hard_copy(d op1) { return ref(ir_fold2(&ctx, IR_OPT(IR_COPY, IR_DOUBLE), ref(op1), 1)); }

		template <class T>
		T param(T typ, const std::string& name, uint8_t num = 0) {
			// NOTE There is likely no chance to verify whether the number is correct.
			//      Numbers can be passed in an arbitrary order.
			num = num > 0 ? num : ++param_pos_cnt;
			return ref(_ir_PARAM(&ctx, type(typ), name.c_str(), num));
		}

		template <class T>
		T var(T typ, const std::string& name) { return ref(_ir_VAR(&ctx, type(typ), name.c_str())); }

		template <class T>
		T call(T typ, ref func) { return ref(_ir_CALL(&ctx, typ, func)); }
		template <class T>
		T call(T typ, ref func, ref a1) { return ref(_ir_CALL_1(&ctx, typ, func, a1)); }
		template <class T>
		T call(T typ, ref func, ref a1, ref a2) { return ref(_ir_CALL_2(&ctx, typ, func, a1, a2)); }
		template <class T>
		T call(T typ, ref func, ref a1, ref a2, ref a3) { return ref(_ir_CALL_3(&ctx, typ, func, a1, a2, a3)); }
		template <class T>
		T call(T typ, ref func, ref a1, ref a2, ref a3, ref a4) { return ref(_ir_CALL_4(&ctx, typ, func, a1, a2, a3, a4)); }
		template <class T>
		T call(T typ, ref func, ref a1, ref a2, ref a3, ref a4, ref a5) { return ref(_ir_CALL_5(&ctx, typ, func, a1, a2, a3, a4, a5)); }

		void tailcall(ref func) { _ir_TAILCALL(&ctx, func); }
		void tailcall(ref func, ref a1) { _ir_TAILCALL_1(&ctx, func, a1); }
		void tailcall(ref func, ref a1, ref a2) { _ir_TAILCALL_2(&ctx, func, a1, a2); }
		void tailcall(ref func, ref a1, ref a2, ref a3) { _ir_TAILCALL_3(&ctx, func, a1, a2, a3); }
		void tailcall(ref func, ref a1, ref a2, ref a3, ref a4) { _ir_TAILCALL_4(&ctx, func, a1, a2, a3, a4); }
		void tailcall(ref func, ref a1, ref a2, ref a3, ref a4, ref a5) { _ir_TAILCALL_5(&ctx, func, a1, a2, a3, a4, a5); }

		// XXX afree to accept ref??
		ref alloca(int32_t size) { return  _ir_ALLOCA(&ctx, size); }
		void afree(ref r) { _ir_AFREE(&ctx, r); }

		// XXX fail, second arg is uint32_t currently
		//addr vaddr(ref var) { return ref(ir_emit1(&ctx, &ctx, var)); }

		template <class T>
		T vload(T var) { return ref(_ir_VLOAD(&ctx, type(var), ref(var))); }
		void vstore(ref var, ref val) { _ir_VSTORE(&ctx, var, val); }

		template <class T>
		T rload(T val, ref reg) { return ref(_ir_RLOAD(&ctx, type(val), reg)); }
		void rstore(ref reg, ref val) { _ir_RSTORE(&ctx, reg, val); }

		// XXX it's about addr, not var, still ok?
		template <class T>
		T load(T addr) { return ref(_ir_LOAD(&ctx, type(addr), ref(addr))); }
		void store(ref addr, ref val) { _ir_STORE(&ctx, addr, val); }

		// XXX type for offset?
		ref tls(ref index, int32_t offset) { return _ir_TLS(&ctx, index, offset); }

		void trap() { ctx.control = ir_emit1(&ctx, IR_TRAP, ctx.control); }

		void start() { _ir_START(&ctx); }
		// XXX datatype for num
		void entry(ref src, int32_t num) { _ir_ENTRY(&ctx, src, num); }
		void begin(ref src) { _ir_BEGIN(&ctx, src); }
		ref if_cond(ref condition) { return _ir_IF(&ctx, condition); }
		void if_true(ref if_ref) { _ir_IF_TRUE(&ctx, if_ref); }
		void if_true_cold(ref if_ref) { _ir_IF_TRUE_cold(&ctx, if_ref); }
		void if_false(ref if_ref) { _ir_IF_FALSE(&ctx, if_ref); }
		void if_false_cold(ref if_ref) { _ir_IF_FALSE_cold(&ctx, if_ref); }
		ref end() { return _ir_END(&ctx); }
		ref end_list(ref list) { return _ir_END_LIST(&ctx, list); }
		void merge(ref src1, ref src2) { return _ir_MERGE_2(&ctx, src1, src2); }
		// XXX datatypes 
		void merge(ir_ref n, ir_ref* inputs) { _ir_MERGE_N(&ctx, n, inputs); }
		void merge_set_op(ref merge, ref pos, ref src) { _ir_MERGE_SET_OP(&ctx, merge, pos, src); }
		void merge_list(ref list) { _ir_MERGE_LIST(&ctx, list); }
		void merge_with(ref src2) { auto _end = end();  merge(_end, src2); }
		void merge_with_empty_true(ref _if) { auto _end = end(); if_true(_if); merge(_end, end()); }
		void merge_with_empty_false(ref _if) { auto _end = end(); if_false(_if); merge(_end, end()); }
		ref loop_begin(ref src1) { return _ir_LOOP_BEGIN(&ctx, src1); }
		ref loop_begin() { return loop_begin(end()); }
		ref loop_end() { return _ir_LOOP_END(&ctx); }
		ref switch_statement (ref val) { return _ir_SWITCH(&ctx, val); }
		void case_val(ref switch_ref, ref val) { _ir_CASE_VAL(&ctx, switch_ref, val); }
		void case_default(ref switch_ref) { _ir_CASE_DEFAULT(&ctx, switch_ref); }
		void ret(ref val) { _ir_RETURN(&ctx, val); }
		void ijmp(addr _addr) { _ir_IJMP(&ctx, ref(_addr)); }
		void unreachable() { _ir_UNREACHABLE(&ctx); }
		void guard(ref condition, addr _addr) { _ir_GUARD(&ctx, condition, ref(_addr)); }
		void guard_not(ref condition, addr _addr) { _ir_GUARD_NOT(&ctx, condition, ref(_addr)); }
		// XXX ref type int32?
		ref snapshot(ref n) { return _ir_SNAPSHOT(&ctx, n); }
		// XXX data types int32?
		void snapshot_set_op(ref snapshot, ref pos, ref val) { _ir_SNAPSHOT_SET_OP(&ctx, snapshot, pos, val); }
		ref exitcall(ref func) { return _ir_EXITCALL(&ctx, func); }
	};
}

#endif
