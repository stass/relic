#!/bin/sh
#
# The generated pkg-config metadata parses and describes this build.
#
# A consumer reaches librelic through pkg-config, so a malformed or
# stale librelic.pc breaks every embedding build while leaving relic's own
# build green.  The file is queried through pkg-config rather than read,
# because what matters is that pkg-config can answer with it.

set -eu

: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"
: "${RELIC_VERSION:?RELIC_VERSION must hold the expected version}"

if ! command -v pkg-config >/dev/null 2>&1; then
	echo "pkg-config is not installed; skipping" >&2
	exit 77
fi

if [ ! -f "$RELIC_BUILDDIR/librelic.pc" ]; then
	echo "$RELIC_BUILDDIR/librelic.pc was not generated" >&2
	exit 1
fi

PKG_CONFIG_PATH="$RELIC_BUILDDIR"
export PKG_CONFIG_PATH

if ! pkg-config --exists librelic; then
	echo "pkg-config does not recognize librelic" >&2
	exit 1
fi

version=$(pkg-config --modversion librelic)
if [ "$version" != "$RELIC_VERSION" ]; then
	echo "librelic.pc reports version '$version', expected '$RELIC_VERSION'" >&2
	exit 1
fi

# The compile and link flags must at least be expressible; a consumer that
# cannot obtain them has no way to build against the library.
pkg-config --cflags librelic >/dev/null
pkg-config --libs librelic >/dev/null

exit 0
