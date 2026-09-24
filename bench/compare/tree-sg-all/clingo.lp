sg(X, X) :- parent(_, X).
sg(X, Y) :- parent(P, X), sg(P, Q), parent(Q, Y).

#show sg/2.
