:- table sg/2.

sg(X, X) :- parent(_, X).
sg(X, Y) :- parent(P, X), sg(P, Q), parent(Q, Y).

answer(X, Y) :- sg(X, Y).
