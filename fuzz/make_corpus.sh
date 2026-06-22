#!/bin/sh
#
# Extract seed corpus from .irt test files for fuzzing.
#
# Usage: fuzz/make_corpus.sh [TESTS_DIR] [CORPUS_DIR]
#   TESTS_DIR  - path to tests directory (default: ./tests)
#   CORPUS_DIR - path to corpus root     (default: ./fuzz/corpus)

TESTS_DIR="${1:-./tests}"
CORPUS_DIR="${2:-./fuzz/corpus}"

TEXT_DIR="$CORPUS_DIR/text"

mkdir -p "$TEXT_DIR"

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
			> "$TEXT_DIR/${outname}.ir" 2>/dev/null
		if [ -s "$TEXT_DIR/${outname}.ir" ]; then
			count=$((count + 1))
		else
			rm -f "$TEXT_DIR/${outname}.ir"
		fi
	fi
done
echo "Extracted $count seed files to $TEXT_DIR/"
