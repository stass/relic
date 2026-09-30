:- table node/1.

node(X) :- edge(X, _).
node(Y) :- edge(_, Y).
degree(X, O, I) :- node(X), aggregate_all(count, edge(X, _), O),
    aggregate_all(count, edge(_, X), I).

answer(X, O, I) :- degree(X, O, I).
