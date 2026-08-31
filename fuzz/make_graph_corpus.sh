#!/bin/sh
#
# Generate a binary seed corpus for the structured graph fuzzer.
#
# Usage: fuzz/make_graph_corpus.sh [CORPUS_DIR]
#   CORPUS_DIR - path to corpus output (default: ./fuzz/corpus-graph)
#
# Each seed is a blob in the encoding that fuzz_graph.c decodes. The
# first byte selects the working type and its high bit selects the fp
# family. The second byte selects the parameter count. The next sixteen
# bytes are two seed constants of eight bytes each. The rest is a stream
# of 3 byte records {op_sel, src1, src2}. The seeds below each drive one
# feature shape so the fuzzer and its custom mutator start from inputs
# that already reach every part of the pipeline instead of an empty file.

CORPUS_DIR="${1:-./fuzz/corpus-graph}"

mkdir -p "$CORPUS_DIR"

seed_count=0

# Emit the given decimal byte values as raw bytes.
emit() {
	fmt=""
	for b in "$@"; do
		fmt="$fmt$(printf '\\%03o' "$b")"
	done
	printf "$fmt"
}

# Write a seed. First argument is the file name, the rest are the bytes
# after the 18 byte prefix, which is prepended here. The prefix carries
# the type byte, the parameter count byte and two nonzero constants so a
# folded division never hits a zero divisor.
#   $1 type byte
#   $2 parameter count byte
#   $3 file name
#   $4.. record bytes
seed() {
	type_byte="$1"
	param_byte="$2"
	name="$3"
	shift 3
	emit "$type_byte" "$param_byte" \
		0 0 0 0 0 0 0 7 \
		0 0 0 0 0 0 0 3 \
		"$@" > "$CORPUS_DIR/$name"
	seed_count=$((seed_count + 1))
}

# Integer working type U32 with four parameters.
#   linear binary chain ADD SUB MUL XOR DIV MOD
seed 2 3 graph_int_linear \
	0 0 1   1 1 2   2 2 3   5 3 4   11 4 5   12 5 6
#   unary NEG NOT
seed 2 3 graph_int_unary \
	128 0 0   129 1 0
#   if/else diamond merged through a PHI
seed 2 3 graph_int_branch \
	64 0 1   65 2 3
#   natural loop with an induction PHI
seed 2 3 graph_int_loop \
	32 0 1   33 1 2
#   conversions, widen and truncate then bitcast through a float
seed 2 3 graph_int_conv \
	16 0 0   17 1 0
#   memory round trip through a named VAR
seed 2 3 graph_int_mem_var \
	24 0 0   24 1 0
#   memory round trip through an ALLOCA stack slot
seed 2 3 graph_int_mem_alloca \
	28 0 0   28 1 0
#   indirect calls with three arguments then none
seed 2 3 graph_int_call \
	26 0 3   26 1 0

# Wider integer type I64 to reach the 64 bit conversion shapes.
seed 7 3 graph_i64_conv \
	16 0 0   17 1 0
seed 7 3 graph_i64_mixed \
	0 0 1   128 2 0   16 1 0   24 3 0   26 0 2

# Double working type with two parameters.
seed 128 1 graph_fp_linear \
	0 0 1   3 1 2   4 2 3   5 3 4
seed 128 1 graph_fp_unary \
	128 0 0   129 1 0
seed 128 1 graph_fp_branch \
	64 0 1   65 1 2
seed 128 1 graph_fp_loop \
	32 0 1   33 1 2
#   conversions, int bits round trip, fp to fp, and fp to int on a param
seed 128 3 graph_fp_conv \
	16 0 0   17 1 0   18 0 0
seed 128 1 graph_fp_call \
	26 0 2   26 1 0

# Float working type to reach the float to int and float to float paths.
seed 129 3 graph_float_conv \
	16 0 0   17 1 0   18 0 0

# A deep mixed graph to give the mutator a rich base to grow from.
seed 2 3 graph_int_deep \
	0 0 1   1 1 2   2 2 3   128 3 0   64 4 5   32 0 1 \
	16 1 0   24 2 0   28 3 0   26 0 3   5 4 5   11 5 6

echo "Wrote $seed_count graph seed files to $CORPUS_DIR/"
