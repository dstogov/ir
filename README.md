# IR - Lightweight JIT Compilation Framework

IR Framework is a practical solution for implementing JIT in medium-size projects. 
It defines Intermediate Representaion (IR), provides a simple API for IR construction and
a set of algorithms for optimization, scheduling, register allocation and code
generation. The resulting generated in-memory code, may be directly executed.

This is not a stable finished product yet. It’s still under active development.
It was started as a base for development of the next generation JIT compiler for PHP-9,
but it's completly PHP independent.

A presentation about IR framework design and implementation details is available at
[researchgate](https://www.researchgate.net/publication/374470404_IR_JIT_Framework_a_base_for_the_next_generation_JIT_for_PHP).

## IR - Intermediate Representation

The Framework uses single Medium level Intermediate Representation during all
phases of optimization, register allocation and code generation. It is inspired
by Sea-Of-Nodes introduced by Cliff Click [1]. Sea-Of-Nodes is used in Java
HotSpot Server Compiler, V8 TurboFan JavaScript Compiler, Java Graal
Compiler...

This representation unifies data and control dependencies into a single graph,
where each instruction represented as a Node and each dependency as an Edge
between Nodes. There are no classical CFG (Control Flow Graph) with Basic
Blocks. Instead, IR uses special Control Nodes that start and finish some
code Regions. The data part of the IR is very similar to SSA (Static Single
Assignment) form. Each variable may be assigned only once, but except to SSA,
in our IR, we don’t have any variables, their versions and name. Everything
is represented by computation Nodes and Edges between them. Of course, we
have special PHI Node that represents the Phi() function.

Despite, our IR is graph based, internally, it’s represented as a plain
two-way grow-able array of Nodes. Dependency Edge are represented as indexes
of the other Node. This physical representation is almost completely repeats
the LuaJIT IR designed by Mike Pall [3].

## IR Generation

IR Framework provides a simple IR Builder API ([ir_builder.h](ir_builder.h)).
It's implemented as a number of C preprocessor macros, where each macro just
creates a corresponding IR node and ties it with the sources.
 
## IR Optimization

In comparison to classical optimizing compilers (like GCC and LLVM), IR
Framework uses very short optimization pipeline. Together with compact IR
representation, this makes it extremely fast and allows to generate quite
good machine code in reasonable time. 

### Folding

Folding is done on the fly, during IR generation. It performs a set of local
transformations, but because of the graph nature of the IR where most data
operations (like ADD) are “floating” (not “pinned” to Basic Block), the scope
of transformations is not limited by Basic Block. It’s important to generate
IR in a proper (Reverse Post) order, that would emit all Nodes before their
first usage. (In case of different order the scope of the folding should be
limited).

All the folding rules are written in a declarative style in the single file -
[ir_fold.h](ir_fold.h)

Folding Engine performs Constants Folding, Copy Propagation, Algebraic
Simplifications, Algebraic Re-Association and Common Sub-Expression
Elimination. The simple and fast declarative implementation is borrowed from
LuaJIT [3].
 
### Sparse Conditional Constant Propagation

This pass implements a classical algorithm originally designed by M. N. Wegman
and F. K. Zadeck [4] for SSA form. Unification of data and control dependencies
made its implementation even simple. Despite of constant propagation itself
this pass also performs global Copy Propagation and re-applies the folding rules.
At the end all the “dead” instructions (instructions that result are not used)
are replaced with NOPs.

### Global Code Motion

Now we have to “fix” places of “floating” instructions. This pass builds CFG
(Control Flow Graph) skeleton and then ”pin” each “floating” instruction to the
best Basic Block. The algorithm is developed by Cliff Click [2].

## Local Scheduling

As the final IR transformation pass, we reorder instructions inside each Basic
Block to satisfy the dependencies. Currently this is done by a simple
topological sorting.

## Target Instruction Selection

This is the first target dependent step of compilation. It aims to combine
instruction Nodes into tiles that allows better instruction fusion. For example
``10 + a + b * 4`` may be calculated by a single x86 instruction
``lea 10(%eax, %ebx, 4), %ecx``. The selection is done by a constrained tree
pattern matching. The current implementation uses simple Max-Munch approach.
(This may be replaced by a smarter BURS method).

## Register Allocation

CPU independent implementation of Linear Scan Register Allocation for SSA form
with second chance bin-packing. [5] [6]

## Machine Code Generations

IR Framework implements X86_64, x86 and AArch64 back-ends. The current
implementation uses DynAsm [?]. (In the future, this should be replaced with
a faster “binary” encoder). Code generator walks throw all instructions of each
basic blocks and emits some code according to “rules” selected during
instruction selection pass. It uses registers, selected by register allocator
and inserts the necessary spill load/store and SSA deconstruction code.

## Tooling

- Ability to load and save IR in a textual form
- Ability to visualize IR graph through graphviz dot.
- Target CPU disassembler for generated code (uses libcapstone [?])
- GDB/JIT interface to allow debugging of JIT-ed code
- Linux perf interface to analyze the code performance

## Building and Playing with IR

IR Framework is under active development and doesn't provide any stable libraries yet.
In case you like to you IR, it's better to embed the necessary sources into your project
(like [PHP]((https://github.com/php/php-src/tree/master/ext/opcache/jit)) does].

However, we provide a simple driver that may be built to run tests and play with IR.

```
git clone https://github.com/dstogov/ir.git
cd ir
make
make test
./ir bench/mandelbrit.ir --run
./ir bench/mandelbrit.ir -S
./ir --help
```

## IR Example

The complete described example may be found in [examples/mandelbrot.c](examples/mandelbrot.c))
and built using ``make examples``.

It generate the code for the following C function:

```c
int32_t mandelbrot(double x, double y)
{
	double cr = y - 0.5;
	double ci = x;
	double zi = 0.0;
	double zr = 0.0;
	int i = 0;

	while(1) {
		i++;
		double temp = zr * zi;
		double zr2 = zr * zr;
		double zi2 = zi * zi;
		zr = zr2 - zi2 + cr;
		zi = temp + temp + ci;
		if (zi2 + zr2 > 16)
			return i;
		if (i > 1000)
			return 0;
	}
	
}
```

This is done through IR builder API by the following code:

```c
void gen_mandelbrot(ir_ctx *ctx)
{
	ir_START();
	ir_ref x = ir_PARAM(IR_DOUBLE, "x", 1);
	ir_ref y = ir_PARAM(IR_DOUBLE, "y", 2);
	ir_ref cr = ir_SUB_D(y, ir_CONST_DOUBLE(0.5));
	ir_ref ci = ir_COPY_D(x);
	ir_ref zi = ir_COPY_D(ir_CONST_DOUBLE(0.0));
	ir_ref zr = ir_COPY_D(ir_CONST_DOUBLE(0.0));
	ir_ref i = ir_COPY_I32(ir_CONST_I32(0));

	ir_ref loop = ir_LOOP_BEGIN(ir_END());
		ir_ref zi_1 = ir_PHI_2(IR_DOUBLE, zi, IR_UNUSED);
		ir_ref zr_1 = ir_PHI_2(IR_DOUBLE, zr, IR_UNUSED);
		ir_ref i_1 = ir_PHI_2(IR_I32, i, IR_UNUSED);

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
					ir_ref loop_end = ir_LOOP_END();

	/* close loop */
	ir_MERGE_SET_OP(loop, 2, loop_end);
	ir_PHI_SET_OP(zi_1, 2, zi_2);
	ir_PHI_SET_OP(zr_1, 2, zr_2);
	ir_PHI_SET_OP(i_1, 2, i_2);
}
```
The textual representation of the IR after system independent optimizations:

```
{
	uintptr_t c_1 = 0;
	bool c_2 = 0;
	bool c_3 = 1;
	double c_4 = 0.5;
	double c_5 = 0;
	int32_t c_6 = 0;
	int32_t c_7 = 1;
	double c_8 = 16;
	int32_t c_9 = 1000;
	l_1 = START(l_22);
	double d_2 = PARAM(l_1, "x", 1);
	double d_3 = PARAM(l_1, "y", 2);
	double d_4 = SUB(d_3, c_4);
	l_5 = END(l_1);
	l_6 = LOOP_BEGIN(l_5, l_29);
	double d_7 = PHI(l_6, c_5, d_28);
	double d_8 = PHI(l_6, c_5, d_26);
	int32_t d_9 = PHI(l_6, c_6, d_10);
	int32_t d_10 = ADD(d_9, c_7);
	double d_11 = MUL(d_8, d_8);
	double d_12 = MUL(d_7, d_7);
	double d_13 = ADD(d_12, d_11);
	bool d_14 = GT(d_13, c_8);
	l_15 = IF(l_6, d_14);
	l_16 = IF_TRUE(l_15);
	l_17 = RETURN(l_16, d_10);
	l_18 = IF_FALSE(l_15);
	bool d_19 = GT(d_10, c_9);
	l_20 = IF(l_18, d_19);
	l_21 = IF_TRUE(l_20);
	l_22 = RETURN(l_21, c_6, l_17);
	l_23 = IF_FALSE(l_20);
	double d_24 = MUL(d_7, d_8);
	double d_25 = SUB(d_11, d_12);
	double d_26 = ADD(d_25, d_4);
	double d_27 = ADD(d_24, d_24);
	double d_28 = ADD(d_27, d_2);
	l_29 = LOOP_END(l_23);
}
```
The visualized graph:

![IR example](https://dstogov.github.io/mandelbrot.svg)

The graph was generated by the commands:

```
./ir bench/mandelbrot.ir --dot mandelbrot.dot
dot -Tsvg mandelbrot.dot -o mandelbrot.svg
```

The final generated assembler code:

```asm
test:
	subsd .L4(%rip), %xmm1
	xorpd %xmm3, %xmm3
	xorpd %xmm2, %xmm2
	xorl %eax, %eax
.L1:
	leal 1(%rax), %eax
	movapd %xmm2, %xmm4
	mulsd %xmm2, %xmm4
	movapd %xmm3, %xmm5
	mulsd %xmm3, %xmm5
	movapd %xmm5, %xmm6
	addsd %xmm4, %xmm6
	ucomisd .L5(%rip), %xmm6
	ja .L2
	cmpl $0x3e8, %eax
	jg .L3
	mulsd %xmm2, %xmm3
	subsd %xmm5, %xmm4
	movapd %xmm4, %xmm2
	addsd %xmm1, %xmm2
	addsd %xmm3, %xmm3
	addsd %xmm0, %xmm3
	jmp .L1
.L2:
	retq
.L3:
	xorl %eax, %eax
	retq
.rodata
.L4:
	.db 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x3f
.L5:
	.db 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x40
```

## LLVM interopability

IR is partially compatible with LLVM. It's possible to convert IR
into LLVM and then compile it.

```
./ir bench/mandelbrot.ir --emit-llvm mandelbrot.ll
llc mandelbrot.ll
gcc mandelbrot.s
./a.out
```

It's also possible to read an LLVM file and convert it to IR, but this
requres LLVM loader and therefore IR should be rebuilt with LLVM support.

```
make clean
make HAVE_LLVM=yes
```

Now you may compile some C file into LLVM, and then convert it to IR.

```
clang -O2 -fno-vectorize -fno-slp-vectorize -S -emit-llvm -o minilua.ll ./dynasm/minilua.c
./ir --llvm-asm minilua.ll --save 2>minilua.ir
./ir minilua.ir --run bench/mandelbrot.lua
```

Note that the last comand above compiles and runs the Lua interpreter.

Also note that the LLVM code produced by clang is already optimized.
In case of benchmarking, it may be more honest to avoid LLVM optimizations.

```
clang -O -Xclang -disable-llvm-passes -c -emit-llvm -o tmp.bc ./dynasm/minilua.c
opt tmp.bc --passes='function(mem2reg)' -S -o minilua.ll
```

## Performace
 
The fllowing table shows the benchmarks execution time in comparison to the same benchmarks compiled by ``gcc -O2`` (the more the better). The C benchmark were compiled by CLAG into LLVM code (without any LLVM optimizations, only SSA construction is necessary now) and then loaded, compiled and executed by IR framework.

| Benhcmark       | Execution time (relative to GCC -02) | 
| --------------- | ------------------------------------ |
| array           |                                 0.86 |
| binary-trees    |                                 0.96 |
| funnkuch-reduce |                                 1.01 |
| hash            |                                 1.21 |
| hash2           |                                 0.86 |
| heapsort        |                                 1.04 |
| lists           |                                 0.98 |
| matrix          |                                 1.10 |
| method-call     |                                 1.00 |
| mandelbrot      |                                 0.95 |
| nbody           |                                 0.95 |
| sieve           |                                 0.93 |
| spectral-norm   |                                 0.94 |
| strcat          |                                 0.96 |
| oggenc          |                                 0.88 |
| minilua         |                                 0.99 |
| AVERAGE         |                                 0.98 |
| GEOMEAN         |                                 0.97 |

Most of the bechmarks a very simple (few screems of code), oggenc and minilua are real applications. As you can see, IR produces code that in average 5% slower than ``GCC -O2``, in worst case the code was ~25% slower, and on some benchmarks it even faster. The folloing chart shows the same data graphically.

![IR benchmark chart](https://dstogov.github.io/ir_bench.svg)


The following table shows the comilation time relative to ``gcc -O2`` (the more the better)

| Benhcmark       | Compilation time (relative to GCC -02) | 
| --------------- | -------------------------------------- |
| oggenc          |                                  35.22 |
| minilua         |                                  32.40 |
| AVERAGE         |                                  33.81 |

This comparison is not completely fair, because GCC compiles C source, but IR takes precompiled LLVM asm

Anyway, **IR framework provides code that is in average 5% slower, but does this ~30 times faster**.

## PHP JIT based on IR

A new experimental JIT for PHP based on this project is developed at [master](https://github.com/php/php-src/tree/master/ext/opcache/jit) php-src branch.

### Building and Testing PHP with JIT based on IR Framework

Install pre-requested libraries. PHP and their extensions may require different libraries.
JIT itself needs just **libcapstone** to produce disassembler output.

```
sudo dnf install capstone-devel
```

Build PHP

```
git clone https://github.com/php/php-src.git
cd php-src
./buildconf --force
mkdir install
./configure --with-capstone --prefix=`pwd`/install --with-config-file-path=`pwd`/install/etc
make
make install
mkdir install/etc
cat > install/etc/php.ini <<EOL
zend_extension=opcache.so
opcache.enable=1
opcache.enable_cli=1
opcache.optimization_level=-1
opcache.jit_buffer_size=32M
opcache.jit=tracing
opcache.huge_code_pages=1
EOL
```

Check if opcache s loaded

```
sapi/cli/php -v | grep -i opcache
```

See JIT in action

```
sapi/cli/php -d opcache.jit=tracing -d opcache.jit_debug=1 Zend/bench.php
sapi/cli/php -d opcache.jit=function -d opcache.jit_debug=1 Zend/bench.php
```

## References

1. C. Click, M. Paleczny. “A Simple Graph-Based Intermediate Representation” In ACM SIGPLAN Workshop on Intermediate Representations (IR '95), pages 35-49, Jan. 1995.
2. C. Click. “Global Code Motion Global Value Numbering” In ACM SIGPLAN Notices, Volume 30, Issue 6, pp 246–257, June 1995
3. M. Pall. “LuaJIT 2.0 intellectual property disclosure and research opportunities” November 2009 http://lua-users.org/lists/lua-l/2009-11/msg00089.html
4.  M. N. Wegman and F. K. Zadeck. "Constant propagation with conditional branches" ACM Transactions on Programming Languages and Systems, 13(2):181-210, April 1991
5. C. Wimmer. “Optimized Interval Splitting in a Linear Scan Register Allocator” In VEE '05: Proceedings of the 1st ACM/USENIX international conference on Virtual execution environments, pages 132–141, June 2005
6. C. Wimmer and M. Franz. “Linear Scan Register Allocation on SSA Form” In CGO '10: Proceedings of the 8-th annual IEEE/ACM international symposium on Code generation and optimization, pages 170–179,  April 2010
