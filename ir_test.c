/*
 * IR - Lightweight JIT Compilation Framework
 * (Mandelbrot example)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 */

#include "ir.h"
#include "ir_builder.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
# include <sys/time.h>
#endif

#define BAILOUT 16
#define MAX_ITERATIONS 1000

void gen_mandelbrot(ir_ctx *ctx)
{
	ir_START();
	ir_ref x = ir_PARAM(IR_DOUBLE, "x", 1);
	ir_ref y = ir_PARAM(IR_DOUBLE, "y", 2);
	ir_ref cr = ir_SUB_D(y, ir_CONST_DOUBLE(0.5));
	ir_ref ci = ir_COPY_D(x);
	ir_ref zi = ir_COPY_D(ir_CONST_DOUBLE(0.0));
	ir_ref zr = ir_COPY_D(ir_CONST_DOUBLE(0.0));
	ir_ref i = ir_COPY_D(ir_CONST_I32(0));

	ir_ref loop = ir_LOOP_BEGIN(ir_END());
		ir_ref zi_1 = ir_PHI_2(zi, IR_UNUSED);
		ir_ref zr_1 = ir_PHI_2(zr, IR_UNUSED);
		ir_ref i_1 = ir_PHI_2(i, IR_UNUSED);

		ir_ref i_2 = ir_ADD_I32(i_1, ir_CONST_I32(1));
		ir_ref temp = ir_MUL_D(zr_1, zi_1);
		ir_ref zr2 = ir_MUL_D(zr_1, zr_1);
		ir_ref zi2 = ir_MUL_D(zi_1, zi_1);
		ir_ref zr_2 = ir_ADD_D(ir_SUB_D(zr2, zi2), cr);
		ir_ref zi_2 = ir_ADD_D(ir_ADD_D(temp, temp), ci);
		ir_ref if_1 = ir_IF(ir_GT(ir_ADD_D(zi2, zr2), ir_CONST_DOUBLE(16.0)));
			ir_IF_TRUE(if_1);
				ir_RETURN(i_2);
			ir_IF_FALSE(if_1);
				ir_ref if_2 = ir_IF(ir_GT(i_2, ir_CONST_I32(1000)));
				ir_IF_TRUE(if_2);
					ir_RETURN(ir_CONST_I32(0));
				ir_IF_FALSE(if_2);
					ir_ref loop_end = ir_LOOP_END(loop);

	/* close loop */
	ir_MERGE_SET_OP(loop, 2, loop_end);
	ir_PHI_SET_OP(zi_1, 2, zi_2);
	ir_PHI_SET_OP(zr_1, 2, zr_2);
	ir_PHI_SET_OP(i_1, 2, i_2);
}

typedef int (*mandelbrot_t)(double, double);

void run(mandelbrot_t mandelbrot)
{
#ifndef _WIN32
	struct timeval aTv;
	gettimeofday(&aTv, NULL);
	long init_time = aTv.tv_sec;
	long init_usec = aTv.tv_usec;
#endif

	int x,y;
	for (y = -39; y < 39; y++) {
		printf("\n");
		for (x = -39; x < 39; x++) {
			int i = mandelbrot(x/40.0, y/40.0);
			if (i==0)
				printf("*");
			else
				printf(" ");
		}
	}
	printf ("\n");

#ifndef _WIN32
	gettimeofday(&aTv,NULL);
	double query_time = (aTv.tv_sec - init_time) + (double)(aTv.tv_usec - init_usec)/1000000.0;
	printf ("C Elapsed %0.3f\n", query_time);
#endif
}

int main(int argc, char **argv)
{
	ir_ctx ctx;
	FILE *f;
	int i;
	int opt_level = 2;
	uint32_t mflags = 0;
	uint64_t debug_regset = 0xffffffffffffffff;

	ir_consistency_check();

	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] == 'O' && strlen(argv[i]) == 3) {
			if (argv[i][2] == '0') {
				opt_level = 0;
			} else if (argv[i][2] == '1') {
				opt_level = 1;
			} else if (argv[i][2] == '2') {
				opt_level = 2;
			} else {
				/* pass */
			}
		} else if (strcmp(argv[i], "-mavx") == 0) {
			mflags |= IR_AVX;
		} else if (strcmp(argv[i], "-muse-fp") == 0) {
			mflags |= IR_USE_FRAME_POINTER;
#ifdef IR_DEBUG
		} else if (strcmp(argv[i], "--debug-sccp") == 0) {
			mflags |= IR_DEBUG_SCCP;
		} else if (strcmp(argv[i], "--debug-gcm") == 0) {
			mflags |= IR_DEBUG_GCM;
		} else if (strcmp(argv[i], "--debug-schedule") == 0) {
			mflags |= IR_DEBUG_SCHEDULE;
		} else if (strcmp(argv[i], "--debug-ra") == 0) {
			mflags |= IR_DEBUG_RA;
#endif
		} else if (strcmp(argv[i], "--debug-regset") == 0) {
			if (i + 1 == argc || argv[i + 1][0] == '-') {
				fprintf(stderr, "ERROR: Invalid usage' (use --help)\n");
				return 1;
			}
			debug_regset = strtoull(argv[i + 1], NULL, 0);
			i++;
		} else {
			/* pass*/
		}
	}

	ir_init(&ctx, 256, 1024);
	ctx.flags |= IR_FUNCTION;
	ctx.flags |= mflags;
	if (opt_level > 0) {
		ctx.flags |= IR_OPT_FOLDING | IR_OPT_CFG | IR_OPT_CODEGEN;
	}
	ctx.fixed_regset = ~debug_regset;
	gen_mandelbrot(&ctx);
//	ir_save(&ctx, stderr);

	ir_build_def_use_lists(&ctx);
	if (opt_level > 1) {
		ir_sccp(&ctx);
	}
	ir_build_cfg(&ctx);
	if (opt_level <= 1) {
		ir_remove_unreachable_blocks(&ctx);
	}
	if (opt_level > 0) {
		ir_build_dominators_tree(&ctx);
		ir_find_loops(&ctx);
		ir_gcm(&ctx);
		ir_schedule(&ctx);
	}
	ir_match(&ctx);
	ir_assign_virtual_registers(&ctx);
	if (opt_level > 0) {
		ir_compute_live_ranges(&ctx);
		ir_coalesce(&ctx);
		ir_reg_alloc(&ctx);
		ir_schedule_blocks(&ctx);
	} else {
		ir_compute_dessa_moves(&ctx);
	}

	ir_truncate(&ctx);
//	ir_dump(&ctx, stderr);
	ir_save(&ctx, stderr);
	ir_dump_live_ranges(&ctx, stderr);
	f = fopen("ir.dot", "w+");
	ir_dump_dot(&ctx, f);
	fclose(f);

	size_t size;
	void *entry = ir_emit_code(&ctx, &size);

	if (entry) {
		ir_disasm("test", entry, size, 0, &ctx, stderr);

#ifndef _WIN32
		ir_perf_map_register("test", entry, size);
		ir_perf_jitdump_open();
		ir_perf_jitdump_register("test", entry,	size);

		ir_mem_unprotect(entry, 4096);
		ir_gdb_register("test", entry, size, sizeof(void*), 0);
		ir_mem_protect(entry, 4096);
#endif

		run((mandelbrot_t)entry);

#ifndef _WIN32
		ir_perf_jitdump_close();
#endif
	}

	ir_free(&ctx);
	return 0;
}
