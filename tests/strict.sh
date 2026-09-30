#!/bin/sh
#
# Run one test, and in strict mode refuse to let it skip.
#
# A test that lacks a tool it needs, such as mandoc, nm or pkg-config, exits
# 77 and the harness counts it as skipped rather than failed.  That is right
# on a developer's machine, where the tool is optional, and wrong where every
# tool has been installed on purpose: there a skip means a check silently
# stopped running.  With RELIC_STRICT_CHECK=yes in the environment, a skip is
# reported as a failure unless the test's file name is one of the
# space-separated names in RELIC_ALLOWED_SKIPS, as symbols.sh is for a build
# with no shared library to audit.  RELIC_STRICT_CHECK unset means no; any
# value but yes or no is an error.
#
# Automake runs every test through this script, as the LOG_COMPILER of each
# kind of test, so the policy holds for make check, make distcheck and the
# sanitizer suites alike, and a test added later is covered without being
# listed anywhere.  The test is the last argument; any before it are the
# command that runs it, such as the shell or tests/langtest.sh.
#
# usage: strict.sh [runner ...] test

set -u

skip_status=77
hard_error_status=99

if [ $# -eq 0 ]; then
	echo "usage: strict.sh [runner ...] test" >&2
	exit "$hard_error_status"
fi

# A misspelt or empty value must not turn the policy off without saying so.
case ${RELIC_STRICT_CHECK-no} in
yes|no)
	;;
*)
	echo "RELIC_STRICT_CHECK must be yes or no," \
	    "not '$RELIC_STRICT_CHECK'" >&2
	exit "$hard_error_status"
	;;
esac

for argument in "$@"; do
	test_path=$argument
done
test_name=$(basename "$test_path")

"$@"
status=$?

if [ "$status" -ne "$skip_status" ] || \
    [ "${RELIC_STRICT_CHECK:-no}" = no ]; then
	exit "$status"
fi

# The names are split on spaces and must not be expanded as patterns.
set -f
for allowed in ${RELIC_ALLOWED_SKIPS:-}; do
	if [ "$allowed" = "$test_name" ]; then
		echo "$test_name skipped, as RELIC_ALLOWED_SKIPS permits" >&2
		exit "$status"
	fi
done

echo "$test_name skipped, and RELIC_STRICT_CHECK=yes requires every" \
    "test to run; name it in RELIC_ALLOWED_SKIPS if the skip is expected" >&2
exit 1
