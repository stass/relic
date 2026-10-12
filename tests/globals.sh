#!/bin/sh
#
# The library contains no mutable global state.
#
# Two relic contexts share nothing, which is what lets an embedding program
# hold several of them without a lock and what makes a run reproducible.  A
# cache, a counter or a lazily initialized table at file scope would break
# both, and neither failure shows up as a build error or as a failing test:
# the first needs two contexts and a thread to appear, the second needs the
# same program to be run twice.
#
# The property is therefore checked against the built objects rather than
# promised in a comment.  Every C object in a writable data, BSS or common
# section is a mutable global, and there must be none.  Constant tables live
# in read-only sections and are not reported; a `static` counter added for
# debugging is, which is the point.
#
# Only symbols that could have come from a relic declaration are considered.
# A compiler puts records of its own in writable sections -- the
# source-location descriptors UndefinedBehaviorSanitizer emits and the
# per-global descriptors AddressSanitizer emits beside every global,
# read-only ones included -- and those are not relic's to remove.
#
# They are told apart by naming convention rather than by a list of names to
# ignore, because a list would have to grow with every compiler and every
# instrumentation mode and the entry that was missing would be the one that
# mattered.  Two conventions do it.  A symbol that was never declared in C at
# all has no C identifier: the object format's own prefix is absent.  A
# symbol that was declared in C by the implementation rather than by relic
# has an identifier reserved to the implementation, which is one beginning
# with two underscores or with an underscore and a capital; relic-owned code
# may not use either. Native global merging can erase a source declaration
# into such a reserved name, whose ownership nm alone cannot recover. The
# bitcode audit preserves data-candidate names through native lowering.

set -eu

: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"
# shellcheck source=tests/object-tools.sh
. "$(dirname "$0")/object-tools.sh"

fail() {
	echo "$1" >&2
	exit 1
}

work=$(mktemp -d) || exit 1
reported="$work/reported"
: > "$reported"
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

# Both the ordinary and the position-independent objects, because a build may
# produce either or both depending on --enable-shared and --enable-static.
# compat/ is scanned alongside lib/: its replacements are linked into the
# shipped library, so a static buffer there would be the library's too.
find "$RELIC_BUILDDIR/lib" -name '*.o' -type f > "$work/objects" ||
    fail "cannot list library objects"
if [ -d "$RELIC_BUILDDIR/compat" ]; then
	find "$RELIC_BUILDDIR/compat" -name '*.o' -type f >> "$work/objects" ||
	    fail "cannot list compatibility objects"
fi

if [ ! -s "$work/objects" ]; then
	fail "no library objects found under $RELIC_BUILDDIR"
fi

# Exit 77 rather than 0: automake reports 0 as a pass, and a check that could
# not run must not be indistinguishable from one that ran and found nothing.
if ! audit_nm_available; then
	echo "nm is not available; skipping the global-state check" >&2
	exit 77
fi

# A symbol in a writable section is an object with static storage duration.
#
# The Mach-O test is written as "every __DATA section except the read-only
# one" rather than as a list of the writable section names.  A list would
# have to be complete, and it would not be: thread-local storage lands in
# __thread_data, __thread_bss and __thread_vars, so naming __data, __bss and
# __common would let a _Thread_local cache through -- and a per-thread cache
# breaks the same property a plain one does, because two contexts on one
# thread would share it.
#
# On ELF nm reports a one-letter type as well as the section, in its System V
# format.  The writable types are bss (b), common (c), data (d), small data
# (g) and small bss (s), in either case, a weak object (V) and a GNU unique
# one (u); a thread-local object lands in .tdata or .tbss and is reported as
# data or bss like any other, so the same set covers it.  The type alone is
# not enough: a constant that holds an address, such as a table of strings, is
# reported as data because the dynamic linker must write the address into it,
# but a position-independent object places it in .data.rel.ro, which is
# read-only once relocated.  That section is the counterpart of Mach-O's
# __DATA_CONST,__const and is exempt for the same reason.
#
# The object list is walked a line at a time rather than by word splitting,
# because a build directory may have a space in its path.
while IFS= read -r object; do
	audit_object "$object" "$work" || fail "$object: cannot audit object"
	case "$audit_format" in
	macho)
		# Mach-O gives every C identifier a leading underscore, so a
		# name without one -- l___unnamed_1, ltmp3 -- was emitted by
		# the compiler or the assembler and never declared in C.
		# A thread-local's generated companion carries a $ in its name.
		# With the prefix stripped, a reserved identifier leaves a
		# symbol beginning ___ or __ and a capital.
		# Tentative definitions can be common symbols without a section.
		# Optimization can split a C object into names such as _state.0.
		found=$(grep -E '\(__DATA(_CONST)?,__|\(common\)' "$audit_listing" |
		    grep -Ev '\(__DATA(_CONST)?,__const\)' |
		    grep -E ' _[A-Za-z0-9_$.]+$' |
		    grep -Ev ' __(_|[A-Z])[A-Za-z0-9_$.]*$' || true)
		;;
	elf)
		# ELF has no such prefix, but a compiler-generated object is a
		# section-local label, whose name begins with a dot, and a
		# reserved identifier is unchanged by the format.
		found=$(awk -F'|' '
		NF == 7 {
			name = $1
			letter = $3
			section = $7
			gsub(/[ \t]/, "", name)
			gsub(/[ \t]/, "", letter)
			gsub(/[ \t]/, "", section)

			if (letter !~ /^[BbCcDdGgSsVu]$/)
				next
			if (section ~ /^\.data\.rel\.ro(\.|$)/)
				next
			if (name ~ /^\./ || name ~ /^(__|_[A-Z])/)
				next
			print letter " " name " " section
		}' "$audit_listing")
		;;
	esac

	if [ -n "$found" ]; then
		echo "$object:" >> "$reported"
		echo "$found" >> "$reported"
	fi
done < "$work/objects"

if [ -s "$reported" ]; then
	echo "librelic has mutable global state:" >&2
	cat "$reported" >&2
	fail "every relic context must own all of its state"
fi

exit 0
