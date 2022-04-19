#include "ir.h"
#include <sys/time.h>

#define BAILOUT 16
#define MAX_ITERATIONS 1000

void gen_mandelbrot(ir_ctx *ctx)
{
	ir_ref start = ir_emit0(ctx, IR_START);
	ir_ref ret;

	ir_ref x_1 = ir_param(ctx, IR_DOUBLE, start, "x", 0);
	ir_ref y_1 = ir_param(ctx, IR_DOUBLE, start, "y", 1);

	ir_ref cr = ir_var(ctx, IR_DOUBLE, start, "cr");
	ir_ref cr_1 = ir_fold2(ctx, IR_OPT(IR_SUB, IR_DOUBLE), y_1,
		ir_const_double(ctx, 0.5));
	ir_bind(ctx, cr, cr_1);                                                    // cr=cr_1
	ir_ref ci = ir_var(ctx, IR_DOUBLE, start, "ci");
	ir_bind(ctx, ci, x_1);                                                     // cr=cr_1, ci=x_1
	ir_ref zi = ir_var(ctx, IR_DOUBLE, start, "zi");
	ir_bind(ctx, zi, ir_const_double(ctx, 0.0));                               // cr=cr_1, ci=x_1, zi=0.0
	ir_ref zr = ir_var(ctx, IR_DOUBLE, start, "zr");
	ir_bind(ctx, zr, ir_const_double(ctx, 0.0));                               // cr=cr_1, ci=x_1, zi=0.0, zr=0.0
	ir_ref i = ir_var(ctx, IR_I32, start, "i");
	ir_bind(ctx, i, ir_const_i32(ctx, 0));                                     // cr=cr_1, ci=x_1, zi=0.0, zr=0.0, i=0

	ir_ref e_1 = ir_emit1(ctx, IR_END, start);
	ir_ref l_1 = ir_emit1(ctx, IR_LOOP_BEGIN, e_1);

	ir_ref zi_1 = ir_emit2(ctx, IR_OPT(IR_PHI, IR_DOUBLE), l_1,
		ir_const_double(ctx, 0.0));
	ir_bind(ctx, zi, zi_1);                                                    // cr=cr_1, ci=x_1, zi=zi_1, zr=0.0, i=0
	ir_ref zr_1 = ir_emit2(ctx, IR_OPT(IR_PHI, IR_DOUBLE), l_1,
		ir_const_double(ctx, 0.0));
	ir_bind(ctx, zi, zi_1);                                                    // cr=cr_1, ci=x_1, zi=zi_1, zr=zr_1, i=0
	ir_ref i_1 = ir_emit2(ctx, IR_OPT(IR_PHI, IR_I32), l_1,
		ir_const_i32(ctx, 0));
	ir_bind(ctx, i, i_1);                                                      // cr=cr_1, ci=x_1, zi=zi_1, zr=zr_1, i=i_1
	ir_ref i_2 = ir_emit2(ctx, IR_OPT(IR_ADD, IR_I32), i_1,
		ir_const_i32(ctx, 1));
	ir_bind(ctx, i, i_1);                                                      // cr=cr_1, ci=x_1, zi=zi_1, zr=zr_1, i=i_2
	ir_ref temp = ir_var(ctx, IR_DOUBLE, l_1, "temp");
	ir_ref temp_1 = ir_fold2(ctx, IR_OPT(IR_MUL, IR_DOUBLE), zr_1, zi_1);
	ir_bind(ctx, temp, temp_1);                                                // ... temp=temp_1
	ir_ref zr2 = ir_var(ctx, IR_DOUBLE, l_1, "zr2");
	ir_ref zr2_1 = ir_fold2(ctx, IR_OPT(IR_MUL, IR_DOUBLE), zr_1, zr_1);
	ir_bind(ctx, zr2, zr2_1);                                                  // ... temp=temp_1, zr2=zr2_1
	ir_ref zi2 = ir_var(ctx, IR_DOUBLE, l_1, "zi2");
	ir_ref zi2_1 = ir_fold2(ctx, IR_OPT(IR_MUL, IR_DOUBLE), zi_1, zi_1);
	ir_bind(ctx, zi2, zi2_1);                                                  // ... temp=temp_1, zr2=zr2_1, zi2=zi2_1
	ir_ref zr_2 = ir_fold2(ctx, IR_OPT(IR_ADD, IR_DOUBLE),
		ir_fold2(ctx, IR_OPT(IR_SUB, IR_DOUBLE), zr2_1, zi2_1),
		cr_1);
	ir_bind(ctx, zr, zr_2);                                                    // cr=cr_1, ci=x_1, zi=zi_1, zr=zr_2, i=i_2
	ir_ref zi_2 = ir_fold2(ctx, IR_OPT(IR_ADD, IR_DOUBLE),
		ir_fold2(ctx, IR_OPT(IR_ADD, IR_DOUBLE), temp_1, temp_1),
		x_1);
	ir_bind(ctx, zi, zi_2);                                                    // cr=cr_1, ci=x_1, zi=zi_2 zr=zr_2, i=i_2

	ir_ref if_1 = ir_emit2(ctx, IR_IF, l_1,
		ir_fold2(ctx, IR_OPT(IR_GT, IR_BOOL),
			ir_fold2(ctx, IR_OPT(IR_ADD, IR_DOUBLE), zi2_1, zr2_1),
			ir_const_double(ctx, 16.0)));

	ir_ref r_1 = ir_emit1(ctx, IR_IF_TRUE, if_1);
	ret = ir_emit2(ctx, IR_OPT(IR_RETURN, IR_I32), r_1, i_2);

	ir_ref r_2 = ir_emit1(ctx, IR_IF_FALSE, if_1);

	ir_ref if_2 = ir_emit2(ctx, IR_IF, r_2,
		ir_fold2(ctx, IR_OPT(IR_GT, IR_BOOL), i_2, ir_const_i32(ctx, 1000)));

	ir_ref r_3 = ir_emit1(ctx, IR_IF_TRUE, if_2);

	ret = ir_emit3(ctx, IR_OPT(IR_RETURN, IR_I32), r_3, ir_const_i32(ctx, 0), ret);

	ir_ref r_4 = ir_emit1(ctx, IR_IF_FALSE, if_2);

	ir_ref l_2 = ir_emit2(ctx, IR_LOOP_END, r_4, l_1);

	ir_set_op2(ctx, l_1, l_2);
	ir_set_op3(ctx, zi_1, zi_2);
	ir_set_op3(ctx, zr_1, zr_2);
	ir_set_op3(ctx, i_1, i_2);

	ir_set_op1(ctx, start, ret);
}


