:- table valueFlow/2, memoryAlias/2, valueAlias/2.

valueFlow(Y, X) :- assign(Y, X).
valueFlow(X, Y) :- assign(X, Z), memoryAlias(Z, Y).
valueFlow(X, Y) :- valueFlow(X, Z), valueFlow(Z, Y).
valueFlow(X, X) :- assign(X, _).
valueFlow(X, X) :- assign(_, X).

memoryAlias(X, W) :- dereference(Y, X), valueAlias(Y, Z), dereference(Z, W).
memoryAlias(X, X) :- assign(_, X).
memoryAlias(X, X) :- assign(X, _).

valueAlias(X, Y) :- valueFlow(Z, X), valueFlow(Z, Y).
valueAlias(X, Y) :- valueFlow(Z, X), memoryAlias(Z, W), valueFlow(W, Y).

answer(X, Y) :- valueFlow(X, Y).
