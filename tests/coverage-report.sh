#!/bin/sh
#
# Report the coverage a run of the test suite reached, per source file of the
# library and the tool: the lines executed, the functions entered, by name
# every function no test entered, and every range of lines none executed.
#
# usage: coverage-report.sh builddir srcdir
#
# The build must have been configured with --enable-coverage and
# --disable-shared and the suite run, so that each object has its .gcda
# beside it; a shared build keeps the library's objects in .libs.  GCOV
# names the gcov program and defaults to gcov; with clang on a system whose
# gcov belongs to gcc, set it to "llvm-cov gcov".
#
# A source file can be compiled into more than one object: the allocation-
# failure suite compiles the whole library again, under names of its own,
# and that copy is the only one whose failure paths ever run.  A line counts
# as executed, and a function as entered, when any object compiled from the
# file executed or entered it, so the report describes what the suite as a
# whole reached.
#
# Coverage is a gap finder, not a target, so nothing here fails on a
# percentage.  The list of functions never entered is the part to act on:
# each is either tested or recorded as a known gap.

set -eu

if [ $# -ne 2 ]; then
	echo "usage: coverage-report.sh builddir srcdir" >&2
	exit 2
fi
builddir=$(cd "$1" && pwd)
srcdir=$2
gcov=${GCOV:-gcov}

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM
: > "$work/summaries"
: > "$work/never-entered"
: > "$work/unexecuted"

# The objects compiled from $directory/$base.c that the suite ran: the
# library's or the tool's own, and any a test program compiled under a
# prefix of its own.
objects_of() {
	directory=$1
	base=$2

	if [ -f "$builddir/$directory/$base.gcda" ]; then
		echo "$builddir/$directory/$base.o"
	fi
	for data in "$builddir/$directory"/*-"$base".gcda; do
		if [ -f "$data" ]; then
			echo "${data%.gcda}.o"
		fi
	done
}

# Merge the annotated listings of every object into the file's summary.  A
# listing holds a section per source the object compiled, the file itself
# and every header whose inline functions it used, each opened by a
# `-:0:Source:path` record.  Only the file's own section is read, so a
# header's inline functions are not counted in every file including them;
# the report covers the .c files and leaves the few inline functions in
# headers out.  The path is the one the compiler recorded, relative to the
# source directory where the prefix map applies and absolute where it does
# not, so a section is the file's when its path ends in the file's.
#
# Within the section, `count:line:text` records give each line's count, `-`
# for a line that is not code and `#####` or `=====` for one never executed,
# and `function name called N` records say how often each function was
# entered.  A line is executed, and a function entered, when any object
# executed or entered it.  The lines no object executed are printed as
# ranges; a range runs across lines that are not code, such as comments and
# blank lines, so that one untested block is one range.
merge_listings='
	function range(first, last) {
		return (last > first ? " " first "-" last : " " first)
	}

	/^ *-: *0:Source:/ {
		source = $0
		sub(/^ *-: *0:Source:/, "", source)
		in_file = source == file || \
		    substr(source, length(source) - length(file)) == "/" file
		next
	}
	!in_file {
		next
	}
	/^function / {
		functions[$2] = 1
		if ($4 + 0 > 0)
			entered[$2] = 1
		next
	}
	{
		split($0, field, ":")
		count = field[1]
		gsub(/ /, "", count)
		line = field[2] + 0
		if (line == 0 || count == "-")
			next
		code[line] = 1
		if (count !~ /^(#####|=====)/)
			executed[line] = 1
		if (line > last)
			last = line
	}

	END {
		for (line in code)
			line_count++
		for (line in executed)
			executed_count++
		for (name in functions) {
			function_count++
			if (name in entered)
				entered_count++
			else
				print file ": " name >> never_entered
		}
		print line_count + 0, executed_count + 0, entered_count + 0, \
		    function_count + 0

		in_gap = 0
		ranges = ""
		for (line = 1; line <= last; line++) {
			if (!(line in code))
				continue
			if (!(line in executed)) {
				if (!in_gap)
					start = line
				in_gap = 1
				end = line
				continue
			}
			if (in_gap)
				ranges = ranges range(start, end)
			in_gap = 0
		}
		if (in_gap)
			ranges = ranges range(start, end)
		if (ranges != "")
			print file ":" ranges >> unexecuted
	}'

for directory in lib cmd/relic; do
	for source in "$srcdir/$directory"/*.c; do
		base=${source##*/}
		base=${base%.c}
		file=$directory/$base.c
		objects=$(objects_of "$directory" "$base")
		if [ -z "$objects" ]; then
			continue
		fi

		# gcov finds a source by the path the compiler recorded, which
		# is relative to the source directory.  -b adds the function
		# records to each source's section.
		counts=$(
			cd "$srcdir" &&
			for object in $objects; do
				$gcov -t -b -o "$object" "$file" 2>/dev/null
			done | awk -v file="$file" \
			    -v unexecuted="$work/unexecuted" \
			    -v never_entered="$work/never-entered" \
			    "$merge_listings")
		echo "$file $counts" >> "$work/summaries"
	done
done

if ! awk '$2 > 0 { found = 1 } END { exit !found }' "$work/summaries"
then
	echo "no coverage data under $builddir; was it configured with" \
	    "--enable-coverage --disable-shared, the suite run, and GCOV" \
	    "set to the compiler's gcov?" >&2
	exit 1
fi

awk '
	BEGIN {
		printf "%-24s %7s %9s %13s\n", "file", "lines", "executed", \
		    "functions"
	}
	$2 > 0 {
		printf "%-24s %7d %8.1f%% %6d of %4d\n", $1, $2, \
		    100 * $3 / $2, $4, $5
		lines += $2
		executed += $3
		entered += $4
		functions += $5
	}
	END {
		printf "%-24s %7d %8.1f%% %6d of %4d\n", "total", lines, \
		    100 * executed / lines, entered, functions
	}' "$work/summaries"

echo
echo "functions never entered:"
if [ -s "$work/never-entered" ]; then
	sort "$work/never-entered" | sed 's/^/  /'
else
	echo "  none"
fi

echo
echo "lines never executed:"
if [ -s "$work/unexecuted" ]; then
	sed 's/^/  /' "$work/unexecuted"
else
	echo "  none"
fi
