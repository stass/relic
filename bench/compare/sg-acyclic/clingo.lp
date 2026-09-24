sg(Y, Y) :- par(_, Y).
sg(X, Y) :- par(X, A), sg(A, B), par(Y, B).

#show sg/2.
