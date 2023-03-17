
#include <iostream>

#include <ir.hxx>

using myfunc_t = int32_t (*)(int32_t, int32_t);

void run(myfunc_t func)
{
	const int32_t N = 2;
	int32_t x, y;
	for (y = -N; y < N; y++) {
		for (x = -N; x < N; x++) {
			int32_t ret = func(x, y);
			std::cout << "x=" << x << " y=" << y << " ret=" << ret << std::endl;
		}
	}
}

int main(int argc, char** argv) {
	ir::consistency_check();

	uint32_t flags = IR_FUNCTION;
	int opt_level = 0;
	if (opt_level > 0)
		flags |= IR_OPT_FOLDING | IR_OPT_CFG | IR_OPT_CODEGEN;
	
	ir::builder bld(flags, 256, 1024);

	// start myfunc
	bld.start();
	auto x = bld.param(ir::i32::t, "x");
	auto y = bld.param(ir::i32::t, "y");
	auto cr = bld.sub(x, y);
	bld.ret(cr);
	// end myfunc
	
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
	} else
		bld.compute_dessa_moves();
	bld.truncate();

	bld.save();
	//return 0;
	auto entry = bld.emit_code();

	run(myfunc_t(entry));

	return 0;
}
