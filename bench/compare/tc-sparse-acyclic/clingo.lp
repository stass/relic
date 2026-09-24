tc(X, Y) :- par(X, Y).
tc(X, Z) :- par(X, Y), tc(Y, Z).

#show tc/2.
