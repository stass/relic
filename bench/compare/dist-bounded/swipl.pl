:- table dist/3.

dist(X, Y, 1) :- par(X, Y).
dist(X, Z, E) :- dist(X, Y, D), D < 5, par(Y, Z), E is D + 1.

answer(X, Y, D) :- dist(X, Y, D).
