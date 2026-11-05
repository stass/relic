/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

export const examples = [
	{
		id: "dependencies",
		name: "What depends on the database?",
		program: `depends(web, api).
depends(api, database).
depends(worker, api).

affected(App, Changed) :- depends(App, Changed).
affected(App, Changed) :-
    depends(App, Middle), affected(Middle, Changed).

affected(App, database)?`,
		answers: [["api", "web", "worker"]]
	},
	{
		id: "cycle",
		name: "Reachability with a cycle",
		program: `edge(a, b). edge(b, c). edge(c, a).
edge(c, d).

path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).

path(a, Destination)?`,
		answers: [["a", "b", "c", "d"]]
	},
	{
		id: "count-dependencies",
		name: "Count dependencies",
		program: `task(build). task(test). task(ship). task(docs).
depends(test, build).
depends(ship, test). depends(ship, docs).

dependency_count(Task, N) :-
    task(Task), N = count { depends(Task, Dependency) }.

dependency_count(Task, N)?`,
		answers: [["build\t0", "docs\t0", "ship\t2", "test\t1"]]
	},
	{
		id: "ready-tasks",
		name: "Ready tasks with negation",
		program: `task(build). task(test). task(ship). task(docs).
depends(test, build).
depends(ship, test). depends(ship, docs).
done(build).

blocked(Task) :- depends(Task, Dependency), not done(Dependency).
ready(Task) :- task(Task), not done(Task), not blocked(Task).

ready(Task)?`,
		answers: [["docs", "test"]]
	},
	{
		id: "subway-routes",
		name: "Subway routes with one transfer",
		program: `% A fictional subway network; segments work in both directions.
segment(red, museum, central).
segment(red, central, harbor).
segment(blue, west_end, central).
segment(blue, central, garden).
segment(blue, garden, airport).
segment(green, harbor, hill).

adjacent(Line, A, B) :- segment(Line, A, B).
adjacent(Line, A, B) :- segment(Line, B, A).
ride(Line, A, B) :- adjacent(Line, A, B).
ride(Line, A, C) :- ride(Line, A, B), adjacent(Line, B, C).

direct(Destination) :- ride(red, museum, Destination).
one_transfer(Destination, Transfer, Line) :-
    ride(red, museum, Transfer), ride(Line, Transfer, Destination),
    Line != red, not direct(Destination).

% Starting at museum on red: destination, transfer station, new line.
one_transfer(Destination, Transfer, Line)?`,
		answers: [["airport\tcentral\tblue", "garden\tcentral\tblue",
		    "hill\tharbor\tgreen", "west_end\tcentral\tblue"]]
	},
	{
		id: "unused-functions",
		name: "Find unused functions in a call graph",
		program: `% Analyze an explicit call graph, starting at main.
function(main). function(parse). function(scan). function(print).
function(legacy_parser). function(debug_dump).
entry(main).
calls(main, parse). calls(parse, scan). calls(main, print).
calls(legacy_parser, debug_dump). calls(debug_dump, legacy_parser).

reachable(Function) :- entry(Function).
reachable(Callee) :- reachable(Caller), calls(Caller, Callee).
unused(Function) :- function(Function), not reachable(Function).

% Reachable functions, then functions unreachable from any entry point.
reachable(Function)?
unused(Function)?`,
		answers: [["main", "parse", "print", "scan"], ["debug_dump", "legacy_parser"]]
	},
	{
		id: "type-inference",
		name: "Infer types in an expression graph",
		program: `% Toy expression graph: assignments copy types; addition needs integers.
int_literal(one). text_literal(label).
assign(x, one). assign(y, x). assign(title, label).
add(total, x, y). add(broken, x, title).

type(Expression, integer) :- int_literal(Expression).
type(Expression, text) :- text_literal(Expression).
type(To, Type) :- assign(To, From), type(From, Type).
type(Sum, integer) :- add(Sum, Left, Right),
    type(Left, integer), type(Right, integer).

type_error(Expression) :- add(Expression, Left, _),
    type(Left, Type), Type != integer.
type_error(Expression) :- add(Expression, _, Right),
    type(Right, Type), Type != integer.

% Inferred types, then additions with a non-integer operand.
type(Expression, Type)?
type_error(Expression)?`,
		answers: [["label\ttext", "one\tinteger", "title\ttext",
		    "total\tinteger", "x\tinteger", "y\tinteger"], ["broken"]]
	}
];
