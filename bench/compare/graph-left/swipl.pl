:- table from/2.

from(X, Y) :- edge(X, Y).
from(X, Z) :- from(X, Y), edge(Y, Z).

answer(X) :- from(n17, X).
