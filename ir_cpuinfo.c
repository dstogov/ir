/*
 * IR - Lightweight JIT Compilation Framework
 * (CPU framework for x86)
 * Copyright (C) 2023 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"

#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)

#ifndef _WIN32
IR_ALWAYS_INLINE void ir_cpuid_ex(uint32_t info[4], uint32_t function, uint32_t index)
{
	__asm__ volatile("cpuid"
		: "=a" (info[0]),
		"=b" (info[1]),
		"=c" (info[2]),
		"=d" (info[3])
		: "0" (function), "2" (index)
		: "memory");
}
IR_ALWAYS_INLINE void ir_cpuid(uint32_t info[4], uint32_t function)
{
	ir_cpuid_ex(info, function, 0);
}
#else
#define ir_cpuid_ex __cpuidex
#define ir_cpuid __cpuid
#endif

/* Intel SDM Vol. 2A */
uint32_t ir_cpuinfo(void)
{
	uint32_t ret = 0;
	uint32_t info_0x1[4] = {0}, info_0x7_0[4] = {0};
#define bit(mask, pos) (((mask) >> (pos)) & 1U)

	ir_cpuid(info_0x1, 0x1);
	if (bit(info_0x1[2], 0U)) ret |= IR_X86_SSE3;
	if (bit(info_0x1[2], 9U)) ret |= IR_X86_SSSE3;
//	if (bit(info_0x1[2], 12U)) ret |= IR_X86_FMA;
//	if (bit(info_0x1[2], 13U)) ret |= CMPXCHG16B;
	if (bit(info_0x1[2], 19U)) ret |= IR_X86_SSE41;
	if (bit(info_0x1[2], 20U)) ret |= IR_X86_SSE42;
//	if (bit(info_0x1[2], 22U)) ret |= IR_X86_MOBBE;
//	if (bit(info_0x1[2], 23U)) ret |= IR_X86_POPCNT;
//	if (bit(info_0x1[2], 25U)) ret |= IR_X86_AES;
	if (bit(info_0x1[2], 28U)) ret |= IR_X86_AVX;
//	if (bit(info_0x1[2], 29U)) ret |= IR_X86_F16C;
//	if (bit(info_0x1[3], 8U)) ret |= CMPXCHG8B;
//	if (bit(info_0x1[3], 15U)) ret |= IR_X86_CMOV;
//	if (bit(info_0x1[3], 23U)) ret |= IR_X86_MMX;
//	if (bit(info_0x1[3], 25U)) ret |= IR_X86_SSE;
	if (bit(info_0x1[3], 26U)) ret |= IR_X86_SSE2;

	ir_cpuid_ex(info_0x7_0, 0x7, 0);
//	if (bit(info_0x7_0[0], 0U)) ret |= IR_X86_SHA512;
//	if (bit(info_0x7_0[0], 4U)) ret |= IR_X86_AVX_VNNI;
//	if (bit(info_0x7_0[0], 5U)) ret |= IR_X86_AVX512BF16;
//	if (bit(info_0x7_0[0], 7U)) ret |= IR_X86_CMPCCXADD;
//	if (bit(info_0x7_0[0], 21U)) ret |= IR_X86_AMX_FP16;
//	if (bit(info_0x7_0[0], 23U)) ret |= IR_X86_AMX_IFMA;
//	if (bit(info_0x7_0[1], 2U)) ret |= IR_X86_SGX;
	if (bit(info_0x7_0[1], 3U)) ret |= IR_X86_BMI1;
//	if (bit(info_0x7_0[1], 4U)) ret |= IR_X86_HLE;
	if (bit(info_0x7_0[1], 5U)) ret |= IR_X86_AVX2;
	if (bit(info_0x7_0[1], 8U)) ret |= IR_X86_BMI2;
//	if (bit(info_0x7_0[1], 9U)) ret |= IR_X86_ERMS; /* REP_MOVSB */
//	if (bit(info_0x7_0[1], 16U)) ret |= IR_X86_AVX512F;
//	if (bit(info_0x7_0[1], 17U)) ret |= IR_X86_AVX512DQ;
//	if (bit(info_0x7_0[1], 19U)) ret |= IR_X86_ADX;
//	if (bit(info_0x7_0[1], 21U)) ret |= IR_X86_AVX512IFMA;
//	if (bit(info_0x7_0[1], 26U)) ret |= IR_X86_AVX512PF;
//	if (bit(info_0x7_0[1], 27U)) ret |= IR_X86_AVX512ER;
//	if (bit(info_0x7_0[1], 28U)) ret |= IR_X86_AVX512CD;
//	if (bit(info_0x7_0[1], 29U)) ret |= IR_X86_SHA;
//	if (bit(info_0x7_0[1], 30U)) ret |= IR_X86_AVX512BW;
//	if (bit(info_0x7_0[1], 31U)) ret |= IR_X86_AVX512VL;
//	if (bit(info_0x7_0[2], 1U)) ret |= IR_X86_AVX512VBMI;
//	if (bit(info_0x7_0[2], 6U)) ret |= IR_X86_AVX512VBMI2;
//	if (bit(info_0x7_0[2], 8U)) ret |= IR_X86_GFNI;
//	if (bit(info_0x7_0[2], 9U)) ret |= IR_X86_VAES;
//	if (bit(info_0x7_0[2], 10U)) ret |= IR_X86_VPCL;
//	if (bit(info_0x7_0[2], 11U)) ret |= IR_X86_AVX512VNNI;
//	if (bit(info_0x7_0[2], 12U)) ret |= IR_X86_AVX512BITALG;
//	if (bit(info_0x7_0[2], 14U)) ret |= IR_X86_AVX512VPOPCNT_DQ;
	if (bit(info_0x7_0[2], 25U)) ret |= IR_X86_CLDEMOTE;
//	if (bit(info_0x7_0[3], 2U)) ret |= IR_X86_AVX512QVNNIW;
//	if (bit(info_0x7_0[3], 3U)) ret |= IR_X86_AVX512QFMA;
//	if (bit(info_0x7_0[3], 4U)) ret |= IR_X86_FSREPMOVSB;
//	if (bit(info_0x7_0[3], 7U)) ret |= IR_X86_AVX512BITALG2;
//	if (bit(info_0x7_0[3], 8U)) ret |= IR_X86_AVX512VP2INTERSECT;
//	if (bit(info_0x7_0[3], 22U)) ret |= IR_X86_AMX_BF16;
//	if (bit(info_0x7_0[3], 23U)) ret |= IR_X86_AVX512FP16;
//	if (bit(info_0x7_0[3], 24U)) ret |= IR_X86_AMX_TILE;
//	if (bit(info_0x7_0[3], 25U)) ret |= IR_X86_AMX_INT8;
#undef bit

	return ret;
}
#else
uint32_t ir_cpuinfo(void)
{
	return 0;
}
#endif /* IR_TARGET_X86 || IR_TARGET_X64 */
