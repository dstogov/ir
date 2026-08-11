/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V 64 hand-written binary instruction encoder --- no DynAsm)
 */

#ifndef IR_RISCV_ENC_H
#define IR_RISCV_ENC_H

#include <stdint.h>

/* ---- base opcodes (bits[6:0]) ---- */
#define RV_OP_LOAD     0x03
#define RV_OP_IMM      0x13
#define RV_OP_STORE    0x23
#define RV_OP_R        0x33  /* register-register ALU */
#define RV_OP_LUI      0x37
#define RV_OP_BRANCH   0x63
#define RV_OP_JALR     0x67
#define RV_OP_JAL      0x6F
#define RV_OP_FP       0x53

/* ---- R-type: funct3/funct7 for OP (0x33) / OP-32 (0x3B) ---- */
#define RV_F3_ADD   0x0
#define RV_F7_ADD   0x00
#define RV_F3_SUB   0x0
#define RV_F7_SUB   0x20
#define RV_F3_MUL   0x0
#define RV_F7_MUL   0x01
#define RV_F3_SLL   0x1
#define RV_F3_SLT   0x2
#define RV_F3_SLTU  0x3
#define RV_F3_XOR   0x4
#define RV_F3_SR    0x5
#define RV_F3_SRL   0x5
#define RV_F3_SRA   0x5
#define RV_F7_SRL   0x00
#define RV_F7_SRA   0x20
#define RV_F3_OR    0x6
#define RV_F3_AND   0x7
#define RV_F7_BASE  0x00

/* ---- I-type: funct3 for OP-IMM (0x13) / OP-IMM-32 (0x1B) ---- */
#define RV_F3_ADDI  0x0
#define RV_F3_SLLI  0x1
#define RV_F3_XORI  0x4
#define RV_F3_SRI   0x5
#define RV_F3_ORI   0x6
#define RV_F3_ANDI  0x7

#define RV_OP_IMM32  0x1B
#define RV_OP_R32    0x3B

/* ---- I-type: JALR (0x67) ---- */
#define RV_F3_JALR  0x0

/*
 * Instruction format encoders.
 * Field layouts follow the RISC-V base ISA spec (RV32I/RV64I, ch. 2.3).
 * B-type and J-type immediates are bit-scrambled --- handled separately
 * from R/I/S so a mistake there can't silently corrupt R/I/S encoding.
 */

static inline uint32_t rv_enc_r(uint32_t funct7, uint32_t rs2, uint32_t rs1,
                                 uint32_t funct3, uint32_t rd, uint32_t opcode)
{
	return (funct7 << 25) | (rs2 << 20) | (rs1 << 15)
	     | (funct3 << 12) | (rd << 7) | opcode;
}

static inline uint32_t rv_enc_i(int32_t imm, uint32_t rs1, uint32_t funct3,
                                 uint32_t rd, uint32_t opcode)
{
	return (((uint32_t)imm & 0xFFF) << 20) | (rs1 << 15)
	     | (funct3 << 12) | (rd << 7) | opcode;
}

static inline uint32_t rv_enc_s(int32_t imm, uint32_t rs2, uint32_t rs1,
                                 uint32_t funct3, uint32_t opcode)
{
	uint32_t u = (uint32_t)imm;
	return ((u & 0xFE0) << 20) | (rs2 << 20) | (rs1 << 15)
	     | (funct3 << 12) | ((u & 0x1F) << 7) | opcode;
}

static inline uint32_t rv_enc_b(int32_t imm, uint32_t rs2, uint32_t rs1,
                                 uint32_t funct3, uint32_t opcode)
{
	uint32_t u = (uint32_t)imm;
	return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25)
	     | (rs2 << 20) | (rs1 << 15) | (funct3 << 12)
	     | (((u >> 1) & 0xF) << 8) | (((u >> 11) & 1) << 7) | opcode;
}

static inline uint32_t rv_enc_u(int32_t imm, uint32_t rd, uint32_t opcode)
{
	return ((uint32_t)imm & 0xFFFFF000) | (rd << 7) | opcode;
}

static inline uint32_t rv_enc_j(int32_t imm, uint32_t rd, uint32_t opcode)
{
	uint32_t u = (uint32_t)imm;
	return (((u >> 20) & 1) << 31) | (((u >> 1) & 0x3FF) << 21)
	     | (((u >> 11) & 1) << 20) | (((u >> 12) & 0xFF) << 12)
	     | (rd << 7) | opcode;
}

/* Convenience wrappers for the handful of instructions this first pass needs. */

static inline uint32_t rv_add(uint32_t rd, uint32_t rs1, uint32_t rs2)
{
	return rv_enc_r(RV_F7_ADD, rs2, rs1, RV_F3_ADD, rd, RV_OP_R);
}

static inline uint32_t rv_addi(uint32_t rd, uint32_t rs1, int32_t imm)
{
	return rv_enc_i(imm, rs1, RV_F3_ADDI, rd, RV_OP_IMM);
}

/* Generic ALU forms: w selects the 32-bit (sign-extending) variant. */

static inline uint32_t rv_alu(int w, uint32_t f7, uint32_t f3,
                              uint32_t rd, uint32_t rs1, uint32_t rs2)
{
	return rv_enc_r(f7, rs2, rs1, f3, rd, w ? RV_OP_R32 : RV_OP_R);
}

