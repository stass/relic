#!/bin/sh
#
# The randomized suite can fail.
#
# A comparison that never disagrees passes forever, whether the engine is
# right or the comparison is broken.  Under -F the driver drops one row from
# the answers of each comparison in turn -- the naive strategy's, compared
# with the default's, and the helper rendering's, compared with the
# program's -- and succeeds only if a seed exposes each, the failure is
# reported as that comparison's own kind, and the shrinker reduces the
# program to a handful of statements.  That connects every part of the
# suite: generation, the child that runs the strategies, the comparisons,
# the status the child reports and the shrinker.

set -eu

: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"

"$RELIC_BUILDDIR/tests/random/random" -F -n 200
