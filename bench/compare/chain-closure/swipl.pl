:- table path/2.

path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).

answer(X, Y) :- path(X, Y).
