reach(X) :- edge(n0, X).
reach(Y) :- reach(X), edge(X, Y).

#show reach/1.
