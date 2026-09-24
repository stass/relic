:- table tc/2.

tc(X, Y) :- par(X, Y).
tc(X, Z) :- par(X, Y), tc(Y, Z).

answer(X) :- tc(X, 500).
