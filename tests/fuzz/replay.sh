#!/bin/sh
#
# Every input in the fuzz corpora is replayed through the fuzz targets that
# read it.
#
# The programs are the language tests, which hold every construct and every
# malformed input the golden suite specifies, and tests/fuzz/corpus, which
# holds the inputs fuzzing found worth keeping; both the load and the eval
# target read them.  The import target reads tables, from
# tests/fuzz/corpus-import, and the regex target patterns and strings, from
# tests/fuzz/corpus-regex.  Each target's replay driver passes every file to
# the target once; a target aborts on an input that exposes a bug, which
# fails this test with the input's name last on its standard error.  This is
# how a finding stays fixed on every platform, including those with no
# libFuzzer to rediscover it.

set -eu

: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

# The language tests are named by their programs alone: an in-tree build
# writes each test's log beside it while this runs.
for target in load eval; do
	echo "replaying the corpora through the $target target"
	"$RELIC_BUILDDIR/tests/fuzz/replay_$target" \
	    "$RELIC_SRCDIR"/tests/lang/*.dl "$RELIC_SRCDIR/tests/fuzz/corpus"
done

# The import target reads tables, and the regex target a pattern and a
# string; each corpus is its target's own.
echo "replaying the import corpus through the import target"
"$RELIC_BUILDDIR/tests/fuzz/replay_import" \
    "$RELIC_SRCDIR/tests/fuzz/corpus-import"
echo "replaying the regex corpus through the regex target"
"$RELIC_BUILDDIR/tests/fuzz/replay_regex" \
    "$RELIC_SRCDIR/tests/fuzz/corpus-regex"
