node(X) :- edge(X, _).
node(Y) :- edge(_, Y).
degree(X, O, I) :- node(X), O = #count { Y : edge(X, Y) },
    I = #count { Z : edge(Z, X) }.

#show degree/3.
