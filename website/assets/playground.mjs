/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

import { evaluate } from "./wasm/relic.mjs";
import { examples } from "./examples.mjs";
import { format_rows } from "./format.mjs";

const form = document.getElementById("playground");
const program = document.getElementById("program");
const select = document.getElementById("example");
const run = document.getElementById("run");
const stop = document.getElementById("stop");
const status = document.getElementById("status");
const results = document.getElementById("results");
const placeholder = document.getElementById("result-placeholder");
const diagnostics = document.getElementById("diagnostics");
let controller = null;

function set_running(running) {
	run.disabled = running;
	stop.disabled = !running;
	select.disabled = running;
	program.readOnly = running;
}

function show_diagnostics(items) {
	document.getElementById("diagnostics-section").hidden = items.length === 0;
	for (const item of items) {
		const entry = document.createElement("li");
		const location = item.location;
		entry.textContent = item.level + (location ?
		    " at " + location.line + ":" + location.column : "") + ": " + item.message;
		diagnostics.append(entry);
	}
}

async function submit(event) {
	event.preventDefault();
	if (controller)
		return;
	controller = new AbortController();
	set_running(true);
	status.textContent = "Running...";
	placeholder.hidden = true;
	results.replaceChildren();
	diagnostics.replaceChildren();
	document.getElementById("results-section").hidden = true;
	document.getElementById("diagnostics-section").hidden = true;
	try {
		const result = await evaluate(program.value, {
			signal: controller.signal, maxRows: 1000, maxOutputBytes: 1048576
		});
		show_diagnostics(result.diagnostics);
		for (let i = 0; i < result.queries.length; i++) {
			const heading = document.createElement("h3");
			heading.textContent = "Query " + (i + 1);
			const text = document.createElement("pre");
			text.textContent = format_rows(result.queries[i].rows);
			results.append(heading, text);
		}
		document.getElementById("results-section").hidden = result.queries.length === 0;
		placeholder.hidden = result.queries.length !== 0;
		placeholder.textContent = result.queries.length === 0 ?
		    "Add a query ending in ? to see its answers." : "";
		status.textContent = "Completed with relic " + result.version + "." +
		    (result.queries.length === 0 ? " No queries in this program." : "");
	} catch (error) {
		show_diagnostics(error.diagnostics ?? []);
		placeholder.hidden = true;
		status.textContent = error.message;
	} finally {
		controller = null;
		set_running(false);
	}
}

if (typeof Worker !== "undefined" && typeof WebAssembly !== "undefined" &&
    typeof BigInt !== "undefined" && typeof AbortController !== "undefined") {
	for (let i = 0; i < examples.length; i++) {
		const option = document.createElement("option");
		option.value = i;
		option.textContent = examples[i].name;
		select.append(option);
	}
	program.value = examples[0].program;
	select.addEventListener("change", () => { program.value = examples[select.value].program; });
	form.addEventListener("submit", submit);
	stop.addEventListener("click", () => controller?.abort());
	set_running(false);
	status.textContent = "Ready.";
	document.getElementById("availability").hidden = true;
}
