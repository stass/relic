# The benchmark data sets, sourced by bench/run.sh and bench/compare/run.sh.
#
# `dataset NAME DIR SCALE` writes one data set into DIR: one file per
# relation, <relation>.tsv, one tuple per line with its columns separated by
# tabs, symbols, strings and integers alike written as their text.  That is
# the form Souffle, Nemo, DuckDB and SQLite read directly; source_facts turns
# it into the fact syntax relic, SWI-Prolog and clingo share, telling a
# symbol from a string by its spelling.  The generators are awk programs
# over a fixed sequence, so a data set is the same on every run and every
# platform.

# dataset NAME DIR SCALE: generate the data set NAME into DIR, every size
# multiplied by SCALE, but for the trees: `tree` has depth 11 + SCALE, and
# `small-tree` 1024 * SCALE - 1 nodes.
# SCALE must be a positive decimal integer: shell arithmetic would read a
# leading zero as octal.  Every relation written is a set: a generator may
# draw the same row twice, and unique_rows keeps only its first occurrence.
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
	tree) dataset_tree "$2" $(((1 << (12 + $3)) - 1)) ;;
	small-tree) dataset_tree "$2" "$(small_tree_nodes "$3")" ;;
	tasks) dataset_tasks "$2" $((50000 * $3)) ;;
	facts) dataset_facts "$2" $((300000 * $3)) ;;
	files) dataset_files "$2" $((300000 * $3)) ;;
	dense-cyclic) dataset_random "$2" $((1000 * $3)) $((50000 * $3)) cyclic ;;
	dense-acyclic) dataset_random "$2" $((1000 * $3)) $((50000 * $3)) acyclic ;;
	sparse-cyclic) dataset_random "$2" $((1000 * $3)) $((5000 * $3)) cyclic ;;
	sparse-acyclic) dataset_random "$2" $((1000 * $3)) $((5000 * $3)) acyclic ;;
	andersen) dataset_andersen "$2" $((2000 * $3)) ;;
	cspa) dataset_cspa "$2" $((1000 * $3)) ;;
	*)
		echo "datasets: there is no data set $1" >&2
		return 1
		;;
	esac || return

	unique_rows "$2"
}

