/*
 * IR - Lightweight JIT Compilation Framework
 * (RISC-V 64 hand-written binary instruction encoder — no DynAsm)
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

/* ---- R-type: funct3/funct7 for OP (0x33) ---- */
#define RV_F3_ADD   0x0
#define RV_F7_ADD   0x00
#define RV_F3_SUB   0x0
#define RV_F7_SUB   0x20
#define RV_F3_MUL   0x0
#define RV_F7_MUL   0x01

/* ---- I-type: funct3 for OP-IMM (0x13) ---- */
#define RV_F3_ADDI  0x0

/* ---- I-type: JALR (0x67) ---- */
#define RV_F3_JALR  0x0

/*
 * Instruction format encoders.
 * Field layouts follow the RISC-V base ISA spec (RV32I/RV64I, ch. 2.3).
 * B-type and J-type immediates are bit-scrambled — handled separately
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

#endif /* IR_RISCV_ENC_H */
