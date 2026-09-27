#!/bin/sh
#
# The allocation-failure suite can refuse every allocation the library makes.
#
# tests/unit/test_exhaustion compiles the library's sources with each
# allocator they call renamed to a replacement that refuses the nth request,
# which is what takes the failure paths the arena, interning and relation
# contracts are built around.  An allocator the renaming misses is a failure
# path the suite never takes, and nothing reports it: the suite still passes,
# having tested less.  So the library objects that suite compiles are checked
# instead of the list of renames.
#
# The check fails closed.  A name an object references must be one of:
#
#	the library's own, or a replacement the suite supplies: relic_*;
#
#	a C library function listed below, each known to hand no memory to its
#	caller.  A routine that allocates for its own use, as fopen(3) does, is
#	listed: its failure reaches relic as that routine's error, which the
#	library handles whatever its cause;
#
#	a name reserved to the implementation -- two underscores, or an
#	underscore and a capital -- which is how compilers, sanitizers and C
#	libraries name what they call on relic's behalf, unless it is one of the
#	allocators under an implementation's spelling, such as glibc's __strdup
#	or a fortified __asprintf_chk.
#
# Anything else fails, so a new C library function is a failure until it is
# confirmed to allocate nothing for its caller and listed, rather than a
# silent gap if it does.  An object nm cannot read fails too.

set -eu

: "${RELIC_BUILDDIR:?RELIC_BUILDDIR must name the build directory}"

fail() {
	echo "$1" >&2
	exit 1
}

reported=$(mktemp) || exit 1
listing=$(mktemp) || exit 1
trap 'rm -f "$reported" "$listing"' EXIT
trap 'exit 1' HUP INT TERM

objects=$(find "$RELIC_BUILDDIR/lib" -name 'tests_unit_test_exhaustion-*.o' \
    -type f 2>/dev/null || true)

if [ -z "$objects" ]; then
	fail "no objects of the allocation-failure suite under $RELIC_BUILDDIR"
fi

if ! command -v nm >/dev/null 2>&1; then
	echo "nm is not available; skipping the allocator check" >&2
	exit 77
fi

# Mach-O gives every C identifier a leading underscore and ELF does not, and
# either may decorate a name after a $ or an @ with a variant or a version.
# The format is determined from the output, as tests/globals.sh does.
first=$(echo "$objects" | sed -n '1p')

if nm -m "$first" >/dev/null 2>&1 &&
    nm -m "$first" 2>/dev/null | grep -q '(__'; then
	prefix=_
elif nm "$first" >/dev/null 2>&1; then
	prefix=
else
	fail "nm understands neither object format for $first"
fi

allocators='malloc|calloc|realloc|reallocf|reallocarray|aligned_alloc'
allocators="$allocators|posix_memalign|memalign|valloc|pvalloc|mmap|mremap"
allocators="$allocators|strdup|strndup|wcsdup|asprintf|vasprintf|getline"
allocators="$allocators|getdelim|open_memstream|open_wmemstream|realpath"
allocators="$allocators|getcwd|scandir|glob|tempnam"

listed='abort|bcmp|bzero|fclose|ferror|fopen|fprintf|fputc|fputs|fread|free'
listed="$listed|getpagesize|memcmp|memcpy|memmove|memset|munmap|qsort"
listed="$listed|snprintf|stderr|strerror|strlcpy|strlen|vsnprintf"

for object in $objects; do
	if ! nm -u "$object" > "$listing" 2>/dev/null; then
		echo "$object: nm cannot read it"
		continue
	fi

	# A name's base strips the underscores, the fortified _chk suffix and
	# the large-file 64 suffix an implementation may spell an allocator
	# with, so that __strdup, __asprintf_chk and mmap64 are all seen for
	# what they are.
	awk -v object="$object" -v prefix="$prefix" \
	    -v allocators="^($allocators)\$" -v listed="^($listed)\$" '
	{
		name = $NF
		if (prefix != "" && substr(name, 1, 1) == prefix)
			name = substr(name, 2)
		sub(/[$@].*/, "", name)

		base = name
		sub(/^_+/, "", base)
		sub(/_chk$/, "", base)
		sub(/64$/, "", base)

		if (name ~ /^relic_/)
			next
		if (base ~ allocators)
			print object ": " name " allocates"
		else if (name !~ /^(__|_[A-Z])/ && name !~ listed)
			print object ": " name " is not known to allocate nothing"
	}' "$listing"
done > "$reported"

if [ -s "$reported" ]; then
	echo "the allocation-failure suite cannot vouch for these names:" >&2
	cat "$reported" >&2
	fail "rename an allocator in RELIC_EXHAUSTION_RENAMES, or list a function that allocates nothing in tests/allocators.sh"
fi

exit 0
