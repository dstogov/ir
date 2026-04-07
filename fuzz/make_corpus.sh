#!/bin/sh
#
# Extract seed corpus from .irt test files for fuzzing.
#
# Usage: fuzz/make_corpus.sh [TESTS_DIR] [CORPUS_DIR]
#   TESTS_DIR  - path to tests directory (default: ./tests)
#   CORPUS_DIR - path to corpus output  (default: ./fuzz/corpus)

TESTS_DIR="${1:-./tests}"
CORPUS_DIR="${2:-./fuzz/corpus}"

LOAD_DIR="$CORPUS_DIR/load"
O0_DIR="$CORPUS_DIR/O0"
O1_DIR="$CORPUS_DIR/O1"
O2_DIR="$CORPUS_DIR/O2"

mkdir -p "$LOAD_DIR" "$O0_DIR" "$O1_DIR" "$O2_DIR"

echo "Extracting seed corpus from tests..."
count=0
for f in $(find "$TESTS_DIR" -name '*.irt' -type f); do
	if [ -f "$f" ]; then
		base=$(basename "$f" .irt)
		dir=$(basename $(dirname "$f"))
		if [ "$dir" = "tests" ]; then
			outname="$base"
		else
			outname="${dir}_${base}"
		fi
		sed -n '/^--CODE--$/,/^--EXPECT--$/{ /^--CODE--$/d; /^--EXPECT--$/d; p; }' "$f" \
			> "$LOAD_DIR/${outname}.ir" 2>/dev/null
		if [ -s "$LOAD_DIR/${outname}.ir" ]; then
			count=$((count + 1))
		else
			rm -f "$LOAD_DIR/${outname}.ir"
		fi
	fi
done
echo "Extracted $count seed files to $LOAD_DIR/"

echo "Copying seed corpus for pipeline fuzzers..."
cp "$LOAD_DIR"/*.ir "$O0_DIR/" 2>/dev/null
cp "$LOAD_DIR"/*.ir "$O1_DIR/" 2>/dev/null
cp "$LOAD_DIR"/*.ir "$O2_DIR/" 2>/dev/null
count=$(ls -1 "$O0_DIR"/*.ir 2>/dev/null | wc -l)
echo "Copied $count seed files each to $O0_DIR/, $O1_DIR/, $O2_DIR/"
