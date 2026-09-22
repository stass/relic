#!/bin/sh
#
# Regenerate the build system from configure.ac and Makefile.am.
#
# This script is for a source checkout only.  A release tarball already
# contains configure and the generated makefiles, and builds with nothing
# beyond a C11 compiler, make and a POSIX shell.

set -eu

cd "$(dirname "$0")"

mkdir -p build-aux m4

# GNU libtool installs its programs under a g-prefix where the platform has a
# different libtool of its own, macOS being the common case.  autoreconf looks
# for libtoolize under its plain name and fails without this.
if ! command -v libtoolize >/dev/null 2>&1; then
	if command -v glibtoolize >/dev/null 2>&1; then
		LIBTOOLIZE=glibtoolize
		export LIBTOOLIZE
	fi
fi

exec autoreconf --force --install --warnings=all
