# Fuzzing Framework for IR JIT

Requires clang with libFuzzer support.

The fuzzing tooling builds on its own and does not touch the production
build. Run every command from this `fuzz/` directory.

## Quick Start

```bash
# 1. Extract seed corpus from the existing tests
make corpus

# 2. Build all fuzz targets
make

# 3. Run individual fuzzers. With no path each target uses its
#    standard corpus directory (corpus/text or corpus/graph) and
#    creates it on demand.
./build/fuzz-text-load
./build/fuzz-text-O0
./build/fuzz-text-O1
./build/fuzz-text-O2
./build/fuzz-graph-O0
./build/fuzz-graph-O1
./build/fuzz-graph-O2
```

## Targets

| Target | What it fuzzes | Key bugs it finds |
|--------|---------------|-------------------|
| `fuzz-text-load` | Text IR parser via ir_load (`-fsyntax-only`) | Buffer overflows, integer overflows, strtab corruption |
| `fuzz-text-O0` | Optimization pipeline at `-O0` | CFG, codegen bugs |
| `fuzz-text-O1` | Optimization pipeline at `-O1` | mem2ssa, GCM, scheduling, RA bugs |
| `fuzz-text-O2` | Optimization pipeline at `-O2` | SCCP, inlining, GCM, scheduling, RA bugs |
| `fuzz-graph-O0` | Optimizer and backend at `-O0` on a structured IR graph | CFG, codegen bugs |
| `fuzz-graph-O1` | Optimizer and backend at `-O1` on a structured IR graph | mem2ssa, GCM, scheduling, RA bugs |
| `fuzz-graph-O2` | Optimizer and backend at `-O2` on a structured IR graph | SCCP, GCM, scheduling, RA bugs |

The `fuzz-text-*` targets feed the text IR parser. The
`fuzz-graph-O*` targets decode each input blob directly into a
structurally valid IR function, so every input reaches the optimizer and
backend instead of being rejected by the parser.

## Corpus

`make corpus` extracts the text seed corpus from the .irt test files in
tests/ into corpus/text/, which every text target shares. Each test's
`--CODE--` section is a valid IR program.

The graph harness takes raw byte inputs and has no seed corpus; its
finds accumulate under corpus/graph/.

With no positional path each target falls back to its standard corpus
directory (corpus/text or corpus/graph) and creates it on demand.
Passing an explicit path overrides this, for example a single crash
file when reproducing.

## Reproducing Crashes

```bash
./build/fuzz-text-load crash-<hash>
./build/fuzz-graph-O2 crash-<hash>
```
