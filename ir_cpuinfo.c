/*
 * IR - Lightweight JIT Compilation Framework
 * (CPU framework for x86)
 * Copyright (C) 2023 by IR project.
 * Authors: Anatol Belski <anbelski@linux.microsoft.com>
 */

#include "ir.h"

#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)

#include "ir_x86.h"

#ifndef _WIN32
IR_ALWAYS_INLINE void ir_cpuid_ex(uint32_t info[4], uint32_t function, uint32_t index)
{
	asm volatile("cpuid"
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
	if (bit(info_0x1[3], 26U)) ret |= IR_X86_SSE2;
	if (bit(info_0x1[2], 0U)) ret |= IR_X86_SSE3;
	if (bit(info_0x1[2], 9U)) ret |= IR_X86_SSSE3;
	if (bit(info_0x1[2], 19U)) ret |= IR_X86_SSE41;
	if (bit(info_0x1[2], 20U)) ret |= IR_X86_SSE42;
	if (bit(info_0x1[2], 28U)) ret |= IR_X86_AVX;

	ir_cpuid_ex(info_0x7_0, 0x7, 0);
	if (bit(info_0x7_0[1], 5U)) ret |= IR_X86_AVX2;

#undef bit

	return ret;
}
#else
uint32_t ir_cpuinfo(void)
{
	return 0;
}
#endif /* IR_TARGET_X86 || IR_TARGET_X64 */
