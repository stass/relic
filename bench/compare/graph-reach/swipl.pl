:- table reach/1.

reach(X) :- edge(n0, X).
reach(Y) :- reach(X), edge(X, Y).

answer(X) :- reach(X).
