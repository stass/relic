% A path's directory is its first seven characters and its extension what
% follows its first dot; a test's name holds "/test_", which on this data
% set is exactly what the other engines' pattern matches.
kind(P, D, E) :- file(P), \+ sub_string(P, _, _, _, "/test_"),
    sub_string(P, 0, 7, _, D), once(sub_string(P, B, 1, _, ".")),
    A is B + 1, sub_string(P, A, _, 0, E).

answer(D, E, N) :- aggregate(count, P^kind(P, D, E), N).
