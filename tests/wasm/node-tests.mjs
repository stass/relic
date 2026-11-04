#!/usr/bin/env node
/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

import assert from "node:assert/strict";
import { cp, mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve, join } from "node:path";
import { spawnSync } from "node:child_process";
import { pathToFileURL } from "node:url";
import { fixtures } from "./fixtures.mjs";
import { run_tests } from "./api-tests.mjs";

const directory = resolve(process.argv[2] ?? "disposable/wasm-dist");
const api = await import(pathToFileURL(join(directory, "relic.mjs")));
const count = await run_tests(api);

const interactive = spawnSync(process.execPath, ["--input-type=module", "-e",
	"const { evaluate } = await import(" +
	JSON.stringify(pathToFileURL(join(directory, "relic.mjs")).href) +
	"); await evaluate('p(a). p(X)?');"], { encoding: "utf8", timeout: 10000 });
assert.equal(interactive.status, 0, "Node module input: " + interactive.stderr);

// Every fixture has both a native transcript and an exact typed JS oracle.
const executable = resolve(process.argv[3] ?? "relic");
for (const fixture of fixtures) {
	const result = spawnSync(executable, ["-s"], { input: fixture.program, encoding: "utf8" });
	assert.equal(result.status, 0, fixture.name + ": " + result.stderr);
	assert.equal(result.stdout, fixture.native, fixture.name + ": native agreement");
}

const missing = await mkdtemp(join(tmpdir(), "relic-missing-assets-"));
try {
	await cp(join(directory, "relic.mjs"), join(missing, "relic.mjs"));
	const isolated = await import(pathToFileURL(join(missing, "relic.mjs")));
	await assert.rejects(isolated.evaluate("p(a)."), error => error.kind === "worker");
} finally {
	await rm(missing, { recursive: true, force: true });
}
console.log(`PASS: ${count} public API scenarios, ${fixtures.length} native agreement fixtures, missing assets`);
