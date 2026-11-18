#!/bin/sh
# Usage: roundtrip.sh <ir> <c|llvm> <cc> <code-file> [-O<n>] --run
# Compiles the C or LLVM code emitted for a "--run" test with <cc> and runs it.

ir=$1 emit=$2 cc=$3 code=$4
[ "$5" = --run ] || opt=$5

tmp=$(mktemp -d) || exit
trap 'rm -rf "$tmp"' EXIT

if [ "$emit" = c ]; then
	src=$tmp/test.c
	# The C emitter doesn't emit #include directives and passes pointers as uintptr_t
	flags="-include stdint.h -include stdbool.h -include math.h -Wno-int-conversion -Wno-implicit-function-declaration"
else
	src=$tmp/test.ll
fi

"$ir" $opt --emit-$emit -o "$src" "$code" &&
"$cc" -w -O1 $flags -o "$tmp/test" "$src" -lm &&
"$tmp/test"
