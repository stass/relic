/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

import create_engine from "./relic-engine.mjs";

const port = typeof self === "undefined" ?
    (await import("node:worker_threads")).parentPort : null;
const decode = new TextDecoder();

/*
 * Each retained object is charged even if its payload is empty. These are
 * copy budgets, not estimates of the JavaScript allocator's object sizes.
 * A failed charge is sticky: callbacks stop copying and C stops at its next
 * stage boundary. No partial answer is published on any failure.
 */
class Bridge {
	constructor(config) {
		this.config = config;
		this.queries = [];
		this.diagnostics = [];
		this.output_bytes = 0;
		this.diagnostic_bytes = 0;
		this.rows = 0;
		this.failed = false;
		this.message = "";
	}

	refuse(message) {
		if (!this.failed)
			this.message = message;
		this.failed = true;
		return 1;
	}

	charge_output(bytes) {
		if (this.failed)
			return 1;
		if (bytes > this.config.maxOutputBytes - this.output_bytes)
			return this.refuse("Evaluation exceeds the output byte limit.");
		this.output_bytes += bytes;
		return 0;
	}

	query() {
		if (this.charge_output(32))
			return 1;
		this.answer = { rows: [] };
		this.queries.push(this.answer);
		return 0;
	}

	row(count) {
		if (this.rows >= this.config.maxRows)
			return this.refuse("Evaluation exceeds the returned row limit.");
		if (this.charge_output(32 + count * 32))
			return 1;
		this.rows++;
		this.cells = [];
		this.answer.rows.push(this.cells);
		return 0;
	}

	integer(value) {
		this.cells.push({ type: "integer", value });
	}

	bytes(type, pointer, length, heap) {
		if (this.charge_output(length))
			return;
		const bytes = heap.subarray(pointer, pointer + length);
		const value = type === 0 ? decode.decode(bytes) : bytes.slice();
		this.cells.push({ type: type === 0 ? "symbol" : "string", value });
	}

	diagnostic(level, message, length, file, file_length, line, column, heap) {
		if (this.failed)
			return;
		const bytes = 64 + length + file_length;
		if (bytes > this.config.maxDiagnosticBytes - this.diagnostic_bytes) {
			this.refuse("Evaluation exceeds the diagnostic byte limit.");
			return;
		}
		this.diagnostic_bytes += bytes;
		this.diagnostics.push({
			level: ["note", "warning", "error"][level],
			message: decode.decode(heap.subarray(message, message + length)),
			location: line === 0 ? null : {
				file: file === 0 ? null : decode.decode(heap.subarray(file, file + file_length)),
				line, column
			}
		});
	}
}

async function execute({ source, config }) {
	const bridge = new Bridge(config);
	let engine, pointer;
	try {
		engine = await create_engine({ bridge, print() {}, printErr() {} });
		pointer = engine._malloc(source.length + 1);
		if (!pointer) {
			send({ error: { kind: "memory", message: "Could not allocate program input.",
			    diagnostics: [], engineClass: null } });
			return;
		}
		engine.HEAPU8.set(source, pointer);
		engine.HEAPU8[pointer + source.length] = 0;
		const status = engine._relic_wasm_evaluate(pointer, config.maxInputBytes,
		    config.maxMemoryBytes, config.maxTuples, config.maxMagic);
		if (bridge.failed || status !== 0) {
			const kinds = ["internal", "rejected", "evaluation", "limit",
			    "internal", "memory", "cancelled", "internal", "internal"];
			const last_error = bridge.diagnostics.findLast(d => d.level === "error");
			send({ error: {
				kind: bridge.failed ? "limit" : (kinds[status] ?? "internal"),
				message: bridge.failed ? bridge.message :
				    (last_error?.message ?? "The engine could not evaluate the program."),
				diagnostics: bridge.diagnostics,
				engineClass: bridge.failed ? null : status
			} });
			return;
		}
		const version = engine.UTF8ToString(engine._relic_version());
		send({ result: { version, queries: bridge.queries, diagnostics: bridge.diagnostics } });
	} catch {
		send({ error: { kind: "worker", message: "The Wasm runtime could not complete evaluation.",
		    diagnostics: bridge.diagnostics, engineClass: null } });
	} finally {
		if (pointer)
			engine._free(pointer);
	}
}

function send(message) {
	if (port)
		port.postMessage(message);
	else
		self.postMessage(message);
}

if (port)
	port.once("message", execute);
else
	self.onmessage = event => { self.onmessage = null; execute(event.data); };
