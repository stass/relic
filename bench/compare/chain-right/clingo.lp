path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).

answer(X) :- path(n0, X).
#show answer/1.
