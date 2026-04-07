# Fuzzing Framework for IR JIT

Requires clang with libFuzzer support.

## Quick Start

```bash
# 1. Extract seed corpus from existing tests
make fuzz-corpus

# 2. Build all fuzz targets
make fuzz

# 3. Run individual fuzzers
./fuzz/build/fuzz-load fuzz/corpus/load/
./fuzz/build/fuzz-O0 fuzz/corpus/O0/
./fuzz/build/fuzz-O1 fuzz/corpus/O1/
./fuzz/build/fuzz-O2 fuzz/corpus/O2/
```

## Targets

| Target | What it fuzzes | Key bugs it finds |
|--------|---------------|-------------------|
| `fuzz-load` | Text IR parser via ir_load (`-fsyntax-only`) | Buffer overflows, integer overflows, strtab corruption |
| `fuzz-O0` | Optimization pipeline at `-O0` | CFG, codegen bugs |
| `fuzz-O1` | Optimization pipeline at `-O1` | mem2ssa, GCM, scheduling, RA bugs |
| `fuzz-O2` | Optimization pipeline at `-O2` | SCCP, inlining, GCM, scheduling, RA bugs |

## Corpus

The seed corpus is extracted from the .irt test files in tests/. Each
test's `--CODE--` section is a valid IR program.

## Reproducing Crashes

```bash
./fuzz/build/fuzz-load crash-<hash>
```
