
#include <iostream>

#include <ir.hxx>

extern "C" {
#ifndef _WIN32
# include <sys/time.h>
#endif
} /* extern "C" */

using mandelbrot_t = int32_t (*)(double, double);

void run(mandelbrot_t mandelbrot)
{
#ifndef _WIN32
	struct timeval aTv;
	gettimeofday(&aTv, NULL);
	long init_time = aTv.tv_sec;
	long init_usec = aTv.tv_usec;
#endif

	int x, y;
	for (y = -39; y < 39; y++) {
		std::cout << std::endl;
		for (x = -39; x < 39; x++) {
			int i = mandelbrot(x / 40.0, y / 40.0);
			if (i == 0)
				std::cout << "*";
			else
				std::cout << " ";
		}
	}
	std::cout << std::endl;

#ifndef _WIN32
	gettimeofday(&aTv, NULL);
	double query_time = (aTv.tv_sec - init_time) + (double)(aTv.tv_usec - init_usec) / 1000000.0;
	printf("C Elapsed %0.3f\n", query_time);
#endif
}

int main(int argc, char** argv) {
	ir::consistency_check();

	uint32_t flags = IR_FUNCTION;
	int opt_level = 2;
	if (opt_level > 0)
		flags |= IR_OPT_FOLDING | IR_OPT_CFG | IR_OPT_CODEGEN;
			
	ir::builder bld(flags, 256, 1024);

	// start gen_mandelbrot
	bld.start();
	auto x = bld.param(ir::d{}, "x");
	auto y = bld.param(ir::d{}, "y");
	auto cr = bld.sub(y, bld.constval(.5));
	auto ci = bld.copy(x);
	auto zi = bld.copy(bld.constval(.0));
	auto zr = bld.copy(bld.constval(.0));
	auto i = bld.copy(bld.constval(ir::i32{0}));

	auto loop = bld.loop_begin();
		ir::d zi_1 = bld.phi(zi);
		ir::d zr_1 = bld.phi(zr);
		ir::i32 i_1 = bld.phi(i);

		auto i_2 = bld.add(i_1, bld.constval(ir::i32{1}));
		auto temp = bld.mul(zr_1, zi_1);
		auto zr2 = bld.mul(zr_1, zr_1);
		auto zi2 = bld.mul(zi_1, zi_1);
		auto zr_2 = bld.add(bld.sub(zr2, zi2), cr);
		auto zi_2 = bld.add(bld.add(temp, temp), ci);
		auto if_1 = bld.if_cond(bld.gt(bld.add(zi2, zr2), bld.constval(6.0)));
			bld.if_true(if_1);
				bld.ret(i_2);
			bld.if_false(if_1);
				auto if_2 = bld.if_cond(bld.gt(i_2, bld.constval(ir::i32{1000})));
				bld.if_true(if_2);
					bld.ret(bld.constval(ir::i32{0}));
				bld.if_false(if_2);
					auto loop_end = bld.loop_end();

	// close loop
	bld.merge_set_op(loop, 2, loop_end);
	bld.phi_set_op(zi_1, 2, zi_2);
	bld.phi_set_op(zr_1, 2, zr_2);
	bld.phi_set_op(i_1, 2, i_2);
	// end gen_mandelbrot

	bld.build_def_use_lists();
	if (opt_level > 1) {
		bld.sccp();
	}
	bld.build_cfg();
	if (opt_level > 0) {
		bld.build_dominators_tree();
		bld.find_loops();
		bld.gcm();
		bld.schedule();
	}
	bld.match();
	bld.assign_virtual_registers();
	if (opt_level > 0) {
		bld.compute_live_ranges();
		bld.coalesce();
		bld.reg_alloc();
		bld.schedule_blocks();
	}
	else
		bld.compute_dessa_moves();
	bld.truncate();

	bld.save();

	auto entry = bld.emit_code();

	run(mandelbrot_t(entry));

	return 0;
}
