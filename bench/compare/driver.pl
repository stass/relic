% Print a workload's answer relation, answer/N, one row per line with its
% columns separated by tabs, the form every engine's answer is compared in.
% Loaded before the workload's program and its facts; runs once they are
% loaded, and exits unsuccessfully if the program defines no answer or
% raises an error.

:- initialization(main, main).

main :-
	current_predicate(answer/Arity),
	functor(Goal, answer, Arity),
	forall(Goal, print_row(Goal)).

print_row(Goal) :-
	Goal =.. [_|Columns],
	atomic_list_concat(Columns, '\t', Row),
	writeln(Row).
