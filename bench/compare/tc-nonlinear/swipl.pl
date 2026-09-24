:- table tc/2.

tc(X, Y) :- par(X, Y).
tc(X, Z) :- tc(X, Y), tc(Y, Z).

answer(X, Y) :- tc(X, Y).
