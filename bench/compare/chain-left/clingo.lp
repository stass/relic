from(X, Y) :- edge(X, Y).
from(X, Z) :- from(X, Y), edge(Y, Z).

answer(X) :- from(n0, X).
#show answer/1.
