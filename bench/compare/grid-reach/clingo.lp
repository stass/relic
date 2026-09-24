reach(X) :- edge(c0_0, X).
reach(Y) :- reach(X), edge(X, Y).

#show reach/1.
