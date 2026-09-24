:- table waits/2.

waits(X, Y) :- depends(X, Y), \+ done(Y).
waits(X, Z) :- depends(X, Y), waits(Y, Z).
blocked(X) :- waits(X, _).
ready(X) :- task(X), \+ done(X), \+ blocked(X).

answer(X) :- ready(X).
