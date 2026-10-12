#!/bin/sh
#
# Object audits distinguish constants from shared state and refuse every
# allocator the failure-injection suite cannot intercept. Native objects and
# LLVM bitcode must establish the same properties, including in paths with
# spaces. A damaged object must fail the audit rather than disappear from it.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
: "${RELIC_CC:=cc}"
: "${RELIC_CFLAGS:=}"
: "${RELIC_LTO_CFLAGS:=}"
# shellcheck source=tests/object-tools.sh
. "$RELIC_SRCDIR/tests/object-tools.sh"

if ! audit_nm_available; then
	echo "nm is not available; skipping the object-audit controls" >&2
	exit 77
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

failures=0

expect() {
	expected=$1
	audit=$2
	fixture=$3
	name=$4
	status=0
	env RELIC_BUILDDIR="$fixture" sh "$RELIC_SRCDIR/tests/$audit.sh" \
	    > "$work/output" 2>&1 || status=$?
	if [ "$status" -ne "$expected" ]; then
		echo "$name: expected $expected from $audit, got $status" >&2
		cat "$work/output" >&2
		failures=$((failures + 1))
	elif [ "$expected" -ne 0 ] &&
	    ! grep -q "$name" "$work/output"; then
		echo "$audit did not identify $name" >&2
		cat "$work/output" >&2
		failures=$((failures + 1))
	fi
}

compile() {
	# Compiler commands and flags have the same word-splitting contract as
	# configure. Object and source paths remain single arguments.
	# shellcheck disable=SC2086
	$RELIC_CC $RELIC_CFLAGS $mode -c "$1" -o "$2"
}

cat > "$work/constants.c" <<'EOF'
static const char *const table[] = { "one", "two" };
const char *lookup(unsigned int n) { return (table[n & 1]); }
EOF
cat > "$work/mutable.c" <<'EOF'
int mutable_counter;
int bump(void) { return (++mutable_counter); }
EOF
cat > "$work/static.c" <<'EOF'
static int static_counter;
int bump(void) { return (++static_counter); }
EOF
cat > "$work/tls.c" <<'EOF'
_Thread_local int thread_counter;
int bump(void) { return (++thread_counter); }
EOF
cat > "$work/common.c" <<'EOF'
int tentative_counter;
EOF
cat > "$work/split.c" <<'EOF'
typedef struct audit_state audit_state_t;
struct audit_state { void *pointer; int count; };
static audit_state_t split_state;
int bump(void *p) { split_state.pointer = p; return (++split_state.count); }
void *read_pointer(void) { return (split_state.pointer); }
EOF
cat > "$work/allowed.c" <<'EOF'
extern void *relic_alloc(unsigned long);
void *call(void) { return (relic_alloc(7)); }
EOF
cat > "$work/allocator.c" <<'EOF'
extern void *malloc(unsigned long);
void *call(void) { return (malloc(7)); }
EOF

for kind in native bitcode; do
	mode=
	case " $RELIC_CFLAGS " in
	*' -flto'* ) mode=-fno-lto ;;
	esac
	if [ "$kind" = bitcode ]; then
		# Bitcode tools are required only by a build that emits bitcode.
		# An ordinary C11 toolchain need not understand LLVM objects.
		case " $RELIC_LTO_CFLAGS $RELIC_CFLAGS " in
		*' -flto=thin'* ) ;;
		*) continue ;;
		esac
		mode=-flto=thin
		if ! compile "$work/constants.c" "$work/probe.o" \
		    > "$work/probe.log" 2>&1; then
			cat "$work/probe.log" >&2
			exit 1
		fi
	fi

	fixture="$work/$kind objects"
	mkdir -p "$fixture/lib" "$fixture/compat"
	object="$fixture/lib/tests_unit_test_exhaustion-fixture.o"
	compile "$work/constants.c" "$object"
	expect 0 globals "$fixture" "$kind-constants"
	compile "$work/mutable.c" "$object"
	expect 1 globals "$fixture" mutable_counter
	compile "$work/static.c" "$object"
	expect 1 globals "$fixture" static_counter
	compile "$work/tls.c" "$object"
	expect 1 globals "$fixture" thread_counter
	compile "$work/common.c" "$object"
	expect 1 globals "$fixture" tentative_counter
	# Bitcode retains the names of independently optimized fields. Native
	# global merging can erase their source identities before nm sees them.
	if [ "$kind" = bitcode ]; then
		compile "$work/split.c" "$object"
		expect 1 globals "$fixture" split_state
	fi
	compile "$work/allowed.c" "$object"
	expect 0 allocators "$fixture" "$kind-allowed"
	compile "$work/allocator.c" "$object"
	expect 1 allocators "$fixture" malloc

	# Keep a readable object first and damage a second one, so checking only
	# the first object's format cannot hide a failed later symbol read.
	compile "$work/constants.c" "$object"
	dd if="$object" of="$fixture/compat/damaged.o" bs=4 count=1 2>/dev/null
	expect 1 globals "$fixture" damaged
	cp "$fixture/compat/damaged.o" "$fixture/lib/tests_unit_test_exhaustion-zdamaged.o"
	rm "$fixture/compat/damaged.o"
	expect 1 allocators "$fixture" damaged
done

if [ "$failures" -ne 0 ]; then
	echo "$failures object-audit case(s) failed" >&2
	exit 1
fi
