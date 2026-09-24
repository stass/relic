# The benchmark data sets, sourced by bench/run.sh and bench/compare/run.sh.
#
# `dataset NAME DIR SCALE` writes one data set into DIR: one file per
# relation, <relation>.tsv, one tuple per line with its columns separated by
# tabs, symbols bare and integers in decimal.  That is the form Souffle and
# Nemo read directly; source_facts turns it into the fact syntax relic,
# SWI-Prolog and clingo share.  The generators are awk programs over a fixed
# sequence, so a data set is the same on every run and every platform.

# dataset NAME DIR SCALE: generate the data set NAME into DIR, every size
# multiplied by SCALE; a tree deepens by one level, doubling, per step.
# SCALE must be a positive decimal integer: shell arithmetic would read a
# leading zero as octal.
dataset() {
	case $3 in
	'' | *[!0-9]* | 0*)
		echo "datasets: the scale must be a positive integer" >&2
		return 1
		;;
	esac

	case $1 in
	unit) dataset_unit "$2" ;;
	chain) dataset_chain "$2" $((2000 * $3)) ;;
	graph) dataset_graph "$2" $((20000 * $3)) $((100000 * $3)) ;;
	grid) dataset_grid "$2" $((150 * $3)) ;;
	tree) dataset_tree "$2" $((12 + $3 - 1)) ;;
	small-tree) dataset_tree "$2" $((9 + $3 - 1)) ;;
	tasks) dataset_tasks "$2" $((10000 * $3)) ;;
	facts) dataset_facts "$2" $((300000 * $3)) ;;
	*)
		echo "datasets: there is no data set $1" >&2
		return 1
		;;
	esac
}

# A linear congruential sequence, so that a "random" graph is the same on
# every run.  The multiplier keeps every intermediate product below 2^53,
# where awk's arithmetic is exact.  A draw scales the state into the bound
# rather than reducing it modulo the bound: with a power-of-two modulus the
# low bits of the state cycle with short periods -- the lowest two alternate
# between two values -- so consecutive draws taken from them are correlated,
# while the high bits vary independently.
lcg='function next_random(bound) {
	state = (state * 1103515 + 12345) % 2147483648
	return int(state / 2147483648 * bound)
}'

# The generators receive their directory through the environment, because
# awk -v processes backslash escapes in the value and would mangle a path
# holding one.

# dataset_unit DIR: one edge, for measuring what every run costs.
dataset_unit() {
	printf 'n0\tn1\n' > "$1/edge.tsv"
}

# dataset_chain DIR N: a path of N edges.
dataset_chain() {
	dataset_dir=$1 awk -v n="$2" 'BEGIN {
		edges = ENVIRON["dataset_dir"] "/edge.tsv"
		for (i = 0; i < n; i++)
			printf "n%d\tn%d\n", i, i + 1 > edges
	}'
}

# dataset_graph DIR N M: N nodes and M edges, a chain through every node so
# all are reachable from n0, and the rest drawn from the sequence.
dataset_graph() {
	dataset_dir=$1 awk -v n="$2" -v m="$3" "$lcg"'
	BEGIN {
		edges = ENVIRON["dataset_dir"] "/edge.tsv"
		state = 42
		for (i = 0; i + 1 < n; i++)
			printf "n%d\tn%d\n", i, i + 1 > edges
		for (i = n - 1; i < m; i++)
			printf "n%d\tn%d\n", next_random(n),
			    next_random(n) > edges
	}'
}

# dataset_grid DIR N: an N by N grid with edges right and down.
dataset_grid() {
	dataset_dir=$1 awk -v n="$2" '
	function cell(r, c) {
		return sprintf("c%d_%d", r, c)
	}
	function edge(from, to) {
		printf "%s\t%s\n", from, to > edges
	}
	BEGIN {
		edges = ENVIRON["dataset_dir"] "/edge.tsv"
		for (r = 0; r < n; r++) {
			for (c = 0; c < n; c++) {
				if (c + 1 < n)
					edge(cell(r, c), cell(r, c + 1))
				if (r + 1 < n)
					edge(cell(r, c), cell(r + 1, c))
			}
		}
	}'
}

# dataset_tree DIR D: a complete binary tree of depth D, as parent(P, C).
dataset_tree() {
	dataset_dir=$1 awk -v depth="$2" 'BEGIN {
		parents = ENVIRON["dataset_dir"] "/parent.tsv"
		nodes = 2 ^ (depth + 1) - 1
		for (i = 1; 2 * i + 1 <= nodes; i++) {
			printf "t%d\tt%d\n", i, 2 * i > parents
			printf "t%d\tt%d\n", i, 2 * i + 1 > parents
		}
	}'
}

# dataset_tasks DIR N: N tasks, k0 to kN-1, each after the first depending on
# one or two earlier ones.  A task is done only if every task it depends on
# is done, and then two times in three; k0, which depends on nothing, is
# done.  The done tasks are therefore closed under dependency, as in a real
# project, and the tasks that are ready -- not done, with every dependency
# done -- are the frontier between the done tasks and the rest.
dataset_tasks() {
	dataset_dir=$1 awk -v n="$2" "$lcg"'
	BEGIN {
		dir = ENVIRON["dataset_dir"]
		tasks = dir "/task.tsv"
		depends = dir "/depends.tsv"
		finished = dir "/done.tsv"
		state = 7

		printf "k0\n" > tasks
		printf "k0\n" > finished
		done[0] = 1
		for (i = 1; i < n; i++) {
			printf "k%d\n", i > tasks
			first = next_random(i)
			printf "k%d\tk%d\n", i, first > depends
			ready = done[first]
			if (next_random(2) == 0) {
				second = next_random(i)
				printf "k%d\tk%d\n", i, second > depends
				ready = ready && done[second]
			}
			if (next_random(3) != 0 && ready) {
				printf "k%d\n", i > finished
				done[i] = 1
			}
		}
	}'
}

# dataset_facts DIR N: N ternary facts, row(Id, Value, Weight), with the
# value a symbol and the weight an integer.
dataset_facts() {
	dataset_dir=$1 awk -v n="$2" "$lcg"'
	BEGIN {
		rows = ENVIRON["dataset_dir"] "/row.tsv"
		state = 99
		for (i = 0; i < n; i++)
			printf "r%d\tv%d\t%d\n", i, next_random(1000), \
			    next_random(100) > rows
	}'
}

# source_facts DIR: every relation in DIR as facts, relation(a, b, 7), a
# syntax relic, SWI-Prolog and clingo all read.  The relations come in the
# order of their file names, each whole, as SWI-Prolog wants a predicate's
# clauses together.
source_facts() {
	for file in "$1"/*.tsv; do
		awk -F '\t' -v relation="$(basename "$file" .tsv)" '{
			printf "%s(%s", relation, $1
			for (i = 2; i <= NF; i++)
				printf ", %s", $i
			print ")."
		}' "$file"
	done
}
