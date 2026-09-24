tc(X, Y) :- par(X, Y).
tc(X, Z) :- tc(X, Y), tc(Y, Z).

#show tc/2.
