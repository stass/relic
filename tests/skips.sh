#!/bin/sh
#
# tests/strict.sh passes a test's result through, and in strict mode turns a
# skip the environment does not allow into a failure.
#
# The runner stands between the harness and every test, so a fault in it
# either hides failures or fails everything.  Each case runs it on a stand-in
# test that exits with a chosen status, under an environment set for that
# case alone, whatever the environment this test itself was run with.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"

runner="$RELIC_SRCDIR/tests/strict.sh"

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# A stand-in test named name.sh that exits with the given status.
make_test() {
	printf '#!/bin/sh\nexit %s\n' "$2" > "$work/$1.sh"
}

failures=0

# Run the runner on a stand-in test and compare the status it returns.
#
# usage: expect status strict allowed test
expect() {
	expected=$1
	strict=$2
	allowed=$3
	test_name=$4

	# The runner runs in the directory holding the stand-in tests, so that
	# an allowed name expanded as a pattern would match one of them.
	status=0
	(
		cd "$work"
		env RELIC_STRICT_CHECK="$strict" RELIC_ALLOWED_SKIPS="$allowed" \
		    sh "$runner" sh "./$test_name" 2>/dev/null
	) || status=$?

	if [ "$status" -ne "$expected" ]; then
		echo "RELIC_STRICT_CHECK='$strict' RELIC_ALLOWED_SKIPS='$allowed'" \
		    "$test_name: expected status $expected, got $status" >&2
		failures=$((failures + 1))
	fi
}

make_test pass 0
make_test fail 1
make_test skip 77
make_test error 99

# Without strict mode every status passes through unchanged.
expect 0 no '' pass.sh
expect 1 no '' fail.sh
expect 77 no '' skip.sh
expect 99 no '' error.sh

# Strict mode changes a skip alone.
expect 0 yes '' pass.sh
expect 1 yes '' fail.sh
expect 99 yes '' error.sh
expect 1 yes '' skip.sh

# An allowed skip is matched by the whole file name, among several.
expect 77 yes 'skip.sh' skip.sh
expect 77 yes 'other.sh skip.sh' skip.sh
expect 1 yes 'skip' skip.sh
expect 1 yes 'skip.sh.old' skip.sh
expect 1 yes '*' skip.sh

# A misspelt or empty setting is a hard error, not strict mode quietly
# turned off.
expect 99 true '' pass.sh
expect 99 YES '' skip.sh
expect 99 '' '' skip.sh

# Without an environment variable of its own the runner is not strict.
status=0
(
	unset RELIC_STRICT_CHECK RELIC_ALLOWED_SKIPS
	sh "$runner" sh "$work/skip.sh" 2>/dev/null
) || status=$?
if [ "$status" -ne 77 ]; then
	echo "unset RELIC_STRICT_CHECK: expected status 77, got $status" >&2
	failures=$((failures + 1))
fi

# The runner's refusal names the test, so the harness log says which one.
message=$(env RELIC_STRICT_CHECK=yes RELIC_ALLOWED_SKIPS= \
    sh "$runner" sh "$work/skip.sh" 2>&1) || true
case $message in
*skip.sh*RELIC_ALLOWED_SKIPS*)
	;;
*)
	echo "the refusal does not name the test: $message" >&2
	failures=$((failures + 1))
	;;
esac

if [ "$failures" -ne 0 ]; then
	echo "$failures case(s) failed" >&2
	exit 1
fi

exit 0
