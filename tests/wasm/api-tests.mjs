/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

import { fixtures } from "./fixtures.mjs";

function assert(condition, message) {
	if (!condition)
		throw new Error(message);
}

function canonical(value) {
	return JSON.stringify(value, (_key, item) => {
		if (typeof item === "bigint")
			return item.toString() + "n";
		if (item instanceof Uint8Array)
			return [...item];
		return item;
	});
}

function sorted_rows(rows) {
	return rows.map(row => canonical(row)).sort();
}

/* Many candidates, no derived rows: tuple/output limits cannot end it early. */
export const busy_program = "n(0). n(Y) :- n(X), X < 999, Y = X + 1. " +
    "slow(X) :- n(X), n(Y), n(Z), X + Y + Z < 0. slow(X)?";

export async function run_tests({ evaluate, RelicError, limits }) {
	let checks = 0;
	const expect_failure = async (program, options, kind, name) => {
		let failure;
		try {
			await evaluate(program, options);
		} catch (error) {
			failure = error;
		}
		assert(failure instanceof RelicError, name + ": rejects with RelicError");
		assert(failure.kind === kind, name + ": expected " + kind + ", got " + failure.kind);
		assert(!Object.hasOwn(failure, "queries"), name + ": no partial answers");
		checks++;
		return failure;
	};

	for (const fixture of fixtures) {
		const result = await evaluate(fixture.program);
		assert(typeof result.version === "string" && result.version.length > 0,
		    fixture.name + ": engine version");
		assert(result.queries.length === fixture.rows.length, fixture.name + ": query count");
		for (let i = 0; i < fixture.rows.length; i++) {
			assert(canonical(sorted_rows(result.queries[i].rows)) ===
			    canonical(sorted_rows(fixture.rows[i])), fixture.name + ": typed rows");
		}
		checks++;
	}

	const kept = await evaluate(String.raw`v("\0\xff"). v(X)?`);
	await evaluate("p(a). p(X)?");
	assert(canonical(kept.queries[0].rows[0][0].value) === "[0,255]",
	    "owned bytes survive worker termination and later calls");
	checks++;

	await expect_failure(null, {}, "argument", "non-string source");
	await expect_failure("p(a).", null, "argument", "null options");
	await expect_failure("p(a).", { maxRow: 1 }, "argument", "unknown option");
	await expect_failure("p(a).", { signal: {} }, "argument", "invalid signal");
	await expect_failure("p(a).\0bad", {}, "argument", "literal NUL");
	for (const source of ["\ud800", "\udc00", "\ud800x"])
		await expect_failure(source, {}, "argument", "unpaired surrogate");
	for (const key of Object.keys(limits)) {
		for (const value of [0, -1, 1.5, NaN, Infinity, limits[key] + 1])
			await expect_failure("p(a).", { [key]: value }, "argument", key + " validation");
	}
	const unicode = 'p("\u00e9").';
	await expect_failure(unicode, { maxInputBytes: unicode.length }, "limit", "UTF-8 byte count");
	await evaluate('p("\ud83d\ude00").');
	await expect_failure("p(a).", { maxInputBytes: 1 }, "limit", "source code-unit bound");
	await expect_failure("p(a). p(b).", { maxTuples: 1 }, "limit", "engine tuple bound");
	await expect_failure("p(a).", { maxMemoryBytes: 1 }, "limit", "engine memory bound");
	await expect_failure("p(a). p(b). p(X)?", { maxRows: 1 }, "limit", "row bound");
	await expect_failure("p(a). p(X)? p(X)?", { maxRows: 1 }, "limit", "aggregate row bound");
	await expect_failure("p(a). p(X)?", { maxOutputBytes: 96 }, "limit", "payload budget before copy");
	await evaluate("p(a). p(X)?", { maxOutputBytes: 97 });
	await expect_failure("p(a). p(b)? p(b)?", { maxOutputBytes: 32 }, "limit", "empty query budget");
	await expect_failure('p(""). p(X)?', { maxOutputBytes: 95 }, "limit", "empty cell budget");
	await expect_failure("p(a). p(a)?", { maxOutputBytes: 63 }, "limit", "zero-column row budget");
	await evaluate("p(a). p(a)?", { maxOutputBytes: 64 });
	const rejected = await expect_failure("p(.", {}, "rejected", "syntax error");
	assert(rejected.engineClass === 1 && rejected.diagnostics.some(d =>
	    d.level === "error" && d.location?.line === 1 && d.location.column > 0),
	    "error diagnostics retain source locations and C class");
	await expect_failure("p(.", { maxDiagnosticBytes: 1 }, "limit", "diagnostic budget");
	await expect_failure("p(X) :- not q(X).", {}, "rejected", "program without queries is checked");
	await expect_failure("p(0). q(X) :- p(Y), X = 1 / Y. q(X)?", {},
	    "evaluation", "evaluation error");
	const fallback = await evaluate(fixtures[0].program, { maxMagic: 1 });
	assert(fallback.queries[0].rows.length === 3, "rewrite work uses native fallback semantics");
	checks++;

	const stopped = new AbortController();
	stopped.abort();
	await expect_failure(busy_program, { signal: stopped.signal }, "cancelled", "abort before startup");
	const running = new AbortController();
	const pending = expect_failure(busy_program, { signal: running.signal },
	    "cancelled", "abort during operation");
	const abort_timer = setTimeout(() => running.abort(), 75);
	try {
		await pending;
	} finally {
		clearTimeout(abort_timer);
	}
	await expect_failure(busy_program, { timeoutMs: 1 }, "timeout", "startup deadline");
	await expect_failure(busy_program, { timeoutMs: 100 }, "timeout", "busy operation deadline");
	const recovered = await evaluate("p(ok). p(X)?");
	assert(recovered.queries[0].rows[0][0].value === "ok", "recovery after all failure kinds");
	checks++;
	return checks;
}
