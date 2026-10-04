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
#
# And it holds tests/lang/LANGUAGE, the list of tests whose expectation the
# language alone fixes, to the tests in both directions: every name in it
# must be a test that qualifies, and every test that qualifies must be named
# in it, listed or omitted with a reason.  A test qualifies when it runs the
# program alone: beside its .dl it has nothing but an expectation, an .exit
# of 0 or 1, and .args holding only -c and -s.  A new test that qualifies
# fails here until someone decides which it is.

set -eu

# Byte order, and bracket ranges that mean bytes.
LC_ALL=C
export LC_ALL

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

# A test's name is the stem its files share and the word the manifest lists
# it by.  Held to lowercase letters, digits, `_` and `-`, it cannot hold a
# dot, which would make one test's files look like those of another, or a
# byte the manifest could not spell.
name_pattern='[a-z0-9_-][a-z0-9_-]*'

misnamed=$(printf '%s\n' "$present" | grep -vx -- "$name_pattern" || :)
if [ -n "$misnamed" ]; then
	printf 'these language tests have names outside [a-z0-9_-]:\n%s\n' \
	    "$misnamed" >&2
	status=1
fi

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
	*.dl|LANGUAGE)
		continue
		;;
	*.log|*.trs)
		# Automake's own record of a test run.  In an in-tree build
		# it lands beside the test it ran, and it is build output
		# rather than part of the test.
		continue
		;;
	*.args|*.expected|*.expected-err|*.exit|*.stdin|*.per-strategy)
		base=${name%.*}
		;;
	*.csv|*.tsv)
		# A table the test imports with -I: <test>.tsv, or
		# <test>.<case>.tsv when it imports several.  The test's
		# arguments must name it, or nothing reads it.
		base=${name%%.*}
		if ! grep -qF -- "$name" "$lang_dir/$base.args" 2>/dev/null
		then
			echo "$name is not named by $base.args" >&2
			status=1
			continue
		fi
		;;
	*)
		echo "$name is not part of any language test" >&2
		status=1
		continue
		;;
	esac

	if [ ! -f "$lang_dir/$base.dl" ]; then
		echo "$name has no $base.dl beside it" >&2
		status=1
	fi
done

# Whether the test `base` runs the program alone, as every test the
# manifest names must: nothing beside its .dl but an expectation, an exit
# status of 0 or 1, and options within -c and -s, read as words with
# pathname expansion off, as the runner reads them.  The body is a
# subshell, so that turning expansion off reaches no further, and each
# verdict is its exit status.
runs_program_alone() (
	base=$1

	[ -f "$lang_dir/$base.dl" ] || exit 1

	for path in "$lang_dir/$base".*; do
		case "${path##*/}" in
		"$base.dl"|"$base.expected"|"$base.expected-err")
			;;
		"$base.log"|"$base.trs")
			;;
		"$base.exit")
			case $(cat "$path") in
			0|1)
				;;
			*)
				exit 1
				;;
			esac
			;;
		"$base.args")
			set -f
			for word in $(cat "$path"); do
				case "$word" in
				-c|-s)
					;;
				*)
					exit 1
					;;
				esac
			done
			;;
		*)
			exit 1
			;;
		esac
	done

	exit 0
)

manifest=$lang_dir/LANGUAGE
if [ ! -f "$manifest" ]; then
	echo "$manifest is missing" >&2
	exit 1
fi

# A line is a comment, blank, a test's name alone, or `omit`, a name and
# the reason it is left out.
blank_run='[[:blank:]][[:blank:]]*'

malformed=$(grep -v -e '^#' -e '^$' -e "^$name_pattern\$" \
    -e "^omit$blank_run$name_pattern${blank_run}[^[:blank:]]" \
    "$manifest" || :)
if [ -n "$malformed" ]; then
	printf '%s has lines that are neither a name nor an omission:\n%s\n' \
	    "$manifest" "$malformed" >&2
	status=1
fi

listed=$(grep -e "^$name_pattern\$" "$manifest" || :)
omitted=$(sed -n "s/^omit$blank_run\\($name_pattern\\).*/\\1/p" \
    "$manifest")

if ! printf '%s\n' "$listed" | sort -c; then
	echo "the tests $manifest lists are not in byte order" >&2
	status=1
fi
if ! printf '%s\n' "$omitted" | sort -c; then
	echo "the tests $manifest omits are not in byte order" >&2
	status=1
fi

named=$(printf '%s\n%s\n' "$listed" "$omitted" | grep -v '^$' | sort)
repeated=$(printf '%s\n' "$named" | uniq -d)
if [ -n "$repeated" ]; then
	printf '%s names these more than once:\n%s\n' "$manifest" \
	    "$repeated" >&2
	status=1
fi

for base in $named; do
	if [ ! -f "$lang_dir/$base.dl" ]; then
		echo "$manifest names $base, which is not a test" >&2
		status=1
	elif ! runs_program_alone "$base"; then
		echo "$base is named by $manifest but does not run the" \
		    "program alone" >&2
		status=1
	fi
done

for base in $present; do
	if runs_program_alone "$base" &&
	    ! printf '%s\n' "$named" | grep -qxF -- "$base"; then
		echo "$base runs the program alone and is neither listed nor" \
		    "omitted by $manifest" >&2
		status=1
	fi
done

exit "$status"
