# Fuzzing Framework for IR JIT

Requires clang with libFuzzer support.

## Quick Start

```bash
# 1. Build the main project first, generates required headers
make BUILD=debug

# 2. Extract seed corpus from existing tests
make -f fuzz/Makefile corpus

# 3. Build and run parser fuzzer
make -f fuzz/Makefile CC=clang fuzz-load
./fuzz/build/fuzz-load fuzz/corpus/load/

# 4. Build and run pipeline fuzzer
make -f fuzz/Makefile CC=clang fuzz-pipeline
./fuzz/build/fuzz-pipeline fuzz/corpus/pipeline/
```

## Targets

| Target | What it fuzzes | Key bugs it finds |
|--------|---------------|-------------------|
| `fuzz-load` | Text IR parser via ir_load | Buffer overflows, integer overflows, strtab corruption |
| `fuzz-pipeline` | Full optimization pipeline | SCCP, GCM, RA algorithmic bugs, assertion failures |

## Corpus

The seed corpus is extracted from the .irt test files in tests/. Each
test's `--CODE--` section is a valid IR program. The pipeline corpus prepends a
byte selecting the optimization level 0, 1, or 2.

## Reproducing Crashes

```bash
./fuzz/build/fuzz-load crash-<hash>
```
