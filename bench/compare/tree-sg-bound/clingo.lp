sg(X, X) :- parent(_, X).
sg(X, Y) :- parent(P, X), sg(P, Q), parent(Q, Y).

answer(X) :- sg(t2048, X).
#show answer/1.
