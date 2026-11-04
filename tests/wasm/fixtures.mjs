/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

const symbol = value => ({ type: "symbol", value });
const integer = value => ({ type: "integer", value });
const bytes = value => ({ type: "string", value: new Uint8Array(value) });

/* Native transcripts and typed answers are independent, readable oracles. */
export const fixtures = [
	{
		name: "recursive cycle",
		program: "edge(a,b). edge(b,c). edge(c,a). " +
		    "path(X,Y) :- edge(X,Y). " +
		    "path(X,Z) :- edge(X,Y), path(Y,Z). path(a,X)?",
		rows: [[[symbol("a")], [symbol("b")], [symbol("c")]]],
		native: "a\nb\nc\n"
	},
	{
		name: "stratified negation",
		program: "p(a). p(b). q(b). r(X) :- p(X), not q(X). r(X)?",
		rows: [[[symbol("a")]]], native: "a\n"
	},
	{
		name: "aggregate count",
		program: "p(a). p(b). total(N) :- N = count { p(X) }. total(N)?",
		rows: [[[integer(2n)]]], native: "2\n"
	},
	{
		name: "regular expression and arithmetic",
		program: 'r(N) :- N = match("abc", "^a") + 2 * 3. r(N)?',
		rows: [[[integer(6n)]]], native: "6\n"
	},
	{
		name: "multiple queries and zero-column answers",
		program: "p(a). p(a)? p(b)? p(X)?",
		rows: [[[]], [], [[symbol("a")]]], native: "\n\n\na\n"
	},
	{
		name: "no queries",
		program: "p(a).", rows: [], native: ""
	},
	{
		name: "exact signed integer extremes",
		program: "v(-9223372036854775808). v(9223372036854775807). v(X)?",
		rows: [[[integer(-9223372036854775808n)], [integer(9223372036854775807n)]]],
		native: "-9223372036854775808\n9223372036854775807\n"
	},
	{
		name: "byte strings preserve NUL and invalid UTF-8",
		program: String.raw`v("\0\xffA\n"). v(X)?`,
		rows: [[[bytes([0, 255, 65, 10])]]], native: '"\\0\\xffA\\n"\n'
	},
	{
		name: "HTML remains a string",
		program: 'p("<img src=x onerror=alert(1)>"). p(X)?',
		rows: [[[bytes([...new TextEncoder().encode("<img src=x onerror=alert(1)>")])]]],
		native: '"<img src=x onerror=alert(1)>"\n'
	}
];
