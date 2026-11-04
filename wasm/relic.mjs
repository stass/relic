/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

const node_threads = typeof process !== "undefined" && process.versions?.node ?
    await import("node:worker_threads") : null;

export const limits = Object.freeze({
	timeoutMs: 5000,
	maxInputBytes: 1048576,
	maxMemoryBytes: 67108864,
	maxTuples: 100000,
	maxMagic: 100000,
	maxRows: 10000,
	maxOutputBytes: 8388608,
	maxDiagnosticBytes: 65536
});

export class RelicError extends Error {
	constructor(kind, message, diagnostics = [], engine_class = null) {
		super(message);
		this.name = "RelicError";
		this.kind = kind;
		this.engineClass = engine_class;
		this.diagnostics = diagnostics;
	}
}

function validate(program, options) {
	if (typeof program !== "string")
		throw new RelicError("argument", "Program must be a string.");
	if (options === null || typeof options !== "object")
		throw new RelicError("argument", "Options must be an object.");
	const config = { ...limits };
	for (const key of Reflect.ownKeys(options)) {
		if (key === "signal")
			continue;
		if (!Object.hasOwn(limits, key))
			throw new RelicError("argument", "Unknown option: " + String(key));
		const value = options[key];
		if (!Number.isSafeInteger(value) || value <= 0 || value > limits[key])
			throw new RelicError("argument", "Invalid limit: " + key);
		config[key] = value;
	}
	const signal = options.signal;
	if (signal !== undefined && !(signal instanceof AbortSignal))
		throw new RelicError("argument", "signal must be an AbortSignal.");
	if (program.length > config.maxInputBytes)
		throw new RelicError("limit", "Program exceeds the input byte limit.");
	for (let i = 0; i < program.length; i++) {
		const unit = program.charCodeAt(i);
		if (unit === 0)
			throw new RelicError("argument", "Program contains a literal NUL.");
		if (unit >= 0xd800 && unit <= 0xdbff) {
			const next = program.charCodeAt(++i);
			if (next >= 0xdc00 && next <= 0xdfff)
				continue;
		} else if (unit < 0xdc00 || unit > 0xdfff) {
			continue;
		}
		throw new RelicError("argument", "Program contains an unpaired surrogate.");
	}
	const source = new TextEncoder().encode(program);
	if (source.length > config.maxInputBytes)
		throw new RelicError("limit", "Program exceeds the input byte limit.");
	return { source, config, signal };
}

/* One settlement owns worker destruction, timer and abort listener cleanup. */
export async function evaluate(program, options = {}) {
	let input;
	try {
		input = validate(program, options);
	} catch (error) {
		if (error instanceof RelicError)
			throw error;
		throw new RelicError("argument", "Could not read program options.");
	}
	const { source, config, signal } = input;
	if (signal?.aborted)
		throw new RelicError("cancelled", "Evaluation cancelled.");

	return new Promise((resolve, reject) => {
		let worker, timer, settled = false;
		const settle = (error, result) => {
			if (settled)
				return;
			settled = true;
			clearTimeout(timer);
			signal?.removeEventListener("abort", abort);
			worker?.terminate();
			if (error)
				reject(error);
			else
				resolve(result);
		};
		const abort = () => settle(new RelicError("cancelled", "Evaluation cancelled."));
		const failure = () => settle(new RelicError("worker", "Could not run the Wasm worker."));
		const reply = data => {
			if (data.error) {
				const e = data.error;
				settle(new RelicError(e.kind, e.message, e.diagnostics, e.engineClass));
			} else {
				settle(null, data.result);
			}
		};

		timer = setTimeout(() => settle(new RelicError("timeout",
		    "Evaluation exceeded its time limit.")), config.timeoutMs);
		try {
			const Worker = node_threads?.Worker ?? globalThis.Worker;
			// The worker runs a file, even when its Node caller runs --eval
			// or --input-type. Caller execution flags do not apply to it.
			worker = new Worker(new URL("./relic-worker.mjs", import.meta.url),
			    node_threads ? { execArgv: [] } : { type: "module" });
			if (node_threads) {
				worker.once("message", reply);
				worker.once("error", failure);
				worker.once("messageerror", failure);
				worker.once("exit", () => { if (!settled) failure(); });
			} else {
				worker.onmessage = event => reply(event.data);
				worker.onerror = event => { event.preventDefault(); failure(); };
				worker.onmessageerror = failure;
			}
			signal?.addEventListener("abort", abort, { once: true });
			if (signal?.aborted) {
				abort();
				return;
			}
			worker.postMessage({ source, config }, [source.buffer]);
		} catch {
			failure();
		}
	});
}
