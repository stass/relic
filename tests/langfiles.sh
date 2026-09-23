#!/bin/sh
#
# Every language test is registered, and every file under tests/lang belongs
# to one.
#
# Automake needs the .dl files listed in TESTS, and a list is only ever as
# good as the last edit to it: a test left out of it is not reported as
# missing, it simply never runs, and a test that never runs passes forever.
# This check is what makes that omission loud.  It reads the registered list
# from the environment and compares it against what is on disk, in both
# directions, so an unregistered test and a registered one that was deleted
# both fail here.
#
# It also rejects a file whose suffix is not one a test is made of.  The
# expectation files are distributed by shipping the whole directory, which
# cannot go stale but also cannot tell a golden file from something left
# behind by a debugging session.  This can.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
: "${RELIC_LANG_TESTS:?RELIC_LANG_TESTS must list the registered tests}"

lang_dir=$RELIC_SRCDIR/tests/lang
status=0

# The registered list holds source-relative paths; reduce it to bare names.
registered=$(for path in $RELIC_LANG_TESTS; do
	name=${path##*/}
	echo "${name%.dl}"
done | sort)

present=$(for path in "$lang_dir"/*.dl; do
	name=${path##*/}
	echo "${name%.dl}"
done | sort)

if [ "$registered" != "$present" ]; then
	echo "the registered language tests differ from the files on disk:" >&2
	printf 'registered:\n%s\n' "$registered" >&2
	printf 'on disk:\n%s\n' "$present" >&2
	status=1
fi

# Every file in the directory must be part of a test that exists.
for path in "$lang_dir"/*; do
	name=${path##*/}

	case "$name" in
	*.dl)
		continue
		;;
	*.log|*.trs)
		# Automake's own record of a test run.  In an in-tree build
		# it lands beside the test it ran, and it is build output
		# rather than part of the test.
		continue
		;;
	*.args|*.expected|*.expected-err|*.exit|*.stdin)
		;;
	*)
		echo "$name is not part of any language test" >&2
		status=1
		continue
		;;
	esac

	base=${name%.*}
	if [ ! -f "$lang_dir/$base.dl" ]; then
		echo "$name has no $base.dl beside it" >&2
		status=1
	fi
done

exit "$status"
