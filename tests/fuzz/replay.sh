#!/bin/sh
#
# Every input in the fuzz corpora is replayed through both fuzz targets.
#
# The corpora are the language tests, which hold every construct and every
# malformed input the golden suite specifies, and tests/fuzz/corpus, which
# holds the inputs fuzzing found worth keeping.  Each target's replay driver
# passes every file to the target once; a target aborts on an input that
# exposes a bug, which fails this test with the input's name last on its
# standard error.  This is how a finding stays fixed on every platform,
# including those with no libFuzzer to rediscover it.

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
