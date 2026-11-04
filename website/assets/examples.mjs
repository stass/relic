/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

export const examples = [
	{
		name: "What depends on the database?",
		program: `depends(web, api).
depends(api, database).
depends(worker, api).

affected(App, Changed) :- depends(App, Changed).
affected(App, Changed) :-
    depends(App, Middle), affected(Middle, Changed).

affected(App, database)?`,
		answer: ["api", "web", "worker"]
	},
	{
		name: "Reachability with a cycle",
		program: `edge(a, b). edge(b, c). edge(c, a).
edge(c, d).

path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).

path(a, Destination)?`,
		answer: ["a", "b", "c", "d"]
	},
	{
		name: "Count dependencies",
		program: `task(build). task(test). task(ship). task(docs).
depends(test, build).
depends(ship, test). depends(ship, docs).

dependency_count(Task, N) :-
    task(Task), N = count { depends(Task, Dependency) }.

dependency_count(Task, N)?`,
		answer: ["build\t0", "docs\t0", "ship\t2", "test\t1"]
	},
	{
		name: "Ready tasks with negation",
		program: `task(build). task(test). task(ship). task(docs).
depends(test, build).
depends(ship, test). depends(ship, docs).
done(build).

blocked(Task) :- depends(Task, Dependency), not done(Dependency).
ready(Task) :- task(Task), not done(Task), not blocked(Task).

ready(Task)?`,
		answer: ["docs", "test"]
	}
];