static inline uint32_t rv_alui(int w, uint32_t f3,
                               uint32_t rd, uint32_t rs1, int32_t imm)
{
	return rv_enc_i(imm, rs1, f3, rd, w ? RV_OP_IMM32 : RV_OP_IMM);
}

/* Shift-immediate: shamt is 6 bits (RV64) / 5 bits (W form); SRAI sets imm bit 10. */
static inline uint32_t rv_shifti(int w, uint32_t f3, int arithmetic,
                                 uint32_t rd, uint32_t rs1, uint32_t shamt)
{
	uint32_t imm = (arithmetic ? 0x400 : 0) | (shamt & (w ? 0x1F : 0x3F));

	return rv_enc_i((int32_t)imm, rs1, f3, rd, w ? RV_OP_IMM32 : RV_OP_IMM);
}

/* li for constants that fit in int12; larger values need lui chains (not yet) */
static inline uint32_t rv_li12(uint32_t rd, int32_t imm)
{
	return rv_addi(rd, 0 /* x0 */, imm);
}

/* mv rd, rs  ==  addi rd, rs, 0 */
static inline uint32_t rv_mv(uint32_t rd, uint32_t rs)
{
	return rv_addi(rd, rs, 0);
}

/* ret  ==  jalr x0, ra, 0 */
static inline uint32_t rv_ret(uint32_t ra_reg)
{
	return rv_enc_i(0, ra_reg, RV_F3_JALR, 0 /* x0 */, RV_OP_JALR);
}

static inline uint32_t rv_jalr(uint32_t rd, uint32_t rs1, int32_t imm)
{
	return rv_enc_i(imm, rs1, RV_F3_JALR, rd, RV_OP_JALR);
}

/* ---- memory: funct3 for LOAD (0x03) / STORE (0x23) ---- */
#define RV_F3_LB  0
#define RV_F3_LH  1
#define RV_F3_LW  2
#define RV_F3_LD  3
#define RV_F3_LBU 4
#define RV_F3_LHU 5
#define RV_F3_LWU 6
#define RV_F3_SB  0
#define RV_F3_SH  1
#define RV_F3_SW  2
#define RV_F3_SD  3

static inline uint32_t rv_load(uint32_t f3, uint32_t rd, uint32_t rs1, int32_t off)
{
	return rv_enc_i(off, rs1, f3, rd, RV_OP_LOAD);
}

static inline uint32_t rv_store(uint32_t f3, uint32_t rs2, uint32_t rs1, int32_t off)
{
	return rv_enc_s(off, rs2, rs1, f3, RV_OP_STORE);
}

/* M extension: div/divu/rem/remu */
static inline uint32_t rv_div(uint32_t rd, uint32_t rs1, uint32_t rs2)
{ return rv_alu(0, 0x01, 0x04, rd, rs1, rs2); }
static inline uint32_t rv_divu(uint32_t rd, uint32_t rs1, uint32_t rs2)
{ return rv_alu(0, 0x01, 0x05, rd, rs1, rs2); }
static inline uint32_t rv_rem(uint32_t rd, uint32_t rs1, uint32_t rs2)
{ return rv_alu(0, 0x01, 0x06, rd, rs1, rs2); }
static inline uint32_t rv_remu(uint32_t rd, uint32_t rs1, uint32_t rs2)
{ return rv_alu(0, 0x01, 0x07, rd, rs1, rs2); }
static inline uint32_t rv_divw(uint32_t rd, uint32_t rs1, uint32_t rs2)
{ return rv_alu(1, 0x01, 0x04, rd, rs1, rs2); }  /* divw */
static inline uint32_t rv_divuw(uint32_t rd, uint32_t rs1, uint32_t rs2)
{ return rv_alu(1, 0x01, 0x05, rd, rs1, rs2); }  /* divuw */
static inline uint32_t rv_remw(uint32_t rd, uint32_t rs1, uint32_t rs2)
{ return rv_alu(1, 0x01, 0x06, rd, rs1, rs2); }  /* remw */
static inline uint32_t rv_remuw(uint32_t rd, uint32_t rs1, uint32_t rs2)
{ return rv_alu(1, 0x01, 0x07, rd, rs1, rs2); }  /* remuw */

/* Zbb: clz/ctz/cpop (I-type, funct3=1, imm[11:5] selects the op) */
#define RV_ZBB_IMM_CLZ  0x600
#define RV_ZBB_IMM_CTZ  0x601
#define RV_ZBB_IMM_CPOP 0x602
static inline uint32_t rv_clz(uint32_t rd, uint32_t rs)
{ return rv_enc_i(RV_ZBB_IMM_CLZ, rs, 1, rd, RV_OP_IMM); }
static inline uint32_t rv_ctz(uint32_t rd, uint32_t rs)
{ return rv_enc_i(RV_ZBB_IMM_CTZ, rs, 1, rd, RV_OP_IMM); }
static inline uint32_t rv_cpop(uint32_t rd, uint32_t rs)
{ return rv_enc_i(RV_ZBB_IMM_CPOP, rs, 1, rd, RV_OP_IMM); }

#endif /* IR_RISCV_ENC_H */
