# Read the answers relic writes with -F json, one document per query, and
# name the runs of answers whose truth the text format cannot show.
#
# In the text format an answer with no rows writes nothing, a query of no
# columns that holds writes one empty line, and the line between two
# answers is empty too.  So in a run of answers with no row of values,
# bounded by answers that have one or by the ends of the output, the empty
# lines show how many held but not which.  JSON writes the two apart: an
# answer is shown when a row holds a value, held when its one row is `[]`,
# and failed when it has no rows.  An answer with no rows is `[]` whatever
# its columns, so a run is named when it holds both a held and a failed
# answer, which asks a little more than the text format needs.
#
# The layout read is the one relic(1) writes: `[]` alone on a line for an
# answer with no rows, or `[` and `]` on lines of their own around one row
# per line, each row a bracketed array, indented, with a comma after every
# row but the last.  Anything else is refused rather than skipped, so that a
# change of layout cannot leave every run unread and every test passing.
#
# Exit status: 0 when no run hides its answers, 1 when one does, each such
# run named on the standard output by its queries' numbers, and 2 when the
# input is not in the layout.

function trimmed(text) {
	sub(/^[ \t]+/, "", text)
	sub(/,$/, "", text)
	return (text)
}

function malformed(why) {
	printf "line %d: %s, found \"%s\"\n", NR, why, $0
	refused = 1
	exit 2
}

# Close the run that ends with query `last`, naming it if it hides which
# of its answers held.
function end_run(last) {
	if (held > 0 && failed > 0) {
		printf "queries %d to %d answer no row of values, " \
		    "and some hold and some fail\n", first, last
		hidden = 1
	}
	held = 0
	failed = 0
}

function answer(kind) {
	if (kind == "shown") {
		end_run(answers)
	} else {
		if (held + failed == 0)
			first = answers + 1
		if (kind == "held")
			held++
		else
			failed++
	}
	answers++
}

{ line = trimmed($0) }

!open && line == "[]" { answer("failed"); next }
!open && line == "[" { open = 1; rows = 0; shown = 0; next }
!open { malformed("expected an answer, [] or [") }

line == "]" {
	open = 0
	answer(shown ? "shown" : rows > 0 ? "held" : "failed")
	next
}
line ~ /^\[.*\]$/ { rows++; if (line != "[]") shown = 1; next }
{ malformed("expected a row or the ] that ends the answer") }

END {
	if (refused)
		exit 2
	if (open) {
		printf "the output ends inside answer %d\n", answers + 1
		exit 2
	}
	end_run(answers)
	exit hidden
}