# unique_rows DIR: remove repeated rows from every relation in DIR, keeping
# the first occurrence of each where it stands.  An engine that keeps facts
# as clauses, as SWI-Prolog does, would otherwise use a repeated fact twice,
# where the engines that store relations as sets use it once.  It runs in a
# subshell, so that its loop variable stays out of the caller's shell.
unique_rows() (
	for file in "$1"/*.tsv; do
		[ -e "$file" ] || continue
		awk '!seen[$0]++' "$file" > "$file.unique" &&
		    mv "$file.unique" "$file" || return
	done
)

# A linear congruential sequence, so that a "random" graph is the same on
# every run.  The multiplier keeps every intermediate product below 2^53,
# where awk's arithmetic is exact.  A draw scales the state into the bound
# rather than reducing it modulo the bound: with a power-of-two modulus the
# low bits of the state cycle with short periods -- the lowest two alternate
# between two values -- so consecutive draws taken from them are correlated,
# while the high bits vary independently.
#
# No expression holds two draws: awk leaves unspecified the order in which
# the operands of an expression or the arguments of a call are evaluated, so
# two draws in one could come out in either order.  Where a line needs two,
# the first is assigned to a variable before the second is drawn.
datasets_random_awk='function next_random(bound) {
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
	dataset_dir=$1 awk -v n="$2" -v m="$3" "$datasets_random_awk"'
	BEGIN {
		edges = ENVIRON["dataset_dir"] "/edge.tsv"
		state = 42
		for (i = 0; i + 1 < n; i++)
			printf "n%d\tn%d\n", i, i + 1 > edges
		for (i = n - 1; i < m; i++) {
			from = next_random(n)
			to = next_random(n)
			printf "n%d\tn%d\n", from, to > edges
		}
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

# small_tree_nodes SCALE: the nodes of the small tree at SCALE, 1024 more
# per step, 1023 (depth 9) at scale 1.  Same generation over the whole tree
# pairs every two nodes of a level, so its answer grows with the square of
# the nodes, and of the scale, as the chain's closure does; a tree a level
# deeper per step would quadruple the answer every step.
small_tree_nodes() {
	echo $((1024 * $1 - 1))
}

# dataset_tree DIR N: a complete binary tree of N nodes, t1 to tN, numbered
# as a heap, t1 the root and tI the parent of t(2I) and t(2I + 1), as
# parent(P, C).  With N one less than a power of two every level is full.
dataset_tree() {
	dataset_dir=$1 awk -v nodes="$2" 'BEGIN {
		parents = ENVIRON["dataset_dir"] "/parent.tsv"
		for (child = 2; child <= nodes; child++)
			printf "t%d\tt%d\n", int(child / 2), child > parents
	}'
}

# dataset_tasks DIR N: N tasks, k0 to kN-1, each after the first depending on
# one or two earlier ones.  The project has worked through the first half of
# its tasks in the order they were made: one of those is done if every task
# it depends on is done, and then 99 times in 100; k0, which depends on
# nothing, is done.  The done tasks are therefore closed under dependency,
# as in a real project, and the tasks that are ready -- not done, with every
# dependency done -- are the frontier between the done tasks and the rest.
#
# Completion has to be all but certain within the worked half.  If a task
# is done with chance p once its one or two dependencies, drawn from the
# tasks before it, are done, the share d of tasks done can hold steady only
# where d = p(d + d^2)/2, which for p below 1 is d = 0 alone: the share
# falls along the project, the faster the further p is below 1.  At two in
# three, 54 of the first 10000 tasks are done; at 99 in 100 the share falls
# slowly enough that most of the worked half is done.
dataset_tasks() {
	dataset_dir=$1 awk -v n="$2" "$datasets_random_awk"'
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
			if (ready && i < n / 2 && next_random(100) != 0) {
				printf "k%d\n", i > finished
				done[i] = 1
			}
		}
	}'
}

# dataset_facts DIR N: N ternary facts, row(Id, Value, Weight), with the
# value a symbol and the weight an integer.
dataset_facts() {
	dataset_dir=$1 awk -v n="$2" "$datasets_random_awk"'
	BEGIN {
		rows = ENVIRON["dataset_dir"] "/row.tsv"
		state = 99
		for (i = 0; i < n; i++) {
			value = next_random(1000)
			weight = next_random(100)
			printf "r%d\tv%d\t%d\n", i, value, weight > rows
		}
	}'
}

# dataset_files DIR N: N file paths, file(Path), each a string such as
# "d07/s03/util_1234.py": a directory of two fixed-width levels, twenty by
# ten, then a name, one of four prefixes and the file's number, so that
# names vary in length, and one of six extensions.  ext(E) lists the
# extensions, for an engine that finds a path's extension by trying each.
dataset_files() {
	dataset_dir=$1 awk -v n="$2" "$datasets_random_awk"'
	BEGIN {
		files = ENVIRON["dataset_dir"] "/file.tsv"
		extensions = ENVIRON["dataset_dir"] "/ext.tsv"
		split("c h py txt json md", extension, " ")
		split("mod_ util_ test_ main", prefix, " ")
		for (e = 1; e <= 6; e++)
			print extension[e] > extensions
		state = 11
		for (i = 0; i < n; i++) {
			top = next_random(20)
			below = next_random(10)
			kind = 1 + next_random(4)
			suffix = 1 + next_random(6)
			printf "d%02d/s%02d/%s%d.%s\n", top, below, \
			    prefix[kind], i, extension[suffix] > files
		}
	}'
}

# dataset_random DIR N M SHAPE: a random graph of the kind OpenRuleBench uses,
# par(From, To), over the nodes 0 to N-1 as integers, with M edges drawn
# from the sequence; 1000 nodes and 50000 edges is the size of
# OpenRuleBench's tc_d1000_parsize50000.  A cyclic graph draws both ends
# uniformly, so it has cycles and self-loops.  An acyclic graph draws two
# different nodes and points the edge from the one of lower rank to the one
# of higher, so ranks are a topological order.  The ranks rotate the node
# numbers to put node 500, which the bound queries name, in the middle of
# that order at every scale, with ancestors and descendants both; at 1000
# nodes a node's rank is its number.
dataset_random() {
	dataset_dir=$1 awk -v n="$2" -v m="$3" -v shape="$4" \
	    "$datasets_random_awk"'
	function rank(node) {
		return (node - 500 + n / 2 + n) % n
	}
	# draw_edge: set from and to to the next edge.
	function draw_edge(    swap) {
		from = next_random(n)
		to = next_random(n)
		if (shape == "acyclic") {
			while (to == from)
				to = next_random(n)
			if (rank(from) > rank(to)) {
				swap = from
				from = to
				to = swap
			}
		}
	}
	BEGIN {
		edges = ENVIRON["dataset_dir"] "/par.tsv"
		state = 5
		for (i = 0; i < m; i++) {
			draw_edge()
			printf "%d\t%d\n", from, to > edges
		}
	}'
}

# The points-to data sets model a program as functions of 20 variables each,
# v0 to v19 in the first function, v20 to v39 in the second and so on.  A
# function's statements relate its own variables, drawn at random, and the
# functions form a binary call tree: function F, after the first, is called
# by function (F - 1) / 2, which passes it arguments, each an assignment of
# one of the caller's variables to one of the callee's.  No call returns a
# value, so a variable receives values from its own function and its
# ancestors in the tree, and from other functions only through memory: a
# pointer passed down lets a descendant write through it into what an
# ancestor's variable points to.  The answers grow roughly in proportion to
# the program, as measured from scale 1 to 16.  Statements drawn across the
# whole program instead join almost every variable into one set of aliases
# past a threshold density, and the answer jumps from linear to quadratic in
# the size of the program at a point that depends on the draws.
#
# A program using these helpers sets dir to the data set's directory and
# defines assignment(target, source), which writes one assignment in its
# analysis's column order; every assignment goes through it.
datasets_program_awk='
function random_variable(f) {
	return sprintf("v%d", f * 20 + next_random(20))
}
function caller(f) {
	return int((f - 1) / 2)
}
function statement(relation, f,    first) {
	first = random_variable(f)
	printf "%s\t%s\n", first, random_variable(f) > (dir "/" relation ".tsv")
}
function local_assignment(f,    target) {
	target = random_variable(f)
	assignment(target, random_variable(f))
}
function arguments(count, f,    i, parameter) {
	if (f == 0)
		return
	for (i = 0; i < count; i++) {
		parameter = random_variable(f)
		assignment(parameter, random_variable(caller(f)))
	}
}'

# dataset_andersen DIR F: a program of F functions for Andersen-style
# points-to analysis, as addressOf(V, W) for V = &W, assign(V, W) for V = W,
# load(V, P) for V = *P and store(P, W) for *P = W.  Each function takes
# the address of 5 variables, and has 10 assignments, 3 loads, 3 stores and
# 3 arguments from its caller.
dataset_andersen() {
	dataset_dir=$1 awk -v functions="$2" \
	    "$datasets_random_awk$datasets_program_awk"'
	function assignment(target, source) {
		printf "%s\t%s\n", target, source > (dir "/assign.tsv")
	}
	BEGIN {
		dir = ENVIRON["dataset_dir"]
		state = 11
		for (f = 0; f < functions; f++) {
			for (i = 0; i < 5; i++)
				statement("addressOf", f)
			for (i = 0; i < 10; i++)
				local_assignment(f)
			for (i = 0; i < 3; i++)
				statement("load", f)
			for (i = 0; i < 3; i++)
				statement("store", f)
			arguments(3, f)
		}
	}'
}

# dataset_cspa DIR F: a program of F functions for Graspan's CSPA rules, as
# assign(W, V) for V = W, the value flowing from the first column to the
# second, and dereference(P, V) for V the value *P.  Each function has 10
# assignments, 5 dereferences and 2 arguments from its caller.
dataset_cspa() {
	dataset_dir=$1 awk -v functions="$2" \
	    "$datasets_random_awk$datasets_program_awk"'
	function assignment(target, source) {
		printf "%s\t%s\n", source, target > (dir "/assign.tsv")
	}
	BEGIN {
		dir = ENVIRON["dataset_dir"]
		state = 13
		for (f = 0; f < functions; f++) {
			for (i = 0; i < 10; i++)
				local_assignment(f)
			for (i = 0; i < 5; i++)
				statement("dereference", f)
			arguments(2, f)
		}
	}'
}

# source_facts DIR: every relation in DIR as facts, relation(a, b, 7), a
# syntax relic, SWI-Prolog and clingo all read.  A value spelled as neither
# an integer nor a symbol is a string, and is quoted; no data set writes a
# string holding a quote or a backslash.  The relations come in the order
# of their file names, each whole, as SWI-Prolog wants a predicate's
# clauses together.
source_facts() {
	for file in "$1"/*.tsv; do
		awk -F '\t' -v relation="$(basename "$file" .tsv)" '
		function term(value) {
			if (value ~ /^-?[0-9]+$/ ||
			    value ~ /^[a-z][A-Za-z0-9_]*$/)
				return value
			return "\"" value "\""
		}
		{
			printf "%s(%s", relation, term($1)
			for (i = 2; i <= NF; i++)
				printf ", %s", term($i)
			print ")."
		}' "$file"
	done
}
