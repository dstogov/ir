#!/bin/bash

IR=~/dstogov-ir-riscv/ir
TESTS_DIR=~/dstogov-ir-riscv/tests/riscv
PASS=0
FAIL=0

run_test() {
    local ir_file=$1
    shift
    local test_name=$(basename $ir_file .ir)
    local c_args=$(echo "$@" | tr ' ' ',')
    local num_args=$#

    # C reference
    $IR $ir_file --emit-c -o /tmp/${test_name}.c 2>/dev/null
    cat > /tmp/${test_name}_main.c << CMAIN
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
extern int32_t test();
int main(int argc, char* argv[]) {
    int32_t args[8] = {0};
    for (int i = 0; i < argc-1 && i < 8; i++) args[i] = atoi(argv[i+1]);
    printf("%d\n", test($c_args));
    return 0;
}
CMAIN
    cc -include stdint.h -include stdbool.h \
        /tmp/${test_name}.c /tmp/${test_name}_main.c \
        -o /tmp/${test_name}_c 2>/dev/null
    if [ ! -f /tmp/${test_name}_c ]; then
        echo "FAIL: $test_name C compile failed"
        FAIL=$((FAIL+1)); return
    fi
    EXPECTED=$(/tmp/${test_name}_c)

    # RISC-V actual - use gcc static to get printf
    $IR $ir_file --emit-riscv -o /tmp/${test_name}.s 2>/dev/null

    # build a C wrapper that calls test() with args and prints result
    cat > /tmp/${test_name}_riscv_main.c << RMAIN
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern int32_t test();
int main(int argc, char* argv[]) {
    int32_t args[8] = {0};
    for (int i = 0; i < argc-1 && i < 8; i++) args[i] = atoi(argv[i+1]);
    printf("%d\n", test($c_args));
    return 0;
}
RMAIN

    riscv64-linux-gnu-as /tmp/${test_name}.s -o /tmp/${test_name}.o 2>/dev/null
    riscv64-linux-gnu-gcc -static \
        /tmp/${test_name}_riscv_main.c /tmp/${test_name}.o \
        -o /tmp/${test_name}_riscv 2>/dev/null
    if [ ! -f /tmp/${test_name}_riscv ]; then
        echo "FAIL: $test_name RISC-V compile failed"
        FAIL=$((FAIL+1)); return
    fi
    ACTUAL=$(qemu-riscv64 /tmp/${test_name}_riscv 2>/dev/null)

    if [ "$EXPECTED" = "$ACTUAL" ]; then
        echo "PASS: $test_name ($@) = $EXPECTED"
        PASS=$((PASS+1))
    else
        echo "FAIL: $test_name ($@) expected=$EXPECTED actual=$ACTUAL"
        FAIL=$((FAIL+1))
    fi
}

run_test $TESTS_DIR/add.ir 3 5
run_test $TESTS_DIR/add.ir 10 20
run_test $TESTS_DIR/add.ir 100 200
run_test $TESTS_DIR/add.ir -1 1

run_test $TESTS_DIR/sub.ir 10 3
run_test $TESTS_DIR/sub.ir 0 5
run_test $TESTS_DIR/sub.ir 100 200

run_test $TESTS_DIR/mul.ir 3 5
run_test $TESTS_DIR/mul.ir 7 8
run_test $TESTS_DIR/mul.ir 100 200

run_test ~/wasm2sea/build/factorial.ir 5
run_test ~/wasm2sea/build/factorial.ir 10

echo ""
echo "Results: $PASS passed, $FAIL failed"