typedef int (*mandelbrot_t)(double, double);

void run(mandelbrot_t mandelbrot)
{
	struct timeval aTv;
	gettimeofday(&aTv, NULL);
	long init_time = aTv.tv_sec;
	long init_usec = aTv.tv_usec;

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

	gettimeofday(&aTv,NULL);
	double query_time = (aTv.tv_sec - init_time) + (double)(aTv.tv_usec - init_usec)/1000000.0;
	printf ("C Elapsed %0.3f\n", query_time);
}

int main(int argc, char **argv)
{
    ir_ctx ctx;
    FILE *f;
    int i;
	int opt_level = 2;
	uint32_t mflags = 0;

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
		} else {
			/* pass*/
		}
	}

	IR_ASSERT(IR_UNUSED == 0);
	IR_ASSERT(IR_NOP == 0);

	IR_ASSERT((IR_EQ ^ 1) == IR_NE);
	IR_ASSERT((IR_LT ^ 3) == IR_GT);
	IR_ASSERT((IR_GT ^ 3) == IR_LT);
	IR_ASSERT((IR_LE ^ 3) == IR_GE);
	IR_ASSERT((IR_GE ^ 3) == IR_LE);
	IR_ASSERT((IR_ULT ^ 3) == IR_UGT);
	IR_ASSERT((IR_UGT ^ 3) == IR_ULT);
	IR_ASSERT((IR_ULE ^ 3) == IR_UGE);
	IR_ASSERT((IR_UGE ^ 3) == IR_ULE);

    ir_init(&ctx, 256, 1024);
	ctx.flags |= IR_FUNCTION;
	ctx.flags |= mflags;
	if (opt_level > 0) {
		ctx.flags |= IR_OPT_FOLDING | IR_OPT_CODEGEN;
	}
    gen_mandelbrot(&ctx);

	ir_build_def_use_lists(&ctx);
	if (opt_level > 1) {
		ir_sccp(&ctx);
	}
	ir_build_cfg(&ctx);
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
	} else {
		ir_compute_dessa_moves(&ctx);
	}

    ir_truncate(&ctx);
//    ir_dump(&ctx, stderr);
    ir_save(&ctx, stderr);
    ir_dump_live_ranges(&ctx, stderr);
    f = fopen("ir.dot", "w+");
    ir_dump_dot(&ctx, f);
    fclose(f);

	size_t size;
	void *entry = ir_emit(&ctx, &size);

	if (entry) {
		ir_disasm("test", entry, size);

		ir_perf_map_register("test", entry, size);
		ir_perf_jitdump_open();
		ir_perf_jitdump_register("test", entry,	size);

		ir_mem_unprotect(entry, 4096);
		ir_gdb_register("test", entry, size, 0, 0);
		ir_mem_protect(entry, 4096);

		run((mandelbrot_t)entry);

		ir_perf_jitdump_close();
	}

	ir_free(&ctx);
	return 0;
}
