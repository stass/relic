dist(X, Y, 1) :- par(X, Y).
dist(X, Z, D + 1) :- dist(X, Y, D), par(Y, Z), D < 5.

#show dist/3.
