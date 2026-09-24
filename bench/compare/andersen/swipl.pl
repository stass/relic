:- table pointsTo/2.

pointsTo(V, W) :- addressOf(V, W).
pointsTo(V, W) :- assign(V, U), pointsTo(U, W).
pointsTo(V, W) :- load(V, P), pointsTo(P, Q), pointsTo(Q, W).
pointsTo(Q, W) :- store(P, V), pointsTo(P, Q), pointsTo(V, W).

answer(X, Y) :- pointsTo(X, Y).
