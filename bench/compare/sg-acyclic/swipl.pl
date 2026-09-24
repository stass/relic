:- table sg/2.

sg(Y, Y) :- par(_, Y).
sg(X, Y) :- par(X, A), sg(A, B), par(Y, B).

answer(X, Y) :- sg(X, Y).
